/* Unit tests for the persistent SoA LDPC decoder context (ldpc_ctx.c).
 *
 * Copyright (C) 2026 Joseph Freivald
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Drives the DATAC15/16 code (H_256_768_22) with no OFDM front end:
 *   - the SoA graph reproduces the implied-staircase rows of init_c_v_nodes(),
 *   - sum-product on the context decodes the shipped test vector to the shipped
 *     answer, as the legacy SumProduct() does,
 *   - normalised min-sum decodes it too,
 *   - a noiseless codeword converges in one iteration,
 *   - a deadline already in the past ends the decode after one iteration and
 *     is reported,
 *   - run_ldpc_decoder() lazily creates the context and matches legacy.
 */
#include "unity.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ldpc_ctx.h"
#include "mpdecode_core.h"
#include "H_256_768_22.h"

extern const uint16_t H_256_768_22_H_rows[];
extern const uint16_t H_256_768_22_H_cols[];
extern const float H_256_768_22_input[];
extern const char H_256_768_22_detected_data[];

#define NCODE 768
#define NPAR  512
#define KDATA 256

static struct LDPC ldpc;

void setUp(void)
{
    memset(&ldpc, 0, sizeof ldpc);
    strcpy(ldpc.name, "H_256_768_22");
    ldpc.max_iter = 100;
    ldpc.dec_type = 0;
    ldpc.q_scale_factor = 1;
    ldpc.r_scale_factor = 1;
    ldpc.CodeLength = NCODE;
    ldpc.NumberParityBits = NPAR;
    ldpc.NumberRowsHcols = H_256_768_22_NUMBERROWSHCOLS;
    ldpc.max_row_weight = H_256_768_22_MAX_ROW_WEIGHT;
    ldpc.max_col_weight = H_256_768_22_MAX_COL_WEIGHT;
    ldpc.H_rows = (uint16_t *) H_256_768_22_H_rows;
    ldpc.H_cols = (uint16_t *) H_256_768_22_H_cols;
    ldpc.ldpc_data_bits_per_frame = KDATA;
    ldpc.ldpc_coded_bits_per_frame = NCODE;
    ldpc.data_bits_per_frame = KDATA;
    ldpc.coded_bits_per_frame = NCODE;
    ldpc.protection_mode = 0; /* LDPC_PROT_EQUAL (interldpc.h) */
    ldpc_set_default_alg(LDPC_ALG_SP);
}

void tearDown(void) { ldpc_free_ctx(&ldpc); }

static void test_graph_rows_match_exporter(void)
{
    ldpc_ctx_t *c = ldpc_ctx_create(&ldpc);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(2047, ldpc_ctx_edges(c));   /* from fixtures/ldpc/H_256_768_22.rows */
    TEST_ASSERT_TRUE(ldpc_ctx_bytes(c) > 0);
    int idx[8];
    /* rows 0, 1, 2 and the last two, 1-based in the .rows file */
    const int r0[] = {35, 116, 257}, r1[] = {7, 214, 257, 258}, r2[] = {24, 84, 258, 259};
    const int rl[] = {183, 224, 767, 768}, rp[] = {234, 255, 766, 767};
    TEST_ASSERT_EQUAL_INT(3, ldpc_ctx_row_indices(&ldpc, 0, idx));
    for (int j = 0; j < 3; j++) TEST_ASSERT_EQUAL_INT(r0[j] - 1, idx[j]);
    TEST_ASSERT_EQUAL_INT(4, ldpc_ctx_row_indices(&ldpc, 1, idx));
    for (int j = 0; j < 4; j++) TEST_ASSERT_EQUAL_INT(r1[j] - 1, idx[j]);
    TEST_ASSERT_EQUAL_INT(4, ldpc_ctx_row_indices(&ldpc, 2, idx));
    for (int j = 0; j < 4; j++) TEST_ASSERT_EQUAL_INT(r2[j] - 1, idx[j]);
    TEST_ASSERT_EQUAL_INT(4, ldpc_ctx_row_indices(&ldpc, NPAR - 2, idx));
    for (int j = 0; j < 4; j++) TEST_ASSERT_EQUAL_INT(rp[j] - 1, idx[j]);
    TEST_ASSERT_EQUAL_INT(4, ldpc_ctx_row_indices(&ldpc, NPAR - 1, idx));
    for (int j = 0; j < 4; j++) TEST_ASSERT_EQUAL_INT(rl[j] - 1, idx[j]);
    ldpc_ctx_destroy(c);
}

