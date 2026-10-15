/* Watterson HF Ionospheric Channel Model (ITU-R F.1487) — Implementation
 *
 * Copyright (C) 2025-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implements the Watterson narrowband HF ionospheric channel model per
 * ITU-R Recommendation F.1487 / CCIR 520. Each propagation path is modelled
 * as a tapped-delay-line with its own delay, Doppler spread, frequency
 * offset, and gain.  Each tap is a complex Gaussian process with a Gaussian
 * Doppler power spectrum whose "spread" (ITU-R F.1487: twice the standard
 * deviation) is the path's doppler_hz: white complex Gaussian noise at a low
 * rate, shaped by a Gaussian FIR, interpolated to the sample rate.
 *
 * Up to and including release 1.9.18 the taps were shaped by a 2nd-order
 * Butterworth IIR run at the sample rate with its cutoff at doppler_hz.
 * Measured against the standard that is both the wrong shape (22 % of the tap
 * power beyond the nominal width against 4.6 % for the Gaussian) and the
 * wrong width (the cutoff is the spectrum's rms width, so "1 Hz" fades at the
 * rate of a 2 Hz CCIR spread).  It also places poles within 1e-4 of the unit
 * circle at 48 kHz.  It is kept as WATTERSON_SPECTRUM_LEGACY so existing
 * results reproduce; see docs/CHANNEL-MODEL.md.
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
static float gaussian(watterson_t *w)
{
    return mercury_prng_gaussian(&w->rng);
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
static void butterworth_2nd_lp(double fc, double fs,
                                double *b0, double *b1, double *b2,
                                double *a1, double *a2)
{
    /* Pre-warp cutoff frequency */
    double c = tan(M_PI * fc / fs);
    double c2 = c * c;
    double sqrt2c = 1.41421356237309505 * c;  /* sqrt(2) */

    double den0 = 1.0 + sqrt2c + c2;

    *b0 = c2 / den0;
    *b1 = 2.0 * c2 / den0;
    *b2 = c2 / den0;

    *a1 = 2.0 * (c2 - 1.0) / den0;
    *a2 = (1.0 - sqrt2c + c2) / den0;
}

/* Apply a 2nd-order IIR filter to one scalar sample.
 *
 * Direct Form I:
 *   y[n] = b0·x[n] + b1·x[n-1] + b2·x[n-2] - a1·y[n-1] - a2·y[n-2]
 *
 * The x[3] and y[3] arrays hold {x[n-2], x[n-1], x[n]}.
 */
static double iir_tick(double xn, double x[3], double y[3],
                       double b0, double b1, double b2,
                       double a1, double a2)
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
    w->spectrum = WATTERSON_SPECTRUM_GAUSSIAN;
    watterson_seed_auto(w);

    return 0;
}

int watterson_set_spectrum(watterson_t *w, int spectrum)
{
    assert(w != NULL);
    if (spectrum != WATTERSON_SPECTRUM_GAUSSIAN && spectrum != WATTERSON_SPECTRUM_LEGACY)
        return -1;
    w->spectrum = spectrum;
    return 0;
}

/* Range limits for watterson_add_path(), see the contract in watterson.h.
 *
 * The Gaussian tap is produced once every R = floor(fs / (64 * doppler_hz))
 * samples.  Converting that ratio to int is undefined once it exceeds
 * INT_MAX, which happens below about fs / (64 * 2^31) Hz (6e-8 Hz at 8 kHz);
 * what the cast then yields depends on the target (wrap-around on one, a
 * saturated INT_MAX on another), and either way the path is built without any
 * error being reported.  The ratio is therefore computed in double and a
 * spread that would need more than WATTERSON_MAX_INTERP samples per tap
 * sample is rejected before anything is allocated. */
#define WATTERSON_MAX_INTERP     1073741824.0   /* 2^30 output samples per tap sample */
#define WATTERSON_MAX_FIR_HALF   4096.0         /* the formula gives 2..124; a defensive bound */
#define WATTERSON_MAX_DELAY_MS   10000.0f       /* 10 s; HF multipath is a few ms */

