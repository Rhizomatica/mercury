/* Upper-sideband modem audio <-> 48 kHz I/Q -- see hl2_dsp.h.
 *
 * Two Kaiser-windowed sinc filters (beta 6.76: 70 dB stopband):
 *
 *   a: 48 kHz low-pass, cutoff 3800 Hz, flat to ~3 kHz and 70 dB down by
 *      4.6 kHz, so nothing that folds into 0..3 kHz survives the 6:1
 *      decimation (and no image survives the 1:6 interpolation).
 *   b: 8 kHz low-pass, cutoff 1450 Hz, shifted up by 1575 Hz: a complex
 *      band-pass for 125..3025 Hz (-6 dB), flat ~190..2960 Hz, that rejects
 *      every negative frequency -- the lower sideband -- and DC.
 *
 * Delay lines are doubled (each sample stored at p and p + N) so every
 * convolution is one straight loop.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hl2_dsp.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS_IQ        48000.0
#define FS_AUDIO     8000.0
#define A_CUTOFF_HZ  3800.0
#define B_CUTOFF_HZ  1450.0
#define B_SHIFT_HZ   1575.0
#define KAISER_BETA  6.76
#define NA_PHASE     (HL2_DSP_NA / HL2_DSP_L)
#define AGC_SMOOTH   0.1f        /* per 20 ms block in dB: ~200 ms        */
#define AGC_PEAK     0.9f        /* a block's peak never exceeds this     */
#define AGC_GAIN_MIN 1e-2f
#define AGC_GAIN_MAX 1e7f
#define AGC_GAIN_0   1e3f

static float a_taps[HL2_DSP_NA];                     /* x[n - k] weights */
static float a_poly[HL2_DSP_L][NA_PHASE];            /* L * a[p + L t]   */
static float b_re[HL2_DSP_NB], b_im[HL2_DSP_NB];
static bool  built;

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; k++)
    {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum)
            break;
    }
    return sum;
}

/* Kaiser-windowed sinc low-pass, unity DC gain. */
static void design_lowpass(double *h, int n, double cutoff, double fs)
{
    double wc = 2.0 * cutoff / fs, mid = (n - 1) / 2.0, sum = 0.0;
    for (int k = 0; k < n; k++)
    {
        double x = k - mid, r = x / mid;
        double sinc = fabs(x) < 1e-9 ? wc : sin(M_PI * wc * x) / (M_PI * x);
        h[k] = sinc * bessel_i0(KAISER_BETA * sqrt(1.0 - r * r)) / bessel_i0(KAISER_BETA);
        sum += h[k];
    }
    for (int k = 0; k < n; k++)
        h[k] /= sum;
}

static void build_tables(void)
{
    if (built)
        return;
    double h[HL2_DSP_NB];

    design_lowpass(h, HL2_DSP_NA, A_CUTOFF_HZ, FS_IQ);
    for (int k = 0; k < HL2_DSP_NA; k++)
        a_taps[k] = (float) h[k];
    for (int p = 0; p < HL2_DSP_L; p++)
        for (int t = 0; t < NA_PHASE; t++)
            a_poly[p][t] = (float)(HL2_DSP_L * h[p + HL2_DSP_L * t]);

    design_lowpass(h, HL2_DSP_NB, B_CUTOFF_HZ, FS_AUDIO);
    double mid = (HL2_DSP_NB - 1) / 2.0;
    for (int k = 0; k < HL2_DSP_NB; k++)
    {
        double ph = 2.0 * M_PI * B_SHIFT_HZ * (k - mid) / FS_AUDIO;
        b_re[k] = (float)(h[k] * cos(ph));
        b_im[k] = (float)(h[k] * sin(ph));
    }
    built = true;
}

/* The newest sample goes one slot down (mod n), stored twice. */
static inline int step(int pos, int n)
{
    return pos ? pos - 1 : n - 1;
}

static inline void store(float *line, int pos, int n, float x)
{
    line[pos] = line[pos + n] = x;
}

void hl2_rx_init(hl2_rx *rx)
{
    build_tables();
    memset(rx, 0, sizeof(*rx));
    rx->gain = AGC_GAIN_0;
}

/* One 20 ms block through the AGC into out. */
static void agc_block(hl2_rx *rx, bool muted, int32_t *out)
{
    if (muted)
    {
        memset(out, 0, sizeof(int32_t) * HL2_DSP_BLOCK);
        return;
    }
    float peak = 0.0f, sq = 0.0f;
    for (int i = 0; i < HL2_DSP_BLOCK; i++)
    {
        float v = fabsf(rx->block[i]);
        peak = v > peak ? v : peak;
        sq += v * v;
    }
    float rms = sqrtf(sq / HL2_DSP_BLOCK);
    float g0 = rx->gain, g1 = g0;
    if (rms > 0.0f)
        g1 = g0 * powf(HL2_DSP_AGC_TARGET / (rms * g0), AGC_SMOOTH);
    if (peak > 0.0f && peak * g1 > AGC_PEAK)
        g1 = AGC_PEAK / peak;
    if (peak > 0.0f && peak * g0 > AGC_PEAK)
        g0 = g1;                                   /* would clip: drop now */
    g1 = g1 < AGC_GAIN_MIN ? AGC_GAIN_MIN : g1 > AGC_GAIN_MAX ? AGC_GAIN_MAX : g1;

    for (int i = 0; i < HL2_DSP_BLOCK; i++)
    {
        float g = g0 + (g1 - g0) * (float)(i + 1) / HL2_DSP_BLOCK;
        double v = (double)(rx->block[i] * g) * 2147483648.0;
        out[i] = v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : (int32_t) v;
    }
    rx->gain = g1;
}

