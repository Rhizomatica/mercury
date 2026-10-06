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
 * fixed_s is the best single rung held throughout: the per-second oracle
 * follows 0.5 Hz fades no round can, so on fading channels the target is
 * between the two.
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

static double rung_goodput(struct sim_channel *ch, int dir, int t, int lv)
{
    int bpf, n; uint64_t air, ovh;
    car_rung_geometry(lv, true, &bpf, &n, &air, &ovh);
    double per = sim_channel_frame_per(ch, (uint64_t)t * 1000, dir, car_level_mode(lv));
    if (!g_measured_per && !sim_channel_syncable(ch, (uint64_t)t * 1000, dir, car_level_mode(lv)))
        per = 1.0;
    return (double)bpf * n * (1.0 - per) / ((double)air + (double)ovh) * 1000.0;
}

/* *fixed gets the goodput of the best single rung held all session: what a
 * sender that cannot follow the fades (a round is longer than a fade) could
 * still get, the target where the per-second oracle is out of reach. */
static double oracle(struct sim_channel *ch, int dir, int horizon_s, int *rung_s, double *fixed, int *fixed_lv)
{
    double sum = 0.0, per_lv[CAR_NLEVELS] = {0};
    for (int t = 0; t < horizon_s; t++) {
        double best = 0.0; int bl = -1;
        for (int lv = 0; lv < CAR_NLEVELS; lv++) {
            double g = rung_goodput(ch, dir, t, lv);
            per_lv[lv] += g;
            if (g > best) { best = g; bl = lv; }
        }
        sum += best;
        if (bl >= 0 && rung_s) rung_s[bl]++;
    }
    *fixed = 0.0; *fixed_lv = 0;
    if (getenv("BOUND_RUNGS"))          /* each rung held throughout */
        for (int lv = 0; lv < CAR_NLEVELS; lv++)
            fprintf(stderr, "  dir %d lv %d: %.1f B/s\n", dir, lv, per_lv[lv] / horizon_s);
    for (int lv = 0; lv < CAR_NLEVELS; lv++)
        if (per_lv[lv] / horizon_s > *fixed) { *fixed = per_lv[lv] / horizon_s; *fixed_lv = lv; }
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
    double f0, f1 = 0.0; int fl0, fl1 = 0;
    double g0 = oracle(ch, 0, horizon, rs[0], &f0, &fl0);
    double g1 = bidir ? oracle(ch, 1, horizon, rs[1], &f1, &fl1) : 0.0;
    double t = (double)CAR_SIM_BYTES / g0 + (bidir ? (double)CAR_SIM_BYTES / g1 : 0.0);
    double tf = (double)CAR_SIM_BYTES / f0 + (bidir ? (double)CAR_SIM_BYTES / f1 : 0.0);
    printf("chan=%s %s bound_s=%.0f fixed_s=%.0f (lv %d/%d) goodput_Bps=%.1f/%.1f rungs:", argv[1],
           bidir ? "bidir" : "oneway", t, tf, fl0, fl1, g0, g1);
    for (int lv = 0; lv < CAR_NLEVELS; lv++) if (rs[0][lv]) printf(" %d:%d%%", lv, 100 * rs[0][lv] / horizon);
    printf("\n");
    return 0;
}
