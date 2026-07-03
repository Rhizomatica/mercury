/*
 * Ring Buffer Unit Tests
 *
 * Tests for common/ring_buffer_posix.c using the non-SHM init variant.
 * Uses Unity test framework.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "unity.h"
#include "ring_buffer_posix.h"

#define TEST_BUF_SIZE 64

static uint8_t backing_buffer[TEST_BUF_SIZE];
static cbuf_handle_t cbuf;

void setUp(void)
{
    memset(backing_buffer, 0, sizeof(backing_buffer));
    cbuf = circular_buf_init(backing_buffer, TEST_BUF_SIZE);
}

void tearDown(void)
{
    if (cbuf) {
        circular_buf_free(cbuf);
        cbuf = NULL;
    }
}

/* Test 1: Init creates an empty buffer */
void test_init_creates_empty_buffer(void)
{
    TEST_ASSERT_NOT_NULL(cbuf);
    TEST_ASSERT_TRUE(circular_buf_empty(cbuf));
    TEST_ASSERT_FALSE(circular_buf_full(cbuf));
    TEST_ASSERT_EQUAL_size_t(TEST_BUF_SIZE, circular_buf_capacity(cbuf));
    TEST_ASSERT_EQUAL_size_t(0, size_buffer(cbuf));
}

/* Test 2: Put a single byte */
void test_put_single_byte(void)
{
    int ret = circular_buf_put(cbuf, 0xAB);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_FALSE(circular_buf_empty(cbuf));
    TEST_ASSERT_EQUAL_size_t(1, size_buffer(cbuf));
}

/* Test 3: Put then get a single byte */
void test_get_single_byte(void)
{
    uint8_t out = 0;

    circular_buf_put(cbuf, 0xCD);
    int ret = circular_buf_get(cbuf, &out);

    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT8(0xCD, out);
    TEST_ASSERT_TRUE(circular_buf_empty(cbuf));
}

/* Test 4: Put and get multiple bytes */
void test_put_get_multiple_bytes(void)
{
    uint8_t data_in[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t data_out[5] = {0};

    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_INT(0, circular_buf_put(cbuf, data_in[i]));
    }
    TEST_ASSERT_EQUAL_size_t(5, size_buffer(cbuf));

    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_INT(0, circular_buf_get(cbuf, &data_out[i]));
    }
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data_in, data_out, 5);
    TEST_ASSERT_TRUE(circular_buf_empty(cbuf));
}

/* Test 5: Buffer full detection */
void test_buffer_full(void)
{
    /* Fill the buffer completely */
    for (size_t i = 0; i < TEST_BUF_SIZE; i++) {
        circular_buf_put(cbuf, (uint8_t)(i & 0xFF));
    }

    TEST_ASSERT_TRUE(circular_buf_full(cbuf));
    TEST_ASSERT_FALSE(circular_buf_empty(cbuf));
    TEST_ASSERT_EQUAL_size_t(TEST_BUF_SIZE, size_buffer(cbuf));
}

/* Test 6: Reset clears the buffer */
void test_buffer_reset(void)
{
    /* Fill some data */
    for (int i = 0; i < 10; i++) {
        circular_buf_put(cbuf, (uint8_t)i);
    }
    TEST_ASSERT_FALSE(circular_buf_empty(cbuf));

    circular_buf_reset(cbuf);

    TEST_ASSERT_TRUE(circular_buf_empty(cbuf));
    TEST_ASSERT_FALSE(circular_buf_full(cbuf));
    TEST_ASSERT_EQUAL_size_t(0, size_buffer(cbuf));
}

/* Test 7: Capacity returns max size */
void test_capacity_returns_max(void)
{
    TEST_ASSERT_EQUAL_size_t(TEST_BUF_SIZE, circular_buf_capacity(cbuf));

    /* Capacity unchanged after adding data */
    circular_buf_put(cbuf, 0x42);
    TEST_ASSERT_EQUAL_size_t(TEST_BUF_SIZE, circular_buf_capacity(cbuf));
}

