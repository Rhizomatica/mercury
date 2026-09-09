/* Deterministic tests for the tone-pattern signalling channel, at the
 * passband int16 interface the ARQ layer actually calls.
 *
 * tests/modem/test_mfsk.c covers the correlator on baseband bins.  This covers
 * the layer above it -- modulation to passband, downmix, detection -- because
 * that is what a caller gets, and a round trip through it is the only thing
 * that proves the two ends agree on the geometry.
 *
 * What is asserted, and why each one is here:
 *
 *   round trip       a generated pattern is detected, and ACK is told from
 *                    ACK+TURN.  The discrimination bit is the only bit this
 *                    channel carries; getting it wrong is a protocol error,
 *                    not a lost frame.
 *   noise            noise alone is never accepted.  A false ACK is worse
 *                    than a missed one: the sender believes a frame landed
 *                    and moves on, leaving the receiver a hole.
 *   silence          all-zero input is not a match (degenerate correlation).
 *   offset           the pattern is found when it does not start at sample 0,
 *                    which is the only case that ever occurs live -- the RX
 *                    slides a window over a stream, it does not get handed an
 *                    aligned burst.
 *   short buffer     less than one pattern is refused rather than scored on
 *                    whatever happens to be in the caller's memory.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "pattern_ack.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* deterministic U(0,1) */
static unsigned long s_rng = 88172645463325252ULL;
static double urand(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return ((s_rng >> 11) + 1.0) / ((1ULL << 53) + 1.0);
}
static double grand(void)
{
    return sqrt(-2.0 * log(urand())) * cos(2.0 * M_PI * urand());
}

void test_geometry_is_sane(void)
{
    const int ns  = pattern_ack_nsymb();
    const int cap = pattern_ack_max_tx_samples();
    TEST_ASSERT_GREATER_THAN_INT(0, ns);
    TEST_ASSERT_GREATER_THAN_INT(0, cap);
    /* 0.64 s at 8 kHz.  Airtime is the reason this channel is worth having,
     * so a change to it should have to walk past a failing test. */
    TEST_ASSERT_EQUAL_INT(5120, cap);
}

void test_roundtrip_ack_and_break(void)
{
    const int cap = pattern_ack_max_tx_samples();
    int16_t *buf  = malloc((size_t)cap * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(buf);

    int is_break = -1;
    int n = pattern_ack_tx(buf, PATTERN_ACK);
    TEST_ASSERT_EQUAL_INT(cap, n);
    TEST_ASSERT_EQUAL_INT(1, pattern_ack_detect(buf, n, &is_break));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, is_break, "plain ACK read as ACK+TURN");

    is_break = -1;
    n = pattern_ack_tx(buf, PATTERN_BREAK);
    TEST_ASSERT_EQUAL_INT(cap, n);
    TEST_ASSERT_EQUAL_INT(1, pattern_ack_detect(buf, n, &is_break));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, is_break, "ACK+TURN read as plain ACK");

    free(buf);
}

void test_noise_is_not_an_ack(void)
{
    const int cap = pattern_ack_max_tx_samples();
    int16_t *buf  = malloc((size_t)cap * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(buf);

    int hits = 0;
    for (int t = 0; t < 20; t++) {
        for (int i = 0; i < cap; i++) {
            double v = 6000.0 * grand();
            if (v >  32767.0) v =  32767.0;
            if (v < -32768.0) v = -32768.0;
            buf[i] = (int16_t)lrint(v);
        }
        int isb = 0;
        if (pattern_ack_detect(buf, cap, &isb)) hits++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits, "noise accepted as a pattern ACK");
    free(buf);
}

void test_silence_is_not_an_ack(void)
{
    const int cap = pattern_ack_max_tx_samples();
    int16_t *buf  = calloc((size_t)cap, sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(buf);
    int isb = 0;
    TEST_ASSERT_EQUAL_INT(0, pattern_ack_detect(buf, cap, &isb));
    free(buf);
}

void test_detected_when_not_sample_aligned(void)
{
    const int cap  = pattern_ack_max_tx_samples();
    const int lead = 1234;                 /* arbitrary, not a symbol multiple */
    int16_t *pat   = malloc((size_t)cap * sizeof(int16_t));
    int16_t *win   = calloc((size_t)(cap + lead), sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pat);
    TEST_ASSERT_NOT_NULL(win);

    int n = pattern_ack_tx(pat, PATTERN_ACK);
    memcpy(win + lead, pat, (size_t)n * sizeof(int16_t));

    int isb = -1;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, pattern_ack_detect(win, cap + lead, &isb),
                                  "pattern missed when offset in the window");
    TEST_ASSERT_EQUAL_INT(0, isb);
    free(pat); free(win);
}

void test_short_buffer_is_refused(void)
{
    const int cap = pattern_ack_max_tx_samples();
    int16_t *buf  = malloc((size_t)cap * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(buf);
    int n = pattern_ack_tx(buf, PATTERN_ACK);
    int isb = 0;
    TEST_ASSERT_EQUAL_INT(0, pattern_ack_detect(buf, n - 1, &isb));
    TEST_ASSERT_EQUAL_INT(0, pattern_ack_detect(NULL, n, &isb));
    free(buf);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_geometry_is_sane);
    RUN_TEST(test_roundtrip_ack_and_break);
    RUN_TEST(test_noise_is_not_an_ack);
    RUN_TEST(test_silence_is_not_an_ack);
    RUN_TEST(test_detected_when_not_sample_aligned);
    RUN_TEST(test_short_buffer_is_refused);
    return UNITY_END();
}
