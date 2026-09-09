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
#include <stdbool.h>
#include <stdlib.h>

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

static mfsk_t       g_m;
static ofdm_frame_t g_o;
static double       g_lpf[PAT_LPF_TAPS];
static double       g_w;
static int          g_nofdm;
static bool         g_ready = false;

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

static void lazy_init(void)
{
    if (g_ready) return;
    mfsk_init(&g_m, PAT_M, PAT_NCAR, 1);
    ofdm_frame_init(&g_o, PAT_NFFT, PAT_NCAR, PAT_GI, 0);
    g_nofdm = ofdm_frame_nofdm(&g_o);
    g_w     = 2.0 * M_PI * PAT_FC / PAT_FS;
    mklpf(g_lpf, PAT_LPF_FC);
    g_ready = true;
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

int pattern_ack_tx(int16_t *out, pattern_kind_t kind)
{
    lazy_init();
    const int ns = g_m.ack_pattern_nsymb;

    mfsk_cplx *bins = (mfsk_cplx *)calloc((size_t)ns * PAT_NCAR, sizeof(mfsk_cplx));
    if (!bins) return 0;
    if (kind == PATTERN_BREAK)
        mfsk_generate_break_pattern(&g_m, bins);
    else
        mfsk_generate_ack_pattern(&g_m, bins);

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

int pattern_ack_detect(const int16_t *pb, int n, int *is_break)
{
    lazy_init();
    if (!pb || n < g_m.ack_pattern_nsymb * g_nofdm)
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
    const int  ns       = g_m.ack_pattern_nsymb;
    const int *lists[2] = { g_m.ack_tones, g_m.break_tones };
    int        scores[2] = { 0, 0 };
    mfsk_detect_patterns(&g_m, &g_o, bf, n, lists, 2,
                         g_m.ack_pattern_len, ns, scores, NULL);
    const int ack_score = scores[0];
    const int brk_score = scores[1];
    free(bb); free(bf);

    const bool ack_hit = ack_score >= g_m.ack_match_threshold;
    const bool brk_hit = brk_score >= g_m.break_match_threshold;
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