/* Test 8: Free size decreases on put */
void test_free_size_decreases_on_put(void)
{
    TEST_ASSERT_EQUAL_size_t(TEST_BUF_SIZE, circular_buf_free_size(cbuf));

    circular_buf_put(cbuf, 0x01);
    TEST_ASSERT_EQUAL_size_t(TEST_BUF_SIZE - 1, circular_buf_free_size(cbuf));

    circular_buf_put(cbuf, 0x02);
    TEST_ASSERT_EQUAL_size_t(TEST_BUF_SIZE - 2, circular_buf_free_size(cbuf));
}

/* Test 9: Wrap-around data integrity */
void test_wrap_around(void)
{
    /* Fill half the buffer */
    for (size_t i = 0; i < TEST_BUF_SIZE / 2; i++) {
        circular_buf_put(cbuf, (uint8_t)i);
    }

    /* Drain it */
    uint8_t tmp;
    for (size_t i = 0; i < TEST_BUF_SIZE / 2; i++) {
        circular_buf_get(cbuf, &tmp);
    }
    TEST_ASSERT_TRUE(circular_buf_empty(cbuf));

    /* Now fill past the wrap point with known pattern */
    uint8_t pattern[TEST_BUF_SIZE];
    for (size_t i = 0; i < TEST_BUF_SIZE; i++) {
        pattern[i] = (uint8_t)(0xA0 + (i & 0x0F));
        circular_buf_put(cbuf, pattern[i]);
    }

    /* Read back and verify */
    uint8_t result[TEST_BUF_SIZE];
    for (size_t i = 0; i < TEST_BUF_SIZE; i++) {
        circular_buf_get(cbuf, &result[i]);
    }
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pattern, result, TEST_BUF_SIZE);
}

/* Test 10: clear_buffer wakes a writer blocked in write_buffer (ring full).
 *
 * Regression guard for the arq.c TX-path deadlock: the bridge worker holds
 * g_app_tx_mtx across write_buffer().  When the ring is full, write_buffer()
 * parks in COND_WAIT; the only drainer (cb_tx_read) also needs g_app_tx_mtx,
 * so nothing ever signals the cond and the ARQ event loop wedges permanently.
 * The fix has two parts: (a) drop the outer mutex from the write sites, and
 * (b) add COND_SIGNAL to clear_buffer() so that teardown-triggered clears
 * wake any blocked writer.  This test covers part (b): if clear_buffer() does
 * not signal, the pthread_join() below will block indefinitely.
 */

struct clear_wake_ctx {
    cbuf_handle_t cbuf;
    volatile int  done;
};

static void *clear_wake_writer(void *arg)
{
    struct clear_wake_ctx *ctx = arg;
    uint8_t extra = 0xFF;
    write_buffer(ctx->cbuf, &extra, 1); /* blocks until ring has space */
    ctx->done = 1;
    return NULL;
}

void test_clear_buffer_wakes_blocked_writer(void)
{
    uint8_t backing[32];
    cbuf_handle_t cbuf = circular_buf_init(backing, sizeof(backing));
    TEST_ASSERT_NOT_NULL(cbuf);

    /* Fill the ring to capacity so the next write_buffer call will block. */
    uint8_t fill[32] = {0};
    TEST_ASSERT_EQUAL_INT(0, write_buffer(cbuf, fill, sizeof(fill)));
    TEST_ASSERT_TRUE(circular_buf_full(cbuf));

    struct clear_wake_ctx ctx = { .cbuf = cbuf, .done = 0 };
    pthread_t tid;
    pthread_create(&tid, NULL, clear_wake_writer, &ctx);

    /* Give the writer thread time to enter COND_WAIT inside write_buffer. */
    struct timespec ts = { 0, 50 * 1000 * 1000 }; /* 50 ms */
    nanosleep(&ts, NULL);

    TEST_ASSERT_EQUAL_INT(0, ctx.done); /* should still be blocked */

    /* clear_buffer() must signal the cond to unblock the writer. */
    clear_buffer(cbuf);

    /* If clear_buffer() did not signal, pthread_join() hangs here. */
    pthread_join(tid, NULL);

    TEST_ASSERT_EQUAL_INT(1, ctx.done);
    circular_buf_free(cbuf);
}

