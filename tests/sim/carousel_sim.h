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

#define CAR_SIM_BYTES 8192        /* each direction */

typedef struct {
    size_t   a2b, b2a;            /* bytes delivered each way */
    bool     intact;              /* what was delivered is a prefix of what was sent */
    uint64_t done_ms;             /* both complete (0: not within the limit) */
    int      collisions;
    bool     stalled;             /* nothing left to happen, data undelivered */
} car_sim_result_t;

/* A connected session, A sending first, B too when bidir; chan as in
 * ab_bench (clean | awgn:<per> | cliff:<snr> | nvis | fade:<snr>:<hz>). */
void carousel_sim_run(uint64_t seed, const char *chan, bool bidir, uint64_t limit_ms, car_sim_result_t *res);

#endif
