/*
 * Pattern ACK detection DSP test
 *
 * Exercises the Welch-Costas pattern-ACK path used by the ARQ layer in place
 * of the coded DATAC16 ACK: mfsk_pattern_tx() generates the int16 passband
 * tone burst, mfsk_pattern_detect() recovers it from a noisy passband window.
 *
 * Radio/DSP-path scope (per the test-scope preference): asserts the physical
 * false-alarm and detection behaviour, not FSM wiring.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "unity.h"
#include "modem_mfsk.h"
#include "mfsk.h"
#include "mfsk_sync.h"
#include "mfsk_ofdm.h"
#include <complex.h>

/* arq.h pattern-kind constants (0 = ACK, 1 = BREAK) — mirrored here so the
 * test does not need the whole ARQ header just for two integers. */
#define PAT_ACK   0
#define PAT_BREAK 1

static uint64_t s_rng = 0x12345;
static double urand(void)
{
    s_rng = s_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(s_rng >> 11) / (double)(1ULL << 53);
}

void setUp(void)    { s_rng = 0x12345; }
void tearDown(void) { }

/* mfsk_detect_patterns() scores several tone lists in one pass so the RX loop
 * stops paying for the same FFTs twice.  That is only safe if it is exactly
 * equivalent to the per-list calls it replaces -- a scoring shortcut that
 * quietly changed a match count would move the ACK detection threshold without
 * anything else noticing.  Assert bit-identical scores AND positions. */
void test_detect_patterns_matches_per_list_calls(void)
{
    mfsk_t m;
    ofdm_frame_t o;
    mfsk_init(&m, 32, 50, 1);
    ofdm_frame_init(&o, 256, 50, 0.25, 0);

    int ns   = m.ack_pattern_nsymb;
    int len  = ns * ofdm_frame_nofdm(&o) * 2;   /* room to slide */
    double complex *rx = malloc(sizeof(double complex) * (size_t)len);
    TEST_ASSERT_NOT_NULL(rx);

    /* Several independent buffers, so the comparison is not one lucky draw. */
    for (int trial = 0; trial < 3; trial++)
    {
        for (int i = 0; i < len; i++)
            rx[i] = (urand() - 0.5) + (urand() - 0.5) * I;

        int pos_a = -2, pos_b = -2;
        int sa = mfsk_detect_pattern(&m, &o, rx, len, m.ack_tones,
                                     m.ack_pattern_len, ns, &pos_a);
        int sb = mfsk_detect_pattern(&m, &o, rx, len, m.break_tones,
                                     m.ack_pattern_len, ns, &pos_b);

        const int *lists[2] = { m.ack_tones, m.break_tones };
        int scores[2] = { -1, -1 }, pos[2] = { -2, -2 };
        mfsk_detect_patterns(&m, &o, rx, len, lists, 2,
                             m.ack_pattern_len, ns, scores, pos);

        TEST_ASSERT_EQUAL_INT(sa, scores[0]);
        TEST_ASSERT_EQUAL_INT(sb, scores[1]);
        TEST_ASSERT_EQUAL_INT(pos_a, pos[0]);
        TEST_ASSERT_EQUAL_INT(pos_b, pos[1]);
    }

    free(rx);
}

/* Baseband of the ACK (kind 0) or BREAK (kind 1) pattern, amplitude a, added
 * into rx at offset off (one stream). */
static void add_pattern_bb(const mfsk_t *m, const ofdm_frame_t *o, int kind, double a,
                           double complex *rx, int off)
{
    int ns = m->ack_pattern_nsymb, Nc = o->Nc, Nfft = o->Nfft, Nofdm = ofdm_frame_nofdm(o);
    mfsk_cplx *bins = calloc((size_t)ns * (size_t)Nc, sizeof(mfsk_cplx));
    double complex sym[2048], pad[2048], td[2048], gi[2048];
    if (kind) mfsk_generate_break_pattern(m, bins); else mfsk_generate_ack_pattern(m, bins);
    for (int p = 0; p < ns; p++) {
        for (int c = 0; c < Nc; c++) sym[c] = bins[p * Nc + c].re + I * bins[p * Nc + c].im;
        ofdm_zero_padder(o, sym, pad);
        ofdm_ifft(o, pad, td);
        for (int n = 0; n < Nfft; n++) td[n] /= sqrt((double)Nfft);
        ofdm_gi_adder(o, td, gi);
        for (int n = 0; n < Nofdm; n++) rx[off + p * Nofdm + n] += a * gi[n];
    }
    free(bins);
}

