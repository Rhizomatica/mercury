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
#define CHUNK 160

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

/* Pacing: the window correlates once per burst of new audio, not once per
 * chunk.  Correlating a multi-burst window on every 160-sample chunk cost
 * 4.5 s of CPU per second of audio and left the receive loop so far behind
 * that ACKs arriving inside WAIT_ACK were processed after it closed.  since_scan
 * is the observable: it must keep counting across chunks and reset only when a
 * full burst of new audio has accumulated. */
void test_window_scans_once_per_burst_not_per_chunk(void)
{
    const int burst = mfsk_pattern_max_tx_samples();
    mfsk_pattern_window_t w = {0};
    int hits = 0;

    push_silence(&w, burst, &hits);                /* first full burst: one scan */
    TEST_ASSERT_EQUAL_INT(0, w.since_scan);

    push_silence(&w, CHUNK, &hits);                /* one more chunk: no scan */
    TEST_ASSERT_EQUAL_INT_MESSAGE(CHUNK, w.since_scan,
        "window correlated on a single new chunk: pacing is gone");
    push_silence(&w, burst - 2 * CHUNK, &hits);    /* still short of a burst */
    TEST_ASSERT_EQUAL_INT(burst - CHUNK, w.since_scan);
    push_silence(&w, CHUNK, &hits);                /* completes the burst: scan */
    TEST_ASSERT_EQUAL_INT(0, w.since_scan);

    /* The window is bounded at two bursts, not three. */
    push_silence(&w, 4 * burst, &hits);
    TEST_ASSERT_LESS_OR_EQUAL_INT(2 * burst, w.cap);
    TEST_ASSERT_EQUAL_INT(0, hits);
    mfsk_pattern_window_free(&w);
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

/* A burst that was never matched must not be reported once the caller waits
 * for a DIFFERENT ACK.  The window holds over a second of audio; a burst left
 * unmatched by the previous exchange would otherwise be found by the next scan
 * and taken as this frame's ACK, advancing the sender past a frame the peer
 * never received.
 *
 * The setup is what makes this a real test.  Matching the burst before the
 * reset would assert nothing, so the scan cadence is first offset by half a
 * burst: the scan that falls during the burst sees only part of it, and the
 * burst then sits COMPLETE in the window with no scan due -- the state a closed
 * gate leaves behind on a real miss. */
void test_reset_discards_a_stale_burst(void)
{
    const int burst = mfsk_pattern_max_tx_samples();
    int16_t *pat = calloc((size_t)burst, sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pat);
    int n = mfsk_pattern_tx(pat, PAT_ACK);

    mfsk_pattern_window_t w = {0};
    int hits = 0;
    push_silence(&w, burst / 2, &hits);
    hits += push_pcm(&w, pat, n, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits, "test setup: burst was matched too early");

    mfsk_pattern_window_reset(&w);                 /* a new ACK becomes due */

    push_silence(&w, 2 * burst, &hits);            /* scans run over what is held */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits,
        "a stale burst from the previous exchange was reported as a new ACK");
    mfsk_pattern_window_free(&w);
    free(pat);
}

/* ...and the control: without the reset, that same held burst IS found.  If
 * this ever stops failing to find it, the test above has stopped testing. */
void test_without_reset_the_stale_burst_is_found(void)
{
    const int burst = mfsk_pattern_max_tx_samples();
    int16_t *pat = calloc((size_t)burst, sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pat);
    int n = mfsk_pattern_tx(pat, PAT_ACK);

    mfsk_pattern_window_t w = {0};
    int hits = 0;
    push_silence(&w, burst / 2, &hits);
    hits += push_pcm(&w, pat, n, NULL);
    TEST_ASSERT_EQUAL_INT(0, hits);
    push_silence(&w, burst, &hits);                /* no reset */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, hits,
        "control: the held burst was not found, so the reset test proves nothing");
    mfsk_pattern_window_free(&w);
    free(pat);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_detect_patterns_matches_per_list_calls);
    RUN_TEST(test_noise_no_false_ack);
    RUN_TEST(test_real_ack_detects);
    RUN_TEST(test_real_break_detects);
    RUN_TEST(test_window_scans_once_per_burst_not_per_chunk);
    RUN_TEST(test_window_finds_a_chunked_ack_at_any_scan_phase);
    RUN_TEST(test_reset_discards_a_stale_burst);
    RUN_TEST(test_without_reset_the_stale_burst_is_found);
    return UNITY_END();
}
