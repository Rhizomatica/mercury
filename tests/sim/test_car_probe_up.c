/* tests/sim/test_car_probe_up.c -- a receiver does not probe a faster rung
 * from one that has delivered nothing.  Includes carousel.c for its statics.
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_lost_first_round_does_not_probe_up);
    RUN_TEST(test_a_delivered_round_still_probes_up);
    return UNITY_END();
}