/* The streaming detector keeps one FFT per step and scores every list by
 * lookup: on the same baseband and grid it must give the batch detector's
 * best score and start for every list, bit for bit -- fed in any chunks. */
void test_stream_detector_matches_batch(void)
{
    mfsk_t m;
    ofdm_frame_t o;
    mfsk_init(&m, 32, 50, 1);
    ofdm_frame_init(&o, 256, 50, 0.25, 0);
    int ns = m.ack_pattern_nsymb, Nofdm = ofdm_frame_nofdm(&o);
    int len = ns * Nofdm * 4;
    double complex *rx = malloc(sizeof(double complex) * (size_t)len);
    TEST_ASSERT_NOT_NULL(rx);
    const int *lists[2] = { m.ack_tones, m.break_tones };

    for (int trial = 0; trial < 12; trial++) {
        for (int i = 0; i < len; i++) rx[i] = (urand() - 0.5) + (urand() - 0.5) * I;
        if (trial % 3 != 0) {    /* a pattern somewhere, at a modest SNR */
            int off = (int)(urand() * (double)(len - ns * Nofdm));
            add_pattern_bb(&m, &o, trial % 2, 0.6 + 0.4 * urand(), rx, off);
        }
        int scores[2], pos[2];
        mfsk_detect_patterns(&m, &o, rx, len, lists, 2, m.ack_pattern_len, ns, scores, pos);

        mfsk_stream_det_t *d = mfsk_stream_det_new(&m, &o, m.ack_pattern_len, ns);
        TEST_ASSERT_NOT_NULL(d);
        mfsk_stream_det_set_list(d, 0, m.ack_tones, m.ack_match_threshold);
        mfsk_stream_det_set_list(d, 1, m.break_tones, m.break_match_threshold);
        for (int i = 0; i < len; ) {
            int c = 1 + (int)(urand() * 700.0);
            if (c > len - i) c = len - i;
            mfsk_stream_event_t ev[4];
            mfsk_stream_det_push(d, rx + i, c, ev, 4);
            i += c;
        }
        for (int l = 0; l < 2; l++) {
            int sc; long long ps;
            mfsk_stream_det_best(d, l, &sc, &ps);
            TEST_ASSERT_EQUAL_INT(scores[l], sc);
            TEST_ASSERT_EQUAL_INT(pos[l], (int)ps);
        }
        mfsk_stream_det_free(d);
    }
    free(rx);
}

/* One pattern, one event, at its start, for the right list -- and none for
 * the other list, or for noise. */
void test_stream_detector_events(void)
{
    mfsk_t m;
    ofdm_frame_t o;
    mfsk_init(&m, 32, 50, 1);
    ofdm_frame_init(&o, 256, 50, 0.25, 0);
    int ns = m.ack_pattern_nsymb, Nofdm = ofdm_frame_nofdm(&o);
    int len = ns * Nofdm * 6;
    double complex *rx = malloc(sizeof(double complex) * (size_t)len);
    TEST_ASSERT_NOT_NULL(rx);
    mfsk_stream_det_t *d = mfsk_stream_det_new(&m, &o, m.ack_pattern_len, ns);
    TEST_ASSERT_NOT_NULL(d);
    mfsk_stream_det_set_list(d, 0, m.ack_tones, m.ack_match_threshold);
    mfsk_stream_det_set_list(d, 1, m.break_tones, m.break_match_threshold);
    for (int trial = 0; trial < 8; trial++) {
        for (int i = 0; i < len; i++) rx[i] = 0.3 * ((urand() - 0.5) + (urand() - 0.5) * I);
        int kind = trial % 2, off = Nofdm * 2 + (int)(urand() * (double)(Nofdm * 2));
        add_pattern_bb(&m, &o, kind, 1.0, rx, off);
        mfsk_stream_det_reset(d);
        mfsk_stream_event_t ev[16];
        int nev = 0;
        for (int i = 0; i < len; i += 160)
            nev += mfsk_stream_det_push(d, rx + i, len - i < 160 ? len - i : 160, ev + nev, 16 - nev);
        TEST_ASSERT_EQUAL_INT(1, nev);
        TEST_ASSERT_EQUAL_INT(kind, ev[0].list);
        TEST_ASSERT_TRUE(ev[0].score >= 14);
        /* Every start up to a guard interval early puts the FFT wholly inside
         * the symbol, and the first best wins; then the step grid. */
        int gi = Nofdm - o.Nfft;
        TEST_ASSERT_TRUE(ev[0].pos >= off - gi - Nofdm / 8 && ev[0].pos <= off + Nofdm / 8);
    }
    mfsk_stream_det_free(d);
    free(rx);
}

