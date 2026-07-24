/*
 * Pattern ACK detection DSP test
 *
 * Exercises the Welch-Costas pattern-ACK path used by the ARQ layer in place
 * of the coded DATAC16 ACK: mfsk_pattern_tx() generates the int16 passband
 * tone burst, mfsk_pattern_detect() recovers it from a noisy passband window.
 *
 * Radio/DSP-path scope (per the test-scope preference): asserts the physical
 * false-alarm and detection behaviour, not FSM wiring.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "unity.h"
#include "modem_mfsk.h"

/* arq.h pattern-kind constants (0 = ACK, 1 = BREAK) — mirrored here so the
 * test does not need the whole ARQ header just for two integers. */
#define PAT_ACK   0
#define PAT_BREAK 1

static uint64_t s_rng = 0x12345;
static double urand(void)
{
    s_rng = s_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(s_rng >> 11) / (double)(1ULL << 53);
}

void setUp(void)    { s_rng = 0x12345; }
void tearDown(void) { }

/* Pure noise must never be mistaken for an ACK (false-alarm rejection). */
void test_noise_no_false_ack(void)
{
    int burst = mfsk_pattern_max_tx_samples();
    TEST_ASSERT_TRUE(burst > 0);

    int L = burst * 3;
    int16_t *pb = (int16_t *)malloc((size_t)L * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pb);

    /* Run many independent noise realisations; none may detect. */
    int false_alarms = 0;
    for (int trial = 0; trial < 40; trial++)
    {
        for (int i = 0; i < L; i++)
            pb[i] = (int16_t)((urand() - 0.5) * 4000.0);  /* moderate noise */
        int is_break = -1;
        if (mfsk_pattern_detect(pb, L, &is_break))
            false_alarms++;
    }
    TEST_ASSERT_EQUAL_INT(0, false_alarms);
    free(pb);
}

/* A real ACK burst planted in a noise window must detect as ACK (not break). */
void test_real_ack_detects(void)
{
    int burst = mfsk_pattern_max_tx_samples();
    int16_t *tone = (int16_t *)malloc((size_t)burst * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(tone);
    int n = mfsk_pattern_tx(tone, PAT_ACK);
    TEST_ASSERT_TRUE(n > 0);

    int gap = burst;                 /* lead-in noise */
    int L   = gap + n + burst;       /* + trailing slack */
    int16_t *pb = (int16_t *)malloc((size_t)L * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pb);
    for (int i = 0; i < L; i++)
        pb[i] = (int16_t)((urand() - 0.5) * 200.0);   /* low noise */
    for (int i = 0; i < n; i++)
        pb[gap + i] = (int16_t)(pb[gap + i] + tone[i]);

    int is_break = -1;
    int hit = mfsk_pattern_detect(pb, L, &is_break);
    TEST_ASSERT_TRUE(hit);
    TEST_ASSERT_EQUAL_INT(0, is_break);   /* ACK, not break */

    free(tone);
    free(pb);
}

/* A real BREAK (ACK+TURN) burst must detect and be flagged as break. */
void test_real_break_detects(void)
{
    int burst = mfsk_pattern_max_tx_samples();
    int16_t *tone = (int16_t *)malloc((size_t)burst * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(tone);
    int n = mfsk_pattern_tx(tone, PAT_BREAK);
    TEST_ASSERT_TRUE(n > 0);

    int gap = burst;
    int L   = gap + n + burst;
    int16_t *pb = (int16_t *)malloc((size_t)L * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pb);
    for (int i = 0; i < L; i++)
        pb[i] = (int16_t)((urand() - 0.5) * 200.0);
    for (int i = 0; i < n; i++)
        pb[gap + i] = (int16_t)(pb[gap + i] + tone[i]);

    int is_break = -1;
    int hit = mfsk_pattern_detect(pb, L, &is_break);
    TEST_ASSERT_TRUE(hit);
    TEST_ASSERT_EQUAL_INT(1, is_break);   /* break, not plain ACK */

    free(tone);
    free(pb);
}

/* Fast windowed ACK: an epoch-tagged pattern (base + 1 epoch symbol) must
 * round-trip the 2-bit epoch AND the break bit, for every (epoch, break) combo,
 * planted in low noise. */
void test_epoch_tagged_roundtrip(void)
{
    int burst = mfsk_pattern_max_tx_samples();
    for (int epoch = 0; epoch < 4; epoch++)
        for (int brk = 0; brk < 2; brk++)
        {
            int kind = MFSK_PATTERN_TAGGED | (epoch << 1) | brk;
            int16_t *tone = (int16_t *)malloc((size_t)burst * sizeof(int16_t));
            int n = mfsk_pattern_tx(tone, kind);
            TEST_ASSERT_TRUE(n > 0);

            int gap = burst, L = gap + n + burst;
            int16_t *pb = (int16_t *)malloc((size_t)L * sizeof(int16_t));
            for (int i = 0; i < L; i++)
                pb[i] = (int16_t)((urand() - 0.5) * 200.0);
            for (int i = 0; i < n; i++)
                pb[gap + i] = (int16_t)(pb[gap + i] + tone[i]);

            int got = -1;
            TEST_ASSERT_TRUE(mfsk_pattern_detect(pb, L, &got));
            TEST_ASSERT_TRUE_MESSAGE(got & MFSK_PATTERN_TAGGED, "epoch symbol not detected");
            TEST_ASSERT_EQUAL_INT(brk,   got & 1);
            TEST_ASSERT_EQUAL_INT(epoch, (got >> 1) & 3);
            free(tone); free(pb);
        }
}

/* A BARE pattern (no epoch symbol) must NOT be misread as epoch-tagged: the
 * trailing noise after the 16 base symbols must stay below the epoch-present
 * threshold (fail-safe — a bare fringe ACK keeps its exact meaning). */
void test_bare_not_misread_as_tagged(void)
{
    int burst = mfsk_pattern_max_tx_samples();
    for (int brk = 0; brk < 2; brk++)
    {
        int16_t *tone = (int16_t *)malloc((size_t)burst * sizeof(int16_t));
        int n = mfsk_pattern_tx(tone, brk);          /* bare: 0 or 1 */
        int gap = burst, L = gap + n + burst;
        int16_t *pb = (int16_t *)malloc((size_t)L * sizeof(int16_t));
        for (int i = 0; i < L; i++)
            pb[i] = (int16_t)((urand() - 0.5) * 200.0);
        for (int i = 0; i < n; i++)
            pb[gap + i] = (int16_t)(pb[gap + i] + tone[i]);

        int got = -1;
        TEST_ASSERT_TRUE(mfsk_pattern_detect(pb, L, &got));
        TEST_ASSERT_FALSE_MESSAGE(got & MFSK_PATTERN_TAGGED, "bare pattern misread as tagged");
        TEST_ASSERT_EQUAL_INT(brk, got & 1);
        free(tone); free(pb);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_noise_no_false_ack);
    RUN_TEST(test_real_ack_detects);
    RUN_TEST(test_real_break_detects);
    RUN_TEST(test_epoch_tagged_roundtrip);
    RUN_TEST(test_bare_not_misread_as_tagged);
    return UNITY_END();
}
