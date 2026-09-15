/* tests/sim/sim_channel.h
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#ifndef SIM_CHANNEL_H
#define SIM_CHANNEL_H
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

/* Empirical per-mode erasure model: each entry maps a FreeDV mode to a
 * measured frame-erasure probability from a reference channel run (e.g. a
 * pathsim NVIS characterization).  Modes not listed use the base per.
 * Takes precedence over the cliff model while installed (count > 0). */
typedef struct { int freedv_mode; double per; } sim_mode_per_t;
#define SIM_MODE_PER_MAX 12
void           sim_channel_set_mode_per(sim_channel_t *ch,
                                        const sim_mode_per_t *table, int count);

/* Per-direction channel SNR (cliff model) for asymmetric-link tests.
 *   dir 0 = A->B (forward), dir 1 = B->A (reverse).
 * Enables the cliff model on that direction only; the other direction keeps
 * whatever it was configured with (base per or its own SNR). */
void           sim_channel_set_dir_snr(sim_channel_t *ch, int dir, double snr_db);

uint32_t       sim_channel_airtime_ms(int freedv_mode, size_t frame_size);

/* Short airtime for a pattern ACK (Welch-Costas tone burst, ~0.64 s). */
uint32_t       sim_channel_pattern_airtime_ms(void);

bool           sim_channel_schedule(sim_channel_t *ch, uint64_t now_ms,
                                     int dir, int freedv_mode, size_t frame_size,
                                     uint64_t *deliver_at_ms);

/* Schedule a pattern ACK: short airtime + its own (low) erasure, which
 * survives ~10 dB deeper than a coded frame.  dir selects the reverse-path
 * SNR when per-direction SNR is set. */
bool           sim_channel_pattern_schedule(sim_channel_t *ch, uint64_t now_ms,
                                             int dir, uint64_t *deliver_at_ms);

double         sim_channel_next_rand(sim_channel_t *ch);
#endif
