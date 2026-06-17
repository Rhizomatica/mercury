/* Watterson HF Ionospheric Channel Model (ITU-R F.1487) — Implementation
 *
 * Copyright (C) 2025-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implements the Watterson narrowband HF ionospheric channel model per
 * ITU-R Recommendation F.1487. Each propagation path is modelled as a
 * tapped-delay-line with its own delay, Doppler spread, frequency offset,
 * and gain. The Doppler spread is shaped using a 2nd-order Butterworth
 * IIR low-pass filter driven by complex white Gaussian noise.
 */

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "watterson.h"
#include "../modem/freedv/comp_prim.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Box-Muller transform: converts uniform [0,1) random numbers to Gaussian.
 * Returns a sample from N(0, 1). */
static float gaussian()
{
    float x = (float)rand() / RAND_MAX;
    float y = (float)rand() / RAND_MAX;

    /* avoid log(0) — extremely unlikely but safe */
    if (x <= 0.0f) x = 1e-9f;

    return sqrtf(-2.0f * logf(x)) * cosf(2.0f * M_PI * y);
}

/* Compute 2nd-order Butterworth LPF IIR coefficients via bilinear transform.
 *
 * The analog prototype is:
 *   H(s) = ωc² / (s² + √2·ωc·s + ωc²)
 *
 * Pre-warp the digital cutoff, then apply the bilinear transform
 * s = 2·fs · (1 - z⁻¹) / (1 + z⁻¹).
 *
 * Result: H(z) = (b0 + b1·z⁻¹ + b2·z⁻²) / (1 + a1·z⁻¹ + a2·z⁻²)
 */
static void butterworth_2nd_lp(float fc, float fs,
                                float *b0, float *b1, float *b2,
                                float *a1, float *a2)
{
    /* Pre-warp cutoff frequency */
    float c = tanf(M_PI * fc / fs);
    float c2 = c * c;
    float sqrt2c = 1.41421356237f * c;  /* √2 */

    float den0 = 1.0f + sqrt2c + c2;

    *b0 = c2 / den0;
    *b1 = 2.0f * c2 / den0;
    *b2 = c2 / den0;

    *a1 = 2.0f * (c2 - 1.0f) / den0;
    *a2 = (1.0f - sqrt2c + c2) / den0;
}

/* Apply a 2nd-order IIR filter to one scalar sample.
 *
 * Direct Form I:
 *   y[n] = b0·x[n] + b1·x[n-1] + b2·x[n-2] - a1·y[n-1] - a2·y[n-2]
 *
 * The x[3] and y[3] arrays hold {x[n-2], x[n-1], x[n]}.
 */
static float iir_tick(float xn, float x[3], float y[3],
                      float b0, float b1, float b2,
                      float a1, float a2)
{
    /* shift input history */
    x[0] = x[1];
    x[1] = x[2];
    x[2] = xn;

    /* shift output history */
    y[0] = y[1];
    y[1] = y[2];

    /* compute next output */
    y[2] = b0 * x[2] + b1 * x[1] + b2 * x[0]
         - a1 * y[1] - a2 * y[0];

    return y[2];
}

/* ------------------------------------------------------------------*\
    Public API
\*-------------------------------------------------------------------*/

int watterson_init(watterson_t *w, int sample_rate)
{
    assert(w != NULL);
    assert(sample_rate > 0);

    memset(w, 0, sizeof(*w));
    w->sample_rate = sample_rate;
    w->num_paths = 0;
    w->awgn_en = 0;
    w->noise_var = 0.0f;
    w->chan_norm = 1.0f;

    return 0;
}

void watterson_dispose(watterson_t *w)
{
    int i;
    assert(w != NULL);

    for (i = 0; i < w->num_paths; i++)
    {
        if (w->paths[i].delay_buf != NULL)
        {
            free(w->paths[i].delay_buf);
            w->paths[i].delay_buf = NULL;
        }
    }
    memset(w, 0, sizeof(*w));
}

