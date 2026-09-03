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
#include "mfsk.h"
#include "mfsk_sync.h"
#include "mfsk_ofdm.h"
#include <complex.h>

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

/* mfsk_detect_patterns() scores several tone lists in one pass so the RX loop
 * stops paying for the same FFTs twice.  That is only safe if it is exactly
 * equivalent to the per-list calls it replaces -- a scoring shortcut that
 * quietly changed a match count would move the ACK detection threshold without
 * anything else noticing.  Assert bit-identical scores AND positions. */
void test_detect_patterns_matches_per_list_calls(void)
{
    mfsk_t m;
    ofdm_frame_t o;
    mfsk_init(&m, 32, 50, 1);
    ofdm_frame_init(&o, 256, 50, 0.25, 0);

    int ns   = m.ack_pattern_nsymb;
    int len  = ns * ofdm_frame_nofdm(&o) * 2;   /* room to slide */
    double complex *rx = malloc(sizeof(double complex) * (size_t)len);
    TEST_ASSERT_NOT_NULL(rx);

    /* Several independent buffers, so the comparison is not one lucky draw. */
    for (int trial = 0; trial < 3; trial++)
    {
        for (int i = 0; i < len; i++)
            rx[i] = (urand() - 0.5) + (urand() - 0.5) * I;

        int pos_a = -2, pos_b = -2;
        int sa = mfsk_detect_pattern(&m, &o, rx, len, m.ack_tones,
                                     m.ack_pattern_len, ns, &pos_a);
        int sb = mfsk_detect_pattern(&m, &o, rx, len, m.break_tones,
                                     m.ack_pattern_len, ns, &pos_b);

        const int *lists[2] = { m.ack_tones, m.break_tones };
        int scores[2] = { -1, -1 }, pos[2] = { -2, -2 };
        mfsk_detect_patterns(&m, &o, rx, len, lists, 2,
                             m.ack_pattern_len, ns, scores, pos);

        TEST_ASSERT_EQUAL_INT(sa, scores[0]);
        TEST_ASSERT_EQUAL_INT(sb, scores[1]);
        TEST_ASSERT_EQUAL_INT(pos_a, pos[0]);
        TEST_ASSERT_EQUAL_INT(pos_b, pos[1]);
    }

    free(rx);
}

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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_detect_patterns_matches_per_list_calls);
    RUN_TEST(test_noise_no_false_ack);
    RUN_TEST(test_real_ack_detects);
    RUN_TEST(test_real_break_detects);
    return UNITY_END();
}
