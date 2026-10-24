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
#define CAR_SIM_MSGS 64           /* a traffic script's messages (carousel_sim.c) */

typedef struct {
    size_t   a2b, b2a;            /* bytes delivered each way */
    bool     intact;              /* what was delivered is a prefix of what was sent */
    uint64_t done_ms;             /* both complete (0: not within the limit) */
    int      collisions;
    int      bad_frames;   /* keydown frames the modem would refuse (length != mode payload) */
    int      overruns;            /* keydowns the turn rules forbid: M's longer than its slot check
                                     allowed (car_overruns), S's outside a slot M's log holds */
    bool     stalled;             /* nothing left to happen, data undelivered */
    int      frames;              /* frames and patterns keyed, both ways */
    int      non_mfsk[2];         /* ...frames each station keyed on another mode than MFSK
                                     (control included; CAR_MFSK_TX) */
    int      keydowns;            /* keydowns, both stations */
    int      kd_overlaps;         /* keydowns that started while the peer was on the air */
    int      kd_unexplained;      /* ... over a keydown of the peer that could be sensed */
    const char *failed;           /* a station failed the session (car_failed), or NULL */
    uint64_t air_ms[2];           /* each station on the air */
    uint64_t silent_ms[2];        /* each station's longest time hearing nothing of its peer, frame
                                     or pattern, to the end of the run: what the FSM's lost-peer
                                     watchdog counts (240 s ends a session) */
    int      msgs;                /* a traffic script's messages (0: a transfer) */
    uint64_t msg_delay_ms[CAR_SIM_MSGS];   /* ...each from written to delivered (UINT64_MAX: never) */
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
 * ab_bench (clean | awgn:<per> | cliff:<snr> | nvis | fade:<snr>:<hz>), or
 * asym:<a2b>:<b2a>, step:<from>:<to>:<at_s>, stepa:<from>:<to>:<at_s>:<d> (a
 * step from A to B, the way back d dB better throughout); cliff:<snr>:<per>
 * loses that share of the frames above the cliff too. */
void carousel_sim_run(uint64_t seed, const char *chan, bool bidir, uint64_t limit_ms, car_sim_result_t *res);

/* The channel such a run uses (fresh, seeded): for bounds. */
struct sim_channel;
struct sim_channel *carousel_sim_channel(uint64_t seed, const char *chan, double *snr_out);

#endif
