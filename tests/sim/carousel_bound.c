/* tests/sim/carousel_bound.c -- what the carousel could do on a sim channel,
 * at best: an upper bound on goodput, to measure carousel_bench against.
 *
 *   carousel_bound <channel> [bidir]      (channel as in carousel_bench)
 *
 * An oracle that knows the channel at every second picks, each second, the
 * rung with the most data per second: its bytes per frame times the chance
 * the frame survives, over its airtime, times the share of a full round that
 * is airtime (the poll, the guards and the turnarounds around every round
 * are the carousel's own, car_rung_geometry).  It never probes, never loses a
 * poll, never waits on a timer.  Time = bytes / that goodput, each way.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "carousel_sim.h"
#include "sim_channel.h"
#include "carousel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CAR_SIM_BYTES
#define CAR_SIM_BYTES 8192
#endif

/* The oracle's goodput in direction dir, bytes per second, over horizon_s;
 * *rung_s gets the seconds it spent on each rung. */
/* Below its cliff the sim still lets 10 % of a mode's frames through
 * (SIM_CLIFF_PER); no real link does, and the carousel's SNR gate rightly
 * never sends there, so the oracle counts them lost.  NVIS is the exception:
 * its per-mode losses are measured ones. */
static bool g_measured_per;

static double oracle(struct sim_channel *ch, int dir, int horizon_s, int *rung_s)
{
    double sum = 0.0;
    for (int t = 0; t < horizon_s; t++) {
        double best = 0.0; int bl = -1;
        for (int lv = 0; lv < CAR_NLEVELS; lv++) {
            int bpf, n; uint64_t air, ovh;
            car_rung_geometry(lv, true, &bpf, &n, &air, &ovh);
            double per = sim_channel_frame_per(ch, (uint64_t)t * 1000, dir, car_level_mode(lv));
            if (!g_measured_per && !sim_channel_syncable(ch, (uint64_t)t * 1000, dir, car_level_mode(lv)))
                per = 1.0;
            double g = (double)bpf * n * (1.0 - per) / ((double)air + (double)ovh) * 1000.0;
            if (g > best) { best = g; bl = lv; }
        }
        sum += best;
        if (bl >= 0 && rung_s) rung_s[bl]++;
    }
    return sum / horizon_s;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <channel> [bidir]\n", argv[0]); return 1; }
    bool bidir = argc > 2 && !strcmp(argv[2], "bidir");
    int horizon = getenv("BOUND_HORIZON_S") ? atoi(getenv("BOUND_HORIZON_S")) : 4 * 3600;
    struct sim_channel *ch = carousel_sim_channel(1, argv[1], NULL);
    g_measured_per = !strcmp(argv[1], "nvis");
    int rs[2][CAR_NLEVELS] = {{0}};
    double g0 = oracle(ch, 0, horizon, rs[0]), g1 = bidir ? oracle(ch, 1, horizon, rs[1]) : 0.0;
    double t = (double)CAR_SIM_BYTES / g0 + (bidir ? (double)CAR_SIM_BYTES / g1 : 0.0);
    printf("chan=%s %s bound_s=%.0f goodput_Bps=%.1f/%.1f rungs:", argv[1], bidir ? "bidir" : "oneway",
           t, g0, g1);
    for (int lv = 0; lv < CAR_NLEVELS; lv++) if (rs[0][lv]) printf(" %d:%d%%", lv, 100 * rs[0][lv] / horizon);
    printf("\n");
    return 0;
}
