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
 * ~20x/s so these give multi-second rise, sub-second fall. */
#define BUSY_FLOOR_FALL 0.20f   /* toward a new, lower minimum   */
#define BUSY_FLOOR_RISE 0.01f   /* toward a new, higher level    */

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

    /* Passband statistics: peak (occupancy evidence) and median-ish low value
     * (noise-floor candidate).  A running min over the passband is a robust,
     * allocation-free proxy for the quiet-bin level. */
    float peak = -1e9f;
    float minv =  1e9f;
    for (int i = lo; i <= hi; i++) {
        float v = spectrum_dB[i];
        if (v > peak) peak = v;
        if (v < minv) minv = v;
    }

    /* Seed / track the noise floor from the passband minimum (quiet bins). */
    if (!st->inited) {
        st->noise_floor_db = minv;
        st->inited = true;
    } else if (minv < st->noise_floor_db) {
        st->noise_floor_db += BUSY_FLOOR_FALL * (minv - st->noise_floor_db);
    } else {
        st->noise_floor_db += BUSY_FLOOR_RISE * (minv - st->noise_floor_db);
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