/* Session lists: the same on both ends, apart from everything else by at most
 * 3 symbols at any time and frequency shift, and the global lists for key 0. */
void test_session_lists_are_separated(void)
{
    mfsk_t m;
    mfsk_init(&m, 32, 50, 1);
    int NS = m.ack_pattern_nsymb;
    int g_ack[16], g_brk[16], g_hail[16];
    mfsk_pattern_expand(&m, m.ack_tones, g_ack);
    mfsk_pattern_expand(&m, m.break_tones, g_brk);
    mfsk_pattern_expand(&m, m.hail_tones, g_hail);
    int ack[MFSK_MAX_ACK_TONES], brk[MFSK_MAX_ACK_TONES];
    TEST_ASSERT_FALSE(mfsk_session_patterns(&m, 0, NULL, 0, ack, brk));
    TEST_ASSERT_EQUAL_INT_ARRAY(m.ack_tones, ack, 8);
    TEST_ASSERT_EQUAL_INT_ARRAY(m.break_tones, brk, 8);
    for (uint32_t key = 1; key < 65536; key += 211) {
        TEST_ASSERT_TRUE(mfsk_session_patterns(&m, key, NULL, 0, ack, brk));
        int ack2[MFSK_MAX_ACK_TONES], brk2[MFSK_MAX_ACK_TONES];
        mfsk_session_patterns(&m, key, NULL, 0, ack2, brk2);          /* the other end */
        TEST_ASSERT_EQUAL_INT_ARRAY(ack, ack2, 8);
        TEST_ASSERT_EQUAL_INT_ARRAY(brk, brk2, 8);
        int ea[16], eb[16];
        mfsk_pattern_expand(&m, ack, ea);
        mfsk_pattern_expand(&m, brk, eb);
        TEST_ASSERT_LESS_OR_EQUAL_INT(3, mfsk_seq_xmatch(32, ea, NS, ea, NS, 4, true));
        TEST_ASSERT_LESS_OR_EQUAL_INT(3, mfsk_seq_xmatch(32, eb, NS, eb, NS, 4, true));
        TEST_ASSERT_LESS_OR_EQUAL_INT(3, mfsk_seq_xmatch(32, ea, NS, eb, NS, 4, false));
        const int *g[3] = { g_ack, g_brk, g_hail };
        for (int i = 0; i < 3; i++) {
            TEST_ASSERT_LESS_OR_EQUAL_INT(3, mfsk_seq_xmatch(32, ea, NS, g[i], NS, 4, false));
            TEST_ASSERT_LESS_OR_EQUAL_INT(3, mfsk_seq_xmatch(32, eb, NS, g[i], NS, 4, false));
        }
    }
}

#define CHUNK 160
static void push_silence_isb(mfsk_pattern_window_t *w, int samples, int *hits, int *is_break);
static void push_silence(mfsk_pattern_window_t *w, int samples, int *hits);
static int push_pcm(mfsk_pattern_window_t *w, const int16_t *pcm, int n, int *is_break);

/* Detected kind through the window, or -1: pattern `kind` sent while the
 * sender's session key is `tx_key`, heard while the receiver's is `rx_key`. */
static int window_hears(uint32_t tx_key, int kind, uint32_t rx_key)
{
    const int burst = mfsk_pattern_max_tx_samples();
    int16_t *pat = calloc((size_t)burst, sizeof(int16_t));
    mfsk_pattern_set_session(tx_key);
    int n = mfsk_pattern_tx(pat, kind);
    mfsk_pattern_set_session(rx_key);
    mfsk_pattern_window_t w = {0};
    int hits = 0, isb = -1;
    push_silence(&w, burst, &hits);
    hits += push_pcm(&w, pat, n, &isb);
    push_silence_isb(&w, 2 * burst, &hits, &isb);
    mfsk_pattern_window_free(&w);
    free(pat);
    return hits ? isb : -1;
}

