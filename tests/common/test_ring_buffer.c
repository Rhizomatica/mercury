/*
 * Ring Buffer Unit Tests
 *
 * Tests for common/ring_buffer_posix.c using the non-SHM init variant.
 * Uses Unity test framework.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <string.h>
#include <stdint.h>

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
    return UNITY_END();
}