static void test_sp_matches_legacy_on_shipped_vector(void)
{
    uint8_t out_legacy[NCODE], out_ctx[NCODE];
    float in[NCODE];
    memcpy(in, H_256_768_22_input, sizeof in);
    int pcc_l = 0;
    int it_l = run_ldpc_decoder_legacy(&ldpc, out_legacy, in, &pcc_l);
    memcpy(in, H_256_768_22_input, sizeof in);
    ldpc_ctx_t *c = ldpc_ctx_create(&ldpc);
    ldpc_stats_t st;
    int it_c = ldpc_ctx_decode(c, in, out_ctx, 100, 0, &st);
    for (int i = 0; i < NCODE; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t) H_256_768_22_detected_data[i], out_legacy[i]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t) H_256_768_22_detected_data[i], out_ctx[i]);
    }
    TEST_ASSERT_TRUE(st.parity_ok);
    TEST_ASSERT_EQUAL_INT(NPAR, st.parity_count);
    TEST_ASSERT_EQUAL_INT(0, st.hit_deadline);
    /* Legacy tests parity on the SIGNS OF THE EXTRINSIC MESSAGES from the
     * previous pass, not on the hard decisions, so it keeps iterating after the
     * decisions already form a codeword; the context stops as soon as they do.
     * Same bits, never more iterations. */
    TEST_ASSERT_TRUE(it_c <= it_l);
    ldpc_ctx_destroy(c);
}

static void test_nms_decodes_shipped_vector(void)
{
    uint8_t out[NCODE];
    float in[NCODE];
    ldpc_ctx_t *c = ldpc_ctx_create(&ldpc);
    const int algs[] = {LDPC_ALG_NMS, LDPC_ALG_NMS16, LDPC_ALG_SPT};
    for (int a = 0; a < 3; a++) {
        memcpy(in, H_256_768_22_input, sizeof in);
        ldpc_ctx_set_alg(c, algs[a], 0.0f);   /* per-code default scale */
        ldpc_stats_t st;
        ldpc_ctx_decode(c, in, out, 100, 0, &st);
        TEST_ASSERT_TRUE(st.parity_ok);
        for (int i = 0; i < NCODE; i++)
            TEST_ASSERT_EQUAL_UINT8((uint8_t) H_256_768_22_detected_data[i], out[i]);
    }
    ldpc_ctx_destroy(c);
}

static void test_noiseless_codeword_one_iteration(void)
{
    unsigned char ib[KDATA], pb[NPAR];
    uint8_t out[NCODE];
    float in[NCODE];
    srand(12345);
    for (int i = 0; i < KDATA; i++) ib[i] = rand() & 1;
    encode(&ldpc, ib, pb);
    for (int i = 0; i < KDATA; i++) in[i] = ib[i] ? -8.0f : 8.0f;
    for (int i = 0; i < NPAR; i++) in[KDATA + i] = pb[i] ? -8.0f : 8.0f;
    ldpc_ctx_t *c = ldpc_ctx_create(&ldpc);
    for (int alg = LDPC_ALG_SP; alg <= LDPC_ALG_SPT; alg++) {
        ldpc_ctx_set_alg(c, alg, 0.0f);
        ldpc_stats_t st;
        int it = ldpc_ctx_decode(c, in, out, 100, 0, &st);
        TEST_ASSERT_EQUAL_INT(1, it);
        TEST_ASSERT_TRUE(st.parity_ok);
        for (int i = 0; i < KDATA; i++) TEST_ASSERT_EQUAL_UINT8(ib[i], out[i]);
        for (int i = 0; i < NPAR; i++)  TEST_ASSERT_EQUAL_UINT8(pb[i], out[KDATA + i]);
    }
    ldpc_ctx_destroy(c);
}

static void test_past_deadline_stops_after_one_iteration(void)
{
    uint8_t out[NCODE];
    float in[NCODE];
    memcpy(in, H_256_768_22_input, sizeof in);
    ldpc_ctx_t *c = ldpc_ctx_create(&ldpc);
    ldpc_stats_t st;
    uint64_t past = ldpc_now_ns() - 1;
    int it = ldpc_ctx_decode(c, in, out, 100, past, &st);
    TEST_ASSERT_EQUAL_INT(1, it);
    TEST_ASSERT_EQUAL_INT(1, st.hit_deadline);
    TEST_ASSERT_EQUAL_INT(1, st.iters);
    ldpc_ctx_destroy(c);
}

