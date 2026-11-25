/* tests/sim/test_car_short_msg.c -- the rung a short message is asked on when
 * the link has been losing frames, and no probe near a threshold for one.  Includes carousel.c for its statics.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "../../datalink_arq/carousel.c"

static car_t car;

void setUp(void)
{
    car_io_t io;
    memset(&io, 0, sizeof(io));
    car_init(&car, &io, 4, 4);
    car.loss_est = 0.8;
}
void tearDown(void) {}

static void score(int lv, double sent, double lost)
{
    car.rung[lv].sent = sent;
    car.rung[lv].lost = lost;
}

/* N3JXB-K7EK, 2026-11-19: DATAC1 failing, a small backlog goes in one DATAC3
 * frame. */
void test_small_message_leaves_a_failing_rung(void)
{
    score(4, 4.0, 3.0);
    TEST_ASSERT_TRUE(level_delivery(&car, 4) < SHORT_RUNG_FAILING);
    int n = 1;
    TEST_ASSERT_EQUAL_INT(3, short_rung(&car, 4, BL_SMALL_BYTES, &n));
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC3, lmode(&car, 3));
    TEST_ASSERT_EQUAL_INT(1, n);
}

/* On a rung that delivers, the loss margin still prices the lower one. */
void test_small_message_stays_on_a_rung_that_delivers(void)
{
    score(4, 10.0, 1.0);
    TEST_ASSERT_TRUE(level_delivery(&car, 4) >= SHORT_RUNG_FAILING);
    int n = 1;
    TEST_ASSERT_EQUAL_INT(4, short_rung(&car, 4, BL_SMALL_BYTES, &n));
}

/* A rung the SNR rules out is not chosen for being short. */
void test_small_message_skips_a_gated_rung(void)
{
    score(4, 4.0, 3.0);
    car.snr_valid = true;
    car.snr_ema = level_min_db(&car, 3) - SNR_GATE_DB - 1.0;
    int n = 1;
    TEST_ASSERT_NOT_EQUAL(3, short_rung(&car, 4, BL_SMALL_BYTES, &n));
}

/* Past the small bucket the margin applies, failing rung or not. */
void test_larger_message_keeps_the_loss_margin(void)
{
    score(4, 4.0, 3.0);
    int n = 1;
    TEST_ASSERT_EQUAL_INT(4, short_rung(&car, 4, 100, &n));
}

/* A short message is not asked on a rung probed within half the gate of
 * its threshold: QAM16C2 at 12 dB, DATAC17 delivering (sBitx bench,
 * 2026-11-25). */
static void short_backlog_at(float snr)
{
    car.snr_valid = true; car.snr_ema = car.snr_last = snr; car.snr_ms = 1;
    car.rung[5].rounds = 3; score(5, 10.0, 0.0);
    car.peer_short = true; car.peer_backlog = BL_SMALL;
}

void test_short_message_does_not_probe_a_marginal_rung(void)
{
    short_backlog_at(12.0f);
    TEST_ASSERT_EQUAL_INT(5, no_marginal_probe(&car, 6));
}

/* With room to spare, or with a backlog that is not short, the probe stays. */
void test_probe_stays_with_room_or_a_long_backlog(void)
{
    short_backlog_at(14.0f);
    TEST_ASSERT_EQUAL_INT(6, no_marginal_probe(&car, 6));
    short_backlog_at(12.0f);
    car.peer_backlog = BL_LARGE;
    TEST_ASSERT_EQUAL_INT(6, no_marginal_probe(&car, 6));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_small_message_leaves_a_failing_rung);
    RUN_TEST(test_small_message_stays_on_a_rung_that_delivers);
    RUN_TEST(test_small_message_skips_a_gated_rung);
    RUN_TEST(test_larger_message_keeps_the_loss_margin);
    RUN_TEST(test_short_message_does_not_probe_a_marginal_rung);
    RUN_TEST(test_probe_stays_with_room_or_a_long_backlog);
    return UNITY_END();
}
