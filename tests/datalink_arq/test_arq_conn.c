/*
 * arq_conn accessor unit tests (single-threaded correctness).
 *
 * These do NOT test the locking itself (that is covered by the ThreadSanitizer
 * integration run); they pin the round-trip semantics of the new accessors so a
 * refactor cannot silently break them.
 *
 * Copyright (C) 2025 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include "unity.h"
#include "arq.h"
#include "arq_conn_accessors.h"

void setUp(void)    { arq_conn_test_reset(); }
void tearDown(void) { }

void test_trx_set_get_roundtrip(void)
{
    arq_set_trx(1);
    TEST_ASSERT_EQUAL_INT(1, arq_get_trx());
    arq_set_trx(0);
    TEST_ASSERT_EQUAL_INT(0, arq_get_trx());
}

void test_get_calls_empty_yields_empty_strings(void)
{
    char my[16] = "x", src[16] = "x", dst[16] = "x";
    arq_conn_get_calls(my, src, dst, sizeof(my));
    TEST_ASSERT_EQUAL_STRING("", my);
    TEST_ASSERT_EQUAL_STRING("", src);
    TEST_ASSERT_EQUAL_STRING("", dst);
}

void test_get_calls_null_args_are_skipped(void)
{
    /* Must not crash when any pointer is NULL. */
    arq_conn_get_calls(NULL, NULL, NULL, 16);
    TEST_PASS();
}

void test_get_calls_truncates_to_bufsz(void)
{
    /* Seed a full-width callsign, read into a short buffer. */
    extern arq_info arq_conn;
    snprintf(arq_conn.my_call_sign, sizeof(arq_conn.my_call_sign), "ABCDEFGHIJKLMNO");
    char my[4];
    arq_conn_get_calls(my, NULL, NULL, sizeof(my));
    TEST_ASSERT_EQUAL_size_t(3, strlen(my));   /* 3 chars + NUL */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_trx_set_get_roundtrip);
    RUN_TEST(test_get_calls_empty_yields_empty_strings);
    RUN_TEST(test_get_calls_null_args_are_skipped);
    RUN_TEST(test_get_calls_truncates_to_bufsz);
    return UNITY_END();
}