static void test_wrapper_creates_context_and_matches_legacy(void)
{
    uint8_t out_w[NCODE], out_l[NCODE];
    float in[NCODE];
    int pcc_w = 0, pcc_l = 0;
    TEST_ASSERT_NULL(ldpc.ctx);
    memcpy(in, H_256_768_22_input, sizeof in);
    run_ldpc_decoder(&ldpc, out_w, in, &pcc_w);
    TEST_ASSERT_NOT_NULL(ldpc.ctx);
    TEST_ASSERT_EQUAL_INT(NPAR, pcc_w);
    memcpy(in, H_256_768_22_input, sizeof in);
    run_ldpc_decoder_legacy(&ldpc, out_l, in, &pcc_l);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(out_l, out_w, NCODE);
    /* a second call must reuse the context, not allocate another */
    void *first = ldpc.ctx;
    memcpy(in, H_256_768_22_input, sizeof in);
    run_ldpc_decoder(&ldpc, out_w, in, &pcc_w);
    TEST_ASSERT_EQUAL_PTR(first, ldpc.ctx);
}

static void test_struct_deadline_and_stats(void)
{
    uint8_t out[NCODE];
    float in[NCODE];
    int pcc = 0;
    memcpy(in, H_256_768_22_input, sizeof in);
    ldpc.deadline_ns = ldpc_now_ns() - 1;        /* already expired */
    int it = run_ldpc_decoder(&ldpc, out, in, &pcc);
    TEST_ASSERT_EQUAL_INT(1, it);
    TEST_ASSERT_EQUAL_INT(1, ldpc.last_stats.hit_deadline);
    TEST_ASSERT_EQUAL_UINT32(1, ldpc.decode_count);
    ldpc.deadline_ns = 0;
    memcpy(in, H_256_768_22_input, sizeof in);
    run_ldpc_decoder(&ldpc, out, in, &pcc);
    TEST_ASSERT_EQUAL_INT(0, ldpc.last_stats.hit_deadline);
    TEST_ASSERT_TRUE(ldpc.last_stats.parity_ok);
    TEST_ASSERT_EQUAL_UINT32(2, ldpc.decode_count);
    TEST_ASSERT_EQUAL_INT(NPAR, pcc);
}

/* Contexts are created from whichever thread opens a mode; the shared phi
 * tables must be filled exactly once (pthread_once).  Eight threads create a
 * context and decode the shipped vector at once; every result must match the
 * single-threaded one.  TSan in CI is the arbiter of the race itself. */
#include <pthread.h>
#define NTHR 8
static uint8_t thr_out[NTHR][768];
static int thr_it[NTHR];
static void *thr_decode(void *arg)
{
    int k = (int)(intptr_t)arg;
    struct LDPC l = ldpc; l.ctx = NULL;          /* the shipped code, set up by setUp() */
    ldpc_ctx_t *c = ldpc_ctx_create(&l);
    ldpc_ctx_set_alg(c, LDPC_ALG_SPT, 0.0f);
    ldpc_stats_t st;
    float in[768]; memcpy(in, H_256_768_22_input, sizeof in);   /* the decoder consumes its input */
    thr_it[k] = ldpc_ctx_decode(c, in, thr_out[k], 50, 0, &st);
    ldpc_ctx_destroy(c);
    return NULL;
}

void test_contexts_can_be_created_concurrently(void)
{
    pthread_t t[NTHR];
    for (int k = 0; k < NTHR; k++) TEST_ASSERT_EQUAL_INT(0, pthread_create(&t[k], NULL, thr_decode, (void *)(intptr_t)k));
    for (int k = 0; k < NTHR; k++) pthread_join(t[k], NULL);
    for (int k = 1; k < NTHR; k++) {
        TEST_ASSERT_EQUAL_INT(thr_it[0], thr_it[k]);
        TEST_ASSERT_EQUAL_MEMORY(thr_out[0], thr_out[k], ldpc.CodeLength);
    }
}

/* Review of #359: the int16 path converted float LLRs with an integer cast
 * after the scale, so a huge magnitude wrapped and could flip the sign.  The
 * clamp is now in float and a NaN is an erasure (on every path).  A noiseless
 * codeword with +-1e9 and +-inf LLRs plus four NaN erasures must decode on
 * every algorithm, and the hard decisions must be the transmitted bits. */
