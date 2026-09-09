/* Tone-pattern signalling — see pattern_ack.h.
 *
 * Owns a lazily-initialised (mfsk_t, ofdm_frame_t) at the tone geometry the
 * patterns are defined on, so the datalink layer can emit and detect them
 * without holding a modem instance.  Nothing here decodes anything: there is
 * no FEC and no demodulator in this path, only tone generation and
 * correlation.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original tone geometry, C++)
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "pattern_ack.h"
#include "mfsk.h"
#include "mfsk_ofdm.h"
#include "mfsk_sync.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Tone geometry.  These must match on both ends of a link; they are not
 * negotiated and there is nothing in the burst that could carry them. */
#define PAT_FS        8000.0
#define PAT_FC        2000.0
#define PAT_NFFT      256
#define PAT_NCAR      50
#define PAT_GI        0.25
#define PAT_M         32
#define PAT_TXAMP     2200.0
#define PAT_LPF_TAPS  63
#define PAT_LPF_FC    1000.0

/* Written once under g_once, read-only afterwards.  Both the TX worker and the
 * RX loop use these concurrently; nothing here is mutated per call, and the
 * per-session tone rotation is applied to a stack copy rather than to g_m, so
 * there is no shared mutable state to race on. */
static mfsk_t         g_m;
static ofdm_frame_t   g_o;
static double         g_lpf[PAT_LPF_TAPS];
static double         g_w;
static int            g_nofdm;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void mklpf(double *lpf, double fc)
{
    double s = 0.0;
    for (int i = 0; i < PAT_LPF_TAPS; i++)
    {
        int k = i - PAT_LPF_TAPS / 2;
        double h = (k == 0) ? 2.0 * M_PI * fc / PAT_FS
                            : sin(2.0 * M_PI * fc / PAT_FS * k) / (M_PI * k);
        h *= 0.54 - 0.46 * cos(2.0 * M_PI * i / (PAT_LPF_TAPS - 1));
        lpf[i] = h; s += h;
    }
    for (int i = 0; i < PAT_LPF_TAPS; i++) lpf[i] /= s;
}

static void init_once(void)
{
    mfsk_init(&g_m, PAT_M, PAT_NCAR, 1);
    ofdm_frame_init(&g_o, PAT_NFFT, PAT_NCAR, PAT_GI, 0);
    g_nofdm = ofdm_frame_nofdm(&g_o);
    g_w     = 2.0 * M_PI * PAT_FC / PAT_FS;
    mklpf(g_lpf, PAT_LPF_FC);
}

static void lazy_init(void)
{
    pthread_once(&g_once, init_once);
}

/* Map a session ID onto one of the 16 rotations that cannot alias.
 *
 * Rotations differing by 8 or 24 (mod 32) score a full 8 of 16 against each
 * other -- see the header -- so from each group {r, r+8, r+16, r+24} at most
 * two are usable.  Taking {0..7} and {16..23} gives exactly that: any two
 * differ by something other than 8 or 24.
 *
 * Four bits of the session ID select one.  Sessions are not numerous enough on
 * one frequency for the pigeonhole to matter much, and a collision only costs
 * what no rotation at all would have cost everywhere. */
int pattern_ack_rotation(uint8_t session_id)
{
    const int idx = session_id & 0x0F;
    return (idx < 8) ? idx : (idx + 8);      /* 0..7, then 16..23 */
}

/* g_m with its ACK and ACK+TURN tone tables rotated for this session.  A copy,
 * because g_m is shared read-only between the TX and RX threads. */
static void session_view(mfsk_t *out, uint8_t session_id)
{
    memcpy(out, &g_m, sizeof(*out));
    const int rot = pattern_ack_rotation(session_id);
    for (int i = 0; i < out->ack_pattern_len; i++)
    {
        out->ack_tones[i]   = (out->ack_tones[i]   + rot) % out->M;
        out->break_tones[i] = (out->break_tones[i] + rot) % out->M;
    }
}

int pattern_ack_nsymb(void)
{
    lazy_init();
    return g_m.ack_pattern_nsymb;
}

int pattern_ack_max_tx_samples(void)
{
    lazy_init();
    return g_m.ack_pattern_nsymb * g_nofdm;
}

int pattern_ack_tx(int16_t *out, pattern_kind_t kind, uint8_t session_id)
{
    lazy_init();
    mfsk_t m;
    session_view(&m, session_id);
    const int ns = m.ack_pattern_nsymb;

    mfsk_cplx *bins = (mfsk_cplx *)calloc((size_t)ns * PAT_NCAR, sizeof(mfsk_cplx));
    if (!bins) return 0;
    if (kind == PATTERN_BREAK)
        mfsk_generate_break_pattern(&m, bins);
    else
        mfsk_generate_ack_pattern(&m, bins);

    int  written = 0;
    long tx_n    = 0;
    for (int s = 0; s < ns; s++)
    {
        double complex fb[PAT_NCAR], pad[PAT_NFFT], t[PAT_NFFT], cp[PAT_NFFT + 128];
        for (int k = 0; k < PAT_NCAR; k++)
            fb[k] = bins[s * PAT_NCAR + k].re + bins[s * PAT_NCAR + k].im * I;
        ofdm_zero_padder(&g_o, fb, pad);
        ofdm_ifft(&g_o, pad, t);
        ofdm_gi_adder(&g_o, t, cp);
        for (int n = 0; n < g_nofdm; n++)
        {
            double ph = g_w * (double)tx_n++;
            double v  = PAT_TXAMP * (creal(cp[n]) * cos(ph) + cimag(cp[n]) * sin(ph));
            if (v >  32767.0) v =  32767.0;
            if (v < -32768.0) v = -32768.0;
            out[written++] = (int16_t)lrint(v);
        }
    }
    free(bins);
    return written;
}