size_t hl2_rx_process(hl2_rx *rx, const float *iq, size_t n, bool mute, int32_t *out)
{
    size_t produced = 0;
    for (size_t s = 0; s < n; s++)
    {
        float i = mute ? 0.0f : iq[2 * s], q = mute ? 0.0f : iq[2 * s + 1];
        rx->pa = step(rx->pa, HL2_DSP_NA);
        store(rx->xi, rx->pa, HL2_DSP_NA, i);
        store(rx->xq, rx->pa, HL2_DSP_NA, q);
        if (++rx->phase < HL2_DSP_L)
            continue;
        rx->phase = 0;

        float ai = 0.0f, aq = 0.0f;
        const float *li = rx->xi + rx->pa, *lq = rx->xq + rx->pa;
        for (int k = 0; k < HL2_DSP_NA; k++)
        {
            ai += a_taps[k] * li[k];
            aq += a_taps[k] * lq[k];
        }

        rx->pb = step(rx->pb, HL2_DSP_NB);
        store(rx->zi, rx->pb, HL2_DSP_NB, ai);
        store(rx->zq, rx->pb, HL2_DSP_NB, aq);
        float y = 0.0f;
        const float *zi = rx->zi + rx->pb, *zq = rx->zq + rx->pb;
        for (int k = 0; k < HL2_DSP_NB; k++)
            y += b_re[k] * zi[k] - b_im[k] * zq[k];       /* Re{b * z} */

        rx->block[rx->nblock++] = y;
        rx->block_muted |= mute;
        if (rx->nblock == HL2_DSP_BLOCK)
        {
            agc_block(rx, rx->block_muted, out + produced);
            produced += HL2_DSP_BLOCK;
            rx->nblock = 0;
            rx->block_muted = false;
        }
    }
    return produced;
}

void hl2_tx_init(hl2_tx *tx)
{
    build_tables();
    memset(tx, 0, sizeof(*tx));
}

void hl2_tx_process(hl2_tx *tx, const int32_t *in, size_t n, int16_t *iq)
{
    for (size_t s = 0; s < n; s++)
    {
        tx->pb = step(tx->pb, HL2_DSP_NB);
        store(tx->x, tx->pb, HL2_DSP_NB, (float)((double) in[s] / 2147483648.0));
        float zi = 0.0f, zq = 0.0f;
        const float *x = tx->x + tx->pb;
        for (int k = 0; k < HL2_DSP_NB; k++)
        {
            zi += b_re[k] * x[k];
            zq += b_im[k] * x[k];
        }
        /* A real tone is two half-amplitude phasors; keep the one. */
        tx->pa = step(tx->pa, NA_PHASE);
        store(tx->zi, tx->pa, NA_PHASE, 2.0f * zi);
        store(tx->zq, tx->pa, NA_PHASE, 2.0f * zq);

        const float *li = tx->zi + tx->pa, *lq = tx->zq + tx->pa;
        for (int p = 0; p < HL2_DSP_L; p++)
        {
            float yi = 0.0f, yq = 0.0f;
            for (int t = 0; t < NA_PHASE; t++)
            {
                yi += a_poly[p][t] * li[t];
                yq += a_poly[p][t] * lq[t];
            }
            float mag = sqrtf(yi * yi + yq * yq);
            if (mag > 1.0f)
            {
                yi /= mag;
                yq /= mag;
            }
            int16_t *o = iq + 2 * (HL2_DSP_L * s + (size_t) p);
            o[0] = (int16_t) lrintf(yi * 32767.0f);
            o[1] = (int16_t) lrintf(yq * 32767.0f);
        }
    }
}

double hl2_dsp_lowpass_gain(double hz)
{
    build_tables();
    double re = 0.0, im = 0.0, w = 2.0 * M_PI * hz / FS_IQ;
    for (int k = 0; k < HL2_DSP_NA; k++)
    {
        re += a_taps[k] * cos(w * k);
        im -= a_taps[k] * sin(w * k);
    }
    return sqrt(re * re + im * im);
}

double hl2_dsp_bandpass_gain(double hz)
{
    build_tables();
    double re = 0.0, im = 0.0, w = 2.0 * M_PI * hz / FS_AUDIO;
    for (int k = 0; k < HL2_DSP_NB; k++)
    {
        double c = cos(w * k), s = sin(w * k);   /* sum b[k] e^{-jwk} */
        re += b_re[k] * c + b_im[k] * s;
        im += b_im[k] * c - b_re[k] * s;
    }
    return sqrt(re * re + im * im);
}
