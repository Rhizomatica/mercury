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

/* The labelling claim of qam_constellations.h, proved in C.  Over all ordered
 * pairs (j, k), j != k, of table points at the minimum distance (within 1e-4
 * relative), the histogram of the Hamming distance between the labels j and k,
 * for distances 1..bps.  A Gray labelling puts every nearest-neighbour pair at
 * Hamming distance 1; the cross constellations (32, 128) have no Gray map, and
 * the header states where their pairs land.  The expected rows for 5 to 8 bits
 * are the header's; QPSK and 16-QAM carry no such line, so their rows were
 * measured here and are what a Gray 4-point ring and a Gray 4 x 4 grid must
 * give (twice the edge count: 8 and 48). */
static void test_tables_nearest_neighbour_hamming(void)
{
    static const int orders[] = {2, 4, 5, 6, 7, 8};
    static const int exp2[] = {8, 0};
    static const int exp4[] = {48, 0, 0, 0};
    static const int exp5[] = {88, 16, 0, 0, 0};
    static const int exp6[] = {224, 0, 0, 0, 0, 0};
    static const int exp7[] = {432, 32, 0, 0, 0, 0, 0};
    static const int exp8[] = {960, 0, 0, 0, 0, 0, 0, 0};
    static const int *const expected[] = {exp2, exp4, exp5, exp6, exp7, exp8};
    for (size_t o = 0; o < sizeof orders / sizeof *orders; o++) {
        int bps = orders[o], M;
        const COMP *S = ldpc_qam_table(bps, &M);
        TEST_ASSERT_NOT_NULL(S);
        double dmin = 1e9;
        for (int j = 0; j < M; j++) for (int k = 0; k < M; k++) if (k != j) {
            double d = hypot(S[j].real - S[k].real, S[j].imag - S[k].imag);
            if (d < dmin) dmin = d;
        }
        int hist[9] = {0};
        for (int j = 0; j < M; j++) for (int k = 0; k < M; k++) if (k != j) {
            double d = hypot(S[j].real - S[k].real, S[j].imag - S[k].imag);
            if (fabs(d - dmin) <= 1e-4 * dmin) hist[__builtin_popcount((unsigned) (j ^ k))]++;
        }
        char msg[48];
        snprintf(msg, sizeof msg, "nearest-neighbour Hamming histogram, %d bits", bps);
        TEST_ASSERT_EQUAL_INT_ARRAY_MESSAGE(expected[o], hist + 1, bps, msg);
    }
}

/* llr_from_qam against the max-log definition, written out here in double:
 * for bit k of a symbol (k = 0 is the label MSB),
 *   LLR_k = EsNo * ( min over points with bit k = 1 of |y - s|^2
 *                  - min over points with bit k = 0 of |y - s|^2 ),
 * positive = bit 0.  500 noisy symbols per bps at Es/N0 6 dB: unit-power
 * table, complex noise variance 1 / EsNo, so each real dimension has
 * variance 1 / (2 EsNo).  The error is max |got - ref| / max |ref| over the
 * bps x 500 LLRs; the gate is 1e-5.  Measured on this host (seed 11 + bps):
 * 1.3e-7 (2 bits), 1.4e-7 (4), 1.5e-7 (5), 1.5e-7 (6), 1.2e-7 (7), 2.0e-7 (8),
 * which is the demapper's float arithmetic against the double reference.
 * LLR_QAM_VERBOSE=1 prints the six values. */
static void test_maxlog_matches_definition(void)
{
    static const int orders[] = {2, 4, 5, 6, 7, 8};
    enum { NS = 500 };
    const float esno_db = 6.0f, EsNo = powf(10.0f, esno_db / 10.0f);
    const float sigma = sqrtf(1.0f / (2.0f * EsNo));
    for (size_t o = 0; o < sizeof orders / sizeof *orders; o++) {
        int bps = orders[o], M;
        const COMP *S = ldpc_qam_table(bps, &M);
        uint32_t seed = 11 + (uint32_t) bps;
        COMP sym[NS]; float amps[NS], got[NS * 8];
        for (int i = 0; i < NS; i++) {
            int label = (int)(frand(&seed) * M) % M;
            sym[i].real = S[label].real + sigma * gauss(&seed);
            sym[i].imag = S[label].imag + sigma * gauss(&seed);
            amps[i] = 1.0f;
        }
        llr_from_qam(got, sym, amps, 1.0f, EsNo, bps, NS);
        double max_err = 0, max_ref = 0;
        for (int i = 0; i < NS; i++) {
            double d[256];
            for (int j = 0; j < M; j++) {
                double er = (double) sym[i].real - S[j].real, ei = (double) sym[i].imag - S[j].imag;
                d[j] = er * er + ei * ei;
            }
            for (int k = 0; k < bps; k++) {
                double m0 = 1e300, m1 = 1e300;
                for (int j = 0; j < M; j++) {
                    if ((j >> (bps - 1 - k)) & 1) { if (d[j] < m1) m1 = d[j]; }
                    else                          { if (d[j] < m0) m0 = d[j]; }
                }
                double ref = EsNo * (m1 - m0), err = fabs(ref - got[bps * i + k]);
                if (err > max_err) max_err = err;
                if (fabs(ref) > max_ref) max_ref = fabs(ref);
            }
        }
        TEST_ASSERT_TRUE(max_ref > 0);
        char msg[64];
        snprintf(msg, sizeof msg, "max-log relative error %.2e at %d bits", max_err / max_ref, bps);
        if (getenv("LLR_QAM_VERBOSE")) printf("  %s\n", msg);
        TEST_ASSERT_TRUE_MESSAGE(max_err / max_ref < 1e-5, msg);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tables_unit_power);
    RUN_TEST(test_noiseless_llr_signs);
    RUN_TEST(test_maxlog_matches_demod2d_somap);
    RUN_TEST(test_modulate_roundtrip_high_order);
    RUN_TEST(test_tables_nearest_neighbour_hamming);
    RUN_TEST(test_maxlog_matches_definition);
    return UNITY_END();
}