int watterson_add_path(watterson_t *w, float delay_ms, float doppler_hz,
                        float freq_hz, float gain)
{
    watterson_path_t *p;
    int idx;

    assert(w != NULL);
    assert(delay_ms >= 0.0f);
    assert(doppler_hz >= 0.0f);
    assert(gain >= 0.0f && gain <= 1.0f);

    if (w->num_paths >= WATTERSON_MAX_PATHS)
        return -1;

    idx = w->num_paths;
    p = &w->paths[idx];

    memset(p, 0, sizeof(*p));

    p->delay_ms = delay_ms;
    p->doppler_hz = doppler_hz;
    p->freq_hz = freq_hz;
    p->gain = gain;

    /* Allocate delay line buffer */
    p->delay_samples = (int)ceilf(delay_ms * w->sample_rate / 1000.0f);
    if (p->delay_samples > 0)
    {
        p->delay_buf = (COMP *)calloc(p->delay_samples, sizeof(COMP));
        if (p->delay_buf == NULL)
            return -1;
    }
    else
    {
        p->delay_buf = NULL;
    }
    p->delay_idx = 0;

    /* Compute Butterworth IIR coefficients for Doppler shaping.
     * The 3 dB cutoff of the Butterworth filter is set to the Doppler
     * spread σ, which yields a good approximation of the Gaussian
     * power spectrum.
     *
     * For σ = 0 (zero Doppler spread, i.e. a static path), we
     * effectively bypass the filter. */
    if (p->doppler_hz > 0.0f)
    {
        /* The filter bandwidth is matched to the Doppler spread.
         * A Butterworth LPF with fc = σ provides a reasonable
         * approximation of the Gaussian spectrum. */
        float fc = p->doppler_hz;

        butterworth_2nd_lp(fc, (float)w->sample_rate,
                           &p->b0, &p->b1, &p->b2,
                           &p->a1, &p->a2);

        /* Normalise the Doppler-shaped tap to unit average power.  The narrow
         * LPF otherwise passes only a tiny fraction of the unit-variance
         * driving noise, attenuating the signal ~20-30 dB and breaking the
         * SNR <-> No calibration.
         *
         * The driven LPF output variance (per I/Q component) for unit white
         * input is exactly the impulse-response energy E_h = sum h[n]^2, so
         * E[|tap|^2] = 2*E_h.  We compute E_h DETERMINISTICALLY from the
         * impulse response.  (A finite random measurement is wrong for very
         * narrow Doppler: the fade is ~constant over any short window, so it
         * would normalise to a single fade realisation, not the ensemble.) */
        {
            float xh[3] = {0,0,0}, yh[3] = {0,0,0};
            double e_h = 0.0;
            float in = 1.0f;        /* unit impulse at n=0, zero thereafter */
            int n;
            for (n = 0; n < 2000000; n++)
            {
                float y = iir_tick(in, xh, yh, p->b0, p->b1, p->b2, p->a1, p->a2);
                in = 0.0f;
                e_h += (double)y * y;
                if (n > 2000 && (double)y * y < e_h * 1e-13)
                    break;          /* impulse-response tail is negligible */
            }
            double tap_pwr = 2.0 * e_h;             /* E[|tap|^2] */
            p->tap_norm = (tap_pwr > 0.0) ? (float)(1.0 / sqrt(tap_pwr)) : 1.0f;
        }

        /* Seed the running filter state so the tap doesn't start at zero. */
        {
            int k;
            for (k = 0; k < 1000; k++)
            {
                iir_tick(gaussian(), p->x_i, p->y_i, p->b0, p->b1, p->b2, p->a1, p->a2);
                iir_tick(gaussian(), p->x_q, p->y_q, p->b0, p->b1, p->b2, p->a1, p->a2);
            }
        }
    }
    else
    {
        /* Static path: unit gain with no filtering */
        p->b0 = 1.0f;
        p->b1 = 0.0f;
        p->b2 = 0.0f;
        p->a1 = 0.0f;
        p->a2 = 0.0f;
        p->tap_norm = 1.0f;   /* tap is (1, 0): already unit power */
    }

    /* Pre-compute frequency offset phase increment per sample */
    p->phase_inc = 2.0f * M_PI * p->freq_hz / (float)w->sample_rate;
    p->phase = 0.0f;

    w->num_paths++;

    /* Normalise the summed channel to unit average power gain: with unit-power
     * taps, E[|h|^2] = sum(gain^2) over paths, so divide the output by its
     * square root.  Keeps the faded signal at the same average power as the
     * input (ch.c hf_gain convention) so SNR3k = -No - 14.82 holds. */
    {
        double sumg2 = 0.0;
        int j;
        for (j = 0; j < w->num_paths; j++)
            sumg2 += (double)w->paths[j].gain * w->paths[j].gain;
        w->chan_norm = (sumg2 > 0.0) ? (float)(1.0 / sqrt(sumg2)) : 1.0f;
    }

    return idx;
}

void watterson_set_noise(watterson_t *w, float nodb)
{
    assert(w != NULL);

    if (nodb < -150.0f)
    {
        w->awgn_en = 0;
        w->noise_var = 0.0f;
        return;
    }

    w->awgn_en = 1;
    /* Same scaling as freedv ch.c for compatibility */
    float No = powf(10.0f, nodb / 10.0f) * 1000000.0f;
    w->noise_var = (float)w->sample_rate * No;
}

void watterson_reset_meas(watterson_t *w)
{
    assert(w != NULL);
    w->sig_pwr_acc = 0.0;
    w->noise_pwr_acc = 0.0;
    w->meas_nsamp = 0;
}

float watterson_measured_snr3k(const watterson_t *w)
{
    assert(w != NULL);
    if (w->sig_pwr_acc <= 0.0 || w->noise_pwr_acc <= 0.0)
        return 999.0f;   /* no AWGN added — effectively noiseless */
    /* SNR in the sample-rate bandwidth, scaled to a 3 kHz reference, in dB.
     * Matches ch.c: SNR3k = 10log10( (Psig/Pnoise) * Fs / 3000 ). */
    double snr_fs = w->sig_pwr_acc / w->noise_pwr_acc;
    return (float)(10.0 * log10(snr_fs * (double)w->sample_rate / 3000.0));
}

