/* Unit tests for the ARQ→TNC notification seam.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include "unity.h"
#include "fff.h"
#include "arq_tnc.h"

DEFINE_FFF_GLOBALS;

FAKE_VOID_FUNC(fake_send_pending);
FAKE_VOID_FUNC(fake_send_cancelpending);
FAKE_VOID_FUNC(fake_send_connected);
FAKE_VOID_FUNC(fake_send_cqframe, const char *, int);
FAKE_VOID_FUNC(fake_send_disconnected);
FAKE_VOID_FUNC(fake_send_buffer, uint32_t);

static const arq_tnc_callbacks_t test_cbs = {
    .send_pending       = fake_send_pending,
    .send_cancelpending = fake_send_cancelpending,
    .send_connected     = fake_send_connected,
    .send_cqframe       = fake_send_cqframe,
    .send_disconnected  = fake_send_disconnected,
    .send_buffer        = fake_send_buffer,
};

void setUp(void)
{
    RESET_FAKE(fake_send_pending);
    RESET_FAKE(fake_send_cancelpending);
    RESET_FAKE(fake_send_connected);
    RESET_FAKE(fake_send_cqframe);
    RESET_FAKE(fake_send_disconnected);
    RESET_FAKE(fake_send_buffer);
    FFF_RESET_HISTORY();
    arq_set_tnc_callbacks(NULL);   /* start each test unregistered */
}

void tearDown(void) {}

/* Before registration, every invoker is a silent no-op (no crash). */
void test_invokers_are_noop_before_registration(void)
{
    arq_tnc_send_connected();
    arq_tnc_send_pending();
    arq_tnc_send_cancelpending();
    arq_tnc_send_disconnected();
    arq_tnc_send_buffer(42);
    arq_tnc_send_cqframe("W1AW", 2300);
    TEST_ASSERT_EQUAL_INT(0, fake_send_connected_fake.call_count);
    TEST_ASSERT_EQUAL_INT(0, fake_send_cqframe_fake.call_count);
}

/* After registration, invokers dispatch to the registered callbacks. */
void test_invokers_dispatch_after_registration(void)
{
    arq_set_tnc_callbacks(&test_cbs);

    arq_tnc_send_connected();
    arq_tnc_send_pending();
    arq_tnc_send_cancelpending();
    arq_tnc_send_disconnected();

    TEST_ASSERT_EQUAL_INT(1, fake_send_connected_fake.call_count);
    TEST_ASSERT_EQUAL_INT(1, fake_send_pending_fake.call_count);
    TEST_ASSERT_EQUAL_INT(1, fake_send_cancelpending_fake.call_count);
    TEST_ASSERT_EQUAL_INT(1, fake_send_disconnected_fake.call_count);
}

/* Arguments are forwarded intact. */
void test_invokers_forward_arguments(void)
{
    arq_set_tnc_callbacks(&test_cbs);

    arq_tnc_send_buffer(1234);
    arq_tnc_send_cqframe("KM6LYW", 500);

    TEST_ASSERT_EQUAL_UINT32(1234, fake_send_buffer_fake.arg0_val);
    TEST_ASSERT_EQUAL_STRING("KM6LYW", fake_send_cqframe_fake.arg0_val);
    TEST_ASSERT_EQUAL_INT(500, fake_send_cqframe_fake.arg1_val);
}

/* A partially-populated table (some NULL fields) does not crash. */
void test_partial_table_null_fields_are_safe(void)
{
    static const arq_tnc_callbacks_t partial = { .send_connected = fake_send_connected };
    arq_set_tnc_callbacks(&partial);

    arq_tnc_send_connected();    /* fires */
    arq_tnc_send_pending();      /* NULL field — no-op, no crash */

    TEST_ASSERT_EQUAL_INT(1, fake_send_connected_fake.call_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_invokers_are_noop_before_registration);
    RUN_TEST(test_invokers_dispatch_after_registration);
    RUN_TEST(test_invokers_forward_arguments);
    RUN_TEST(test_partial_table_null_fields_are_safe);
    return UNITY_END();
}
