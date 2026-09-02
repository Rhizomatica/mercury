/* Message store unit tests
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "message_store.h"
#include "hermes_log.h"

#include "unity.h"

/* hermes_log stub so message_store.c links without the async logger. */
void hermes_logf(hermes_log_level_t level, const char *component,
                 const char *fmt, ...)
{
    (void)level; (void)component; (void)fmt;
}

static const char *TEST_PATH = "/tmp/mercury_msgstore_test.jsonl";

static char get_buf[8192];

void setUp(void)
{
    remove(TEST_PATH);
}

void tearDown(void)
{
    msg_store_shutdown();
    remove(TEST_PATH);
}

void test_append_and_get_roundtrip(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));

    msg_store_append(MSG_PLANE_ARQ, MSG_DIR_TX, "MYCALL", "hello world");

    TEST_ASSERT_EQUAL_size_t(1, msg_store_count());
    TEST_ASSERT_TRUE(msg_store_get(0, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"plane\":\"arq\""));
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"dir\":\"tx\""));
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"peer\":\"MYCALL\""));
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"text\":\"hello world\""));
}

void test_feed_splits_on_newlines(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));

    const uint8_t data[] = "one\ntwo\nthree\n";
    msg_store_feed(MSG_PLANE_ARQ, MSG_DIR_RX, "PEER", data, sizeof(data) - 1);

    TEST_ASSERT_EQUAL_size_t(3, msg_store_count());
    TEST_ASSERT_TRUE(msg_store_get(0, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"text\":\"one\""));
}

void test_feed_filters_binary(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));

    /* Control bytes (< 0x20) are not printable text. */
    const uint8_t binary[] = { 'A', 'B', 0x00, 'C', 'D', '\n', 'E', 0x01, '\n' };
    msg_store_feed(MSG_PLANE_ARQ, MSG_DIR_RX, "PEER", binary, sizeof(binary));

    TEST_ASSERT_EQUAL_size_t(0, msg_store_count());
}

void test_feed_filters_overlong_line(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));

    static uint8_t longline[4096];
    memset(longline, 'x', sizeof(longline) - 1);
    longline[sizeof(longline) - 2] = '\n';
    longline[sizeof(longline) - 1] = '\0';

    msg_store_feed(MSG_PLANE_ARQ, MSG_DIR_TX, "P", longline, sizeof(longline) - 1);

    TEST_ASSERT_EQUAL_size_t(0, msg_store_count());
}

void test_persistence_reload(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));
    msg_store_append(MSG_PLANE_BCAST, MSG_DIR_TX, "ME", "persist me");
    TEST_ASSERT_EQUAL_size_t(1, msg_store_count());
    msg_store_shutdown();

    /* Re-open the same file: the message must be loaded back. */
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));
    TEST_ASSERT_EQUAL_size_t(1, msg_store_count());
    TEST_ASSERT_TRUE(msg_store_get(0, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"text\":\"persist me\""));
}

void test_ring_capacity(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 2));

    msg_store_append(MSG_PLANE_ARQ, MSG_DIR_RX, "P", "m1");
    msg_store_append(MSG_PLANE_ARQ, MSG_DIR_RX, "P", "m2");
    msg_store_append(MSG_PLANE_ARQ, MSG_DIR_RX, "P", "m3");

    /* Only the two newest survive; oldest-first order is m2, m3. */
    TEST_ASSERT_EQUAL_size_t(2, msg_store_count());
    TEST_ASSERT_TRUE(msg_store_get(0, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"text\":\"m2\""));
    TEST_ASSERT_TRUE(msg_store_get(1, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"text\":\"m3\""));
}

void test_get_out_of_range(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));
    TEST_ASSERT_EQUAL_size_t(0, msg_store_get(0, get_buf, sizeof(get_buf)));
    TEST_ASSERT_EQUAL_size_t(0, msg_store_get(5, get_buf, sizeof(get_buf)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_append_and_get_roundtrip);
    RUN_TEST(test_feed_splits_on_newlines);
    RUN_TEST(test_feed_filters_binary);
    RUN_TEST(test_feed_filters_overlong_line);
    RUN_TEST(test_persistence_reload);
    RUN_TEST(test_ring_capacity);
    RUN_TEST(test_get_out_of_range);
    return UNITY_END();
}
