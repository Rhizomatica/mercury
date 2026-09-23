/* HERMES Modem — channel-busy (occupancy) detector
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See channel_busy.h for the interface contract.  The classifier keeps a
 * slowly-tracked passband noise floor and asserts BUSY when the passband peak
 * rises threshold_db above that floor for on_debounce_ms, releasing only after
 * it falls below (threshold_db - hysteresis_db) for hang_ms.
 */
#include "channel_busy.h"

#include <math.h>
#include <stddef.h>

/* Noise-floor tracker asymmetric EMA: fall fast toward a lower reading (so a
 * genuinely quiet channel re-establishes a low floor quickly), rise slowly
 * toward a higher reading (so a sustained signal does NOT get absorbed into the
 * floor and mask itself).  Coefficients are per-update; the RX spectrum arrives
 * ~20x/s so these give multi-second rise, sub-second fall.
 *
 * While BUSY the rise is much slower still.  A digital mode covers most of the
 * passband, so the floor candidate sits inside it; at the normal rate a
 * continuous +12 dB signal was absorbed and read CLEAR 13 s into the
 * transmission -- the moment a host would be told it may key over it.  Held,
 * a continuous wideband signal stays BUSY ~60 s at +12 dB and ~95 s at +20 dB;
 * one covering half the passband or less never moves the floor at all.  The
 * cost: a step up in the noise itself (a new QRM source, the band opening)
 * cannot be told from a wideband signal and reads BUSY until the floor catches
 * up, ~100 s for a 12 dB step and ~145 s for 20 dB. */
#define BUSY_FLOOR_FALL      0.20f     /* toward a new, lower reading           */
#define BUSY_FLOOR_RISE      0.003f    /* toward a higher one: ~17 s at 20/s     */
#define BUSY_FLOOR_RISE_HELD 0.0006f   /* the same while BUSY: ~80 s at 20/s     */

void channel_busy_init(busy_state_t *st)
{
    if (!st) return;
    st->busy           = false;
    st->noise_floor_db = 0.0f;
    st->inited         = false;
    st->above_since_ms = 0;
    st->below_since_ms = 0;
}

/* Map a passband edge frequency to a spectrum bin index. Real-input FFT: bin i
 * is centred at i * (Fs/2)/nbins, so bin = f / ((Fs/2)/nbins). */
static int freq_to_bin(int freq_hz, int nbins, int sample_rate_hz)
{
    if (sample_rate_hz <= 0 || nbins <= 0) return 0;
    float hz_per_bin = (float)(sample_rate_hz / 2) / (float)nbins;
    if (hz_per_bin <= 0.0f) return 0;
    int bin = (int)(freq_hz / hz_per_bin);
    if (bin < 0) bin = 0;
    if (bin > nbins - 1) bin = nbins - 1;
    return bin;
}