static double gaussian_interp_ratio(double fs, double doppler_hz)
{
    return floor(fs / (64.0 * doppler_hz));
}

/* Gaussian Doppler tap generator.
 *
 * The tap process is sampled at fs/R with R chosen so the low rate is about
 * 64 times the spread (at least 1 sample per output sample).  The FIR is the
 * sampled impulse response exp(-4 pi^2 sigma^2 t^2), truncated where it has
 * fallen to 1e-4, and scaled so that unit-variance complex white noise in
 * gives E[|tap|^2] = 1 out.  The history is pre-filled with noise so the
 * tap starts in its steady-state distribution rather than ramping from zero. */
static int gaussian_tap_setup(watterson_t *w, watterson_path_t *p)
{
    double sigma = 0.5 * (double)p->doppler_hz;             /* ITU-R F.1487: spread = 2 sigma */
    double fs = (double)w->sample_rate;
    double Rd = gaussian_interp_ratio(fs, (double)p->doppler_hz);
    if (!(Rd <= WATTERSON_MAX_INTERP)) return -1;           /* checked by the caller too; NaN lands here */
    int R = (int)Rd;
    if (R < 1) R = 1;
    double fsd = fs / (double)R;
    /* half-length where exp(-4 pi^2 sigma^2 t^2) = 1e-4.  fsd is between 64
     * and 128 times the spread (R is floored) unless R clamps to 1, so the
     * result is 2 to 124 taps a side for any accepted spread; the bound only
     * keeps the cast defined. */
    double t_half = sqrt(log(1e4)) / (2.0 * M_PI * sigma);
    double half_d = ceil(t_half * fsd);
    if (!(half_d <= WATTERSON_MAX_FIR_HALF)) return -1;
    int half = (int)half_d;
    if (half < 2) half = 2;
    int len = 2 * half + 1;
    float *fir = (float *)malloc((size_t)len * sizeof(float));
    COMP  *hist = (COMP *)calloc((size_t)len, sizeof(COMP));
    if (!fir || !hist) { free(fir); free(hist); return -1; }
    double e = 0.0;
    for (int k = 0; k < len; k++)
    {
        double t = (double)(k - half) / fsd;
        double h = exp(-4.0 * M_PI * M_PI * sigma * sigma * t * t);
        fir[k] = (float)h;
        e += h * h;
    }
    /* per-component noise variance is 1, so E[|tap|^2] = 2 * sum(h^2) * g^2 = 1 */
    float g = (float)(1.0 / sqrt(2.0 * e));
    for (int k = 0; k < len; k++) fir[k] *= g;
    p->fir = fir; p->fir_len = len; p->fir_hist = hist; p->fir_idx = 0;
    p->interp_ratio = R; p->interp_phase = 0;
    for (int k = 0; k < len; k++)
    {
        hist[k].real = gaussian(w);
        hist[k].imag = gaussian(w);
    }
    p->tap_norm = 1.0f;
    return 0;
}

/* Next low-rate tap sample: push one complex noise draw, run the FIR. */
static COMP gaussian_tap_next(watterson_t *w, watterson_path_t *p)
{
    p->fir_hist[p->fir_idx].real = gaussian(w);
    p->fir_hist[p->fir_idx].imag = gaussian(w);
    p->fir_idx++;
    if (p->fir_idx >= p->fir_len) p->fir_idx = 0;
    double re = 0.0, im = 0.0;
    int idx = p->fir_idx;                 /* oldest sample first */
    for (int k = 0; k < p->fir_len; k++)
    {
        re += (double)p->fir[k] * p->fir_hist[idx].real;
        im += (double)p->fir[k] * p->fir_hist[idx].imag;
        idx++;
        if (idx >= p->fir_len) idx = 0;
    }
    COMP t; t.real = (float)re; t.imag = (float)im;
    return t;
}

void watterson_seed(watterson_t *w, unsigned int seed)
{
    assert(w != NULL);
    mercury_prng_seed(&w->rng, seed);
    w->seed = seed;
}

