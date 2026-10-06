/* tests/sim/carousel_sim.h -- two carousel data planes on the sim's channel
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CAROUSEL_SIM_H
#define CAROUSEL_SIM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef CAR_SIM_BYTES
#define CAR_SIM_BYTES 8192        /* each direction */
#endif

typedef struct {
    size_t   a2b, b2a;            /* bytes delivered each way */
    bool     intact;              /* what was delivered is a prefix of what was sent */
    uint64_t done_ms;             /* both complete (0: not within the limit) */
    int      collisions;
    int      bad_frames;   /* keydown frames the modem would refuse (length != mode payload) */
    bool     stalled;             /* nothing left to happen, data undelivered */
    int      frames;              /* frames and patterns keyed, both ways */
    int      keydowns;            /* keydowns, both stations */
    int      kd_overlaps;         /* keydowns that started while the peer was on the air */
    int      kd_unexplained;      /* ... over a keydown of the peer that could be sensed */
    const char *failed;           /* a station failed the session (car_failed), or NULL */
} car_sim_result_t;

/* Exploration: from the next run on, no random loss -- the frames (and
 * patterns) with these indices, counted from 0 in the order they are keyed,
 * are lost and every other one arrives.  n < 0 restores the channel model. */
void carousel_sim_force_losses(const int *frame_idx, int n);
/* Exploration: the keydowns with these indices (counted from 0 in the order
 * they are keyed, both stations) are never sensed by the peer -- a carrier
 * sense failure.  n <= 0: sensing as the model has it. */
void carousel_sim_force_unsensed(const int *keydown_idx, int n);
/* Exploration: each of these keydowns is lost whole, and a BREAK pattern
 * nobody sent reaches the station that keyed it 1.2 s after it ends -- a
 * faded round "answered" by another station or a detector false alarm.
 * n <= 0: none. */
void carousel_sim_force_spurious_break(const int *keydown_idx, int n);
/* Exploration: these frames (indices as for carousel_sim_force_losses) arrive
 * with one byte flipped, as a corrupt frame that passed its CRC would. */
void carousel_sim_force_corrupt(const int *frame_idx, int n);

/* A connected session, A sending first, B too when bidir; chan as in
 * ab_bench (clean | awgn:<per> | cliff:<snr> | nvis | fade:<snr>:<hz>). */
void carousel_sim_run(uint64_t seed, const char *chan, bool bidir, uint64_t limit_ms, car_sim_result_t *res);

/* The channel such a run uses (fresh, seeded): for bounds. */
struct sim_channel;
struct sim_channel *carousel_sim_channel(uint64_t seed, const char *chan, double *snr_out);

#endif
