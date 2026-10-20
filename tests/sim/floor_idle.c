/* tests/sim/floor_idle.c -- an idle session on the carousel alone
 * (carousel_sim.c, with patterns and so the floor's continuation slots), with
 * a share of the frames lost.  It counts the runs in which a station heard
 * nothing from its peer for 240 s or more: the runs the FSM's lost-peer
 * watchdog (ARQ_CAR_PEER_LOST_MS) would end.  This sim has no FSM, so nothing
 * ends here.  A bench, not part of `make test`; built by
 * `make -C tests floor_idle`.
 *
 *   floor_idle <from_seed> <to_seed> <channel>
 *
 * The channel is as for carousel_sim_run, usually cliff:<snr>:<per>.  It
 * prints the runs, the runs silent 240 s at each station, the keydowns and
 * air time of both stations, and the collisions, overruns and keydown
 * overlaps.  For an idle session, A sends 30 bytes and B answers 30 min later:
 *
 *   CAR_CS_DECODABLE=1 CAR_SCRIPT='A30,B30+1800000' ./floor_idle 1 100 cliff:-9:0.2
 *
 * Copyright (C) 2026 Joseph Freivald
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "carousel_sim.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: floor_idle <from seed> <to seed> <channel>\n"); return 2; }
    int runs = 0, silent[2] = { 0, 0 }, collisions = 0, overruns = 0, overlaps = 0;
    long keydowns = 0;
    double air_s = 0;
    for (uint64_t seed = (uint64_t)atoll(argv[1]); seed <= (uint64_t)atoll(argv[2]); seed++) {
        car_sim_result_t r;
        carousel_sim_run(seed, argv[3], false, 4000000, &r);
        runs++;
        for (int i = 0; i < 2; i++) silent[i] += r.silent_ms[i] >= 240000;
        collisions += r.collisions; overruns += r.overruns; overlaps += r.kd_overlaps;
        keydowns += r.keydowns;
        air_s += (r.air_ms[0] + r.air_ms[1]) / 1000.0;
    }
    printf("%s runs=%d silent>=240s: A %d, B %d; keydowns=%ld air=%.0f s collisions=%d overruns=%d kd_overlaps=%d\n",
           argv[3], runs, silent[0], silent[1], keydowns, air_s, collisions, overruns, overlaps);
    return 0;
}
