/* tests/sim/car_explore.c -- the carousel under every loss pattern and every
 * carrier-sense failure, up to k of them
 *
 * A random channel samples loss patterns; this enumerates them.  The session
 * is run once clean to count its frames, then once for every set of at most
 * k lost frames among them (the frames counted in the order they are keyed,
 * both ways, patterns included), on the real carousel.c, with carrier sense
 * that always works.  Every run must:
 *   - deliver an exact prefix of what was sent, and all of it (integrity and
 *     completion, within the limit);
 *   - never key over the peer (with sensing that works, an overlap is a
 *     protocol fault, not bad luck);
 *   - never key a frame the modem would refuse.
 * A failure prints the loss set, which replays it: CAR_TRACE=1
 * test_car_explore replay <bidir> i j ... traces one.
 *
 * Built with a small transfer (CAR_SIM_BYTES) so that runs are short.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "carousel_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT_MS (4ULL * 3600 * 1000)

static int failures, runs;

static void check(bool bidir, const int *idx, int n, const char *chan)
{
    car_sim_result_t r;
    carousel_sim_force_losses(idx, n);
    carousel_sim_run(1, chan, bidir, LIMIT_MS, &r);
    runs++;
    bool ok = r.intact && r.done_ms && r.collisions == 0 && r.bad_frames == 0 &&
              r.a2b == CAR_SIM_BYTES && r.b2a == (bidir ? CAR_SIM_BYTES : 0);
    if (!ok) {
        failures++;
        if (failures <= 20) {
            printf("FAIL %s%s lost {", chan, bidir ? " bidir" : "");
            for (int i = 0; i < n; i++) printf("%s%d", i ? "," : "", idx[i]);
            printf("}: intact=%d done=%llu a2b=%zu b2a=%zu collisions=%d bad=%d stalled=%d\n",
                   r.intact, (unsigned long long)r.done_ms, r.a2b, r.b2a, r.collisions,
                   r.bad_frames, r.stalled);
        }
    }
}

/* Every subset of {0..N-1} of size <= k, in lexicographic order. */
static void subsets(bool bidir, int N, int k, const char *chan)
{
    int idx[8];
    check(bidir, idx, 0, chan);
    for (int size = 1; size <= k && size <= N; size++) {
        for (int i = 0; i < size; i++) idx[i] = i;
        for (;;) {
            check(bidir, idx, size, chan);
            int p = size - 1;
            while (p >= 0 && idx[p] == N - size + p) p--;
            if (p < 0) break;
            idx[p]++;
            for (int q = p + 1; q < size; q++) idx[q] = idx[q - 1] + 1;
        }
    }
}

/* Carrier sense failures: every set of at most k keydowns the peer does not
 * sense.  The data must still arrive whole.  Keying over an unsensed keydown
 * is expected -- more than once if it is long, since blind timers keep
 * running under it -- but a failure must not cascade: an overlap with a
 * keydown that could be sensed means the protocol kept keying over the peer
 * after the event. */
static void explore_cs(bool bidir, int k, const char *chan)
{
    car_sim_result_t r;
    carousel_sim_force_losses(NULL, -1);
    carousel_sim_force_unsensed(NULL, 0);
    carousel_sim_force_losses(NULL, 0);           /* no random loss */
    carousel_sim_run(1, chan, bidir, LIMIT_MS, &r);
    int N = r.keydowns, idx[8], nruns = 0, with = 0, cascade = 0, worst = 0, bad = 0;
    for (int size = 1; size <= k && size <= N; size++) {
        for (int i = 0; i < size; i++) idx[i] = i;
        for (;;) {
            carousel_sim_force_unsensed(idx, size);
            carousel_sim_run(1, chan, bidir, LIMIT_MS, &r);
            nruns++; runs++;
            bool ok = r.intact && r.done_ms && r.bad_frames == 0 &&
                      r.a2b == CAR_SIM_BYTES && r.b2a == (bidir ? CAR_SIM_BYTES : 0);
            if (!ok) { bad++; failures++; }
            if (r.kd_overlaps) with++;
            if (r.kd_unexplained) {
                cascade++; failures++;
                if (cascade <= 10) {
                    printf("CASCADE %s%s unsensed {", chan, bidir ? " bidir" : "");
                    for (int i = 0; i < size; i++) printf("%s%d", i ? "," : "", idx[i]);
                    printf("}: %d overlapping keydowns, %d over a sensed one\n",
                           r.kd_overlaps, r.kd_unexplained);
                }
            }
            if (!ok && bad <= 10) {
                printf("FAIL %s%s unsensed {", chan, bidir ? " bidir" : "");
                for (int i = 0; i < size; i++) printf("%s%d", i ? "," : "", idx[i]);
                printf("}: intact=%d done=%llu a2b=%zu b2a=%zu\n", r.intact,
                       (unsigned long long)r.done_ms, r.a2b, r.b2a);
            }
            if (r.kd_overlaps > worst) worst = r.kd_overlaps;
            int p = size - 1;
            while (p >= 0 && idx[p] == N - size + p) p--;
            if (p < 0) break;
            idx[p]++;
            for (int q = p + 1; q < size; q++) idx[q] = idx[q - 1] + 1;
        }
    }
    carousel_sim_force_unsensed(NULL, 0);
    printf("%-9s %-6s keydowns=%d unsensed<=%d: %d runs, %d with an overlap, worst %d, %d cascading, %d failed\n",
           chan, bidir ? "bidir" : "oneway", N, k, nruns, with, worst, cascade, bad);
    fflush(stdout);
}

