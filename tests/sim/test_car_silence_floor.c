/* tests/sim/test_car_silence_floor.c -- a station that moves its control to
 * the floor on its own silence keeps a peer that still hears the control mode.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "carousel_sim.h"

#include <stdlib.h>

void setUp(void) {}
void tearDown(void) { unsetenv("CAR_PHANTOM_CARRIER"); }

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

/* A carrier that is not the peer's answer (QRM, another station) must not
 * keep the caller on the control mode while the peer, deaf to it, listens at
 * the floor: never two keydowns in a row there.  Both ways down, a carrier
 * after every keydown of the caller's: unbounded (#653's first version), 29
 * sessions of 40 went silent 240 s or more; bounded, 1, as before the rule
 * none. */
void test_car_silence_floor_is_not_held_off_by_a_phantom_carrier(void)
{
    setenv("CAR_PHANTOM_CARRIER", "1", 1);
    int over = 0;
    for (uint64_t seed = 1; seed <= 10; seed++) {
        car_sim_result_t r;
        carousel_sim_run(seed, "step:10:-12:60", true, 60ULL * 60 * 1000, &r);
        TEST_ASSERT_TRUE(r.intact);
        TEST_ASSERT_EQUAL_INT(0, r.collisions);
        if (r.silent_ms[0] >= 240000 || r.silent_ms[1] >= 240000) over++;
    }
    TEST_ASSERT_LESS_OR_EQUAL_INT(1, over);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_car_silence_floor_keeps_a_peer_that_hears_the_control_mode);
    RUN_TEST(test_car_silence_floor_is_not_held_off_by_a_phantom_carrier);
    return UNITY_END();
}
