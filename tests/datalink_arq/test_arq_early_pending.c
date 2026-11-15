/* Early PENDING decision logic (datalink_arq/arq_early_pending.c): a pure
 * state machine on a caller-supplied clock, so each scenario steps it the way
 * arq.c's event loop does, every 500 ms or sooner.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unity.h"
#include "../../datalink_arq/arq_early_pending.h"

static arq_ep_state_t s;

void setUp(void)    { arq_ep_init(&s); }
void tearDown(void) { }

/* Hold a lock that started at lock_start from t0 to t1 (inclusive), stepping
 * every 250 ms; returns the first action other than NONE, or NONE. */
static arq_ep_action_t hold(uint64_t t0, uint64_t t1, uint64_t lock_start, uint64_t *at)
{
    for (uint64_t t = t0; t <= t1; t += 250)
    {
        arq_ep_action_t a = arq_ep_step(&s, t, true, true, t - lock_start);
        if (a != ARQ_EP_NONE)
        {
            if (at) *at = t;
            return a;
        }
    }
    return ARQ_EP_NONE;
}

static arq_ep_action_t idle(uint64_t t0, uint64_t t1, uint64_t *at)
{
    for (uint64_t t = t0; t <= t1; t += 250)
    {
        arq_ep_action_t a = arq_ep_step(&s, t, true, false, 0);
        if (a != ARQ_EP_NONE)
        {
            if (at) *at = t;
            return a;
        }
    }
    return ARQ_EP_NONE;
}

/* Nothing while nothing is locked, and nothing while not eligible. */
void test_quiet_channel_sends_nothing(void)
{
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, idle(1000, 60000, NULL));
    for (uint64_t t = 1000; t < 10000; t += 250)
        TEST_ASSERT_EQUAL(ARQ_EP_NONE, arq_ep_step(&s, t, false, true, t - 1000));
    TEST_ASSERT_EQUAL_UINT(0, s.n_sent);
}

/* A momentary trial sync is not a frame. */
void test_short_lock_sends_nothing(void)
{
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, hold(1000, 1000 + ARQ_EP_MIN_LOCK_MS - 250, 1000, NULL));
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, idle(1500, 10000, NULL));
}

/* A DATAC16 CALL for us: PENDING 0.5 s in, then the FSM takes it. */
void test_call_for_us_is_promoted_without_cancel(void)
{
    uint64_t at = 0;
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_PENDING, hold(1000, 5000, 1000, &at));
    TEST_ASSERT_EQUAL_UINT64(1000 + ARQ_EP_MIN_LOCK_MS, at);
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, hold(at + 250, 4740, 1000, NULL));
    /* decoded at 4.74 s; the next pass sees the FSM in ACCEPTING */
    arq_ep_promoted(&s);
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, arq_ep_step(&s, 5000, false, false, 0));
    TEST_ASSERT_EQUAL_UINT(1, s.n_promoted);
    TEST_ASSERT_EQUAL_UINT(0, s.n_cancelled);
}

/* A frame that is not a CALL for us: CANCELPENDING ARQ_EP_GRACE_MS after the
 * lock ends. */
void test_frame_not_for_us_is_cancelled_after_grace(void)
{
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_PENDING, hold(1000, 4740, 1000, NULL));
    uint64_t at = 0;
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_CANCEL, idle(5000, 20000, &at));
    TEST_ASSERT_EQUAL_UINT64(5000 + ARQ_EP_GRACE_MS, at);
    TEST_ASSERT_EQUAL_UINT(1, s.n_cancelled);
    TEST_ASSERT_FALSE(s.active);
}

/* An MFSK CALL is 13.5 s: the PENDING holds through all of it. */
void test_long_mfsk_lock_holds(void)
{
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_PENDING, hold(1000, 2000, 1000, NULL));
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, hold(2000, 1000 + 27000, 1000, NULL));
    TEST_ASSERT_TRUE(s.active);
}

