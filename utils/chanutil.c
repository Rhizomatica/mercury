/* chanutil — see chanutil.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "chanutil.h"
#include "common/watterson.h"
#include "modem/freedv/comp.h"
#include "modem/freedv/ht_coeff.h"

#define CHAN_FS 8000

struct chanutil { watterson_t w; };

int chanutil_preset_from_name(const char *s)
{
    if (!s) return -1;
    if (!strcmp(s, "awgn") || !strcmp(s, "AWGN")) return CHAN_AWGN;
    if (!strcmp(s, "mpp")  || !strcmp(s, "MPP"))  return CHAN_MPP;
    if (!strcmp(s, "mpg")  || !strcmp(s, "MPG"))  return CHAN_MPG;
    if (!strcmp(s, "mpd")  || !strcmp(s, "MPD"))  return CHAN_MPD;
    if (!strcmp(s, "awgnc"))                      return CHAN_AWGN_C;
    return -1;
}

const char *chanutil_preset_name(int preset)
{
    switch (preset)
    {
    case CHAN_AWGN:   return "AWGN";
    case CHAN_MPP:    return "MPP (2 path, 1.0 ms, 1.0 Hz)";
    case CHAN_MPG:    return "MPG (2 path, 0.5 ms, 0.1 Hz)";
    case CHAN_MPD:    return "MPD (2 path, 2.0 ms, 2.0 Hz)";
    case CHAN_AWGN_C: return "AWGN via the complex pipeline (calibration)";
    default:          return "?";
    }
}

static int add_paths(watterson_t *w, int preset)
{
    switch (preset)
    {
    case CHAN_AWGN:
    case CHAN_AWGN_C:
        /* One static unit-gain path: no fading, but keep the same code path so
         * the AWGN and faded rows are produced by identical machinery. */
        return watterson_add_path(w, 0.0f, 0.0f, 0.0f, 1.0f) < 0 ? -1 : 0;
    case CHAN_MPG:
        if (watterson_add_path(w, 0.0f, 0.1f, 0.0f, 0.7f) < 0) return -1;
        if (watterson_add_path(w, 0.5f, 0.1f, 0.0f, 0.7f) < 0) return -1;
        return 0;
    case CHAN_MPP:
        if (watterson_add_path(w, 0.0f, 1.0f, 0.0f, 0.7f) < 0) return -1;
        if (watterson_add_path(w, 1.0f, 1.0f, 0.0f, 0.7f) < 0) return -1;
        return 0;
    case CHAN_MPD:
        if (watterson_add_path(w, 0.0f, 2.0f, 0.0f, 0.7f) < 0) return -1;
        if (watterson_add_path(w, 2.0f, 2.0f, 0.0f, 0.7f) < 0) return -1;
        return 0;
    default:
        return -1;
    }
}

/* Settle the Doppler taps before any burst is presented.
 *
 * watterson_init() zeroes the tap IIR state, and the taps are produced by
 * filtering the model's own white noise -- so they START AT ZERO and ramp to
 * their steady-state distribution over a few filter time constants.  A burst
 * handed to a fresh channel therefore opens inside an artificial deep fade,
 * which is exactly where its preamble is, so the damage lands on acquisition
 * and reads as a catastrophically bad mode.  Measured: DATAC16 on MPP showed
 * 10 % delivered at -7.3 dB against the 67 % in docs/MODES.md until the taps
 * were warmed first.
 *
 * Zeros are the right warm-up input -- the tap generator does not depend on
 * the signal -- and the measurement accumulators are reset afterwards so the
 * warm-up's noise does not enter the reported SNR. */
static void warm_up(watterson_t *w)
{
    float min_doppler = 1e9f;
    for (int p = 0; p < w->num_paths; p++)
        if (w->paths[p].doppler_hz > 0.0f && w->paths[p].doppler_hz < min_doppler)
            min_doppler = w->paths[p].doppler_hz;

    if (min_doppler > 1e8f) return;          /* no fading path: nothing to settle */

    double warm_s = 5.0 / (double)min_doppler;
    if (warm_s < 2.0)  warm_s = 2.0;
    if (warm_s > 20.0) warm_s = 20.0;

    int   warm_n = (int)(warm_s * CHAN_FS);
    COMP *warm   = calloc((size_t)warm_n, sizeof(COMP));
    if (warm)
    {
        watterson_process(w, warm, warm_n);
        free(warm);
    }
    watterson_reset_meas(w);
}