static void test_extreme_llrs_saturate_instead_of_wrapping(void)
{
    unsigned char ib[KDATA], pb[NPAR];
    uint8_t out[NCODE];
    float in[NCODE], work[NCODE];
    srand(777);
    for (int i = 0; i < KDATA; i++) ib[i] = rand() & 1;
    encode(&ldpc, ib, pb);
    for (int i = 0; i < KDATA; i++) in[i] = ib[i] ? -1e9f : 1e9f;
    for (int i = 0; i < NPAR; i++) in[KDATA + i] = pb[i] ? -1e9f : 1e9f;
    in[10] = ib[10] ? -INFINITY : INFINITY;
    in[11] = ib[11] ? -INFINITY : INFINITY;
    in[3] = NAN; in[100] = NAN; in[KDATA + 7] = NAN; in[NCODE - 1] = NAN;
    ldpc_ctx_t *c = ldpc_ctx_create(&ldpc);
    for (int alg = LDPC_ALG_SP; alg <= LDPC_ALG_SPT; alg++) {
        memcpy(work, in, sizeof work);
        ldpc_ctx_set_alg(c, alg, 0.0f);
        ldpc_stats_t st;
        int it = ldpc_ctx_decode(c, work, out, 100, 0, &st);
        TEST_ASSERT_TRUE_MESSAGE(st.parity_ok, ldpc_alg_name(alg));
        TEST_ASSERT_TRUE_MESSAGE(it <= 5, ldpc_alg_name(alg));
        for (int i = 0; i < KDATA; i++) TEST_ASSERT_EQUAL_UINT8_MESSAGE(ib[i], out[i], ldpc_alg_name(alg));
        for (int i = 0; i < NPAR; i++)  TEST_ASSERT_EQUAL_UINT8_MESSAGE(pb[i], out[KDATA + i], ldpc_alg_name(alg));
    }
    ldpc_ctx_destroy(c);
}

/* A min-sum scale above 1 is clamped to 1 (the Q8 factor in nms16 would wrap):
 * the decode asked for at 1.5 is the decode at 1.0, bit for bit. */
static void test_nms_scale_above_one_is_clamped(void)
{
    uint8_t out1[NCODE], out2[NCODE];
    float in[NCODE];
    ldpc_stats_t s1, s2;
    ldpc_ctx_t *c = ldpc_ctx_create(&ldpc);
    ldpc_ctx_set_alg(c, LDPC_ALG_NMS16, 1.0f);
    memcpy(in, H_256_768_22_input, sizeof in);
    int it1 = ldpc_ctx_decode(c, in, out1, 50, 0, &s1);
    ldpc_ctx_set_alg(c, LDPC_ALG_NMS16, 1.5f);
    memcpy(in, H_256_768_22_input, sizeof in);
    int it2 = ldpc_ctx_decode(c, in, out2, 50, 0, &s2);
    TEST_ASSERT_EQUAL_INT(it1, it2);
    TEST_ASSERT_EQUAL_INT(s1.parity_count, s2.parity_count);
    TEST_ASSERT_EQUAL_MEMORY(out1, out2, NCODE);
    ldpc_ctx_destroy(c);
}

/* The process-wide default algorithm is read from LDPC_ALG exactly once
 * (pthread_once).  Eight FIRST decodes through run_ldpc_decoder() at the same
 * time, each on its own struct LDPC so each creates its context under the
 * AUTO default, must all agree.  TSan is the arbiter of the race itself. */
static struct LDPC thr_l[NTHR];
static int thr_pcc[NTHR];
static void *thr_wrapper(void *arg)
{
    int k = (int)(intptr_t)arg;
    thr_l[k] = ldpc; thr_l[k].ctx = NULL;
    float in[768]; memcpy(in, H_256_768_22_input, sizeof in);
    thr_it[k] = run_ldpc_decoder(&thr_l[k], thr_out[k], in, &thr_pcc[k]);
    ldpc_free_ctx(&thr_l[k]);
    return NULL;
}

static void test_first_decodes_through_the_wrapper_can_race(void)
{
    pthread_t t[NTHR];
    for (int k = 0; k < NTHR; k++) TEST_ASSERT_EQUAL_INT(0, pthread_create(&t[k], NULL, thr_wrapper, (void *)(intptr_t)k));
    for (int k = 0; k < NTHR; k++) pthread_join(t[k], NULL);
    for (int k = 1; k < NTHR; k++) {
        TEST_ASSERT_EQUAL_INT(thr_it[0], thr_it[k]);
        TEST_ASSERT_EQUAL_INT(thr_pcc[0], thr_pcc[k]);
        TEST_ASSERT_EQUAL_MEMORY(thr_out[0], thr_out[k], ldpc.CodeLength);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_struct_deadline_and_stats);
    RUN_TEST(test_graph_rows_match_exporter);
    RUN_TEST(test_sp_matches_legacy_on_shipped_vector);
    RUN_TEST(test_nms_decodes_shipped_vector);
    RUN_TEST(test_noiseless_codeword_one_iteration);
    RUN_TEST(test_past_deadline_stops_after_one_iteration);
    RUN_TEST(test_wrapper_creates_context_and_matches_legacy);
    RUN_TEST(test_contexts_can_be_created_concurrently);
    RUN_TEST(test_extreme_llrs_saturate_instead_of_wrapping);
    RUN_TEST(test_nms_scale_above_one_is_clamped);
    RUN_TEST(test_first_decodes_through_the_wrapper_can_race);
    return UNITY_END();
}