/* Bound to a session, a station hears its peer's ACK and BREAK -- and neither
 * another session's patterns nor the global ones every station used to send. */
void test_session_patterns_heard_only_by_the_session(void)
{
    TEST_ASSERT_EQUAL_INT(PAT_ACK,   window_hears(0x1D2B, PAT_ACK, 0x1D2B));
    TEST_ASSERT_EQUAL_INT(PAT_BREAK, window_hears(0x1D2B, PAT_BREAK, 0x1D2B));
    TEST_ASSERT_EQUAL_INT(-1, window_hears(0x4A71, PAT_ACK, 0x1D2B));
    TEST_ASSERT_EQUAL_INT(-1, window_hears(0x4A71, PAT_BREAK, 0x1D2B));
    TEST_ASSERT_EQUAL_INT(-1, window_hears(0, PAT_ACK, 0x1D2B));
    TEST_ASSERT_EQUAL_INT(-1, window_hears(0, PAT_BREAK, 0x1D2B));
    TEST_ASSERT_EQUAL_INT(-1, window_hears(0x1D2B, PAT_ACK, 0));
    TEST_ASSERT_EQUAL_INT(PAT_ACK,   window_hears(0, PAT_ACK, 0));     /* no session: global */
    TEST_ASSERT_EQUAL_INT(PAT_BREAK, window_hears(0, PAT_BREAK, 0));
    mfsk_pattern_set_session(0);
}

/* The NAV table keeps its separation, and class boundaries round up. */
void test_nav_table(void)
{
    mfsk_t m;
    mfsk_init(&m, 32, 50, 1);
    int NS = m.ack_pattern_nsymb, e[MFSK_NAV_CLASSES][16], g[3][16];
    mfsk_pattern_expand(&m, m.ack_tones, g[0]);
    mfsk_pattern_expand(&m, m.break_tones, g[1]);
    mfsk_pattern_expand(&m, m.hail_tones, g[2]);
    for (int k = 0; k < MFSK_NAV_CLASSES; k++) mfsk_pattern_expand(&m, mfsk_nav_tones[k], e[k]);
    for (int k = 0; k < MFSK_NAV_CLASSES; k++) {
        TEST_ASSERT_LESS_OR_EQUAL_INT(3, mfsk_seq_xmatch(32, e[k], NS, e[k], NS, 4, true));
        for (int i = 0; i < 3; i++)
            TEST_ASSERT_LESS_OR_EQUAL_INT(3, mfsk_seq_xmatch(32, e[k], NS, g[i], NS, 4, false));
        for (int j = 0; j < k; j++)
            TEST_ASSERT_LESS_OR_EQUAL_INT(4, mfsk_seq_xmatch(32, e[k], NS, e[j], NS, 4, false));
    }
    TEST_ASSERT_EQUAL_INT(0, mfsk_nav_class_for_ms(0));
    TEST_ASSERT_EQUAL_INT(0, mfsk_nav_class_for_ms(2000));
    TEST_ASSERT_EQUAL_INT(1, mfsk_nav_class_for_ms(2001));
    TEST_ASSERT_EQUAL_INT(MFSK_NAV_CLASSES - 1, mfsk_nav_class_for_ms(600000));
    for (uint32_t ms = 500; ms < 40000; ms += 137) {
        int k = mfsk_nav_class_for_ms(ms);
        TEST_ASSERT_TRUE(ms > mfsk_nav_class_ms(MFSK_NAV_CLASSES - 1) || mfsk_nav_class_ms(k) >= ms);
        TEST_ASSERT_TRUE(k == 0 || mfsk_nav_class_ms(k - 1) < ms);
    }
}

/* Every class is heard as itself -- no other class, no ACK or BREAK -- with
 * its start, whether a session binds the ACK/BREAK lists or not. */