bool channel_busy_update(busy_state_t *st, const busy_cfg_t *cfg,
                         const float *spectrum_dB, int nbins,
                         int sample_rate_hz, uint64_t now_ms,
                         bool *out_busy)
{
    if (!st || !cfg || !spectrum_dB || nbins <= 0) {
        if (out_busy && st) *out_busy = st->busy;
        return false;
    }

    int lo = freq_to_bin(BUSY_PASSBAND_LO_HZ, nbins, sample_rate_hz);
    int hi = freq_to_bin(BUSY_PASSBAND_HI_HZ, nbins, sample_rate_hz);
    if (hi <= lo) { if (out_busy) *out_busy = st->busy; return false; }

    /* Passband statistics over BANDS, not single bins: peak (occupancy
     * evidence) and minimum (noise-floor candidate).
     *
     * One FFT bin of noise is an exponential variable, so across ~300 bins the
     * loudest sits ~7 dB above the mean and the quietest ~25-40 dB below it.
     * Comparing single bins made pure noise read ~40 dB of "excess", far over
     * any threshold: the detector reported BUSY on an empty channel almost all
     * the time (measured on estacao3; only digital silence ever cleared it,
     * because that silenced the peak too).  Averaging the power over
     * BUSY_BAND_HZ bands (~19 bins at 8 kHz) cuts the band-to-band spread of
     * noise to a few dB, while anything worth calling occupancy -- a digital
     * mode, voice, even a carrier well above the noise -- still stands out of
     * its band. */
    float hz_per_bin = (float)(sample_rate_hz / 2) / (float)nbins;
    int band_bins = (hz_per_bin > 0.0f) ? (int)(BUSY_BAND_HZ / hz_per_bin + 0.5f) : 1;
    if (band_bins < 1) band_bins = 1;

    float band_db[BUSY_MAX_BANDS];
    int   nb = 0;
    float peak = -1e9f;
    for (int b0 = lo; b0 <= hi && nb < BUSY_MAX_BANDS; b0 += band_bins) {
        int b1 = b0 + band_bins - 1;
        if (b1 > hi) b1 = hi;
        double pw = 0.0;
        for (int i = b0; i <= b1; i++)
            pw += pow(10.0, spectrum_dB[i] / 10.0);
        float v = (float)(10.0 * log10(pw / (double)(b1 - b0 + 1) + 1e-30));
        band_db[nb++] = v;
        if (v > peak) peak = v;
    }

    /* The floor candidate is the lower-QUARTILE band, not the quietest one.
     * The minimum of noise sits in its low tail, so a floor tracked from it
     * put ordinary noise 8-9 dB "above the floor" -- inside the hysteresis
     * band, where a BUSY, once asserted, could never release.  The quartile is
     * where the noise is, and a signal has to cover three quarters of the
     * passband to move it (the median let a wide digital mode drag the floor
     * up under itself within seconds). */
    for (int i = 1; i < nb; i++) {           /* insertion sort: nb <= 64 */
        float v = band_db[i];
        int j = i - 1;
        while (j >= 0 && band_db[j] > v) { band_db[j + 1] = band_db[j]; j--; }
        band_db[j + 1] = v;
    }
    float floor_cand = nb ? band_db[nb / 4] : peak;

    /* Digital silence is not a quiet channel: it is no audio at all -- e.g.
     * the zeros a radio's capture carries while it transmits.  Measuring it
     * dragged the floor to ~-220 dB, after which ordinary noise read as a
     * 100 dB signal.  Leave the state and the floor untouched. */
    if (peak < BUSY_SILENCE_DB) {
        if (out_busy) *out_busy = st->busy;
        return false;
    }

    /* Seed / track the noise floor from the lower-quartile band. */
    if (!st->inited) {
        st->noise_floor_db = floor_cand;
        st->inited = true;
    } else if (floor_cand < st->noise_floor_db) {
        st->noise_floor_db += BUSY_FLOOR_FALL * (floor_cand - st->noise_floor_db);
    } else {
        float rise = st->busy ? BUSY_FLOOR_RISE_HELD : BUSY_FLOOR_RISE;
        st->noise_floor_db += rise * (floor_cand - st->noise_floor_db);
    }

    float excess       = peak - st->noise_floor_db;
    float assert_level = cfg->threshold_db;
    float release_level = cfg->threshold_db - cfg->hysteresis_db;

    bool changed = false;

    if (!st->busy) {
        /* Currently CLEAR — look for a sustained rise above the assert level. */
        if (excess >= assert_level) {
            if (st->above_since_ms == 0)
                st->above_since_ms = now_ms ? now_ms : 1;  /* 0 is the "unset" sentinel */
            if (now_ms - st->above_since_ms >= cfg->on_debounce_ms) {
                st->busy = true;
                st->above_since_ms = 0;
                st->below_since_ms = 0;
                changed = true;
            }
        } else {
            st->above_since_ms = 0;  /* dipped back down — restart debounce */
        }
    } else {
        /* Currently BUSY — release only after a sustained drop below release. */
        if (excess < release_level) {
            if (st->below_since_ms == 0)
                st->below_since_ms = now_ms ? now_ms : 1;
            if (now_ms - st->below_since_ms >= cfg->hang_ms) {
                st->busy = false;
                st->above_since_ms = 0;
                st->below_since_ms = 0;
                changed = true;
            }
        } else {
            st->below_since_ms = 0;  /* still active — restart hang timer */
        }
    }

    if (out_busy) *out_busy = st->busy;
    return changed;
}
