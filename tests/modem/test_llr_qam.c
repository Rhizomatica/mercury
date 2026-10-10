/* Unit tests for the generic max-log demapper and the higher-order QAM tables.
 *
 * Copyright (C) 2026 Joseph Freivald
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mpdecode_core.h"
#include "interldpc.h"

void setUp(void) {}
void tearDown(void) {}

static float frand(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return (float)(*s >> 8) / 16777216.0f; }
static float gauss(uint32_t *s) { float u = frand(s) + 1e-7f, v = frand(s); return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v); }

static void test_tables_unit_power(void)
{
    for (int bps = 2; bps <= 8; bps++) {
        if (bps == 3) { TEST_ASSERT_NULL(ldpc_qam_table(3, NULL)); continue; }
        int M = 0;
        const COMP *S = ldpc_qam_table(bps, &M);
        TEST_ASSERT_NOT_NULL(S);
        TEST_ASSERT_EQUAL_INT(1 << bps, M);
        double p = 0, dmin = 1e9;
        for (int j = 0; j < M; j++) {
            p += S[j].real * S[j].real + S[j].imag * S[j].imag;
            for (int k = 0; k < M; k++) if (k != j) {
                double dr = S[j].real - S[k].real, di = S[j].imag - S[k].imag, d = sqrt(dr * dr + di * di);
                if (d < dmin) dmin = d;
            }
        }
        TEST_ASSERT_FLOAT_WITHIN(1e-3, 1.0, (float)(p / M));
        TEST_ASSERT_TRUE(dmin > 0.1);
    }
}

static void test_noiseless_llr_signs(void)
{
    static const int orders[] = {2, 4, 5, 6, 7, 8};
    for (size_t o = 0; o < sizeof orders / sizeof *orders; o++) {
        int bps = orders[o], M;
        const COMP *S = ldpc_qam_table(bps, &M);
        float llr[8];
        for (int label = 0; label < M; label++) {
            float amp = 1.0f;
            llr_from_qam(llr, &S[label], &amp, 1.0f, 2.0f, bps, 1);
            for (int k = 0; k < bps; k++) {
                int bit = (label >> (bps - 1 - k)) & 1;
                TEST_ASSERT_TRUE_MESSAGE(bit ? (llr[k] < 0.0f) : (llr[k] > 0.0f), "LLR sign vs label bit");
            }
        }
    }
}

static void test_maxlog_matches_demod2d_somap(void)
{
    /* QPSK and 16-QAM: the generic max-log demapper must agree with the
       Demod2D+Somap path (max-star) in sign nearly always and in magnitude
       closely, at a cliff-like Es/No of 3 dB. */
    static const int orders[] = {2, 4};
    uint32_t seed = 7;
    for (size_t o = 0; o < 2; o++) {
        int bps = orders[o], M;
        const COMP *S = ldpc_qam_table(bps, &M);
        enum { NS = 2000 };
        COMP sym[NS]; float amps[NS];
        float EsNo = 2.0f, sigma = sqrtf(1.0f / (2.0f * EsNo));
        for (int i = 0; i < NS; i++) {
            int label = (int)(frand(&seed) * M) % M;
            sym[i].real = S[label].real + sigma * gauss(&seed);
            sym[i].imag = S[label].imag + sigma * gauss(&seed);
            amps[i] = 1.0f;
        }
        float *a = malloc(sizeof(float) * NS * bps), *b = malloc(sizeof(float) * NS * bps);
        symbols_to_llrs(a, sym, amps, EsNo, 1.0f, bps, NS);
        llr_from_qam(b, sym, amps, 1.0f, EsNo, bps, NS);
        /* max-star (Somap) and max-log differ exactly where the two best
           metrics are close, i.e. where the LLR is near zero and its sign is
           immaterial; require agreement wherever either side is confident. */
        int disagree_conf = 0, disagree_all = 0; double sum_abs = 0, sum_diff = 0;
        for (int i = 0; i < NS * bps; i++) {
            int d = (a[i] < 0) != (b[i] < 0);
            disagree_all += d;
            if (d && (fabsf(a[i]) > 1.0f || fabsf(b[i]) > 1.0f)) disagree_conf++;
            sum_abs += fabs(a[i]); sum_diff += fabs(a[i] - b[i]);
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, disagree_conf, "confident-LLR sign disagreement");
        TEST_ASSERT_TRUE_MESSAGE(disagree_all <= NS * bps / 20, "sign disagreement > 5% overall");
        TEST_ASSERT_TRUE_MESSAGE(sum_diff < 0.20 * sum_abs, "magnitude difference > 20%");
        free(a); free(b);
    }
}

static void test_modulate_roundtrip_high_order(void)
{
    static const int orders[] = {5, 6, 7, 8};
    uint32_t seed = 99;
    for (size_t o = 0; o < 4; o++) {
        int bps = orders[o], M;
        const COMP *S = ldpc_qam_table(bps, &M);
        enum { NSYM = 300 };
        int codeword[NSYM * 8];
        for (int i = 0; i < NSYM * bps; i++) codeword[i] = frand(&seed) < 0.5f;
        COMP sym[NSYM];
        psk_modulate_frame(bps, sym, codeword, NSYM);
        float llr[NSYM * 8], amps[NSYM];
        for (int i = 0; i < NSYM; i++) amps[i] = 1.0f;
        llr_from_qam(llr, sym, amps, 1.0f, 10.0f, bps, NSYM);
        for (int i = 0; i < NSYM; i++) {
            /* the symbol must be a table point */
            int found = 0;
            for (int j = 0; j < M; j++)
                if (fabsf(sym[i].real - S[j].real) < 1e-6f && fabsf(sym[i].imag - S[j].imag) < 1e-6f) found = 1;
            TEST_ASSERT_TRUE(found);
            for (int k = 0; k < bps; k++)
                TEST_ASSERT_EQUAL_INT(codeword[bps * i + k], llr[bps * i + k] < 0.0f);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tables_unit_power);
    RUN_TEST(test_noiseless_llr_signs);
    RUN_TEST(test_maxlog_matches_demod2d_somap);
    RUN_TEST(test_modulate_roundtrip_high_order);
    return UNITY_END();
}
