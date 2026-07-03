/* tests/sim/sim_channel.c
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#include "sim_channel.h"
#include "arq_protocol.h"
#include <stdlib.h>

struct sim_channel { uint64_t state; double per; uint32_t guard_ms; };

sim_channel_t *sim_channel_create(const sim_channel_cfg_t *cfg)
{
    sim_channel_t *ch = calloc(1, sizeof(*ch));
    if (!ch) return NULL;
    ch->state    = cfg->seed ? cfg->seed : 0x9E3779B97F4A7C15ULL;
    ch->per      = cfg->per;
    ch->guard_ms = cfg->guard_ms;
    return ch;
}

void sim_channel_destroy(sim_channel_t *ch) { free(ch); }

/* SplitMix64: deterministic, seedable, no global state. */
double sim_channel_next_rand(sim_channel_t *ch)
{
    uint64_t z = (ch->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return (double)(z >> 11) / (double)(1ULL << 53);
}

uint32_t sim_channel_airtime_ms(int freedv_mode, size_t frame_size)
{
    (void)frame_size;
    for (int i = 0; i < arq_mode_table_count; i++)
        if (arq_mode_table[i].freedv_mode == freedv_mode)
            return (uint32_t)(arq_mode_table[i].frame_duration_s * 1000.0f + 0.5f);
    /* Unknown mode: use DATAC15's duration as a safe nonzero fallback. */
    return 4400;
}

bool sim_channel_schedule(sim_channel_t *ch, uint64_t now_ms,
                          int dir, int freedv_mode, size_t frame_size,
                          uint64_t *deliver_at_ms)
{
    (void)dir;
    if (sim_channel_next_rand(ch) < ch->per)
        return false;   /* erased */
    uint32_t air = sim_channel_airtime_ms(freedv_mode, frame_size);
    *deliver_at_ms = now_ms + air + ch->guard_ms;
    return true;
}