/* A lock that never ends is not a CALL: cancelled past the cap, and not
 * re-armed by the same lock. */
void test_endless_lock_cancelled_at_cap_once(void)
{
    uint64_t at = 0;
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_PENDING, hold(1000, 2000, 1000, NULL));
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_CANCEL, hold(2000, 60000, 1000, &at));
    TEST_ASSERT_TRUE(at - 1000 > ARQ_EP_MAX_LOCK_MS);
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, hold(at + 250, 120000, 1000, NULL));
    /* a new lock after it ends is armed again */
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, idle(120250, 121000, NULL));
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_PENDING, hold(121000, 123000, 121000, NULL));
}

/* A second frame that starts during the grace period keeps the PENDING up:
 * a corrupt DATAC16 CALL followed by the caller's next one. */
void test_new_lock_during_grace_keeps_pending(void)
{
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_PENDING, hold(1000, 4740, 1000, NULL));
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, idle(5000, 6000, NULL));
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, hold(6250, 9000, 6250, NULL));
    TEST_ASSERT_TRUE(s.active);
    TEST_ASSERT_EQUAL_UINT(1, s.n_sent);
}

/* The poll can miss the gap between two locks; a younger lock is still a new
 * one, so a lock spent at the cap does not block it. */
void test_younger_lock_counts_as_new_after_cap(void)
{
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_PENDING, hold(1000, 2000, 1000, NULL));
    uint64_t at = 0;
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_CANCEL, hold(2000, 60000, 1000, &at));
    uint64_t t = at + 250;
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_PENDING, hold(t, t + 2000, t, NULL));
}

/* Past the false-alarm budget, locks send nothing until the oldest ages out. */
void test_false_alarm_budget(void)
{
    uint64_t t = 1000;
    for (int i = 0; i < ARQ_EP_FALSE_BUDGET; i++)
    {
        TEST_ASSERT_EQUAL(ARQ_EP_SEND_PENDING, hold(t, t + 4000, t, NULL));
        uint64_t at = 0;
        TEST_ASSERT_EQUAL(ARQ_EP_SEND_CANCEL, idle(t + 4000, t + 10000, &at));
        t = at + 1000;
    }
    uint64_t first_cancel = 1000 + 4000 + ARQ_EP_GRACE_MS;
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, hold(t, t + 4000, t, NULL));
    TEST_ASSERT_EQUAL_UINT(1, s.n_suppressed);
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, idle(t + 4250, first_cancel + ARQ_EP_FALSE_WINDOW_MS - 250, NULL));
    t = first_cancel + ARQ_EP_FALSE_WINDOW_MS;
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_PENDING, hold(t, t + 2000, t, NULL));
}

/* The host leaving LISTENING (a CONNECT of its own, LISTEN OFF) drops an
 * outstanding early PENDING silently: the host has the radio back. */
void test_ineligible_drops_silently(void)
{
    TEST_ASSERT_EQUAL(ARQ_EP_SEND_PENDING, hold(1000, 2000, 1000, NULL));
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, arq_ep_step(&s, 2250, false, false, 0));
    TEST_ASSERT_FALSE(s.active);
    TEST_ASSERT_EQUAL(ARQ_EP_NONE, idle(2500, 10000, NULL));
    TEST_ASSERT_EQUAL_UINT(0, s.n_cancelled);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_quiet_channel_sends_nothing);
    RUN_TEST(test_short_lock_sends_nothing);
    RUN_TEST(test_call_for_us_is_promoted_without_cancel);
    RUN_TEST(test_frame_not_for_us_is_cancelled_after_grace);
    RUN_TEST(test_long_mfsk_lock_holds);
    RUN_TEST(test_endless_lock_cancelled_at_cap_once);
    RUN_TEST(test_new_lock_during_grace_keeps_pending);
    RUN_TEST(test_younger_lock_counts_as_new_after_cap);
    RUN_TEST(test_false_alarm_budget);
    RUN_TEST(test_ineligible_drops_silently);
    return UNITY_END();
}
