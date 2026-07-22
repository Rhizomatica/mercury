/*
 * Virtual clock tests (common/virtual_clock.c) — the time base behind the
 * -x sock lockstep bench transport.
 *
 * hermes_uptime_ms() is stubbed so the wall-clock fallback is deterministic.
 * virtual_clock_enable() is one-way for the process lifetime, so the tests
 * run in a fixed order: all wall-mode assertions first, then enable once.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unity.h"
#include "virtual_clock.h"

#include <stdint.h>

/* Stub for common/hermes_log.c's monotonic clock (virtual_clock.c's only
 * dependency), so wall time is under test control. */
static uint64_t fake_wall_ms = 1000;
uint64_t hermes_uptime_ms(void) { return fake_wall_ms; }

void setUp(void)    { }
void tearDown(void) { }

/* ---- wall mode (before any enable) ---- */

void test_wall_mode_tracks_uptime(void)
{
    TEST_ASSERT_FALSE(virtual_clock_enabled());
    fake_wall_ms = 1000;
    TEST_ASSERT_EQUAL_UINT64(1000, time_now_ms());
    fake_wall_ms = 5555;
    TEST_ASSERT_EQUAL_UINT64(5555, time_now_ms());
}

void test_set_without_enable_does_not_switch_modes(void)
{
    /* set() only stores the value; the wall fallback stays active until
     * enable() flips the mode. */
    virtual_clock_set(999999);
    TEST_ASSERT_FALSE(virtual_clock_enabled());
    fake_wall_ms = 7777;
    TEST_ASSERT_EQUAL_UINT64(7777, time_now_ms());
}

/* ---- virtual mode (enable is one-way; must run after the wall tests) ---- */

void test_enable_switches_to_epoch(void)
{
    virtual_clock_enable(VIRTUAL_CLOCK_EPOCH_MS);
    TEST_ASSERT_TRUE(virtual_clock_enabled());
    TEST_ASSERT_EQUAL_UINT64(VIRTUAL_CLOCK_EPOCH_MS, time_now_ms());
}

void test_set_advances_virtual_time(void)
{
    virtual_clock_set(VIRTUAL_CLOCK_EPOCH_MS + 128);
    TEST_ASSERT_EQUAL_UINT64(VIRTUAL_CLOCK_EPOCH_MS + 128, time_now_ms());
    virtual_clock_set(VIRTUAL_CLOCK_EPOCH_MS + 25600);
    TEST_ASSERT_EQUAL_UINT64(VIRTUAL_CLOCK_EPOCH_MS + 25600, time_now_ms());
}

void test_wall_clock_no_longer_leaks_through(void)
{
    fake_wall_ms = 123456789;
    TEST_ASSERT_EQUAL_UINT64(VIRTUAL_CLOCK_EPOCH_MS + 25600, time_now_ms());
}

void test_set_stores_verbatim(void)
{
    /* The clock itself is a plain register: monotonicity across sim
     * reconnects is the transport's job (audioio.c floors it), not this
     * module's.  Pin the store-verbatim semantics so a behavior change
     * here is a deliberate one. */
    virtual_clock_set(VIRTUAL_CLOCK_EPOCH_MS + 100);
    TEST_ASSERT_EQUAL_UINT64(VIRTUAL_CLOCK_EPOCH_MS + 100, time_now_ms());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_wall_mode_tracks_uptime);
    RUN_TEST(test_set_without_enable_does_not_switch_modes);
    RUN_TEST(test_enable_switches_to_epoch);
    RUN_TEST(test_set_advances_virtual_time);
    RUN_TEST(test_wall_clock_no_longer_leaks_through);
    RUN_TEST(test_set_stores_verbatim);
    UNITY_END();
    return 0;
}
