/*
 * ARQ Timing Unit Tests
 *
 * Tests for datalink_arq/arq_timing.c — timing context init,
 * recording functions, and cumulative counters.
 *
 * Copyright (C) 2025 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>

#include "unity.h"
#include "arq_timing.h"

/* Provided by arq_test_stubs.c */
extern void mock_set_uptime_ms(uint64_t ms);

static arq_timing_ctx_t ctx;

void setUp(void)
{
    arq_timing_init(&ctx);
    mock_set_uptime_ms(1000); /* start at 1 second */
}

void tearDown(void) { }

/* Init zeros all fields */
void test_timing_init_zeros_context(void)
{
    arq_timing_record_car_tx(&ctx, 10);
    arq_timing_record_car_rx(&ctx, 20);
    arq_timing_init(&ctx);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.tx_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.rx_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.frames_tx);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.frames_rx);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.data_rx_ms);
}

/* The byte counters accumulate; empty records change nothing */
void test_car_counters_accumulate(void)
{
    arq_timing_record_car_tx(&ctx, 48);
    arq_timing_record_car_tx(&ctx, 0);
    arq_timing_record_car_tx(&ctx, 52);
    TEST_ASSERT_EQUAL_UINT64(100, ctx.tx_bytes);

    mock_set_uptime_ms(2000);
    arq_timing_record_car_rx(&ctx, 30);
    arq_timing_record_car_rx(&ctx, -1);
    TEST_ASSERT_EQUAL_UINT64(30, ctx.rx_bytes);
    TEST_ASSERT_EQUAL_UINT64(2000, ctx.data_rx_ms);
}

/* A new session starts its counters from zero */
void test_connect_resets_counters(void)
{
    arq_timing_record_car_tx(&ctx, 48);
    ctx.frames_tx = 3;
    arq_timing_record_connect(&ctx, 23);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.tx_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.frames_tx);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_timing_init_zeros_context);
    RUN_TEST(test_car_counters_accumulate);
    RUN_TEST(test_connect_resets_counters);
    return UNITY_END();
}
