/* tests/sim/test_carousel.c -- the carousel data plane's invariants on the
 * sim's channel: every run completes, delivers exactly what was sent, and
 * never keys over the peer.
 *
 * Timing is not asserted: tests/sim/README.md has the throughput matrix, run
 * with carousel_bench.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "carousel_sim.h"

#include <stdio.h>
#include <stdlib.h>

#define LIMIT_MS (8ULL * 3600 * 1000)

void setUp(void) {}
/* Here, not at the end of a test: a failed assertion leaves the test at once,
 * and the next one would run with its sensing. */
void tearDown(void) { unsetenv("CAR_CS_DECODABLE"); }

static void check(uint64_t seed, const char *chan, bool bidir)
{
    car_sim_result_t r;
    char what[64];
    carousel_sim_run(seed, chan, bidir, LIMIT_MS, &r);
    snprintf(what, sizeof(what), "seed %llu %s %s", (unsigned long long)seed, chan, bidir ? "bidir" : "oneway");
    TEST_ASSERT_TRUE_MESSAGE(r.intact, what);
    TEST_ASSERT_FALSE_MESSAGE(r.stalled, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.collisions, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.bad_frames, what);
    TEST_ASSERT_EQUAL_MESSAGE(CAR_SIM_BYTES, r.a2b, what);
    TEST_ASSERT_EQUAL_MESSAGE(bidir ? CAR_SIM_BYTES : 0, r.b2a, what);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, r.done_ms, what);
}

static const char *CHANNELS[] = {
    "clean", "awgn:0.10", "awgn:0.25", "cliff:3", "cliff:10", "nvis",
    "fade:3:0.5", "fade:8:1.0", "fade:15:0.1", "fade:25:1.0",
    /* the MFSK floor, answered by patterns */
    "cliff:-9", "cliff:-11", "fade:-9:0.5",
};

static void test_carousel_bidir_completes_intact(void)
{
    for (size_t c = 0; c < sizeof(CHANNELS) / sizeof(CHANNELS[0]); c++)
        for (uint64_t seed = 1; seed <= 10; seed++)
            check(seed, CHANNELS[c], true);
}

static void test_carousel_oneway_completes_intact(void)
{
    for (size_t c = 0; c < sizeof(CHANNELS) / sizeof(CHANNELS[0]); c++)
        for (uint64_t seed = 1; seed <= 5; seed++)
            check(seed, CHANNELS[c], false);
}

/* A link one way below the control mode: its receiver's control frames go on
 * MFSK, and each end learns which way from the other's control-deaf report.
 * With carrier sense only on what the listener can sync on (as on air), the
 * control-mode-only plane keyed over itself on these -- asym:14:-10 26
 * collisions, asym:-9:3 465 and 2 of 8 unfinished, cliff:-9 740 -- and on air
 * a gateway at 2 % polled a deaf estacao2 for minutes. */
static void test_carousel_floor_control_plane(void)
{
    static const char *ASYM[] = { "asym:14:-10", "asym:-10:14", "asym:-9:3", "cliff:-9",
                                  /* a marginal probe rung the listener cannot
                                   * sync on: no quick re-poll over it */
                                  "cliff:-5", "asym:-6:14" };
    setenv("CAR_CS_DECODABLE", "1", 1);
    for (size_t c = 0; c < sizeof(ASYM) / sizeof(ASYM[0]); c++)
        for (uint64_t seed = 1; seed <= 4; seed++)
            check(seed, ASYM[c], true);
    unsetenv("CAR_CS_DECODABLE");
}

/* A poll off a floor stream, lost: the sender carries on at the floor, and a
 * receiver bound to the rung it asked for neither hears nor senses it.  Its
 * re-poll keyed into that round -- on air (car23, 3 %) 3.7 s into the
 * gateway's 27 s MFSK; here 2-7 collisions on the fade:-5 seeds below and on
 * these asym:-6:14 bidir seeds -- until it listened at the floor first.
 * (fade:-5 seeds 1, 3 and 7 keep one collision each of another kind: the
 * sender's 90 s silence nudge, after two polls lost in a fade, landing on the
 * receiver's third, which it cannot sense either.) */
static void test_carousel_lost_probe_poll(void)
{
    static const uint64_t SEEDS[] = { 2, 4, 5, 6 };
    setenv("CAR_CS_DECODABLE", "1", 1);
    for (size_t i = 0; i < sizeof(SEEDS) / sizeof(SEEDS[0]); i++)
        check(SEEDS[i], "fade:-5:0.5", false);
    check(8, "asym:-6:14", true);
    check(11, "asym:-6:14", true);
    unsetenv("CAR_CS_DECODABLE");
}

/* Under fading a round on the air is not always synced on, so not sensed:
 * taking silence at the sense check for a lost poll, the quick re-poll keyed
 * into rounds -- 226 collisions in 20 one-way fade:3 runs, 178 on nvis --
 * where waiting for the window keys over none. */
static void test_carousel_fading_no_blind_repoll(void)
{
    static const char *FADE[] = { "fade:3:0.5", "fade:8:1.0", "nvis" };
    setenv("CAR_CS_DECODABLE", "1", 1);
    for (size_t c = 0; c < sizeof(FADE) / sizeof(FADE[0]); c++)
        for (uint64_t seed = 1; seed <= 4; seed++)
            check(seed, FADE[c], false);
    for (uint64_t seed = 1; seed <= 2; seed++) {
        check(seed, "fade:3:0.5", true);
        check(seed, "fade:8:1.0", true);
    }
    unsetenv("CAR_CS_DECODABLE");
}

/* Both ways at the deep floor, under fading: the receiver takes the turn
 * with a handover on MFSK after its turn quantum, in place of a pattern, and
 * the sender, continuing at the pattern delay, keyed into it -- 19, 12 and
 * 11 collisions on these seeds -- until it waited for the handover once the
 * quantum was near. */
static void test_carousel_floor_handover_not_keyed_over(void)
{
    setenv("CAR_CS_DECODABLE", "1", 1);
    for (uint64_t seed = 1; seed <= 3; seed++)
        check(seed, "fade:-9:0.5", true);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_carousel_bidir_completes_intact);
    RUN_TEST(test_carousel_oneway_completes_intact);
    RUN_TEST(test_carousel_floor_control_plane);
    RUN_TEST(test_carousel_lost_probe_poll);
    RUN_TEST(test_carousel_fading_no_blind_repoll);
    RUN_TEST(test_carousel_floor_handover_not_keyed_over);
    return UNITY_END();
}
