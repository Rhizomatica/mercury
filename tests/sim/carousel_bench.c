/* tests/sim/carousel_bench.c -- the carousel data plane alone on the sim's
 * channel (ab_bench runs it under the full FSM).
 *
 *   carousel_bench <seed> <channel> [bidir]      (channel as in ab_bench)
 *
 *   CAR_TRACE=1  print every keydown
 *   SILENCE_S=n  arq:silence_fallback_s (default 150)
 *   SIM_STATS=1  frames and airtime per mode
 *   CAR_CHAT, CAR_SCRIPT, CAR_KBD, CAR_PACE   application traffic (carousel_sim.c);
 *                each message's delay, written to delivered, is reported
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "carousel_sim.h"
#include "arq_protocol.h"

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
    /* SILENCE_S: arq:silence_fallback_s for this run (#442). */
    if (getenv("SILENCE_S")) arq_protocol_set_silence_fallback_s(atoi(getenv("SILENCE_S")));
    /* CAR_LIMIT_S: a longer run than the default (the deep fringe is slow). */
    uint64_t limit = getenv("CAR_LIMIT_S") ? (uint64_t)atoll(getenv("CAR_LIMIT_S")) * 1000 : LIMIT_MS;
    carousel_sim_run(seed, argv[2], bidir, limit, &r);
    printf("seed=%llu chan=%s %s a2b=%zu b2a=%zu integrity=%s done_ms=%llu collisions=%d overruns=%d%s\n",
           (unsigned long long)seed, argv[2], bidir ? "bidir" : "oneway", r.a2b, r.b2a,
           r.intact ? "OK" : "CORRUPT", (unsigned long long)r.done_ms, r.collisions, r.overruns,
           r.stalled ? " STALLED" : "");
    if (r.msgs) {
        /* delays in order, mean, the slowest, and the air both stations used */
        uint64_t d[CAR_SIM_MSGS], sum = 0;
        int n = 0;
        for (int k = 0; k < r.msgs; k++) if (r.msg_delay_ms[k] != UINT64_MAX) { d[n++] = r.msg_delay_ms[k]; sum += d[n - 1]; }
        for (int i = 1; i < n; i++) for (int j = i; j > 0 && d[j] < d[j - 1]; j--) { uint64_t x = d[j]; d[j] = d[j - 1]; d[j - 1] = x; }
        printf("msgs=%d delivered=%d delay_mean=%.1f delay_p90=%.1f delay_max=%.1f air=%.1f+%.1f\n", r.msgs, n,
               n ? sum / 1000.0 / n : 0.0, n ? d[(n * 9 + 9) / 10 - 1] / 1000.0 : 0.0, n ? d[n - 1] / 1000.0 : 0.0,
               r.air_ms[0] / 1000.0, r.air_ms[1] / 1000.0);
    }
    return r.intact ? 0 : 2;
}
