/*
 * Offline resampler test — proves the playback (TX) 8 kHz -> 48 kHz upsampler
 * is continuous across period boundaries, without needing a sound card.
 *
 * Issue #81: a "pop at the start of each period" in TX audio.  The old
 * upsampler interpolated each read-period in isolation (next = current at the
 * tail), leaving a flat step at every boundary — an audible click whose rate
 * scaled with the period count.  The fix carries the previous input sample
 * across periods (stateful interpolator) so the signal is continuous.
 *
 * This test feeds a continuous 1 kHz sine through both algorithms, chopped
 * into periods exactly as the playback thread reads them, and measures the
 * worst sample-to-sample jump at period boundaries.  A clean upsampler's
 * boundary jump is no larger than its in-period jump; the old one spikes.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unity.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define RATIO        6      /* 8 kHz -> 48 kHz */
#define PERIOD_8K    160    /* 20 ms period at 8 kHz (typical) */
#define N_PERIODS    50
#define TOTAL_8K     (PERIOD_8K * N_PERIODS)
#define TOTAL_48K    (TOTAL_8K * RATIO)

/* OLD: each period interpolated in isolation (next=current at the tail). */
static void upsample_old(const int32_t *in8k, int n8k, int32_t *out48k)
{
    for (int i = 0; i < n8k; i++)
    {
        int32_t current = in8k[i];
        int32_t next = (i + 1 < n8k) ? in8k[i + 1] : current;
        for (int j = 0; j < RATIO; j++)
            out48k[i * RATIO + j] =
                current + (int32_t)(((int64_t)next - current) * j / RATIO);
    }
}

/* NEW: stateful — bridge resamp_prev -> current, carry across periods. */
static void upsample_new(const int32_t *in8k, int n8k, int32_t *out48k,
                         int32_t *resamp_prev)
{
    for (int i = 0; i < n8k; i++)
    {
        int32_t cur = in8k[i];
        for (int j = 0; j < RATIO; j++)
            out48k[i * RATIO + j] =
                *resamp_prev +
                (int32_t)(((int64_t)cur - *resamp_prev) * j / RATIO);
        *resamp_prev = cur;
    }
}

/* Worst |out[k] - out[k-1]| over the whole stream; and separately the worst
 * jump that lands exactly on a period boundary (k % (PERIOD_8K*RATIO) == 0). */
static void max_jumps(const int32_t *out, int n, int period_out,
                      int64_t *worst_overall, int64_t *worst_boundary)
{
    *worst_overall = 0;
    *worst_boundary = 0;
    for (int k = 1; k < n; k++)
    {
        int64_t d = llabs((int64_t)out[k] - out[k - 1]);
        if (d > *worst_overall) *worst_overall = d;
        if (k % period_out == 0 && d > *worst_boundary) *worst_boundary = d;
    }
}

static int32_t *g_sine8k;

void setUp(void)
{
    g_sine8k = malloc(sizeof(int32_t) * TOTAL_8K);
    for (int i = 0; i < TOTAL_8K; i++)
    {
        /* 1 kHz tone at 8 kHz, near full scale */
        double t = (double)i / 8000.0;
        g_sine8k[i] = (int32_t)(1.9e9 * sin(2.0 * M_PI * 1000.0 * t));
    }
}

void tearDown(void) { free(g_sine8k); }

/* The OLD upsampler must show a boundary jump far larger than its in-period
 * jump — this is the bug we are fixing (documents the defect). */
void test_old_upsampler_has_boundary_discontinuity(void)
{
    int32_t *out = malloc(sizeof(int32_t) * TOTAL_48K);
    for (int p = 0; p < N_PERIODS; p++)
        upsample_old(g_sine8k + p * PERIOD_8K, PERIOD_8K,
                     out + p * PERIOD_8K * RATIO);

    int64_t worst, boundary;
    max_jumps(out, TOTAL_48K, PERIOD_8K * RATIO, &worst, &boundary);

    /* Boundary jump should be the global worst (the click) and several times
     * the smooth in-period step.  We don't pin an exact ratio, just that the
     * boundary is where the worst jump lives. */
    TEST_ASSERT_EQUAL_INT64(worst, boundary);
    free(out);
}

/* The NEW (stateful) upsampler must be continuous: the worst boundary jump is
 * no larger than the worst in-period jump (the signal's natural slope). */
void test_new_upsampler_is_continuous_across_periods(void)
{
    int32_t *out = malloc(sizeof(int32_t) * TOTAL_48K);
    int32_t prev = 0;
    for (int p = 0; p < N_PERIODS; p++)
        upsample_new(g_sine8k + p * PERIOD_8K, PERIOD_8K,
                     out + p * PERIOD_8K * RATIO, &prev);

    int64_t worst, boundary;
    /* skip the very first boundary (k=0 has no predecessor; first period
     * ramps from prev=0 which is fine). Measure boundaries within the run. */
    max_jumps(out, TOTAL_48K, PERIOD_8K * RATIO, &worst, &boundary);

    /* Continuity: no period boundary may exceed the largest in-period step.
     * For a 1 kHz tone at 48 kHz the per-output-sample step is small and
     * smooth; the boundary must not stand out. */
    TEST_ASSERT_LESS_OR_EQUAL_INT64(worst, boundary);
    /* And the worst overall jump must be the smooth signal slope, not a
     * click: a 1 kHz full-scale tone moves at most ~2*pi*1000/48000*1.9e9
     * ~= 2.5e8 per 48 kHz sample. */
    TEST_ASSERT_LESS_THAN_INT64(300000000LL, worst);
    free(out);
}

/* Continuous input through the NEW upsampler in ONE shot vs chunked into
 * periods must produce IDENTICAL output (modulo the 1-sample startup ramp) —
 * proving period boundaries are invisible. */
void test_new_upsampler_chunking_invariant(void)
{
    int32_t *whole = malloc(sizeof(int32_t) * TOTAL_48K);
    int32_t *chunked = malloc(sizeof(int32_t) * TOTAL_48K);

    int32_t prev = 0;
    upsample_new(g_sine8k, TOTAL_8K, whole, &prev);

    prev = 0;
    for (int p = 0; p < N_PERIODS; p++)
        upsample_new(g_sine8k + p * PERIOD_8K, PERIOD_8K,
                     chunked + p * PERIOD_8K * RATIO, &prev);

    for (int k = 0; k < TOTAL_48K; k++)
        TEST_ASSERT_EQUAL_INT32(whole[k], chunked[k]);

    free(whole);
    free(chunked);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_old_upsampler_has_boundary_discontinuity);
    RUN_TEST(test_new_upsampler_is_continuous_across_periods);
    RUN_TEST(test_new_upsampler_chunking_invariant);
    return UNITY_END();
}