/* Spurious BREAKs: after every set of at most k keydowns, a BREAK nobody sent
 * reaches the station that keyed it -- a pattern detector's false alarm, or
 * another station's pattern (patterns carry no session).  The data must still
 * arrive whole, and with sensing that works nobody keys over anybody. */
static void explore_brk(bool bidir, int k, const char *chan)
{
    car_sim_result_t r;
    carousel_sim_force_unsensed(NULL, 0);
    carousel_sim_force_spurious_break(NULL, 0);
    carousel_sim_force_losses(NULL, 0);           /* no random loss */
    carousel_sim_run(1, chan, bidir, LIMIT_MS, &r);
    int N = r.keydowns, idx[8], nruns = 0, bad = 0;
    for (int size = 1; size <= k && size <= N; size++) {
        for (int i = 0; i < size; i++) idx[i] = i;
        for (;;) {
            carousel_sim_force_spurious_break(idx, size);
            carousel_sim_run(1, chan, bidir, LIMIT_MS, &r);
            nruns++; runs++;
            bool ok = r.intact && r.done_ms && r.collisions == 0 && r.bad_frames == 0 &&
                      r.a2b == CAR_SIM_BYTES && r.b2a == (bidir ? CAR_SIM_BYTES : 0);
            if (!ok) {
                bad++; failures++;
                if (bad <= 10) {
                    printf("FAIL %s%s spurious BREAK after {", chan, bidir ? " bidir" : "");
                    for (int i = 0; i < size; i++) printf("%s%d", i ? "," : "", idx[i]);
                    printf("}: intact=%d done=%llu a2b=%zu b2a=%zu collisions=%d stalled=%d\n", r.intact,
                           (unsigned long long)r.done_ms, r.a2b, r.b2a, r.collisions, r.stalled);
                }
            }
            int p = size - 1;
            while (p >= 0 && idx[p] == N - size + p) p--;
            if (p < 0) break;
            idx[p]++;
            for (int q = p + 1; q < size; q++) idx[q] = idx[q - 1] + 1;
        }
    }
    carousel_sim_force_spurious_break(NULL, 0);
    printf("%-9s %-6s keydowns=%d spurious<=%d: %d runs, %d failed\n",
           chan, bidir ? "bidir" : "oneway", N, k, nruns, bad);
    fflush(stdout);
}

static const char *chan_list(void)
{
    /* A fast rung, the middle, the DATAC15/MFSK boundary, the MFSK floor;
     * CAR_EXPLORE_CHANS overrides. */
    return getenv("CAR_EXPLORE_CHANS") ? getenv("CAR_EXPLORE_CHANS") : "cliff:20 cliff:3 cliff:-5 cliff:-9";
}

static void explore_losses(int only_bidir, int k)
{
    if (k > 8) k = 8;
    for (int bidir = 0; bidir <= 1; bidir++) {
        if (only_bidir >= 0 && only_bidir != bidir) continue;
        char list[256];
        snprintf(list, sizeof list, "%s", chan_list());   /* strtok consumes it */
        for (char *t = strtok(list, " "); t; t = strtok(NULL, " ")) {
            car_sim_result_t r;
            carousel_sim_force_losses(NULL, 0);
            carousel_sim_run(1, t, bidir, LIMIT_MS, &r);
            int before = failures, before_runs = runs;
            subsets(bidir, r.frames, k, t);
            printf("%-9s %-6s frames=%d k<=%d: %d runs, %d failures\n", t, bidir ? "bidir" : "oneway",
                   r.frames, k, runs - before_runs, failures - before);
            fflush(stdout);
        }
    }
}

