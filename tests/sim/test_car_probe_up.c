/* tests/sim/test_car_probe_up.c -- a receiver does not probe a faster rung
 * from one that has delivered nothing, and a probe whose poll was lost does
 * not count against the rung.  Includes carousel.c for its statics.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "../../datalink_arq/carousel.c"

static car_t car;
static void no_pattern(void *ctx, int kind) { (void)ctx; (void)kind; }

void setUp(void)
{
    car_io_t io;
    memset(&io, 0, sizeof(io));
    io.pattern = no_pattern;   /* a peer that can send the floor's patterns */
    car_init(&car, &io, 3, 0);
    car.snr_valid = true; car.snr_ema = car.snr_last = 3.1f; car.snr_ms = 1;
}
void tearDown(void) {}

/* One round of one frame on lv, as a receiver scores it. */
static void score(int lv, int seen)
{
    car.poll_level = lv; car.poll_n = 1;
    car.round_lv = lv; car.round_seen = seen; car.round_frames = 1; car.rx_tail = seen > 0;
    rx_score(&car, lv, 1, 10000);
    car.polls++;
}

/* On air (KG9NI to K7EK, 2026-11-23 00:36): K7EK lost its one DATAC3 round at
 * 3.1 dB and asked for DATAC1.  The rung stays where the SNR started it. */
void test_a_lost_first_round_does_not_probe_up(void)
{
    score(3, 0);
    TEST_ASSERT_TRUE(fabs(level_delivery(&car, 3) - 0.5) < 1e-9);   /* the prior alone */
    TEST_ASSERT_TRUE(choose_level(&car, 10000) <= 3);
}

/* A round that came whole still opens the way up, as before. */
void test_a_delivered_round_still_probes_up(void)
{
    car.snr_ema = car.snr_last = 9.0f;
    score(3, 1);
    TEST_ASSERT_TRUE(choose_level(&car, 10000) > 3);
}

/* On air (sBitx bench, 2026-11-25): my poll asking for an untried rung went
 * unheard, and M's REQ about the round I had answered came after my fallback.
 * The rung was never asked: untried still, not 0 of 1. */
void test_a_lost_poll_leaves_the_rung_untried(void)
{
    car.rung[5].probe_at = 7000;             /* held at the fallback (CAR_T_W) */
    car.rx_mk = 9; car.req_mk = 9; car.req_lv = 3; car.req_n = 1;   /* REQ: a round I had */
    s_settle_probe(&car, 10000, true);
    TEST_ASSERT_EQUAL_INT(0, car.rung[5].rounds);
    TEST_ASSERT_TRUE(car.rung[5].probe_at == 0);
}

/* Anything else M sends next: the probe was lost, scored as the fallback did. */
void test_a_lost_probe_is_scored_when_the_next_round_comes(void)
{
    car.rung[5].probe_at = 7000;
    s_settle_probe(&car, 10000, false);
    TEST_ASSERT_EQUAL_INT(1, car.rung[5].rounds);
    TEST_ASSERT_TRUE(fabs(car.rung[5].lost - 1.0) < 1e-9);
}

/* A start misread low at the connect (on air, a CALL at -2.6 dB on a
 * 12-17 dB link) moves up with my reading before anything is measured, and
 * never down below the connect's. */
void test_an_unmeasured_start_follows_a_higher_reading(void)
{
    car.snr_level = 1;
    car.snr_ema = car.snr_last = 15.0f;
    TEST_ASSERT_TRUE(choose_level(&car, 10000) > 1);
    car.snr_level = 4;
    car.snr_ema = car.snr_last = -5.0f;
    TEST_ASSERT_EQUAL_INT(4, choose_level(&car, 10000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_lost_first_round_does_not_probe_up);
    RUN_TEST(test_a_delivered_round_still_probes_up);
    RUN_TEST(test_a_lost_poll_leaves_the_rung_untried);
    RUN_TEST(test_a_lost_probe_is_scored_when_the_next_round_comes);
    RUN_TEST(test_an_unmeasured_start_follows_a_higher_reading);
    return UNITY_END();
}
