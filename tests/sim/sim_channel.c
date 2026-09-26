/* tests/sim/sim_channel.c
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#include "sim_channel.h"
#include "arq_protocol.h"
#include "freedv_api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Rayleigh fading: a sum of sinusoids per direction (Jakes-style). */
#define FADE_OSC 8
#define SIM_STAT_MODES 32

struct sim_channel {
    uint64_t state;
    double   per;
    uint32_t guard_ms;
    bool     cliff_enabled;   /* mode-aware erasure (see sim_channel_set_snr) */
    double   snr_db;          /* current channel SNR when cliff_enabled */
    sim_mode_per_t mode_per[SIM_MODE_PER_MAX]; /* empirical per-mode erasure */
    int      mode_per_count;
    bool     fading;          /* time-varying SNR (see sim_channel_set_fading) */
    double   fade_mean_db, fade_doppler_hz;
    double   fade_w[2][FADE_OSC], fade_ph[2][FADE_OSC][2];  /* per direction */
    uint32_t stat_sent[SIM_STAT_MODES], stat_ok[SIM_STAT_MODES];
    uint64_t stat_air_ms[SIM_STAT_MODES];
};

/* Per-mode SNR cliff (dB): below this the mode effectively stops decoding.
 * Approximates the MPP delivery curves in docs/MODES.md. */
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
    default:                  return -7.0;  /* DATAC15 / DATAC16 floor modes */
    }
}

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

/* SIM_STATS set: print the frames each mode carried when the channel goes. */
void sim_channel_destroy(sim_channel_t *ch)
{
    if (ch && getenv("SIM_STATS"))
        for (int m = 0; m < SIM_STAT_MODES; m++)
            if (ch->stat_sent[m])
                printf("  mode %2d: %4u frames %4u ok %6.0f s air\n", m, ch->stat_sent[m],
                       ch->stat_ok[m], ch->stat_air_ms[m] / 1000.0);
    free(ch);
}

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

static double next_rand(sim_channel_t *ch);

void sim_channel_set_fading(sim_channel_t *ch, double mean_snr_db, double doppler_hz)
{
    if (!ch) return;
    ch->fading = true;
    ch->fade_mean_db = mean_snr_db;
    ch->fade_doppler_hz = doppler_hz;
    for (int d = 0; d < 2; d++)
        for (int k = 0; k < FADE_OSC; k++) {
            /* arrival angles spread over the circle, jittered; random phases */
            double a = 2.0 * M_PI * (k + next_rand(ch)) / FADE_OSC;
            ch->fade_w[d][k] = 2.0 * M_PI * doppler_hz * cos(a);
            ch->fade_ph[d][k][0] = 2.0 * M_PI * next_rand(ch);
            ch->fade_ph[d][k][1] = 2.0 * M_PI * next_rand(ch);
        }
}

/* |h(t)|^2 for one direction, mean 1. */
static double fade_power(const sim_channel_t *ch, int dir, double t_s)
{
    double re = 0, im = 0;
    for (int k = 0; k < FADE_OSC; k++) {
        re += cos(ch->fade_w[dir][k] * t_s + ch->fade_ph[dir][k][0]);
        im += cos(ch->fade_w[dir][k] * t_s + ch->fade_ph[dir][k][1]);
    }
    return (re * re + im * im) / FADE_OSC;
}

/* Effective SNR of a frame spanning [t0, t0+air): the instantaneous SNR at 8
 * points across it, combined by an exponential effective-SNR mapping.  A
 * frame's codeword spans the whole frame, so a deep fade over part of it hurts
 * more than the plain mean would say, and less than the worst instant.  beta
 * scales with the mode's operating point (its cliff), as in EESM: a fixed
 * small beta would reduce every fast mode to its worst instant. */
static double frame_snr_db(const sim_channel_t *ch, int dir, uint64_t t0_ms,
                           uint32_t air_ms, double cliff_db)
{
    double beta = pow(10.0, cliff_db / 10.0);
    if (beta < 0.5) beta = 0.5;
    double acc = 0;
    for (int i = 0; i < 8; i++) {
        double t = (t0_ms + (air_ms * (i + 0.5)) / 8.0) / 1000.0;
        double snr = pow(10.0, ch->fade_mean_db / 10.0) * fade_power(ch, dir & 1, t);
        acc += exp(-snr / beta);
    }
    double eff = -beta * log(acc / 8.0);
    return eff > 1e-9 ? 10.0 * log10(eff) : -90.0;
}

/* SplitMix64: deterministic, seedable, no global state. */
static double next_rand(sim_channel_t *ch) { return sim_channel_next_rand(ch); }

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
    if (ch->fading)
    {
        uint32_t air = sim_channel_airtime_ms(freedv_mode, frame_size);
        double cliff = mode_cliff_db(freedv_mode);
        if (frame_snr_db(ch, dir, now_ms, air, cliff) < cliff)
            per = SIM_CLIFF_PER;
    }
    else if (ch->mode_per_count > 0)
    {
        for (int i = 0; i < ch->mode_per_count; i++)
            if (ch->mode_per[i].freedv_mode == freedv_mode)
            {
                per = ch->mode_per[i].per;
                break;
            }
    }
    else if (ch->cliff_enabled && ch->snr_db < mode_cliff_db(freedv_mode))
        per = SIM_CLIFF_PER;
    bool erased = sim_channel_next_rand(ch) < per;
    uint32_t air = sim_channel_airtime_ms(freedv_mode, frame_size);
    if (freedv_mode >= 0 && freedv_mode < SIM_STAT_MODES) {
        ch->stat_sent[freedv_mode]++;
        ch->stat_ok[freedv_mode] += !erased;
        ch->stat_air_ms[freedv_mode] += air;
    }
    if (erased)
        return false;
    *deliver_at_ms = now_ms + air + ch->guard_ms;
    return true;
}
