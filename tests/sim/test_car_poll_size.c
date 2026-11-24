/* tests/sim/test_car_poll_size.c -- how many frames a poll asks for when the
 * peer has said it holds little.  Includes carousel.c for its statics.
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
    car_init(&car, &io, 3, 3);
}
void tearDown(void) {}

/* On the rung it already had, a small answer is asked for in the frames it takes. */
void test_small_answer_shrinks_the_poll_on_the_same_rung(void)
{
    int n = 7;
    TEST_ASSERT_EQUAL_INT(3, short_rung(&car, 3, BL_SMALL_BYTES, &n));
    TEST_ASSERT_EQUAL_INT(1, n);
}

/* A medium backlog (N3JXB-K7EK, 2026-11-19: asked for 7 DATAC3 frames) takes
 * fewer than a full keydown. */
void test_medium_answer_shrinks_the_poll(void)
{
    car.loss_est = 0.1;
    int n = 7;
    TEST_ASSERT_EQUAL_INT(3, short_rung(&car, 3, BL_MEDIUM_BYTES, &n));
    TEST_ASSERT_EQUAL_INT(5, n);
}

/* The hint never asks for more than the rung has earned. */
void test_hint_does_not_grow_the_poll(void)
{
    int n = 1;
    TEST_ASSERT_EQUAL_INT(3, short_rung(&car, 3, BL_MEDIUM_BYTES, &n));
    TEST_ASSERT_EQUAL_INT(1, n);
}

/* No hint, no change: a large or unknown backlog keeps the full poll. */
void test_no_hint_keeps_the_poll(void)
{
    int n = 7;
    TEST_ASSERT_EQUAL_INT(3, short_rung(&car, 3, 0, &n));
    TEST_ASSERT_EQUAL_INT(7, n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_small_answer_shrinks_the_poll_on_the_same_rung);
    RUN_TEST(test_medium_answer_shrinks_the_poll);
    RUN_TEST(test_hint_does_not_grow_the_poll);
    RUN_TEST(test_no_hint_keeps_the_poll);
    return UNITY_END();
}
