/* TX burst pacing: the keyed window must track the audio, not the timer.
 *
 * Copyright (C) 2026 Rhizomatica
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * ---------------------------------------------------------------------------
 *
 * Regression cover for the on-air stall of 2026-08-13 (Windows station): a
 * relative-sleep pacing loop accumulated ~1.1 s of extra PTT per burst on a
 * 15.6 ms scheduler tick, so the peer's ACK -- sent 700 ms after decode --
 * always arrived while we were still keyed.  The link never advanced past the
 * first data frame.  The bug is invisible on a fine-grained Linux clock, so the
 * tick is simulated here rather than slept through.
 */

#include "unity.h"
#include "tx_pacing.h"

/* No real sleeping: a simulated clock that, like a coarse OS timer, can only
 * ever wake up late -- each sleep is rounded UP to the next tick boundary. */
static uint64_t sim_now_us;
static uint64_t sim_tick_us;

static void sim_sleep(uint64_t requested_us)
{
    if (requested_us == 0)
        return;
    uint64_t ticks = (requested_us + sim_tick_us - 1) / sim_tick_us;
    sim_now_us += ticks * sim_tick_us;
}

/* Run a whole burst through the pacer; return the real time the PTT was held. */
static uint64_t keyed_window_us(uint64_t duration_us, uint64_t step_us, uint64_t tick_us)
{
    sim_now_us = 0;
    sim_tick_us = tick_us;

    uint64_t t_start = sim_now_us;
    uint64_t waited_us = 0;
    while (waited_us < duration_us)
    {
        uint64_t elapsed_us = sim_now_us - t_start;
        uint64_t sleep_us = tx_pace_sleep_us(waited_us, elapsed_us, duration_us,
                                             step_us, &waited_us);
        sim_sleep(sleep_us);
    }
    return sim_now_us - t_start;
}

/* The naive loop that shipped: sleep(step) per slice, ignoring elapsed time. */
static uint64_t keyed_window_us_relative(uint64_t duration_us, uint64_t step_us,
                                         uint64_t tick_us)
{
    sim_now_us = 0;
    sim_tick_us = tick_us;

    uint64_t t_start = sim_now_us;
    uint64_t waited_us = 0;
    while (waited_us < duration_us)
    {
        uint64_t s = step_us;
        if (s > duration_us - waited_us)
            s = duration_us - waited_us;
        sim_sleep(s);
        waited_us += s;
    }
    return sim_now_us - t_start;
}

#define STEP_US   (50 * 1000ULL)      /* TX_SPECTRUM_STEP_MS */
#define DATAC15   (4410 * 1000ULL)
#define DATAC16   (3740 * 1000ULL)

/* The property that matters on air: however coarse the host timer, the tail of
 * dead carrier must stay far below the 700 ms in which the peer answers. */
void test_keyed_window_survives_a_coarse_scheduler_tick(void)
{
    const uint64_t ticks_us[] = { 1000, 4000, 15600, 31250 };  /* Linux..Windows..worse */

    for (unsigned i = 0; i < sizeof(ticks_us) / sizeof(ticks_us[0]); i++)
    {
        uint64_t held = keyed_window_us(DATAC15, STEP_US, ticks_us[i]);
        uint64_t tail = held - DATAC15;

        char msg[128];
        snprintf(msg, sizeof(msg), "tick %llu us: PTT tail %llu us",
                 (unsigned long long)ticks_us[i], (unsigned long long)tail);

        /* One tick of slop is unavoidable; anything proportional to the burst
         * length is the accumulation bug coming back. */
        TEST_ASSERT_TRUE_MESSAGE(tail <= 2 * ticks_us[i], msg);
        TEST_ASSERT_TRUE_MESSAGE(tail < 200000, msg);  /* << 700 ms ACK guard */
    }
}

/* Guards the guard: prove the assertion above actually discriminates, i.e. the
 * shipped loop fails it.  Without this a future refactor could satisfy the test
 * while reintroducing the defect. */
void test_relative_sleep_loop_overruns_the_ack_guard(void)
{
    uint64_t held = keyed_window_us_relative(DATAC15, STEP_US, 15600);
    uint64_t tail = held - DATAC15;

    /* ~1.1 s observed on air; assert only that it blows the 700 ms window. */
    TEST_ASSERT_TRUE_MESSAGE(tail > 700000,
        "relative-sleep loop should overrun the ACK guard -- test is not discriminating");
}

/* A host that falls behind must catch up by not sleeping, never by keying for
 * longer: the pacer returns 0 once elapsed time has passed the deadline. */
void test_late_host_never_extends_the_keyed_window(void)
{
    uint64_t next = 0;
    uint64_t sleep_us = tx_pace_sleep_us(0, /* elapsed */ 900 * 1000ULL,
                                         DATAC16, STEP_US, &next);
    TEST_ASSERT_EQUAL_UINT64(0, sleep_us);
    TEST_ASSERT_EQUAL_UINT64(STEP_US, next);
}

/* The last slice is short: pacing must stop exactly at the end of the audio,
 * never round a partial step up past it. */
void test_final_slice_stops_at_the_end_of_the_burst(void)
{
    uint64_t next = 0;
    (void)tx_pace_sleep_us(DATAC16 - 10000, 0, DATAC16, STEP_US, &next);
    TEST_ASSERT_EQUAL_UINT64(DATAC16, next);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_keyed_window_survives_a_coarse_scheduler_tick);
    RUN_TEST(test_relative_sleep_loop_overruns_the_ack_guard);
    RUN_TEST(test_late_host_never_extends_the_keyed_window);
    RUN_TEST(test_final_slice_stops_at_the_end_of_the_burst);
    return UNITY_END();
}
