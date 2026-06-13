/*
 * Polyphase FIR resampler — see resampler.h.
 *
 * Prototype filter: windowed-sinc low-pass, fc = 3400 Hz at the 48 kHz rate
 * (passband flat past the widest modem waveform ~2.5 kHz, stopband by the
 * 8 kHz Nyquist of 4 kHz), Hamming window, RESAMP_NTAPS taps, normalised to
 * unity DC gain.  The same prototype is the anti-imaging filter on upsample
 * and the anti-aliasing filter on downsample.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "resampler.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FILT_FS   48000.0
#define FILT_FC    3400.0

/* h_up[p][t]  = L * proto[p + L*t]  — polyphase subfilter for output phase p.
 * h_down[k]   = proto[k]            — flat prototype for decimation. */
static float h_up[RESAMP_L][RESAMP_TAPS_PER_PHASE];
static float h_down[RESAMP_NTAPS];
static int   g_inited;

static inline int32_t clamp_i32(double v)
{
    if (v >  2147483647.0) return  2147483647;
    if (v < -2147483648.0) return (int32_t)(-2147483648.0);
    return (int32_t)v;
}

void resampler_global_init(void)
{
    double proto[RESAMP_NTAPS];
    double sum = 0.0;
    const int N = RESAMP_NTAPS;
    const double wc = 2.0 * FILT_FC / FILT_FS;   /* normalised cutoff (×Nyquist) */
    const double mid = (N - 1) / 2.0;

    for (int n = 0; n < N; n++) {
        double x = n - mid;
        double sinc = (fabs(x) < 1e-9) ? wc
                                       : sin(M_PI * wc * x) / (M_PI * x);
        double ham = 0.54 - 0.46 * cos(2.0 * M_PI * n / (N - 1));
        proto[n] = sinc * ham;
        sum += proto[n];
    }
    /* Normalise to unity DC gain. */
    for (int n = 0; n < N; n++)
        proto[n] /= sum;

    for (int n = 0; n < N; n++)
        h_down[n] = (float)proto[n];

    /* Polyphase decomposition for the interpolator.  Output sample
     * (i*L + p) = L * sum_t proto[p + L*t] * x[i - t].  The L gain
     * compensates the zero-stuffing energy loss; fold it into the table. */
    for (int p = 0; p < RESAMP_L; p++)
        for (int t = 0; t < RESAMP_TAPS_PER_PHASE; t++) {
            int idx = p + RESAMP_L * t;
            h_up[p][t] = (idx < N) ? (float)(RESAMP_L * proto[idx]) : 0.0f;
        }

    g_inited = 1;
}

/* ---------------- Upsampler 8k -> 48k ---------------- */

void resamp_up_reset(resamp_up_t *r)
{
    memset(r->hist, 0, sizeof(r->hist));
}

int resamp_up_process(resamp_up_t *r, const int32_t *in, int n_in, int32_t *out)
{
    if (!g_inited) resampler_global_init();

    int o = 0;
    for (int i = 0; i < n_in; i++) {
        /* Shift newest input into hist[0]. */
        for (int t = RESAMP_TAPS_PER_PHASE - 1; t > 0; t--)
            r->hist[t] = r->hist[t - 1];
        r->hist[0] = in[i];

        for (int p = 0; p < RESAMP_L; p++) {
            double acc = 0.0;
            const float *hp = h_up[p];
            for (int t = 0; t < RESAMP_TAPS_PER_PHASE; t++)
                acc += (double)hp[t] * (double)r->hist[t];
            out[o++] = clamp_i32(acc);
        }
    }
    return o;
}

/* ---------------- Downsampler 48k -> 8k ---------------- */

void resamp_down_reset(resamp_down_t *r)
{
    memset(r->hist, 0, sizeof(r->hist));
    r->phase = 0;
}

int resamp_down_process(resamp_down_t *r, const int32_t *in, int n_in,
                        int32_t *out)
{
    if (!g_inited) resampler_global_init();

    int o = 0;
    for (int i = 0; i < n_in; i++) {
        /* Shift newest input into hist[0]. */
        for (int t = RESAMP_NTAPS - 1; t > 0; t--)
            r->hist[t] = r->hist[t - 1];
        r->hist[0] = in[i];

        if (++r->phase >= RESAMP_L) {
            r->phase = 0;
            double acc = 0.0;
            for (int t = 0; t < RESAMP_NTAPS; t++)
                acc += (double)h_down[t] * (double)r->hist[t];
            out[o++] = clamp_i32(acc);
        }
    }
    return o;
}