void test_nav_classes_heard_as_sent(void)
{
    const int burst = mfsk_pattern_max_tx_samples();
    int16_t *pat = calloc((size_t)burst, sizeof(int16_t));
    for (int sess = 0; sess < 2; sess++) {
        mfsk_pattern_set_session(sess ? 0x2B47 : 0);
        for (int k = 0; k < MFSK_NAV_CLASSES; k++) {
            int n = mfsk_nav_tx(pat, k);
            TEST_ASSERT_EQUAL_INT(burst, n);
            mfsk_pattern_window_t w = {0};
            mfsk_pattern_ev_t ev[16];
            int nev = 0, lead = burst + 123;
            int16_t z[CHUNK] = {0};
            for (int t = 0; t < lead; t += CHUNK) {
                int c = lead - t < CHUNK ? lead - t : CHUNK;
                nev += mfsk_pattern_window_events(&w, z, c, ev + nev, 16 - nev);
            }
            for (int t = 0; t < n; t += CHUNK)
                nev += mfsk_pattern_window_events(&w, pat + t, n - t < CHUNK ? n - t : CHUNK, ev + nev, 16 - nev);
            for (int t = 0; t < 2 * burst; t += CHUNK)
                nev += mfsk_pattern_window_events(&w, z, CHUNK, ev + nev, 16 - nev);
            char what[64];
            snprintf(what, sizeof what, "NAV class %d, session %d", k, sess);
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, nev, what);
            TEST_ASSERT_EQUAL_INT_MESSAGE(MFSK_PAT_NAV(k), ev[0].kind, what);
            int gi = burst / 16 - 256;    /* a guard interval: Nofdm - Nfft */
            TEST_ASSERT_TRUE_MESSAGE(ev[0].start >= lead - gi - 64 && ev[0].start <= lead + 64, what);
            mfsk_pattern_window_free(&w);
        }
    }
    mfsk_pattern_set_session(0);
    free(pat);
}

/* Events a clean burst produces through the window: kinds into kinds[]. */
static int burst_events(const int16_t *pat, int n, int *kinds, int maxk)
{
    const int burst = mfsk_pattern_max_tx_samples();
    mfsk_pattern_window_t w = {0};
    mfsk_pattern_ev_t ev[16];
    int nev = 0;
    int16_t *x = calloc((size_t)(4 * burst), sizeof(int16_t));
    for (int i = 0; i < 4 * burst; i++) x[i] = (int16_t)((urand() - 0.5) * 60.0);   /* a little noise */
    for (int i = 0; i < n; i++) x[burst + i] = (int16_t)(x[burst + i] + pat[i]);
    for (int t = 0; t < 4 * burst; t += CHUNK)
        nev += mfsk_pattern_window_events(&w, x + t, CHUNK, ev + nev, 16 - nev);
    mfsk_pattern_window_free(&w);
    free(x);
    for (int e = 0; e < nev && e < maxk; e++) kinds[e] = ev[e].kind;
    return nev;
}

/* A clean pattern is one event of its own kind.  On air (2026-10-06, 3 %) a
 * session ACK at +15 dB was also read as NAV class 11, at its threshold, 35 ms
 * earlier -- a 32 s hold the sender then waited out: at high SNR a window
 * straddling two symbols can match either tone, so a list cross-reads more
 * than the whole-symbol separation says.  The window reports only the
 * strongest of overlapping events. */
void test_clean_patterns_are_one_event_each(void)
{
    const int burst = mfsk_pattern_max_tx_samples();
    int16_t *pat = calloc((size_t)burst, sizeof(int16_t));
    int kinds[16];
    for (uint32_t key = 1; key < 65536; key += 977) {
        mfsk_pattern_set_session(key);
        for (int kind = 0; kind < 2; kind++) {
            int n = mfsk_pattern_tx(pat, kind);
            int nev = burst_events(pat, n, kinds, 16);
            char what[64];
            snprintf(what, sizeof what, "key %u, %s", key, kind ? "BREAK" : "ACK");
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, nev, what);
            TEST_ASSERT_EQUAL_INT_MESSAGE(kind, kinds[0], what);
        }
    }
    for (int k = 0; k < MFSK_NAV_CLASSES; k++) {
        int n = mfsk_nav_tx(pat, k);
        int nev = burst_events(pat, n, kinds, 16);
        TEST_ASSERT_EQUAL_INT(1, nev);
        TEST_ASSERT_EQUAL_INT(MFSK_PAT_NAV(k), kinds[0]);
    }
    mfsk_pattern_set_session(0);
    free(pat);
}

