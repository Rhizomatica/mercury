/* tests/sim/test_car_silence_floor.c -- a station that moves its control to
 * the floor on its own silence keeps a peer that still hears the control mode.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "carousel_sim.h"

void setUp(void) {}
void tearDown(void) {}

/* The way back gone for 170 s, the way out fine: the caller's polls still
 * reach the callee on the control mode.  Moved to the floor on its silence
 * alone, they went where the callee does not listen until 90 s after the last
 * one it decoded, and the silence ran past the FSM's 240 s watchdog (on air,
 * KG9NI to K7EK, 2026-11-22). */
void test_car_silence_floor_keeps_a_peer_that_hears_the_control_mode(void)
{
    int over = 0;
    for (uint64_t seed = 1; seed <= 20; seed++) {
        car_sim_result_t r;
        carousel_sim_run(seed, "gap:6:-20:40:170", true, 30ULL * 60 * 1000, &r);
        TEST_ASSERT_TRUE(r.intact);
        TEST_ASSERT_EQUAL_INT(0, r.overruns);
        if (r.silent_ms[0] >= 240000 || r.silent_ms[1] >= 240000) over++;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(2, over);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_car_silence_floor_keeps_a_peer_that_hears_the_control_mode);
    return UNITY_END();
}