unsigned int watterson_seed_auto(watterson_t *w)
{
    assert(w != NULL);
    w->seed = mercury_prng_seed_auto(&w->rng, "MERCURY_WATTERSON_SEED");
    return w->seed;
}

unsigned int watterson_get_seed(const watterson_t *w)
{
    assert(w != NULL);
    return w->seed;
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
        free(w->paths[i].fir);      w->paths[i].fir = NULL;
        free(w->paths[i].fir_hist); w->paths[i].fir_hist = NULL;
    }
    memset(w, 0, sizeof(*w));
}

int watterson_add_path(watterson_t *w, float delay_ms, float doppler_hz,
                        float freq_hz, float gain)
{
    watterson_path_t *p;
    int idx;

    assert(w != NULL);

    /* Range checks are returns, not asserts: the command-line tool hands user
     * values straight here, and asserts vanish under NDEBUG. */
    if (!isfinite(delay_ms) || delay_ms < 0.0f || delay_ms > WATTERSON_MAX_DELAY_MS)
        return -1;
    if (!isfinite(doppler_hz) || doppler_hz < 0.0f)
        return -1;
    if (!isfinite(freq_hz))
        return -1;
    if (!isfinite(gain) || gain < 0.0f || gain > 1.0f)
        return -1;
    /* A positive spread below about sample_rate / 2^36: see the limits above.
     * Applied to both spectra so the contract does not depend on the mode. */
    if (doppler_hz > 0.0f &&
        !(gaussian_interp_ratio((double)w->sample_rate, (double)doppler_hz) <= WATTERSON_MAX_INTERP))
        return -1;

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
    /* The line was one sample short up to and including release 1.9.18 (see
     * watterson_process); the legacy spectrum keeps that so a pinned seed
     * reproduces the old channel. */
    if (w->spectrum == WATTERSON_SPECTRUM_LEGACY && p->delay_samples > 0)
        p->delay_samples -= 1;
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

    p->spectrum = w->spectrum;

    if (p->doppler_hz > 0.0f && p->spectrum == WATTERSON_SPECTRUM_GAUSSIAN)
    {
        if (gaussian_tap_setup(w, p) < 0)
        {
            free(p->delay_buf);
            p->delay_buf = NULL;
            return -1;
        }
        /* prime the interpolator with two consecutive low-rate samples */
        p->tap_prev = gaussian_tap_next(w, p);
        p->tap_next = gaussian_tap_next(w, p);
    }
    else if (p->doppler_hz > 0.0f)
    {
        /* LEGACY: 2nd-order Butterworth IIR at the sample rate, 3 dB cutoff at
         * doppler_hz (which is the resulting spectrum's rms width, not the
         * ITU-R spread).  Bypassed for a static path. */
        double fc = p->doppler_hz;

        butterworth_2nd_lp(fc, (double)w->sample_rate,
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
            double xh[3] = {0,0,0}, yh[3] = {0,0,0};
            double e_h = 0.0;
            double in = 1.0;        /* unit impulse at n=0, zero thereafter */
            int n;
            for (n = 0; n < 2000000; n++)
            {
                double y = iir_tick(in, xh, yh, p->b0, p->b1, p->b2, p->a1, p->a2);
                in = 0.0;
                e_h += y * y;
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
                iir_tick(gaussian(w), p->x_i, p->y_i, p->b0, p->b1, p->b2, p->a1, p->a2);
                iir_tick(gaussian(w), p->x_q, p->y_q, p->b0, p->b1, p->b2, p->a1, p->a2);
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

/* ---- named presets -------------------------------------------------------
 *
 * ITU-R F.1487 gives each case as two equal-power Rayleigh paths with a delay
 * spread and a Doppler spread, which it defines as 2 sigma of the Gaussian
 * Doppler spectrum.  The itur-* values are those of F.1487 Annex 3.  The
 * ccir-good/moderate/poor values are the commonly published CCIR 520-2
 * numbers, which equal F.1487's mid-latitude quiet, moderate and disturbed
 * cases; ccir-flutter is the CCIR 520-2 flutter channel, which F.1487 does
 * not have (no copy of CCIR 520-2 was available, so no clause is cited).
 * See docs/CHANNEL-MODEL.md, "Sources".
 * tests/common/test_watterson.c checks every preset in this table through
 * the preset itself: an impulse comes out at the delay the row states, and
 * the Gaussian spectrum's tails and -10 dB fade rate match their closed
 * forms where the legacy Butterworth's do not.  There is no second copy of
 * the table anywhere. */
#define G WATTERSON_SPECTRUM_GAUSSIAN
#define L WATTERSON_SPECTRUM_LEGACY
#define P2(d, s) 2, {{0.0f, (s), 0.70710678f}, {(d), (s), 0.70710678f}}
static const watterson_preset_t presets[] = {
    {"ccir-good",     "CCIR 520 Good: 0.5 ms, 0.1 Hz",                      G, P2(0.5f, 0.1f)},
    {"ccir-moderate", "CCIR 520 Moderate: 1 ms, 0.5 Hz",                    G, P2(1.0f, 0.5f)},
    {"ccir-poor",     "CCIR 520 Poor: 2 ms, 1 Hz",                          G, P2(2.0f, 1.0f)},
    {"ccir-flutter",  "CCIR 520 Flutter: 0.5 ms, 10 Hz",                    G, P2(0.5f, 10.0f)},
    {"itur-lq",       "ITU-R F.1487 low latitude, quiet: 0.5 ms, 0.5 Hz",    G, P2(0.5f, 0.5f)},
    {"itur-lm",       "ITU-R F.1487 low latitude, moderate: 2 ms, 1.5 Hz",   G, P2(2.0f, 1.5f)},
    {"itur-ld",       "ITU-R F.1487 low latitude, disturbed: 6 ms, 10 Hz",   G, P2(6.0f, 10.0f)},
    {"itur-mq",       "ITU-R F.1487 mid latitude, quiet: 0.5 ms, 0.1 Hz",    G, P2(0.5f, 0.1f)},
    {"itur-mm",       "ITU-R F.1487 mid latitude, moderate: 1 ms, 0.5 Hz",   G, P2(1.0f, 0.5f)},
    {"itur-md",       "ITU-R F.1487 mid latitude, disturbed: 2 ms, 1 Hz",    G, P2(2.0f, 1.0f)},
    {"itur-mdv",      "ITU-R F.1487 mid latitude, disturbed NVIS: 7 ms, 1 Hz", G, P2(7.0f, 1.0f)},
    {"itur-hq",       "ITU-R F.1487 high latitude, quiet: 1 ms, 0.5 Hz",     G, P2(1.0f, 0.5f)},
    {"itur-hm",       "ITU-R F.1487 high latitude, moderate: 3 ms, 10 Hz",   G, P2(3.0f, 10.0f)},
    {"itur-hd",       "ITU-R F.1487 high latitude, disturbed: 7 ms, 30 Hz",  G, P2(7.0f, 30.0f)},
    /* legacy: Butterworth taps, doppler_hz is the filter cutoff (rms width) */
    {"good",     "legacy Good: 1 path, 0.1 Hz cutoff (Butterworth)",            L, 1, {{0.0f, 0.1f, 0.8f}}},
    {"moderate", "legacy Moderate: 2 paths, 1 ms, 1.0 Hz cutoff (Butterworth)", L, 2, {{0.0f, 1.0f, 0.7f}, {1.0f, 1.0f, 0.7f}}},
    {"poor",     "legacy Poor: 2 paths, 2 ms, 1.0 Hz cutoff (Butterworth)",     L, 2, {{0.0f, 1.0f, 0.7f}, {2.0f, 1.0f, 0.7f}}},
    {"flutter",  "legacy Flutter: 2 paths, 2 ms, 10 Hz cutoff (Butterworth)",   L, 2, {{0.0f, 10.0f, 0.7f}, {2.0f, 10.0f, 0.7f}}},
    {"mpg",      "legacy MPG: 2 paths, 0.5 ms, 0.1 Hz cutoff (Butterworth)",    L, 2, {{0.0f, 0.1f, 0.7f}, {0.5f, 0.1f, 0.7f}}},
    {"mpp",      "legacy MPP: 2 paths, 1 ms, 1.0 Hz cutoff (Butterworth)",      L, 2, {{0.0f, 1.0f, 0.7f}, {1.0f, 1.0f, 0.7f}}},
    {"mpd",      "legacy MPD: 2 paths, 2 ms, 2.0 Hz cutoff (Butterworth)",      L, 2, {{0.0f, 2.0f, 0.7f}, {2.0f, 2.0f, 0.7f}}},
};
#undef G
#undef L
#undef P2

const watterson_preset_t *watterson_presets(int *count)
{
    if (count) *count = (int)(sizeof(presets) / sizeof(presets[0]));
    return presets;
}

const watterson_preset_t *watterson_preset_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++)
        if (strcmp(presets[i].name, name) == 0) return &presets[i];
    return NULL;
}

int watterson_apply_preset(watterson_t *w, const watterson_preset_t *preset)
{
    assert(w != NULL);
    if (!preset) return -1;
    /* drop any paths already present, keep the sample rate, noise and rng */
    for (int i = 0; i < w->num_paths; i++)
    {
        free(w->paths[i].delay_buf); free(w->paths[i].fir); free(w->paths[i].fir_hist);
    }
    memset(w->paths, 0, sizeof(w->paths));
    w->num_paths = 0;
    w->chan_norm = 1.0f;
    /* Build with the preset's spectrum, then put back the caller's selection
     * so a path added after the preset is not silently given the preset's
     * tap filter. */
    int saved_spectrum = w->spectrum;
    int rc = 0;
    w->spectrum = preset->spectrum;
    for (int i = 0; i < preset->npaths && rc == 0; i++)
        if (watterson_add_path(w, preset->path[i].delay_ms, preset->path[i].doppler_hz, 0.0f, preset->path[i].gain) < 0)
            rc = -1;
    w->spectrum = saved_spectrum;
    return rc;
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
                noise.real = gaussian(w) * sqrtf(w->noise_var * 0.5f);
                noise.imag = gaussian(w) * sqrtf(w->noise_var * 0.5f);
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
            if (wp->doppler_hz > 0.0f && wp->spectrum == WATTERSON_SPECTRUM_GAUSSIAN)
            {
                /* linear interpolation between consecutive low-rate samples */
                float a = (float)wp->interp_phase / (float)wp->interp_ratio;
                tap.real = wp->tap_prev.real + a * (wp->tap_next.real - wp->tap_prev.real);
                tap.imag = wp->tap_prev.imag + a * (wp->tap_next.imag - wp->tap_prev.imag);
                wp->interp_phase++;
                if (wp->interp_phase >= wp->interp_ratio)
                {
                    wp->interp_phase = 0;
                    wp->tap_prev = wp->tap_next;
                    wp->tap_next = gaussian_tap_next(w, wp);
                }
            }
            else if (wp->doppler_hz > 0.0f)
            {
                float xn_i = gaussian(w);
                float xn_q = gaussian(w);
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

            /* Delay line: read the slot first (written delay_samples iterations
             * ago), then overwrite it with the current sample.  Writing first and
             * reading the next slot, as this did up to and including release
             * 1.9.18, delivers one sample LESS than configured (and no delay at
             * all for a one-sample line); the legacy spectrum compensates in
             * watterson_add_path() so its realisations are unchanged. */
            if (wp->delay_samples > 0)
            {
                delayed = wp->delay_buf[wp->delay_idx];
                wp->delay_buf[wp->delay_idx] = samples[i];
                wp->delay_idx++;
                if (wp->delay_idx >= wp->delay_samples)
                    wp->delay_idx = 0;
            }
            else
            {
                delayed = samples[i];
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
            noise.real = gaussian(w) * sqrtf(w->noise_var * 0.5f);
            noise.imag = gaussian(w) * sqrtf(w->noise_var * 0.5f);
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