/* Pure noise must never be mistaken for an ACK (false-alarm rejection). */
void test_noise_no_false_ack(void)
{
    int burst = mfsk_pattern_max_tx_samples();
    TEST_ASSERT_TRUE(burst > 0);

    int L = burst * 3;
    int16_t *pb = (int16_t *)malloc((size_t)L * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pb);

    /* Run many independent noise realisations; none may detect. */
    int false_alarms = 0;
    for (int trial = 0; trial < 40; trial++)
    {
        for (int i = 0; i < L; i++)
            pb[i] = (int16_t)((urand() - 0.5) * 4000.0);  /* moderate noise */
        int is_break = -1;
        if (mfsk_pattern_detect(pb, L, &is_break))
            false_alarms++;
    }
    TEST_ASSERT_EQUAL_INT(0, false_alarms);
    free(pb);
}

/* The pattern as a radio df Hz off would hear it: its analytic signal, from
 * a DFT over the MFSK band only (1-3 kHz; the tones sit at 1.5-2.5 kHz),
 * moved df up and taken back to real. */
static void shift_passband(const int16_t *in, int n, double df, int16_t *out)
{
    int k0 = (int)(1000.0 * n / 8000.0), k1 = (int)(3000.0 * n / 8000.0);
    double complex *X = (double complex *)calloc((size_t)(k1 - k0 + 1), sizeof(double complex));
    for (int k = k0; k <= k1; k++) {
        double complex acc = 0;
        for (int t = 0; t < n; t++) acc += in[t] * cexp(-2.0 * I * M_PI * k * t / n);
        X[k - k0] = 2.0 * acc;
    }
    for (int t = 0; t < n; t++) {
        double complex acc = 0;
        for (int k = k0; k <= k1; k++) acc += X[k - k0] * cexp(2.0 * I * M_PI * k * t / n);
        double v = creal(acc / n * cexp(2.0 * I * M_PI * df * t / 8000.0));
        out[t] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    free(X);
}

static int detect_shifted(int kind, double df, int *is_break)
{
    int burst = mfsk_pattern_max_tx_samples();
    int16_t *tone = (int16_t *)malloc((size_t)burst * sizeof(int16_t));
    int16_t *sh   = (int16_t *)malloc((size_t)burst * sizeof(int16_t));
    int n = mfsk_pattern_tx(tone, kind);
    shift_passband(tone, n, df, sh);
    int gap = burst, L = gap + n + burst;
    int16_t *pb = (int16_t *)malloc((size_t)L * sizeof(int16_t));
    for (int i = 0; i < L; i++) pb[i] = (int16_t)((urand() - 0.5) * 200.0);
    for (int i = 0; i < n; i++) pb[gap + i] = (int16_t)(pb[gap + i] + sh[i]);
    *is_break = -1;
    int hit = mfsk_pattern_detect(pb, L, is_break);
    free(tone); free(sh); free(pb);
    return hit;
}

/* Two radios are never on exactly the same frequency.  Taking a symbol only
 * when its tone was the peak bin, the detector held to +/-15 Hz, half a tone
 * spacing: on air an IC-7100 and an sBitx at 7.050 MHz never detected a
 * pattern from each other (0 of 5), where the MFSK data decoder, which
 * searches its offset, decoded the same frames.  It now searches +/-2 bins. */
void test_ack_detects_off_frequency(void)
{
    static const double DF[] = { -40.0, -25.0, 25.0, 40.0 };
    for (size_t i = 0; i < sizeof(DF) / sizeof(DF[0]); i++) {
        char what[48];
        int isb;
        snprintf(what, sizeof(what), "ACK %+.0f Hz off", DF[i]);
        TEST_ASSERT_TRUE_MESSAGE(detect_shifted(PAT_ACK, DF[i], &isb), what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, isb, what);
    }
    int isb;
    TEST_ASSERT_TRUE_MESSAGE(detect_shifted(PAT_BREAK, 25.0, &isb), "BREAK +25 Hz off");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, isb, "BREAK +25 Hz off");
}

