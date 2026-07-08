/* HERMES Modem — channel-busy (occupancy) detector
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A VARA-style "channel busy" detector: it classifies the HF channel as BUSY
 * or CLEAR from the RX power spectrum (the same FFT already produced for the
 * UI waterfall), using a tracked noise floor plus hysteresis and hang/debounce
 * timing to avoid flapping.  The host is notified with VARA's exact
 * "BUSY ON" / "BUSY OFF" syntax (see tnc_send_busy()).
 *
 * The classifier is a pure, clock-injected state machine so it is unit-testable
 * without any audio hardware.
 */
#ifndef CHANNEL_BUSY_H
#define CHANNEL_BUSY_H

#include <stdbool.h>
#include <stdint.h>

/* Occupancy band: the SSB passband we actually care about (Hz).  Energy
 * outside this range (DC/LF artefacts, out-of-passband splatter) is ignored. */
#define BUSY_PASSBAND_LO_HZ 300
#define BUSY_PASSBAND_HI_HZ 2700

typedef struct {
    float    threshold_db;     /* BUSY when passband peak >= floor + threshold_db     */
    float    hysteresis_db;    /* CLEAR only when peak < floor + threshold - hyst      */
    uint32_t on_debounce_ms;   /* peak must stay above for this long to assert BUSY    */
    uint32_t hang_ms;          /* peak must stay below for this long to release BUSY   */
} busy_cfg_t;

typedef struct {
    bool     busy;             /* current debounced occupancy state                    */
    float    noise_floor_db;   /* slowly-tracked passband noise floor                  */
    bool     inited;           /* false until the first update seeds the floor         */
    uint64_t above_since_ms;   /* 0 = not currently above the assert threshold         */
    uint64_t below_since_ms;   /* 0 = not currently below the release threshold        */
} busy_state_t;

/* Sensible defaults for HF NVIS.  Detector is opt-in (disabled unless a config
 * enables it); these are the values used when it is on and unconfigured. */
static const busy_cfg_t BUSY_CFG_DEFAULT = {
    .threshold_db   = 10.0f,
    .hysteresis_db  = 3.0f,
    .on_debounce_ms = 300,
    .hang_ms        = 1500,
};

/* Reset a detector to the pre-seed state. */
void channel_busy_init(busy_state_t *st);

/* Feed one RX power-spectrum frame.
 *
 *   spectrum_dB[0..nbins-1] : magnitude spectrum in dB; bin i is centred at
 *                             frequency i * (sample_rate_hz/2) / nbins
 *                             (real-input FFT, bins span 0 .. Fs/2).
 *   now_ms                  : monotonic milliseconds (injected for testability).
 *
 * Updates the tracked noise floor and the debounced BUSY/CLEAR state.  Returns
 * true iff the debounced state CHANGED on this call, with *out_busy set to the
 * new state (edge-triggered — matches VARA's BUSY ON/OFF emission).  Returns
 * false (and leaves *out_busy at the current state) when nothing changed or the
 * inputs are unusable. */
bool channel_busy_update(busy_state_t *st, const busy_cfg_t *cfg,
                         const float *spectrum_dB, int nbins,
                         int sample_rate_hz, uint64_t now_ms,
                         bool *out_busy);

#endif /* CHANNEL_BUSY_H */
