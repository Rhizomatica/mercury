/* tests/sim/sim_channel.h
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#ifndef SIM_CHANNEL_H
#define SIM_CHANNEL_H
/* Not a modem mode: a 0.64 s Welch-Costas pattern (ACK/BREAK). */
#define SIM_MODE_PATTERN 101

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint64_t seed;
    double   per;       /* per-frame erasure probability [0,1] */
    uint32_t guard_ms;  /* guard added on top of airtime for each delivery */
} sim_channel_cfg_t;

typedef struct sim_channel sim_channel_t;

sim_channel_t *sim_channel_create(const sim_channel_cfg_t *cfg);
void           sim_channel_destroy(sim_channel_t *ch);
/* Change the erasure probability mid-simulation (models a fade onset). */
void           sim_channel_set_per(sim_channel_t *ch, double per);

/* Enable the mode-aware cliff model: frames in a mode whose SNR cliff lies
 * above the current channel SNR are erased with high probability (0.9);
 * modes below their cliff see the base per.  This is what makes "downgrade
 * to a more robust mode" the winning strategy, as on real HF — a mode-blind
 * per cannot reward downgrades.  Cliffs approximate the MPP curves in
 * docs/MODES.md. */
void           sim_channel_set_snr(sim_channel_t *ch, double snr_db);
/* The same, with its own SNR each way: dir 0 (station A sending) and dir 1. */
void           sim_channel_set_snr_asym(sim_channel_t *ch, double snr_dir0_db, double snr_dir1_db);

/* Empirical per-mode erasure model: each entry maps a FreeDV mode to a
 * measured frame-erasure probability from a reference channel run (e.g. a
 * pathsim NVIS characterization).  Modes not listed use the base per.
 * Takes precedence over the cliff model while installed (count > 0). */
typedef struct { int freedv_mode; double per; } sim_mode_per_t;
#define SIM_MODE_PER_MAX 12
/* With per-mode PER: the return direction (1) is also lost below this SNR. */
void           sim_channel_set_return_snr(sim_channel_t *ch, double snr_db);
/* Direction 1 also loses this share of its frames on this mode, whatever the
 * SNR says: a control mode that decodes some of the time where the floor gets
 * through (on air, KG9NI hearing K7EK at -6..-7 dB, 2026-11-11). */
void sim_channel_set_return_mode_per(sim_channel_t *ch, int freedv_mode, double per);
void           sim_channel_set_mode_per(sim_channel_t *ch,
                                        const sim_mode_per_t *table, int count);

/* Time-varying channel: Rayleigh fading around mean_snr_db, independent per
 * direction, at doppler_hz (fade rate); each frame is judged by its effective
 * SNR against the mode cliffs.  Overrides the other models. */
void           sim_channel_set_fading(sim_channel_t *ch, double mean_snr_db, double doppler_hz);

/* The control mode's SNR reads this much off (dB): the control frames
 * under-read the SNR against the data modes -- on MPD by up to ~10 dB against
 * the wide modes at high SNR (#501), the narrowband control frame sitting in
 * a notch the wide data modes average across.  Unlike CAR_SNR_OFFSET, which
 * moves only what a receiver reports, this is the channel's: it applies
 * wherever the channel judges or reports a frame's SNR, so the reading is
 * low and the same deficit decides the decode -- once the mode is below its
 * cliff the frames stop decoding, as any mode below its cliff does.  The one
 * exception is the per-mode PER path's return-cliff test, which reads the
 * channel's SNR without the offset: on such a channel (nvis) the knob is
 * estimate-only, the decode carried by the tabulated PER.  0 (the default):
 * the channel reads as it does. */
void           sim_channel_set_ctl_snr_offset(sim_channel_t *ch, double db);
double         sim_channel_ctl_snr_offset(const sim_channel_t *ch);
/* The SNR a frame of this mode reads at t0_ms: the fading model's per-frame
 * figure, else the caller's fallback figure, with the control mode's offset
 * applied.  What a modem would report for such a frame. */
double         sim_channel_mode_snr_db(const sim_channel_t *ch, uint64_t t0_ms,
                                       int dir, int freedv_mode, double fallback_db);

uint32_t       sim_channel_airtime_ms(int freedv_mode, size_t frame_size);
bool           sim_channel_schedule(sim_channel_t *ch, uint64_t now_ms,
                                     int dir, int freedv_mode, size_t frame_size,
                                     uint64_t *deliver_at_ms);
double         sim_channel_next_rand(sim_channel_t *ch);
/* Whether a receiver could sync on such a frame at all: its SNR is above the
 * mode's cliff, whether or not this one frame is then erased.  (A decoder
 * syncs on a frame whose CRC then fails.)  Draws no random number. */
/* The SNR a modem reports for a frame starting at t0: NAN unless fading. */
double         sim_channel_frame_snr(const sim_channel_t *ch, uint64_t t0_ms, int dir, int freedv_mode);
bool           sim_channel_syncable(const sim_channel_t *ch, uint64_t now_ms,
                                    int dir, int freedv_mode);
/* The erasure probability sim_channel_schedule() would draw against for such
 * a frame starting now (no random number drawn): for bounds. */
double         sim_channel_frame_per(const sim_channel_t *ch, uint64_t now_ms, int dir, int freedv_mode);
#endif
