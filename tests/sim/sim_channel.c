/* tests/sim/sim_channel.c
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#include "sim_channel.h"
#include "arq_protocol.h"
#include "freedv_api.h"
#include "modem_mfsk.h"   /* MERCURY_MODE_MFSK */
#include <stdlib.h>

struct sim_channel {
    uint64_t state;
    double   per;
    uint32_t guard_ms;
    bool     cliff_enabled;   /* mode-aware erasure (see sim_channel_set_snr) */
    double   snr_db;          /* current channel SNR when cliff_enabled */
    bool     dir_cliff[2];    /* per-direction cliff enable (0=fwd, 1=rev) */
    double   dir_snr_db[2];   /* per-direction channel SNR when dir_cliff */
    sim_mode_per_t mode_per[SIM_MODE_PER_MAX]; /* empirical per-mode erasure */
    int      mode_per_count;
};

/* Per-mode SNR cliff (dB): below this the mode effectively stops decoding.
 * Approximates the MPP delivery curves in docs/MODES.md.  MFSK is the fringe
 * floor (~-13 dB non-coherent): it survives ~6 dB deeper than DATAC15. */
static double mode_cliff_db(int freedv_mode)
{
    switch (freedv_mode)
    {
    case FREEDV_MODE_QAM16C2: return 13.0;
    case FREEDV_MODE_DATAC17: return  8.0;
    case FREEDV_MODE_DATAC1:  return  5.0;
    case FREEDV_MODE_DATAC3:  return  0.0;
    case FREEDV_MODE_DATAC4:  return -4.0;
    case FREEDV_MODE_DATAC13: return -4.0;
    case FREEDV_MODE_DATAC14: return -2.0;
    case MERCURY_MODE_MFSK:   return -13.0; /* weak-signal fringe floor */
    default:                  return -7.0;  /* DATAC15 / DATAC16 floor modes */
    }
}

/* Pattern-ACK SNR cliff (dB): the Welch-Costas tone burst survives ~10 dB
 * deeper than a coded DATAC16 ACK — 100% down to ~-9 dB, usable to ~-13 dB. */
#define SIM_PATTERN_CLIFF_DB   (-13.0)
#define SIM_PATTERN_PER         0.02   /* residual loss above the pattern cliff */
#define SIM_PATTERN_AIRTIME_MS  640    /* ~0.64 s (vs 3.74 s DATAC16 ACK)      */

/* Erasure probability of a frame above its mode's cliff.  Not 1.0: even a
 * dead mode occasionally lands a frame on real HF, and a tiny success rate
 * keeps unbounded-retry pathologies observable rather than instantly fatal. */
#define SIM_CLIFF_PER 0.90

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

void sim_channel_set_per(sim_channel_t *ch, double per)
{
    if (ch)
        ch->per = per;
}

void sim_channel_set_snr(sim_channel_t *ch, double snr_db)
{
    if (ch)
    {
        ch->cliff_enabled = true;
        ch->snr_db        = snr_db;
    }
}

void sim_channel_set_mode_per(sim_channel_t *ch,
                              const sim_mode_per_t *table, int count)
{
    if (!ch) return;
    if (count > SIM_MODE_PER_MAX) count = SIM_MODE_PER_MAX;
    for (int i = 0; i < count; i++)
        ch->mode_per[i] = table[i];
    ch->mode_per_count = count;
}

void sim_channel_set_dir_snr(sim_channel_t *ch, int dir, double snr_db)
{
    if (!ch || dir < 0 || dir > 1) return;
    ch->dir_cliff[dir]  = true;
    ch->dir_snr_db[dir] = snr_db;
}

uint32_t sim_channel_pattern_airtime_ms(void) { return SIM_PATTERN_AIRTIME_MS; }

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
    double per = ch->per;
    if (ch->mode_per_count > 0)
    {
        for (int i = 0; i < ch->mode_per_count; i++)
            if (ch->mode_per[i].freedv_mode == freedv_mode)
            {
                per = ch->mode_per[i].per;
                break;
            }
    }
    else if (dir >= 0 && dir <= 1 && ch->dir_cliff[dir])
    {
        /* Per-direction cliff (asymmetric link). */
        if (ch->dir_snr_db[dir] < mode_cliff_db(freedv_mode))
            per = SIM_CLIFF_PER;
    }
    else if (ch->cliff_enabled && ch->snr_db < mode_cliff_db(freedv_mode))
        per = SIM_CLIFF_PER;
    if (sim_channel_next_rand(ch) < per)
        return false;   /* erased */
    uint32_t air = sim_channel_airtime_ms(freedv_mode, frame_size);
    *deliver_at_ms = now_ms + air + ch->guard_ms;
    return true;
}

bool sim_channel_pattern_schedule(sim_channel_t *ch, uint64_t now_ms,
                                  int dir, uint64_t *deliver_at_ms)
{
    /* The pattern ACK's own erasure: it survives far deeper than a coded
     * frame, so it uses SIM_PATTERN_PER above its cliff (and the base per
     * floor), and the cliff only bites below ~-13 dB. */
    double per = SIM_PATTERN_PER;
    if (ch->per > per) per = ch->per;   /* honour an explicit flat loss */
    double snr;
    bool have_snr = false;
    if (dir >= 0 && dir <= 1 && ch->dir_cliff[dir]) { snr = ch->dir_snr_db[dir]; have_snr = true; }
    else if (ch->cliff_enabled)                     { snr = ch->snr_db;          have_snr = true; }
    if (have_snr && snr < SIM_PATTERN_CLIFF_DB)
        per = SIM_CLIFF_PER;
    if (sim_channel_next_rand(ch) < per)
        return false;   /* erased */
    *deliver_at_ms = now_ms + SIM_PATTERN_AIRTIME_MS + ch->guard_ms;
    return true;
}
