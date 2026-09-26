/* tests/sim/carousel_bench.c -- the carousel data plane on the sim's channel,
 * to compare with the stop-and-wait ARQ (ab_bench).
 *
 *   carousel_bench <seed> <channel> [bidir]      (channel as in ab_bench)
 *
 *   CAR_TRACE=1  print every keydown
 *   SIM_STATS=1  frames and airtime per mode
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "carousel_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LIMIT_MS
#define LIMIT_MS        (30ULL * 60 * 1000)
#endif

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <seed> <channel> [bidir]\n", argv[0]); return 1; }
    uint64_t seed = (uint64_t)atoll(argv[1]);
    bool bidir = argc > 3 && !strcmp(argv[3], "bidir");
    car_sim_result_t r;
    carousel_sim_run(seed, argv[2], bidir, LIMIT_MS, &r);
    printf("seed=%llu chan=%s %s a2b=%zu b2a=%zu integrity=%s done_ms=%llu collisions=%d%s\n",
           (unsigned long long)seed, argv[2], bidir ? "bidir" : "oneway", r.a2b, r.b2a,
           r.intact ? "OK" : "CORRUPT", (unsigned long long)r.done_ms, r.collisions,
           r.stalled ? " STALLED" : "");
    return r.intact ? 0 : 2;
}
