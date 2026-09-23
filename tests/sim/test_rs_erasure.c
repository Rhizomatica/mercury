/* tests/sim/test_rs_erasure.c -- the erasure code recovers any K of K+R pieces
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "rs_erasure.h"

#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static unsigned long long rng = 0x9E3779B97F4A7C15ULL;
static unsigned rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (unsigned)(rng >> 11);
}

/* Encode K data + R repair, keep a random K of them, decode, compare. */
static void roundtrip(int K, int R, size_t P, int trials)
{
    static uint8_t data[RS_MAX_PIECES][64], all[RS_MAX_PIECES][64], got[RS_MAX_PIECES][64];
    const uint8_t *dptr[RS_MAX_PIECES];
    for (int i = 0; i < K; i++) {
        for (size_t k = 0; k < P; k++) data[i][k] = (uint8_t)rnd();
        dptr[i] = data[i];
        memcpy(all[i], data[i], P);
    }
    for (int j = 0; j < R; j++)
        TEST_ASSERT_EQUAL_INT(0, rs_encode_repair(K, P, dptr, j, all[K + j]));

    for (int t = 0; t < trials; t++) {
        int order[RS_MAX_PIECES];
        for (int i = 0; i < K + R; i++) order[i] = i;
        for (int i = K + R - 1; i > 0; i--) {          /* shuffle */
            int s = (int)(rnd() % (unsigned)(i + 1));
            int tmp = order[i]; order[i] = order[s]; order[s] = tmp;
        }
        int idx[RS_MAX_PIECES];
        const uint8_t *pcs[RS_MAX_PIECES];
        uint8_t *out[RS_MAX_PIECES];
        for (int r = 0; r < K; r++) {
            idx[r] = order[r];
            pcs[r] = all[order[r]];
            out[r] = got[r];
        }
        TEST_ASSERT_EQUAL_INT(0, rs_decode(K, P, idx, pcs, out));
        for (int i = 0; i < K; i++)
            TEST_ASSERT_EQUAL_MEMORY(data[i], out[i], P);
    }
}

void test_any_k_of_n_recover_the_block(void)
{
    roundtrip(1, 5, 24, 20);
    roundtrip(10, 10, 24, 50);
    roundtrip(64, 32, 24, 20);
    roundtrip(200, 56, 24, 5);
}

/* The extreme: a full block with a single repair piece. */
void test_largest_block(void)
{
    roundtrip(255, 1, 24, 5);
}

/* All data pieces present: decoding is the identity (the systematic case). */
void test_systematic_pieces_are_the_data(void)
{
    roundtrip(30, 0, 24, 1);
}

void test_bad_input_is_refused(void)
{
    uint8_t a[8] = {0}, b[8] = {0}, o0[8], o1[8];
    const uint8_t *pcs[2] = { a, b };
    uint8_t *out[2] = { o0, o1 };
    int dup[2] = { 3, 3 };
    TEST_ASSERT_EQUAL_INT(-1, rs_decode(2, 8, dup, pcs, out));
    const uint8_t *d[1] = { a };
    TEST_ASSERT_EQUAL_INT(-1, rs_encode_repair(10, 8, d, 246, o0));  /* j >= 256-K */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_any_k_of_n_recover_the_block);
    RUN_TEST(test_largest_block);
    RUN_TEST(test_systematic_pieces_are_the_data);
    RUN_TEST(test_bad_input_is_refused);
    return UNITY_END();
}