/* Test 11: read_buffer wakes a writer blocked in write_buffer (ring full).
 *
 * This is the mechanism the TX-path deadlock fix relies on: with the outer
 * g_app_tx_mtx gone from the write sites, a writer parked in write_buffer()'s
 * COND_WAIT is woken by the drain-side COND_SIGNAL in read_buffer().  If that
 * signal is ever removed, any >APP_TX_BUF_SIZE transfer deadlocks again and
 * the pthread_join() below hangs. */
static void *full_ring_writer(void *arg)
{
    struct clear_wake_ctx *ctx = arg;
    uint8_t extra[8];
    memset(extra, 0xEE, sizeof(extra));
    write_buffer(ctx->cbuf, extra, sizeof(extra)); /* blocks: ring is full */
    ctx->done = 1;
    return NULL;
}

void test_read_buffer_wakes_blocked_writer(void)
{
    uint8_t backing[32];
    cbuf_handle_t rb = circular_buf_init(backing, sizeof(backing));
    TEST_ASSERT_NOT_NULL(rb);

    uint8_t fill[32];
    memset(fill, 0x11, sizeof(fill));
    TEST_ASSERT_EQUAL_INT(0, write_buffer(rb, fill, sizeof(fill)));
    TEST_ASSERT_TRUE(circular_buf_full(rb));

    struct clear_wake_ctx ctx = { .cbuf = rb, .done = 0 };
    pthread_t tid;
    pthread_create(&tid, NULL, full_ring_writer, &ctx);

    struct timespec ts = { 0, 50 * 1000 * 1000 }; /* 50 ms */
    nanosleep(&ts, NULL);
    TEST_ASSERT_EQUAL_INT(0, ctx.done); /* writer must be parked */

    /* Drain enough for the pending 8-byte write; the read must signal. */
    uint8_t sink[16];
    TEST_ASSERT_EQUAL_INT(0, read_buffer(rb, sink, sizeof(sink)));
    TEST_ASSERT_EQUAL_HEX8(0x11, sink[0]);

    pthread_join(tid, NULL); /* hangs here if read_buffer stops signalling */
    TEST_ASSERT_EQUAL_INT(1, ctx.done);

    /* The woken writer's bytes must be in the ring, after the old data. */
    uint8_t rest[24];
    TEST_ASSERT_EQUAL_INT(0, read_buffer(rb, rest, sizeof(rest)));
    TEST_ASSERT_EQUAL_HEX8(0x11, rest[15]);
    TEST_ASSERT_EQUAL_HEX8(0xEE, rest[16]);
    TEST_ASSERT_EQUAL_HEX8(0xEE, rest[23]);
    circular_buf_free(rb);
}

/* Test 12: write_buffer wakes a reader blocked in read_buffer (ring empty) —
 * the dual of Test 11, guarding the COND_SIGNAL in write_buffer()'s success
 * path that the RX-side consumers rely on. */
static void *empty_ring_reader(void *arg)
{
    struct clear_wake_ctx *ctx = arg;
    uint8_t got = 0;
    read_buffer(ctx->cbuf, &got, 1); /* blocks: ring is empty */
    ctx->done = (got == 0x77) ? 1 : -1;
    return NULL;
}

void test_write_buffer_wakes_blocked_reader(void)
{
    uint8_t backing[32];
    cbuf_handle_t rb = circular_buf_init(backing, sizeof(backing));
    TEST_ASSERT_NOT_NULL(rb);

    struct clear_wake_ctx ctx = { .cbuf = rb, .done = 0 };
    pthread_t tid;
    pthread_create(&tid, NULL, empty_ring_reader, &ctx);

    struct timespec ts = { 0, 50 * 1000 * 1000 }; /* 50 ms */
    nanosleep(&ts, NULL);
    TEST_ASSERT_EQUAL_INT(0, ctx.done); /* reader must be parked */

    uint8_t byte = 0x77;
    TEST_ASSERT_EQUAL_INT(0, write_buffer(rb, &byte, 1));

    pthread_join(tid, NULL); /* hangs here if write_buffer stops signalling */
    TEST_ASSERT_EQUAL_INT(1, ctx.done); /* woke AND read the right byte */
    circular_buf_free(rb);
}

