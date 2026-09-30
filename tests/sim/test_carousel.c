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
void tearDown(void) {}

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
 * gateway's 27 s MFSK; here 2-4 collisions on every fade:-5 seed below and on
 * these asym:-6:14 bidir seeds -- until it listened at the floor first. */
static void test_carousel_lost_probe_poll(void)
{
    setenv("CAR_CS_DECODABLE", "1", 1);
    for (uint64_t seed = 1; seed <= 4; seed++)
        check(seed, "fade:-5:0.5", false);
    check(8, "asym:-6:14", true);
    check(11, "asym:-6:14", true);
    unsetenv("CAR_CS_DECODABLE");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_carousel_bidir_completes_intact);
    RUN_TEST(test_carousel_oneway_completes_intact);
    RUN_TEST(test_carousel_floor_control_plane);
    RUN_TEST(test_carousel_lost_probe_poll);
    return UNITY_END();
}