void watterson_process(watterson_t *w, COMP *samples, int n)
{
    int i, p;

    assert(w != NULL);
    assert(samples != NULL);
    assert(n > 0);

    if (w->num_paths == 0)
    {
        /* No paths configured: just add AWGN if enabled */
        if (w->awgn_en)
        {
            for (i = 0; i < n; i++)
            {
                COMP noise;
                /* Per-component variance noise_var/2 so the COMPLEX noise power
                 * is noise_var = Fs*No, matching ch.c (whose gaussian() carries
                 * the sqrt(1/2)).  Our gaussian() is unit-variance, so apply the
                 * 1/2 here — otherwise the channel is 3 dB hotter than ch and the
                 * SNR<->No calibration (and the mode thresholds) shift by 3 dB. */
                noise.real = gaussian() * sqrtf(w->noise_var * 0.5f);
                noise.imag = gaussian() * sqrtf(w->noise_var * 0.5f);
                w->sig_pwr_acc   += (double)samples[i].real * samples[i].real +
                                    (double)samples[i].imag * samples[i].imag;
                w->noise_pwr_acc += (double)noise.real * noise.real +
                                    (double)noise.imag * noise.imag;
                w->meas_nsamp++;
                samples[i] = cadd(samples[i], noise);
            }
        }
        return;
    }

    for (i = 0; i < n; i++)
    {
        COMP out;
        out.real = 0.0f;
        out.imag = 0.0f;

        for (p = 0; p < w->num_paths; p++)
        {
            watterson_path_t *wp = &w->paths[p];
            COMP tap, delayed, contrib;

            /* Generate Doppler-shaped complex tap coefficient.
             * Drive the IIR with complex white Gaussian noise and
             * obtain the filtered output as the tap gain.
             *
             * For a static path (doppler_hz == 0), the IIR filter
             * coefficients are set so the output is 1.0 + j0.0. */
            if (wp->doppler_hz > 0.0f)
            {
                float xn_i = gaussian();
                float xn_q = gaussian();
                tap.real = iir_tick(xn_i, wp->x_i, wp->y_i,
                                    wp->b0, wp->b1, wp->b2,
                                    wp->a1, wp->a2);
                tap.imag = iir_tick(xn_q, wp->x_q, wp->y_q,
                                    wp->b0, wp->b1, wp->b2,
                                    wp->a1, wp->a2);
            }
            else
            {
                tap.real = 1.0f;
                tap.imag = 0.0f;
            }

            /* Apply unit-power normalisation then path gain, so each path's
             * average power is gain^2 (E[|tap|^2] = 1). */
            tap = fcmult(wp->gain * wp->tap_norm, tap);

            /* Write current sample into delay line */
            if (wp->delay_buf != NULL)
            {
                wp->delay_buf[wp->delay_idx] = samples[i];
            }

            /* Read delayed sample from delay line */
            if (wp->delay_samples > 0)
            {
                int read_idx = wp->delay_idx + 1;
                if (read_idx >= wp->delay_samples)
                    read_idx -= wp->delay_samples;
                delayed = wp->delay_buf[read_idx];
            }
            else
            {
                delayed = samples[i];
            }

            /* Advance delay line write pointer */
            if (wp->delay_buf != NULL)
            {
                wp->delay_idx++;
                if (wp->delay_idx >= wp->delay_samples)
                    wp->delay_idx = 0;
            }

            /* Multiply delayed signal by tap coefficient (fading) */
            contrib = cmult(tap, delayed);

            /* Apply frequency offset via complex rotation */
            if (fabsf(wp->freq_hz) > 0.0f)
            {
                COMP rot;
                rot.real = cosf(wp->phase);
                rot.imag = sinf(wp->phase);
                contrib = cmult(contrib, rot);
                wp->phase += wp->phase_inc;
                /* wrap phase to [-π, π) */
                while (wp->phase > M_PI)  wp->phase -= 2.0f * M_PI;
                while (wp->phase < -M_PI) wp->phase += 2.0f * M_PI;
            }

            out = cadd(out, contrib);
        }

        /* Normalise summed multi-path power to unit average gain (ch.c hf_gain
         * convention) so the faded signal keeps the input's average power and
         * the AWGN below lands at SNR3k = -No - 14.82. */
        out = fcmult(w->chan_norm, out);

        /* AWGN (optional) */
        if (w->awgn_en)
        {
            COMP noise;
            /* per-component variance noise_var/2 => complex noise power = Fs*No,
             * matching ch.c (see the no-paths branch above). */
            noise.real = gaussian() * sqrtf(w->noise_var * 0.5f);
            noise.imag = gaussian() * sqrtf(w->noise_var * 0.5f);
            w->sig_pwr_acc   += (double)out.real * out.real +
                                (double)out.imag * out.imag;
            w->noise_pwr_acc += (double)noise.real * noise.real +
                                (double)noise.imag * noise.imag;
            w->meas_nsamp++;
            out = cadd(out, noise);
        }

        samples[i] = out;
    }
}
