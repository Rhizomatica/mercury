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
    TEST_ASSERT_EQUAL_UINT64(0, ctx.tx_queue_ms);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.tx_start_ms);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.tx_end_ms);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.ack_rx_ms);
    TEST_ASSERT_EQUAL_UINT32(0, ctx.rtt_ms);
    TEST_ASSERT_EQUAL_UINT32(0, ctx.retry_count);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.tx_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.rx_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.retries_total);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.frames_tx);
    TEST_ASSERT_EQUAL_UINT64(0, ctx.frames_rx);
}

/* Record TX queue */
void test_record_tx_queue(void)
{
    mock_set_uptime_ms(1000);
    arq_timing_record_tx_queue(&ctx, 0, 18 /*DATAC4*/, 100, 48);

    TEST_ASSERT_EQUAL_UINT64(1000, ctx.tx_queue_ms);
    TEST_ASSERT_EQUAL_UINT64(48, ctx.tx_bytes);
    /* Note: frames_tx is incremented in record_tx_start, not here */
}

/* Record TX start */
void test_record_tx_start(void)
{
    mock_set_uptime_ms(1100);
    arq_timing_record_tx_start(&ctx, 0, 18, 100);

    TEST_ASSERT_EQUAL_UINT64(1100, ctx.tx_start_ms);
}

/* Record TX end */
void test_record_tx_end(void)
{
    mock_set_uptime_ms(1100);
    arq_timing_record_tx_start(&ctx, 0, 18, 100);

    mock_set_uptime_ms(3600);
    arq_timing_record_tx_end(&ctx, 0);

    TEST_ASSERT_EQUAL_UINT64(3600, ctx.tx_end_ms);
}

/* Record ACK RX */
void test_record_ack_rx(void)
{
    mock_set_uptime_ms(1000);
    arq_timing_record_tx_start(&ctx, 0, 18, 100);

    mock_set_uptime_ms(4000);
    arq_timing_record_ack_rx(&ctx, 0, 50 /*=500ms*/, -30 /*peer_snr_x10*/);

    TEST_ASSERT_EQUAL_UINT64(4000, ctx.ack_rx_ms);
    TEST_ASSERT_EQUAL_INT(-30, ctx.last_snr_peer_x10);
}

/* Record data RX */
void test_record_data_rx(void)
{
    mock_set_uptime_ms(2000);
    arq_timing_record_data_rx(&ctx, 0, 48, -20);

    TEST_ASSERT_EQUAL_UINT64(2000, ctx.data_rx_ms);
    TEST_ASSERT_EQUAL_UINT64(1, ctx.frames_rx);
    TEST_ASSERT_EQUAL_INT(-20, ctx.last_snr_local_x10);
}

/* Record retry */
void test_record_retry(void)
{
    arq_timing_record_retry(&ctx, 0, 1, "timeout");
    TEST_ASSERT_EQUAL_UINT32(1, ctx.retry_count);
    TEST_ASSERT_EQUAL_UINT64(1, ctx.retries_total);

    arq_timing_record_retry(&ctx, 0, 2, "timeout");
    TEST_ASSERT_EQUAL_UINT32(2, ctx.retry_count);
    TEST_ASSERT_EQUAL_UINT64(2, ctx.retries_total);
}

/* Cumulative counters across cycles */
void test_cumulative_counters(void)
{
    mock_set_uptime_ms(1000);
    arq_timing_record_tx_queue(&ctx, 0, 18, 100, 48);
    arq_timing_record_tx_start(&ctx, 0, 18, 100);
    arq_timing_record_tx_queue(&ctx, 1, 18, 52, 48);
    arq_timing_record_tx_start(&ctx, 1, 18, 52);

    TEST_ASSERT_EQUAL_UINT64(96, ctx.tx_bytes);
    TEST_ASSERT_EQUAL_UINT64(2, ctx.frames_tx);

    arq_timing_record_data_rx(&ctx, 0, 48, -20);
    arq_timing_record_data_rx(&ctx, 1, 48, -20);

    TEST_ASSERT_EQUAL_UINT64(2, ctx.frames_rx);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_timing_init_zeros_context);
    RUN_TEST(test_record_tx_queue);
    RUN_TEST(test_record_tx_start);
    RUN_TEST(test_record_tx_end);
    RUN_TEST(test_record_ack_rx);
    RUN_TEST(test_record_data_rx);
    RUN_TEST(test_record_retry);
    RUN_TEST(test_cumulative_counters);
    return UNITY_END();
}