/* A real ACK burst planted in a noise window must detect as ACK (not break). */
void test_real_ack_detects(void)
{
    int burst = mfsk_pattern_max_tx_samples();
    int16_t *tone = (int16_t *)malloc((size_t)burst * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(tone);
    int n = mfsk_pattern_tx(tone, PAT_ACK);
    TEST_ASSERT_TRUE(n > 0);

    int gap = burst;                 /* lead-in noise */
    int L   = gap + n + burst;       /* + trailing slack */
    int16_t *pb = (int16_t *)malloc((size_t)L * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pb);
    for (int i = 0; i < L; i++)
        pb[i] = (int16_t)((urand() - 0.5) * 200.0);   /* low noise */
    for (int i = 0; i < n; i++)
        pb[gap + i] = (int16_t)(pb[gap + i] + tone[i]);

    int is_break = -1;
    int hit = mfsk_pattern_detect(pb, L, &is_break);
    TEST_ASSERT_TRUE(hit);
    TEST_ASSERT_EQUAL_INT(0, is_break);   /* ACK, not break */

    free(tone);
    free(pb);
}

/* A real BREAK (ACK+TURN) burst must detect and be flagged as break. */
void test_real_break_detects(void)
{
    int burst = mfsk_pattern_max_tx_samples();
    int16_t *tone = (int16_t *)malloc((size_t)burst * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(tone);
    int n = mfsk_pattern_tx(tone, PAT_BREAK);
    TEST_ASSERT_TRUE(n > 0);

    int gap = burst;
    int L   = gap + n + burst;
    int16_t *pb = (int16_t *)malloc((size_t)L * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pb);
    for (int i = 0; i < L; i++)
        pb[i] = (int16_t)((urand() - 0.5) * 200.0);
    for (int i = 0; i < n; i++)
        pb[gap + i] = (int16_t)(pb[gap + i] + tone[i]);

    int is_break = -1;
    int hit = mfsk_pattern_detect(pb, L, &is_break);
    TEST_ASSERT_TRUE(hit);
    TEST_ASSERT_EQUAL_INT(1, is_break);   /* break, not plain ACK */

    free(tone);
    free(pb);
}

/* ---- The streaming window rx_thread uses ---------------------------------
 * CHUNK matches RX_DECODE_CHUNK_SAMPLES: the window is fed exactly as the live
 * receive loop feeds it. */

/* A match can surface during the silence that FOLLOWS a burst -- that is when
 * the next paced scan comes round -- so the break flag is recorded here too. */
static void push_silence_isb(mfsk_pattern_window_t *w, int samples, int *hits, int *is_break)
{
    int16_t z[CHUNK] = {0};
    for (int off = 0; off < samples; off += CHUNK)
    {
        int take = (samples - off < CHUNK) ? (samples - off) : CHUNK;
        int isb = 0;
        if (mfsk_pattern_window_push(w, z, take, &isb)) { (*hits)++; if (is_break) *is_break = isb; }
    }
}

static void push_silence(mfsk_pattern_window_t *w, int samples, int *hits)
{
    push_silence_isb(w, samples, hits, NULL);
}

static int push_pcm(mfsk_pattern_window_t *w, const int16_t *pcm, int n, int *is_break)
{
    int hits = 0;
    for (int off = 0; off < n; off += CHUNK)
    {
        int take = (n - off < CHUNK) ? (n - off) : CHUNK;
        int isb = 0;
        if (mfsk_pattern_window_push(w, pcm + off, take, &isb)) { hits++; if (is_break) *is_break = isb; }
    }
    return hits;
}

/* The window reports a burst as it ends, not a scan later: within a few steps
 * of its last sample (plus the low-pass filter's delay), and only once. */
void test_window_reports_a_burst_promptly_and_once(void)
{
    const int burst = mfsk_pattern_max_tx_samples();
    int16_t *pat = calloc((size_t)burst, sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pat);
    int n = mfsk_pattern_tx(pat, PAT_ACK);
    mfsk_pattern_window_t w = {0};
    int hits = 0, isb = -1;
    push_silence(&w, burst, &hits);
    hits += push_pcm(&w, pat, n, &isb);
    int after = 0;
    while (!hits && after < burst) { push_silence_isb(&w, CHUNK, &hits, &isb); after += CHUNK; }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, hits, "ACK not reported");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(burst / 4, after, "ACK reported late");
    TEST_ASSERT_EQUAL_INT(0, isb);
    push_silence(&w, 4 * burst, &hits);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, hits, "one burst, more than one event");
    mfsk_pattern_window_free(&w);
    free(pat);
}

