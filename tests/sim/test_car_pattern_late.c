/* tests/sim/test_car_pattern_late.c -- a pattern keyed on time is not taken
 * for an early one when its receiver learns of its own keydown's end late.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "carousel_sim.h"

#include <stdlib.h>

void setUp(void) {}
void tearDown(void) { unsetenv("CAR_TXEND_LATE_MS"); }

/* On air (N3JXB to KG9NI, 1.9.25, 2026-11-23) KG9NI's TX_COMPLETE came
 * ~0.25 s after its audio ended, and it ignored 6 of the 12 patterns N3JXB
 * keyed on time as "out of time".  In the sim, each station 300 ms late:
 * one-way 8 KB at the floor did not finish in an hour on 3 seeds of 3. */
void test_car_patterns_heard_with_a_late_tx_end(void)
{
    setenv("CAR_TXEND_LATE_MS", "300", 1);
    for (uint64_t seed = 1; seed <= 3; seed++) {
        car_sim_result_t r;
        carousel_sim_run(seed, "cliff:-8", false, 60ULL * 60 * 1000, &r);
        TEST_ASSERT_TRUE(r.intact);
        TEST_ASSERT_EQUAL_INT(0, r.collisions);
        TEST_ASSERT_TRUE_MESSAGE(r.done_ms != 0, "8 KB at the floor not done in an hour");
        TEST_ASSERT_TRUE(r.done_ms < 30ULL * 60 * 1000);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_car_patterns_heard_with_a_late_tx_end);
    return UNITY_END();
}
