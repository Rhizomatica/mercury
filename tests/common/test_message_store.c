/* Message store unit tests
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
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

/* The 2026-09-12 case: a binary broadcast frame whose 0x0A bytes split it into
 * short runs that each contain no other control byte.  Checked line by line,
 * those runs were stored as chat. */
void test_feed_drops_binary_frame_whose_pieces_look_like_text(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 50));

    uint8_t frame[256];
    uint32_t x = 12345;
    for (size_t i = 0; i < sizeof(frame); i++)
    {
        x = x * 1103515245u + 12345u;
        frame[i] = (uint8_t)(x >> 16);
    }
    /* Plant newlines around a control-free run of high bytes, as found in the
     * polluted history, and make sure the frame also carries a control byte. */
    frame[40] = '\n';
    for (size_t i = 41; i < 52; i++) frame[i] |= 0x80;
    frame[52] = '\n';
    frame[100] = 0x01;
    /* A piece that is perfectly good text on its own.  Only dropping the whole
     * chunk keeps it out; the per-line checks would accept it. */
    frame[120] = '\n';
    frame[121] = 'o';
    frame[122] = 'k';
    frame[123] = '\n';

    msg_store_feed(MSG_PLANE_BCAST, MSG_DIR_RX, "", frame, sizeof(frame));

    TEST_ASSERT_EQUAL_size_t(0, msg_store_count());
}

void test_feed_rejects_invalid_utf8_line(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));

    /* No control bytes at all, but not UTF-8: a lone continuation byte, a lead
     * byte cut short, and an overlong encoding of '/'. */
    const uint8_t lone[]     = { 'a', 0x95, 'b', '\n' };
    const uint8_t cut[]      = { 'a', 0xE2, 0x9C, '\n' };
    const uint8_t overlong[] = { 0xC0, 0xAF, '\n' };
    msg_store_feed(MSG_PLANE_BCAST, MSG_DIR_RX, "P", lone, sizeof(lone));
    msg_store_feed(MSG_PLANE_BCAST, MSG_DIR_RX, "P", cut, sizeof(cut));
    msg_store_feed(MSG_PLANE_BCAST, MSG_DIR_RX, "P", overlong, sizeof(overlong));

    TEST_ASSERT_EQUAL_size_t(0, msg_store_count());
}

void test_feed_keeps_utf8_text_even_split_across_chunks(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));

    const char *whole = "Ol\xc3\xa1, Jos\xc3\xa9 \xe2\x9c\x93 73\n";
    msg_store_feed(MSG_PLANE_ARQ, MSG_DIR_RX, "P", (const uint8_t *)whole, strlen(whole));
    /* The ARQ byte stream can cut a multi-byte character in two. */
    msg_store_feed(MSG_PLANE_ARQ, MSG_DIR_RX, "P", (const uint8_t *)"Ol\xc3", 3);
    msg_store_feed(MSG_PLANE_ARQ, MSG_DIR_RX, "P", (const uint8_t *)"\xa1\n", 2);

    TEST_ASSERT_EQUAL_size_t(2, msg_store_count());
    TEST_ASSERT_TRUE(msg_store_get(0, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "Ol\xc3\xa1, Jos\xc3\xa9 \xe2\x9c\x93 73"));
    TEST_ASSERT_TRUE(msg_store_get(1, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"text\":\"Ol\xc3\xa1\""));
}

void test_chat_right_after_a_binary_frame_is_kept(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));

    const uint8_t binary[] = { 'x', 0x00, 0xFF, 'y' };   /* no trailing newline */
    msg_store_feed(MSG_PLANE_BCAST, MSG_DIR_RX, "P", binary, sizeof(binary));
    msg_store_feed(MSG_PLANE_BCAST, MSG_DIR_RX, "P", (const uint8_t *)"hi\n", 3);

    TEST_ASSERT_EQUAL_size_t(1, msg_store_count());
    TEST_ASSERT_TRUE(msg_store_get(0, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"text\":\"hi\""));
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

void test_reset_discards_partial_line(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));

    /* Session ends without a trailing newline; a reset must drop that residue
     * so the next session's message is stored alone with its own peer. */
    msg_store_feed(MSG_PLANE_ARQ, MSG_DIR_RX, "AAA", (const uint8_t *)"hello", 5);
    msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_RX);
    msg_store_feed(MSG_PLANE_ARQ, MSG_DIR_RX, "BBB", (const uint8_t *)"hi\n", 3);

    TEST_ASSERT_EQUAL_size_t(1, msg_store_count());
    TEST_ASSERT_TRUE(msg_store_get(0, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"text\":\"hi\""));
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"peer\":\"BBB\""));
    TEST_ASSERT_NULL(strstr(get_buf, "hello"));
}