/* Pacing must cost no sensitivity: a real ACK fed in live-sized chunks is found
 * wherever it lands relative to the scan cadence. */
void test_window_finds_a_chunked_ack_at_any_scan_phase(void)
{
    const int burst = mfsk_pattern_max_tx_samples();
    int16_t *pat = calloc((size_t)burst, sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pat);
    int n = mfsk_pattern_tx(pat, PAT_ACK);
    TEST_ASSERT_GREATER_THAN(0, n);

    for (int phase = 0; phase < burst; phase += burst / 8)
    {
        mfsk_pattern_window_t w = {0};
        int hits = 0, isb = -1;
        push_silence(&w, phase, &hits);
        hits += push_pcm(&w, pat, n, &isb);
        push_silence_isb(&w, 2 * burst, &hits, &isb);   /* let a scan come round */
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, hits, "chunked ACK missed at some scan phase");
        TEST_ASSERT_EQUAL_INT(0, isb);
        mfsk_pattern_window_free(&w);
    }
    free(pat);
}

/* A burst cut by a reset must not be reported once the caller waits for a
 * DIFFERENT pattern: what came before the reset belongs to the previous
 * exchange, and taking its tail for this frame's ACK would advance the sender
 * past a frame the peer never received. */
void test_reset_discards_a_stale_burst(void)
{
    const int burst = mfsk_pattern_max_tx_samples();
    int16_t *pat = calloc((size_t)burst, sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pat);
    int n = mfsk_pattern_tx(pat, PAT_ACK);

    mfsk_pattern_window_t w = {0};
    int hits = 0;
    push_silence(&w, burst / 2, &hits);
    hits += push_pcm(&w, pat, n / 2, NULL);
    mfsk_pattern_window_reset(&w);                 /* a new ACK becomes due */
    hits += push_pcm(&w, pat + n / 2, n - n / 2, NULL);
    push_silence(&w, 2 * burst, &hits);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits,
        "a burst from before the reset was reported as a new ACK");
    mfsk_pattern_window_free(&w);
    free(pat);
}

/* ...and the control: without the reset, that same split burst IS found.  If
 * this ever stops finding it, the test above has stopped testing. */
void test_without_reset_the_stale_burst_is_found(void)
{
    const int burst = mfsk_pattern_max_tx_samples();
    int16_t *pat = calloc((size_t)burst, sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pat);
    int n = mfsk_pattern_tx(pat, PAT_ACK);

    mfsk_pattern_window_t w = {0};
    int hits = 0;
    push_silence(&w, burst / 2, &hits);
    hits += push_pcm(&w, pat, n / 2, NULL);
    hits += push_pcm(&w, pat + n / 2, n - n / 2, NULL);
    push_silence(&w, 2 * burst, &hits);
    TEST_ASSERT_EQUAL_INT(1, hits);
    mfsk_pattern_window_free(&w);
    free(pat);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_detect_patterns_matches_per_list_calls);
    RUN_TEST(test_stream_detector_matches_batch);
    RUN_TEST(test_stream_detector_events);
    RUN_TEST(test_session_lists_are_separated);
    RUN_TEST(test_session_patterns_heard_only_by_the_session);
    RUN_TEST(test_nav_table);
    RUN_TEST(test_nav_classes_heard_as_sent);
    RUN_TEST(test_clean_patterns_are_one_event_each);
    RUN_TEST(test_noise_no_false_ack);
    RUN_TEST(test_real_ack_detects);
    RUN_TEST(test_real_break_detects);
    RUN_TEST(test_ack_detects_off_frequency);
    RUN_TEST(test_window_reports_a_burst_promptly_and_once);
    RUN_TEST(test_window_finds_a_chunked_ack_at_any_scan_phase);
    RUN_TEST(test_reset_discards_a_stale_burst);
    RUN_TEST(test_without_reset_the_stale_burst_is_found);
    return UNITY_END();
}
