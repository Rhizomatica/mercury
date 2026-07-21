/* Deterministic unit tests for the HARQ combined-decode acceptance gate.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * freedv_comp_short_rx_ofdm() re-decodes the Chase-combined LLRs and must NOT
 * accept the result on CRC16 alone: a non-converged combine still emits a
 * codeword that passes CRC16 ~2^-16 of the time, which forges an in-window
 * DATA frame once burst_frames>1.  The gate (ldpc_harq_combine_parity_ok)
 * requires >=90% of the mother-code parity checks in addition to CRC.
 *
 * These tests drive Mercury's OWN LDPC decoder (run_ldpc_decoder) on the
 * DATAC15 code (H_256_768_22) with a fixed-seed AWGN model and assert:
 *   - a genuine (correct) decode always clears the gate  (no false-reject),
 *   - a non-converged / garbage decode never clears it   (forgery blocked),
 *   - the threshold arithmetic itself is sane at the boundary.
 * No OFDM front-end, no audio, no threads.
 */
#include "unity.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "mpdecode_core.h"
#include "H_256_768_22.h"

extern const uint16_t H_256_768_22_H_rows[];
extern const uint16_t H_256_768_22_H_cols[];

#define KDATA 256 /* data bits per frame */
#define NCODE 768 /* CodeLength          */
#define NPAR  512 /* NumberParityBits    */

static struct LDPC ldpc;

void setUp(void)
{
    memset(&ldpc, 0, sizeof ldpc);
    strcpy(ldpc.name, "H_256_768_22");
    ldpc.max_iter = H_256_768_22_MAX_ITER;
    ldpc.dec_type = 0;
    ldpc.q_scale_factor = 1;
    ldpc.r_scale_factor = 1;
    ldpc.CodeLength = NCODE;
    ldpc.NumberParityBits = NPAR;
    ldpc.NumberRowsHcols = H_256_768_22_NUMBERROWSHCOLS;
    ldpc.max_row_weight = H_256_768_22_MAX_ROW_WEIGHT;
    ldpc.max_col_weight = H_256_768_22_MAX_COL_WEIGHT;
    ldpc.H_rows = (uint16_t *)H_256_768_22_H_rows;
    ldpc.H_cols = (uint16_t *)H_256_768_22_H_cols;
    ldpc.ldpc_data_bits_per_frame = NCODE - NPAR;
    ldpc.ldpc_coded_bits_per_frame = NCODE;
    ldpc.data_bits_per_frame = KDATA;
    ldpc.coded_bits_per_frame = NCODE;
    ldpc.protection_mode = 0; /* EQUAL — run_ldpc_decoder called directly */
}
void tearDown(void) {}

/* Fixed-seed PCG32 + Box-Muller so the tests are fully deterministic. */
static uint64_t rng_s, rng_inc;
static void rng_seed(uint64_t s) { rng_s = s; rng_inc = 0x14057b7ef767814fULL; }
static uint32_t rng_u32(void)
{
    uint64_t old = rng_s;
    rng_s = old * 6364136223846793005ULL + rng_inc;
    uint32_t xs = ((old >> 18u) ^ old) >> 27u;
    uint32_t rot = old >> 59u;
    return (xs >> rot) | (xs << ((-rot) & 31));
}
static double rng_u01(void) { return (rng_u32() + 0.5) / 4294967296.0; }
static double rng_gauss(void)
{
    double u1 = rng_u01(), u2 = rng_u01();
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

/* Encode random info bits, transmit BPSK through AWGN at noise sd `sig`,
 * decode.  Returns parityCheckCount; *correct set iff data bits recovered. */
static int decode_awgn_frame(double sig, int *correct)
{
    uint8_t ibits[KDATA], pbits[NPAR], out[NCODE];
    float llr[NCODE];
    for (int i = 0; i < KDATA; i++) ibits[i] = rng_u32() & 1;
    encode(&ldpc, ibits, pbits);
    for (int i = 0; i < KDATA; i++) {
        double y = (1.0 - 2.0 * ibits[i]) + sig * rng_gauss();
        llr[i] = (float)(2.0 * y / (sig * sig));
    }
    for (int i = 0; i < NPAR; i++) {
        double y = (1.0 - 2.0 * pbits[i]) + sig * rng_gauss();
        llr[KDATA + i] = (float)(2.0 * y / (sig * sig));
    }
    int pcc = 0;
    run_ldpc_decoder(&ldpc, out, llr, &pcc);
    if (correct) *correct = (memcmp(out, ibits, KDATA) == 0);
    return pcc;
}

/* The threshold predicate itself: reject just below 90%, accept at/above. */
void test_gate_threshold_boundary(void)
{
    int t = (NPAR * 9) / 10;                 /* 460 for the DATAC15 code */
    TEST_ASSERT_FALSE(ldpc_harq_combine_parity_ok(t - 1, NPAR));
    TEST_ASSERT_TRUE(ldpc_harq_combine_parity_ok(t, NPAR));
    TEST_ASSERT_TRUE(ldpc_harq_combine_parity_ok(NPAR, NPAR));   /* converged */
    TEST_ASSERT_FALSE(ldpc_harq_combine_parity_ok(NPAR / 2, NPAR)); /* ~garbage */
}

/* No-harm: a genuine correct decode always clears the gate (clean goodput
 * is never taxed).  Low noise -> reliable decode. */
void test_correct_decode_clears_gate(void)
{
    rng_seed(0xC0FFEEULL);
    int decoded = 0;
    for (int t = 0; t < 32; t++) {
        int correct = 0;
        int pcc = decode_awgn_frame(0.55, &correct); /* well above threshold */
        if (correct) {
            decoded++;
            TEST_ASSERT_TRUE_MESSAGE(
                ldpc_harq_combine_parity_ok(pcc, NPAR),
                "a correct decode must clear the HARQ parity gate");
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(0, decoded); /* sanity: some did decode */
}

/* Forgery blocked: a non-converged decode never clears the gate, so it can
 * never be accepted on CRC16 alone.  Pure-noise LLRs (no coherent signal)
 * never converge. */
void test_nonconverged_decode_never_clears_gate(void)
{
    rng_seed(0xBADBAD00ULL);
    uint8_t out[NCODE];
    float llr[NCODE];
    for (int t = 0; t < 64; t++) {
        for (int i = 0; i < NCODE; i++)
            llr[i] = (float)(2.0 * rng_gauss()); /* no transmitted codeword */
        int pcc = 0;
        run_ldpc_decoder(&ldpc, out, llr, &pcc);
        TEST_ASSERT_FALSE_MESSAGE(
            ldpc_harq_combine_parity_ok(pcc, NPAR),
            "a non-converged (garbage) decode must be rejected by the gate");
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gate_threshold_boundary);
    RUN_TEST(test_correct_decode_clears_gate);
    RUN_TEST(test_nonconverged_decode_never_clears_gate);
    return UNITY_END();
}
