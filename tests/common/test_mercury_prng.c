/* Tests for common/mercury_prng.c
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The point of this generator is that a seed means the SAME channel on every
 * platform, so the tests that matter are the ones pinning actual output values
 * rather than statistical properties.
 */
#include "unity.h"

#include <math.h>
#include <string.h>

#include "common/mercury_prng.h"

void setUp(void) {}
void tearDown(void) {}

/* The first ten outputs of glibc's random() for srandom(1).  Hardcoded, not
 * computed: comparing against the host's random() would only prove we match
 * THIS libc, which is exactly the dependency this module exists to remove.
 * These values must never change -- a pinned-seed measurement recorded in a
 * commit message has to stay replayable. */
static const uint32_t glibc_seed1[10] = {
    1804289383u,  846930886u, 1681692777u, 1714636915u, 1957747793u,
     424238335u,  719885386u, 1649760492u,  596516649u, 1189641421u
};

void test_seed1_matches_the_reference_stream(void)
{
    mercury_prng_t p;
    mercury_prng_seed(&p, 1);
    for (int i = 0; i < 10; i++)
        TEST_ASSERT_EQUAL_UINT32(glibc_seed1[i], mercury_prng_u31(&p));
}

/* glibc maps seed 0 to 1; an all-zero additive state emits zeros forever. */
void test_seed_zero_is_mapped_to_one(void)
{
    mercury_prng_t a, b;
    mercury_prng_seed(&a, 0);
    mercury_prng_seed(&b, 1);
    for (int i = 0; i < 100; i++)
        TEST_ASSERT_EQUAL_UINT32(mercury_prng_u31(&b), mercury_prng_u31(&a));
}

void test_same_seed_replays_exactly(void)
{
    mercury_prng_t a, b;
    mercury_prng_seed(&a, 20260818);
    mercury_prng_seed(&b, 20260818);
    for (int i = 0; i < 1000; i++)
        TEST_ASSERT_EQUAL_UINT32(mercury_prng_u31(&b), mercury_prng_u31(&a));
}

void test_different_seeds_diverge(void)
{
    mercury_prng_t a, b;
    int same = 0;
    mercury_prng_seed(&a, 1001);
    mercury_prng_seed(&b, 1002);
    for (int i = 0; i < 1000; i++)
        if (mercury_prng_u31(&a) == mercury_prng_u31(&b)) same++;
    /* A handful of coincidental collisions is fine; lockstep is not. */
    TEST_ASSERT_LESS_THAN_INT(10, same);
}

/* Two instances must be independent.  This is the property rand() cannot
 * offer, and its absence is how an unrelated caller silently reorders the
 * draws a channel model sees. */
void test_instances_do_not_share_state(void)
{
    mercury_prng_t a, b, ref;
    mercury_prng_seed(&a, 7);
    mercury_prng_seed(&ref, 7);
    mercury_prng_seed(&b, 999);

    for (int i = 0; i < 500; i++)
    {
        (void)mercury_prng_u31(&b);          /* interleave a foreign consumer */
        TEST_ASSERT_EQUAL_UINT32(mercury_prng_u31(&ref), mercury_prng_u31(&a));
    }
}

void test_uniform_stays_in_range(void)
{
    mercury_prng_t p;
    mercury_prng_seed(&p, 12345);
    for (int i = 0; i < 20000; i++)
    {
        float u = mercury_prng_uniform(&p);
        TEST_ASSERT_TRUE(u >= 0.0f && u <= 1.0f);
    }
}

/* The port must not perturb the channel.  Reproduces the exact expression
 * common/watterson.c used before this module existed -- (float)rand()/RAND_MAX
 * on glibc's TYPE_3 stream -- and requires bit-equality, because the Doppler
 * IIR's near-unit-circle poles turn a last-bit difference into a completely
 * different fade. */
void test_uniform_is_bit_exact_with_the_expression_it_replaced(void)
{
    mercury_prng_t raw, uni;
    mercury_prng_seed(&raw, 1);
    mercury_prng_seed(&uni, 1);

    for (int i = 0; i < 5000; i++)
    {
        float expect = (float)mercury_prng_u31(&raw) / (float)2147483647;
        float got    = mercury_prng_uniform(&uni);
        /* Bit-equality, not near-equality. */
        TEST_ASSERT_EQUAL_MEMORY(&expect, &got, sizeof(float));
    }
}

/* Enough to catch a broken Box-Muller (wrong scaling, or cos/sin swapped so
 * the variance halves), which is what feeds the channel's tap gains. */
void test_gaussian_is_zero_mean_unit_variance(void)
{
    mercury_prng_t p;
    double sum = 0.0, sumsq = 0.0;
    const int N = 200000;

    mercury_prng_seed(&p, 4242);
    for (int i = 0; i < N; i++)
    {
        double g = mercury_prng_gaussian(&p);
        sum += g;
        sumsq += g * g;
    }
    double mean = sum / N;
    double var  = sumsq / N - mean * mean;

    TEST_ASSERT_TRUE(fabs(mean) < 0.02);
    TEST_ASSERT_TRUE(fabs(var - 1.0) < 0.05);
}

/* The struct carries no pointers, so it can be copied to fork a stream. */
void test_state_is_trivially_copyable(void)
{
    mercury_prng_t a, copy;
    mercury_prng_seed(&a, 555);
    for (int i = 0; i < 37; i++) (void)mercury_prng_u31(&a);

    memcpy(&copy, &a, sizeof(copy));
    for (int i = 0; i < 200; i++)
        TEST_ASSERT_EQUAL_UINT32(mercury_prng_u31(&a), mercury_prng_u31(&copy));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_seed1_matches_the_reference_stream);
    RUN_TEST(test_seed_zero_is_mapped_to_one);
    RUN_TEST(test_same_seed_replays_exactly);
    RUN_TEST(test_different_seeds_diverge);
    RUN_TEST(test_instances_do_not_share_state);
    RUN_TEST(test_uniform_stays_in_range);
    RUN_TEST(test_uniform_is_bit_exact_with_the_expression_it_replaced);
    RUN_TEST(test_gaussian_is_zero_mean_unit_variance);
    RUN_TEST(test_state_is_trivially_copyable);
    return UNITY_END();
}
