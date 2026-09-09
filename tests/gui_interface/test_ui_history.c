/* UI history frame builder unit tests
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_history.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_empty(void)
{
    char *out = ui_history_frame_build("", 0, 0);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"history\",\"messages\":[]}", out);
    free(out);
}

void test_one_message(void)
{
    const char *snap = "{\"plane\":\"arq\",\"text\":\"hi\"}\n";
    char *out = ui_history_frame_build(snap, 1, strlen(snap));
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"history\",\"messages\":[{\"plane\":\"arq\",\"text\":\"hi\"}]}",
        out);
    free(out);
}

void test_many_messages(void)
{
    const char *snap = "{\"a\":1}\n{\"b\":2}\n{\"c\":3}\n";
    char *out = ui_history_frame_build(snap, 3, strlen(snap));
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"history\",\"messages\":[{\"a\":1},{\"b\":2},{\"c\":3}]}",
        out);
    free(out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty);
    RUN_TEST(test_one_message);
    RUN_TEST(test_many_messages);
    return UNITY_END();
}