void test_snapshot(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));
    msg_store_append(MSG_PLANE_ARQ, MSG_DIR_RX, "P", "m1");
    msg_store_append(MSG_PLANE_ARQ, MSG_DIR_RX, "P", "m2");

    size_t count = 0, len = 0;
    char *snap = msg_store_snapshot(&count, &len);
    TEST_ASSERT_NOT_NULL(snap);
    TEST_ASSERT_EQUAL_size_t(2, count);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_NOT_NULL(strstr(snap, "\"text\":\"m1\""));
    TEST_ASSERT_NOT_NULL(strstr(snap, "\"text\":\"m2\""));
    free(snap);
}

void test_snapshot_empty(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));

    size_t count = 99, len = 99;
    char *snap = msg_store_snapshot(&count, &len);
    TEST_ASSERT_NULL(snap);
    TEST_ASSERT_EQUAL_size_t(0, count);
    TEST_ASSERT_EQUAL_size_t(0, len);
}

void test_file_trimmed_at_shutdown(void)
{
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 3));
    for (int i = 1; i <= 20; i++)
    {
        char m[8];
        snprintf(m, sizeof(m), "m%d", i);
        msg_store_append(MSG_PLANE_ARQ, MSG_DIR_RX, "P", m);
    }

    /* In-memory ring keeps the last 3. */
    TEST_ASSERT_EQUAL_size_t(3, msg_store_count());

    /* During the session the file grows (bounded only by session traffic);
     * a clean shutdown trims it back down to the ring. */
    msg_store_shutdown();

    FILE *f = fopen(TEST_PATH, "r");
    TEST_ASSERT_NOT_NULL(f);
    int lines = 0;
    char line[8192];
    while (fgets(line, sizeof(line), f))
        lines++;
    fclose(f);
    TEST_ASSERT_EQUAL_INT(3, lines);

    /* A reload keeps exactly those last 3, oldest first. */
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 3));
    TEST_ASSERT_EQUAL_size_t(3, msg_store_count());
    TEST_ASSERT_TRUE(msg_store_get(0, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"text\":\"m18\""));
    TEST_ASSERT_TRUE(msg_store_get(2, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"text\":\"m20\""));
}

void test_file_trimmed_at_init(void)
{
    /* A first session writes 10 messages with a cap of 10 (no trim). */
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 10));
    for (int i = 1; i <= 10; i++)
    {
        char m[8];
        snprintf(m, sizeof(m), "m%d", i);
        msg_store_append(MSG_PLANE_ARQ, MSG_DIR_RX, "P", m);
    }
    msg_store_shutdown();

    /* Re-opening with a smaller cap trims the oversized file to the last 3. */
    TEST_ASSERT_EQUAL_INT(0, msg_store_init(TEST_PATH, 3));
    TEST_ASSERT_EQUAL_size_t(3, msg_store_count());
    TEST_ASSERT_TRUE(msg_store_get(0, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"text\":\"m8\""));
    TEST_ASSERT_TRUE(msg_store_get(2, get_buf, sizeof(get_buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(get_buf, "\"text\":\"m10\""));

    /* The file is now 3 lines on disk too. */
    FILE *f = fopen(TEST_PATH, "r");
    TEST_ASSERT_NOT_NULL(f);
    int lines = 0;
    char line[8192];
    while (fgets(line, sizeof(line), f))
        lines++;
    fclose(f);
    TEST_ASSERT_EQUAL_INT(3, lines);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_append_and_get_roundtrip);
    RUN_TEST(test_feed_splits_on_newlines);
    RUN_TEST(test_feed_filters_binary);
    RUN_TEST(test_feed_drops_binary_frame_whose_pieces_look_like_text);
    RUN_TEST(test_feed_rejects_invalid_utf8_line);
    RUN_TEST(test_feed_keeps_utf8_text_even_split_across_chunks);
    RUN_TEST(test_chat_right_after_a_binary_frame_is_kept);
    RUN_TEST(test_feed_filters_overlong_line);
    RUN_TEST(test_persistence_reload);
    RUN_TEST(test_ring_capacity);
    RUN_TEST(test_get_out_of_range);
    RUN_TEST(test_reset_discards_partial_line);
    RUN_TEST(test_snapshot);
    RUN_TEST(test_snapshot_empty);
    RUN_TEST(test_file_trimmed_at_shutdown);
    RUN_TEST(test_file_trimmed_at_init);
    return UNITY_END();
}