/* Test 13: two concurrent writers + one reader, no outer lock.
 *
 * The deadlock fix removed g_app_tx_mtx from the write sites on the grounds
 * that the ring's internal mutex alone serializes concurrent writers
 * (arq_payload_bridge_worker and arq_queue_data) against the event-loop
 * reader.  This test hammers that claim: every byte written by each writer
 * must come out exactly once — a lost signal deadlocks (join hangs), a
 * head/tail race drops or duplicates bytes (count mismatch). */
#define CW_CHUNK   16
#define CW_CHUNKS  256   /* 4 KB per writer through a 64-byte ring */

struct cw_writer_ctx {
    cbuf_handle_t cbuf;
    uint8_t       tag;
};

static void *cw_writer(void *arg)
{
    struct cw_writer_ctx *ctx = arg;
    uint8_t chunk[CW_CHUNK];
    memset(chunk, ctx->tag, sizeof(chunk));
    for (int i = 0; i < CW_CHUNKS; i++)
        write_buffer(ctx->cbuf, chunk, sizeof(chunk));
    return NULL;
}

void test_concurrent_writers_data_integrity(void)
{
    uint8_t backing[64];
    cbuf_handle_t rb = circular_buf_init(backing, sizeof(backing));
    TEST_ASSERT_NOT_NULL(rb);

    struct cw_writer_ctx w1 = { .cbuf = rb, .tag = 0xA5 };
    struct cw_writer_ctx w2 = { .cbuf = rb, .tag = 0x5A };
    pthread_t t1, t2;
    pthread_create(&t1, NULL, cw_writer, &w1);
    pthread_create(&t2, NULL, cw_writer, &w2);

    /* Drain from this thread (the single reader, like the ARQ event loop). */
    const size_t total = 2u * CW_CHUNKS * CW_CHUNK;
    size_t got = 0, n_a5 = 0, n_5a = 0;
    while (got < total)
    {
        size_t avail = size_buffer(rb);
        if (avail == 0)
        {
            struct timespec ts = { 0, 1 * 1000 * 1000 }; /* 1 ms */
            nanosleep(&ts, NULL);
            continue;
        }
        uint8_t sink[64];
        if (avail > sizeof(sink)) avail = sizeof(sink);
        TEST_ASSERT_EQUAL_INT(0, read_buffer(rb, sink, avail));
        for (size_t i = 0; i < avail; i++)
        {
            if (sink[i] == 0xA5) n_a5++;
            else if (sink[i] == 0x5A) n_5a++;
            else TEST_FAIL_MESSAGE("corrupted byte from ring");
        }
        got += avail;
    }

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    TEST_ASSERT_EQUAL_size_t((size_t)CW_CHUNKS * CW_CHUNK, n_a5);
    TEST_ASSERT_EQUAL_size_t((size_t)CW_CHUNKS * CW_CHUNK, n_5a);
    TEST_ASSERT_EQUAL_size_t(0, size_buffer(rb)); /* nothing left over */
    circular_buf_free(rb);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_creates_empty_buffer);
    RUN_TEST(test_put_single_byte);
    RUN_TEST(test_get_single_byte);
    RUN_TEST(test_put_get_multiple_bytes);
    RUN_TEST(test_buffer_full);
    RUN_TEST(test_buffer_reset);
    RUN_TEST(test_capacity_returns_max);
    RUN_TEST(test_free_size_decreases_on_put);
    RUN_TEST(test_wrap_around);
    RUN_TEST(test_clear_buffer_wakes_blocked_writer);
    RUN_TEST(test_read_buffer_wakes_blocked_writer);
    RUN_TEST(test_write_buffer_wakes_blocked_reader);
    RUN_TEST(test_concurrent_writers_data_integrity);
    return UNITY_END();
}
