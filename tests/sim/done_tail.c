/* tests/sim/done_tail.c -- how long a sender goes on after its receiver has
 * everything, before it learns so (a bench, not run by `make test`).
 *
 *   done_tail <seed_from> <seed_to> <channel>
 *
 * A one-way transfer, run on past the receiver's completion (CAR_CONFIRM)
 * until the sender's blocks are all retired.  The "tail" is the time between
 * the two: what a host reading BUFFER, or an application waiting for the
 * transfer to end, spends with the data already delivered.  At the floor the
 * receiver's "done" is a poll on the control mode; on a return path where
 * DATAC16 is often lost (ctlloss:A:B:P) the sender misses it and keeps
 * sending floor rounds of data the receiver already has.  On air, VE4KLM ->
 * K7EK 15 Nov 2026: K7EK had all 753 bytes at 23:04:35 UTC, its two "done"
 * polls (23:04:37, 23:05:14) were not decoded, and VE4KLM sent a 27 s round
 * more before its host gave up.
 *
 * Per seed: the receiver's completion, the sender's, the tail, the "done"
 * polls keyed and heard, and the floor rounds the sender continued on
 * silence, and the sender's longest time hearing nothing of the receiver
 * (the FSM's lost-peer watchdog ends a session at 240 s).  Then the tail's
 * median, 90th percentile and maximum.
 *
 *   CAR_SCRIPT   the traffic (default A753: one 753-byte message, as on air)
 *   CAR_LIMIT_S  the run's limit (default 1800)
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "carousel_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s <seed_from> <seed_to> <channel>\n", argv[0]); return 1; }
    uint64_t from = (uint64_t)atoll(argv[1]), to = (uint64_t)atoll(argv[2]);
    const char *chan = argv[3];
    if (to < from || to - from >= 1000) { fprintf(stderr, "at most 1000 seeds\n"); return 1; }
    setenv("CAR_CONFIRM", "1", 1);
    if (!getenv("CAR_SCRIPT")) setenv("CAR_SCRIPT", "A753", 1);
    uint64_t limit = getenv("CAR_LIMIT_S") ? (uint64_t)atoll(getenv("CAR_LIMIT_S")) * 1000 : 1800000ULL;

    static uint64_t tails[1000];
    int n = 0, unconfirmed = 0, bad = 0;
    long polls = 0, heard = 0;
    uint64_t worst_silent = 0;
    int over_watchdog = 0;   /* the sender heard nothing for the FSM's 240 s */
    for (uint64_t seed = from; seed <= to; seed++) {
        car_sim_result_t r;
        carousel_sim_run(seed, chan, false, limit, &r);
        if (!r.intact || !r.done_ms) { bad++; printf("seed=%llu %s\n", (unsigned long long)seed, r.intact ? "UNFINISHED" : "CORRUPT"); continue; }
        polls += r.done_polls; heard += r.done_heard;
        if (!r.confirm_ms) {
            unconfirmed++;
            printf("seed=%llu done=%.1f s confirm=never done_polls=%d heard=%d continues=%d\n",
                   (unsigned long long)seed, r.done_ms / 1000.0, r.done_polls, r.done_heard, r.floor_continues);
            continue;
        }
        uint64_t tail = r.confirm_ms - r.done_ms;
        tails[n++] = tail;
        if (r.silent_ms[0] > worst_silent) worst_silent = r.silent_ms[0];
        if (r.silent_ms[0] >= 240000) over_watchdog++;
        printf("seed=%llu done=%.1f s confirm=%.1f s tail=%.1f s done_polls=%d heard=%d continues=%d sender_silent=%.1f s\n",
               (unsigned long long)seed, r.done_ms / 1000.0, r.confirm_ms / 1000.0, tail / 1000.0,
               r.done_polls, r.done_heard, r.floor_continues, r.silent_ms[0] / 1000.0);
    }
    qsort(tails, (size_t)n, sizeof(tails[0]), cmp_u64);
    printf("chan=%s runs=%d tail_median=%.1f s tail_p90=%.1f s tail_max=%.1f s unconfirmed=%d bad=%d "
           "done_polls=%ld heard=%ld (%.0f %%) sender_silent_max=%.1f s over_240s=%d\n",
           chan, n, n ? tails[n / 2] / 1000.0 : 0.0, n ? tails[(n * 9 + 9) / 10 - 1] / 1000.0 : 0.0,
           n ? tails[n - 1] / 1000.0 : 0.0, unconfirmed, bad, polls, heard,
           polls ? 100.0 * (double)heard / (double)polls : 0.0, worst_silent / 1000.0, over_watchdog);
    return bad ? 2 : 0;
}