int pattern_ack_detect(const int16_t *pb, int n, uint8_t session_id,
                       int *is_break)
{
    lazy_init();
    mfsk_t m;
    session_view(&m, session_id);
    if (!pb || n < m.ack_pattern_nsymb * g_nofdm)
        return 0;

    /* Passband -> complex baseband + LPF. */
    double complex *bb = (double complex *)malloc((size_t)n * sizeof(double complex));
    double complex *bf = (double complex *)malloc((size_t)n * sizeof(double complex));
    if (!bb || !bf) { free(bb); free(bf); return 0; }

    for (int i = 0; i < n; i++)
    {
        double x  = (double)pb[i];
        double ph = g_w * (double)i;
        bb[i] = 2.0 * x * cos(ph) + I * 2.0 * x * sin(ph);
    }
    for (int i = 0; i < n; i++)
    {
        double complex a = 0;
        for (int k = 0; k < PAT_LPF_TAPS; k++)
        {
            int j = i - k + PAT_LPF_TAPS / 2;
            if (j >= 0 && j < n) a += g_lpf[k] * bb[j];
        }
        bf[i] = a;
    }

    /* Score ack and break in ONE pass.  Scoring them separately redoes every
     * FFT over the same samples for the sake of a different tone list, which
     * doubles the cost of the most expensive thing in the RX loop. */
    const int  ns        = m.ack_pattern_nsymb;
    const int *lists[2]  = { m.ack_tones, m.break_tones };
    int        scores[2] = { 0, 0 };
    mfsk_detect_patterns(&m, &g_o, bf, n, lists, 2,
                         m.ack_pattern_len, ns, scores, NULL);
    const int ack_score = scores[0];
    const int brk_score = scores[1];
    free(bb); free(bf);

    const bool ack_hit = ack_score >= m.ack_match_threshold;
    const bool brk_hit = brk_score >= m.break_match_threshold;
    if (!ack_hit && !brk_hit)
        return 0;

    /* Prefer the higher-scoring list so the two are told apart cleanly. */
    if (brk_hit && (!ack_hit || brk_score >= ack_score))
    {
        if (is_break) *is_break = 1;
        return 1;
    }
    if (is_break) *is_break = 0;
    return 1;
}

/* ======================================================================
 * Sliding detection window — see pattern_ack.h
 * ====================================================================== */

int pattern_ack_window_push(pattern_ack_window_t *w, const int16_t *pcm, int n,
                            uint8_t session_id, int *is_break)
{
    if (!w || !pcm || n <= 0)
        return 0;

    const int burst = pattern_ack_max_tx_samples();
    if (burst <= 0)
        return 0;
    /* One burst to hold the pattern, one to cover the scan interval below. */
    const int need = burst * 2;

    if (w->cap < need)
    {
        int16_t *nw = (int16_t *)realloc(w->buf, (size_t)need * sizeof(int16_t));
        if (!nw)
            return 0;               /* no window, no detection; never a crash */
        w->buf = nw;
        w->cap = need;
        if (w->len > need) w->len = need;
    }

    /* Keep the newest samples: an older burst that has already slid most of
     * the way out cannot be completed by anything arriving now. */
    int add = n;
    if (add > w->cap) { pcm += (n - w->cap); add = w->cap; }
    if (w->len + add > w->cap)
    {
        int drop = w->len + add - w->cap;
        memmove(w->buf, w->buf + drop, (size_t)(w->len - drop) * sizeof(int16_t));
        w->len -= drop;
    }
    memcpy(w->buf + w->len, pcm, (size_t)add * sizeof(int16_t));
    w->len += add;
    w->since_scan += add;

    if (w->len < burst)
        return 0;
    /* Pace the correlation: see the header.  Scanning per chunk instead of per
     * burst is a 32x cost increase for no extra sensitivity, and overloads the
     * thread that feeds the decoders. */
    if (w->since_scan < burst)
        return 0;
    w->since_scan = 0;

    int isb = 0;
    if (!pattern_ack_detect(w->buf, w->len, session_id, &isb))
        return 0;

    w->len = 0;                     /* consume: one burst, one event */
    w->since_scan = 0;
    if (is_break) *is_break = isb;
    return 1;
}

void pattern_ack_window_reset(pattern_ack_window_t *w)
{
    if (!w) return;
    w->len        = 0;
    w->since_scan = 0;
}

void pattern_ack_window_free(pattern_ack_window_t *w)
{
    if (!w) return;
    free(w->buf);
    w->buf = NULL;
    w->cap = 0;
    w->len = 0;
    w->since_scan = 0;
}
