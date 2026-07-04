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
uint32_t       sim_channel_airtime_ms(int freedv_mode, size_t frame_size);
bool           sim_channel_schedule(sim_channel_t *ch, uint64_t now_ms,
                                     int dir, int freedv_mode, size_t frame_size,
                                     uint64_t *deliver_at_ms);
double         sim_channel_next_rand(sim_channel_t *ch);
#endif