chanutil_t *chanutil_open(int preset, float no_dbhz, unsigned seed)
{
    chanutil_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (watterson_init(&c->w, CHAN_FS) != 0) { free(c); return NULL; }
    if (add_paths(&c->w, preset) < 0)
    {
        watterson_dispose(&c->w);
        free(c);
        return NULL;
    }
    watterson_set_noise(&c->w, no_dbhz);
    watterson_reset_meas(&c->w);

    /* The fading realisation is driven by the model's own RNG, which is
     * per-instance -- so seed THAT, not libc's global rand().  srand() here
     * used to work only by accident, and stopped controlling the channel the
     * moment anything else in the process drew a number. */
    watterson_seed(&c->w, seed);
    warm_up(&c->w);
    return c;
}

void chanutil_close(chanutil_t *c)
{
    if (!c) return;
    watterson_dispose(&c->w);
    free(c);
}

int chanutil_run(chanutil_t *c, int16_t *pb, int n, float *snr3k_out)
{
    if (!c || !pb || n <= 0) return -1;

    /* The Hilbert FIR has HT_N taps and therefore a group delay of HT_N/2.
     * Producing only n outputs for n inputs would (a) shift the burst late by
     * that delay and (b) silently DROP its last HT_N/2 samples, whose filter
     * response lands past the end of the buffer.  On a burst whose preamble is
     * only 880 samples that is not a rounding error, and it lands on
     * acquisition -- which is what sets DATAC16's floor.  So run the filter
     * over a padded buffer and hand back the delay-compensated middle,
     * sample-aligned with the input. */
    const int lat   = HT_N / 2;
    const int ext_n = n + HT_N;

    COMP  *cx    = calloc((size_t)ext_n, sizeof(COMP));
    float *htbuf = calloc((size_t)(ext_n + HT_N), sizeof(float));
    if (!cx || !htbuf) { free(cx); free(htbuf); return -1; }

    /* Real passband -> analytic signal (Hilbert), as watterson_test.c does.
     * Feeding the real samples in as COMP{real,0} instead would apply the
     * complex channel gain to both sidebands, which is not what a fading HF
     * path does to an SSB signal. */
    for (int i = 0; i < n; i++) htbuf[HT_N + i] = (float)pb[i];
    for (int i = 0; i < ext_n; i++)
    {
        int   j  = HT_N + i;
        float re = 0.0f, im = 0.0f;
        for (int k = 0; k < HT_N; k++)
        {
            re += htbuf[j - k] * ht_coeff[k].real;
            im += htbuf[j - k] * ht_coeff[k].imag;
        }
        cx[i].real = re;
        cx[i].imag = im;
    }

    watterson_reset_meas(&c->w);
    watterson_process(&c->w, cx, ext_n);

    for (int i = 0; i < n; i++)
    {
        float s = cx[i + lat].real;
        if (s >  32767.0f) s =  32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        pb[i] = (int16_t)lrintf(s);
    }

    /* watterson_measured_snr3k() is 3.01 dB optimistic for a REAL-output path,
     * and the correction is exact rather than a fudge.
     *
     * The model measures Psig on the signal it was handed -- the analytic
     * signal, whose power is 2P for a real signal of power P, since
     * E[x^2] = E[H{x}^2] = P.  We then hand the modem Re{}, which is x, of
     * power P.  Taking the real part leaves the noise power spectral density
     * unchanged (complex noise of power nvar over Fs has the same PSD as its
     * real part, power nvar/2 over Fs/2), so the noise term is right and only
     * the signal term is doubled.  Hence exactly a factor of two.
     *
     * Verified against the harness's own direct real-domain AWGN path, which
     * hits its requested SNR to 0.01 dB: before this correction the same mode
     * at the same nominal SNR read 3.00 dB apart between the two paths.
     *
     * Corrected here rather than in common/watterson.c on purpose -- the
     * model's convention is right for the complex-domain use it was written
     * and calibrated for, and changing it would silently move every other
     * measurement that depends on it. */
    if (snr3k_out)
        *snr3k_out = watterson_measured_snr3k(&c->w) - 3.01f;

    free(cx);
    free(htbuf);
    return 0;
}

int chanutil_fade(int16_t *pb, int n, int preset, float no_dbhz,
                  unsigned seed, float *snr3k_out)
{
    chanutil_t *c = chanutil_open(preset, no_dbhz, seed);
    if (!c) return -1;
    int rc = chanutil_run(c, pb, n, snr3k_out);
    chanutil_close(c);
    return rc;
}
