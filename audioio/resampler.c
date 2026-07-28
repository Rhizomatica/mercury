/*
 * Polyphase FIR resampler — see resampler.h.
 *
 * Prototype filter: windowed-sinc low-pass, fc = 3400 Hz (passband flat past
 * the widest modem waveform ~2.5 kHz, stopband by the 8 kHz Nyquist of 4 kHz),
 * Hamming window, L*RESAMP_TAPS_PER_PHASE taps, normalised to unity DC gain.
 * The same prototype is the anti-imaging filter on upsample and the
 * anti-aliasing filter on downsample.
 *
 * The cutoff is normalised against the DEVICE rate (8000*L), so the filter
 * keeps the same 3400 Hz corner at every supported ratio; the tap count grows
 * with L so the impulse response spans a constant amount of TIME.
 *
 * L == 1 (an 8 kHz device: no rate change) is a pass-through — filtering there
 * would only add delay and passband droop for nothing.
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

#define FILT_FC    3400.0

/* h_up[p][t] = L * proto[p + L*t] — polyphase subfilter for output phase p.
 * h_down[k]  = proto[k]           — flat prototype for decimation.
 * The two directions are independent: playback and capture can negotiate
 * different device rates, so each keeps its own ratio and table. */
static float h_up[RESAMP_L_MAX][RESAMP_TAPS_PER_PHASE];
static float h_down[RESAMP_NTAPS_MAX];
static int   g_l_up   = 0;    /* 0 = not built yet */
static int   g_l_down = 0;

static inline int32_t clamp_i32(double v)
{
    if (v >  2147483647.0) return  2147483647;
    if (v < -2147483648.0) return (int32_t)(-2147483648.0);
    return (int32_t)v;
}

int resampler_ratio_for_rate(int device_rate_hz)
{
    if (device_rate_hz <= 0)
        return 0;
    if (device_rate_hz % RESAMP_MODEM_FS != 0)
        return 0;                       /* 44100 and friends */
    int L = device_rate_hz / RESAMP_MODEM_FS;
    if (L < 1 || L > RESAMP_L_MAX)
        return 0;
    return L;
}

/* Windowed-sinc prototype for ratio L, normalised to unity DC gain.
 * proto must hold L*RESAMP_TAPS_PER_PHASE doubles. */
static void build_proto(int L, double *proto)
{
    const int    N   = L * RESAMP_TAPS_PER_PHASE;
    const double fs  = (double)RESAMP_MODEM_FS * (double)L;
    const double wc  = 2.0 * FILT_FC / fs;    /* normalised cutoff (xNyquist) */
    const double mid = (N - 1) / 2.0;
    double sum = 0.0;

    for (int n = 0; n < N; n++) {
        double x = n - mid;
        double sinc = (fabs(x) < 1e-9) ? wc
                                       : sin(M_PI * wc * x) / (M_PI * x);
        double ham = 0.54 - 0.46 * cos(2.0 * M_PI * n / (N - 1));
        proto[n] = sinc * ham;
        sum += proto[n];
    }
    for (int n = 0; n < N; n++)
        proto[n] /= sum;
}

void resampler_init_up(int L)
{
    if (L < 1 || L > RESAMP_L_MAX) return;
    if (L == 1) { g_l_up = 1; return; }      /* pass-through, no table */

    double proto[RESAMP_NTAPS_MAX];
    const int N = L * RESAMP_TAPS_PER_PHASE;
    build_proto(L, proto);

    /* Polyphase decomposition for the interpolator.  Output sample
     * (i*L + p) = L * sum_t proto[p + L*t] * x[i - t].  The L gain
     * compensates the zero-stuffing energy loss; fold it into the table. */
    for (int p = 0; p < L; p++)
        for (int t = 0; t < RESAMP_TAPS_PER_PHASE; t++) {
            int idx = p + L * t;
            h_up[p][t] = (idx < N) ? (float)(L * proto[idx]) : 0.0f;
        }
    g_l_up = L;
}

void resampler_init_down(int L)
{
    if (L < 1 || L > RESAMP_L_MAX) return;
    if (L == 1) { g_l_down = 1; return; }    /* pass-through, no table */

    double proto[RESAMP_NTAPS_MAX];
    const int N = L * RESAMP_TAPS_PER_PHASE;
    build_proto(L, proto);

    for (int n = 0; n < N; n++)
        h_down[n] = (float)proto[n];
    g_l_down = L;
}

void resampler_global_init(void)
{
    resampler_init_up(RESAMP_L);
    resampler_init_down(RESAMP_L);
}

/* ---------------- Upsampler 8k -> 8000*L ---------------- */

void resamp_up_reset(resamp_up_t *r)
{
    memset(r->hist, 0, sizeof(r->hist));
}

int resamp_up_process(resamp_up_t *r, const int32_t *in, int n_in, int32_t *out)
{
    if (!g_l_up) resampler_init_up(RESAMP_L);
    const int L = g_l_up;

    if (L == 1) {                       /* device already at the modem rate */
        memcpy(out, in, (size_t)n_in * sizeof(int32_t));
        return n_in;
    }

    int o = 0;
    for (int i = 0; i < n_in; i++) {
        /* Shift newest input into hist[0]. */
        for (int t = RESAMP_TAPS_PER_PHASE - 1; t > 0; t--)
            r->hist[t] = r->hist[t - 1];
        r->hist[0] = in[i];

        for (int p = 0; p < L; p++) {
            double acc = 0.0;
            const float *hp = h_up[p];
            for (int t = 0; t < RESAMP_TAPS_PER_PHASE; t++)
                acc += (double)hp[t] * (double)r->hist[t];
            out[o++] = clamp_i32(acc);
        }
    }
    return o;
}

/* ---------------- Downsampler 8000*L -> 8k ---------------- */

void resamp_down_reset(resamp_down_t *r)
{
    memset(r->hist, 0, sizeof(r->hist));
    r->phase = 0;
}

int resamp_down_process(resamp_down_t *r, const int32_t *in, int n_in,
                        int32_t *out)
{
    if (!g_l_down) resampler_init_down(RESAMP_L);
    const int L = g_l_down;

    if (L == 1) {                       /* device already at the modem rate */
        memcpy(out, in, (size_t)n_in * sizeof(int32_t));
        return n_in;
    }

    const int ntaps = L * RESAMP_TAPS_PER_PHASE;
    int o = 0;
    for (int i = 0; i < n_in; i++) {
        /* Shift newest input into hist[0]. */
        for (int t = ntaps - 1; t > 0; t--)
            r->hist[t] = r->hist[t - 1];
        r->hist[0] = in[i];

        if (++r->phase >= L) {
            r->phase = 0;
            double acc = 0.0;
            for (int t = 0; t < ntaps; t++)
                acc += (double)h_down[t] * (double)r->hist[t];
            out[o++] = clamp_i32(acc);
        }
    }
    return o;
}