static void explore_spurious(int k)
{
    if (k > 8) k = 8;
    for (int bidir = 0; bidir <= 1; bidir++) {
        char list[256];
        snprintf(list, sizeof list, "%s", chan_list());
        for (char *t = strtok(list, " "); t; t = strtok(NULL, " ")) explore_brk(bidir, k, t);
    }
}

static void explore_sensing(int k)
{
    if (k > 8) k = 8;
    for (int bidir = 0; bidir <= 1; bidir++) {
        char list[256];
        snprintf(list, sizeof list, "%s", chan_list());
        for (char *t = strtok(list, " "); t; t = strtok(NULL, " ")) explore_cs(bidir, k, t);
    }
}

/* No arguments (make test): every set of up to 3 lost frames, of up to 3
 * unsensed keydowns and of single spurious BREAKs, a second or two at the
 * default transfer size.
 *   all|0|1 <k>       lost frames, both ways / one way / both directions
 *   cs <k>            unsensed keydowns
 *   brk <k>           spurious BREAKs
 *   replay <bidir> i...            one loss set (CAR_EXPLORE_CHAN, CAR_TRACE=1)
 *   csreplay <bidir> <chan> kd...  one sensing-failure set
 *   brkreplay <bidir> <chan> kd... one spurious-BREAK set */
int main(int argc, char **argv)
{
    if (argc == 1) {
        explore_losses(-1, 3);
        explore_sensing(3);
        explore_spurious(1);
        printf("%d runs, %d failures\n", runs, failures);
        return failures != 0;
    }
    if (argc > 3 && !strcmp(argv[1], "csreplay")) {
        int idx[64], n = 0;
        for (int i = 4; i < argc && n < 64; i++) idx[n++] = atoi(argv[i]);
        car_sim_result_t r;
        carousel_sim_force_losses(NULL, 0);
        carousel_sim_force_unsensed(idx, n);
        carousel_sim_run(1, argv[3], atoi(argv[2]) != 0, LIMIT_MS, &r);
        printf("collisions=%d overlapping keydowns=%d (over a sensed one %d) intact=%d done=%llu\n",
               r.collisions, r.kd_overlaps, r.kd_unexplained, r.intact, (unsigned long long)r.done_ms);
        return 0;
    }
    if (!strcmp(argv[1], "brk")) {
        explore_spurious(argc > 2 ? atoi(argv[2]) : 1);
        return failures != 0;
    }
    if (argc > 3 && !strcmp(argv[1], "brkreplay")) {    /* brkreplay <bidir> <chan> kd... */
        int idx[64], n = 0;
        for (int i = 4; i < argc && n < 64; i++) idx[n++] = atoi(argv[i]);
        car_sim_result_t r;
        carousel_sim_force_losses(NULL, 0);
        carousel_sim_force_spurious_break(idx, n);
        carousel_sim_run(1, argv[3], atoi(argv[2]) != 0, LIMIT_MS, &r);
        printf("intact=%d done=%llu a2b=%zu b2a=%zu collisions=%d\n", r.intact,
               (unsigned long long)r.done_ms, r.a2b, r.b2a, r.collisions);
        return 0;
    }
    if (!strcmp(argv[1], "cs")) {
        explore_sensing(argc > 2 ? atoi(argv[2]) : 1);
        return failures != 0;
    }
    if (!strcmp(argv[1], "replay")) {
        bool bidir = argc > 2 && atoi(argv[2]);
        int idx[64], n = 0;
        for (int i = 3; i < argc && n < 64; i++) idx[n++] = atoi(argv[i]);
        check(bidir, idx, n, getenv("CAR_EXPLORE_CHAN") ? getenv("CAR_EXPLORE_CHAN") : "cliff:20");
        printf("%s\n", failures ? "FAILED" : "ok");
        return failures != 0;
    }
    explore_losses(strcmp(argv[1], "all") ? atoi(argv[1]) : -1, argc > 2 ? atoi(argv[2]) : 2);
    return failures != 0;
}
