/* The frame-error-rate claims of the LDPC decoder record (docs/LDPC-DECODER.md)
 * as unit tests, in C, on pinned seeds.  No MATLAB.
 *
 * Copyright (C) 2026 Joseph Freivald
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every frame set comes from mercury_prng on one pinned seed, so the counts
 * below are reproducible on this host (seed 1 also matches Linux gcc; other
 * compilers move a frame on some seeds, so every gate carries a margin): random info bits, codec2 encode(),
 * BPSK (bit 0 -> +1) over AWGN at a stated Es/N0, exact LLRs 4 EsNo y
 * (positive = bit 0, codec2 convention).  Every decoder under test sees the
 * same frames, so the comparisons are paired.
 *
 *   - at the FER 10 % cliff of H_256_768_22 the context sp decoder gives the
 *     legacy loop's answer on every frame, and the same bits wherever both
 *     converged;
 *   - spt tracks sp, and nms16 tracks float nms, in frame errors;
 *   - float min-sum is LLR-scale invariant, bit for bit;
 *   - the modem's fixed 3 dB LLR constant loses frames that exact LLRs decode;
 *   - every code's parity-check rows annihilate its codewords, and the
 *     codeword is systematic (this replaces the MATLAB suite's H * c = 0 check);
 *   - the legacy loop's all-zero-payload exit, pinned.
 *
 * LDPC_FER_VERBOSE=1 prints the seed, every measured count and the time of
 * each test.  LDPC_FER_SEED=n draws other frames; the tolerances were set on
 * seeds 1-30 (see each test).
 *
 * The Es/N0 of each test was measured, not taken from the record.  On this
 * code the record's "cliff fixture" is at -3.0 dB, where exact LLRs give
 * FER 1 % (0 to 3 errors in 200 frames over seeds 1-8).  The FER 10 % cliff
 * is 0.5 dB lower: 12 to 33 errors in 200 at -3.5 dB over seeds 1-30, and the
 * committed curve (docs/ldpc-figures/data/ldpc_fer_H_256_768_22.csv, column
 * C_sp50) crosses FER 1e-1 at -3.48 dB by the same log-linear interpolation
 * the record used.  The paired decoder comparisons run at -3.5 dB, where a
 * 0.04 dB gap is visible; the LLR-scale test runs at -3.0 dB, where exact
 * LLRs decode nearly every frame.
 */
#include "unity.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ldpc_codes.h"
#include "ldpc_ctx.h"
#include "mpdecode_core.h"
#include "mercury_prng.h"

#define CLIFF_CODE   "H_256_768_22"
#define CLIFF_DB     (-3.5f)   /* FER 10 % with exact LLRs, measured (see above) */
#define SCALE_DB     (-3.0f)   /* FER 1 % with exact LLRs, measured             */
#define NFRAMES      200
#define CAP          50
#define FIXED_ESNO_DB 3.0f     /* the modem's LLR constant, ofdm_mode.c EsNodB    */

static unsigned seed = 1;
static int verbose;
static uint64_t t_start;

void setUp(void) { t_start = ldpc_now_ns(); }
void tearDown(void)
{
    if (verbose) printf("    %.1f ms\n", (double) (ldpc_now_ns() - t_start) * 1e-6);
}

/* One frame set of one code: info bits, parity bits and the channel samples y
 * of nf frames, plus an LLR scratch row. */
typedef struct {
    struct LDPC l;
    int N, M, K, nf;
    float esno;              /* linear */
    unsigned char *ib, *pb;  /* nf x K, nf x M */
    float *y;                /* nf x N, BPSK plus noise */
    float *llr;              /* N, filled by frames_llr() */
} frames_t;

static void frames_make(frames_t *F, const char *code, float esno_db, int nf)
{
    char name[32];
    snprintf(name, sizeof name, "%s", code);
    memset(F, 0, sizeof *F);
    ldpc_codes_setup(&F->l, name);
    F->N = F->l.CodeLength;
    F->M = F->l.NumberParityBits;
    F->K = F->N - F->M;
    F->nf = nf;
    F->esno = powf(10.0f, esno_db / 10.0f);
    F->ib = malloc((size_t) nf * F->K);
    F->pb = malloc((size_t) nf * F->M);
    F->y = malloc(sizeof(float) * (size_t) nf * F->N);
    F->llr = malloc(sizeof(float) * F->N);
    TEST_ASSERT_NOT_NULL(F->ib); TEST_ASSERT_NOT_NULL(F->pb);
    TEST_ASSERT_NOT_NULL(F->y);  TEST_ASSERT_NOT_NULL(F->llr);
    /* Es = 1 per BPSK symbol, so the noise variance per real dimension is
     * 1 / (2 EsNo), as in the record's sweep. */
    const float sigma = sqrtf(1.0f / (2.0f * F->esno));
    mercury_prng_t p;
    mercury_prng_seed(&p, seed);
    for (int f = 0; f < nf; f++) {
        unsigned char *ib = F->ib + (size_t) f * F->K, *pb = F->pb + (size_t) f * F->M;
        float *y = F->y + (size_t) f * F->N;
        for (int i = 0; i < F->K; i++) ib[i] = mercury_prng_u31(&p) & 1;
        encode(&F->l, ib, pb);
        for (int i = 0; i < F->N; i++) {
            int b = i < F->K ? ib[i] : pb[i - F->K];
            y[i] = (b ? -1.0f : 1.0f) + sigma * mercury_prng_gaussian(&p);
        }
    }
}

static void frames_free(frames_t *F)
{
    ldpc_free_ctx(&F->l);
    free(F->ib); free(F->pb); free(F->y); free(F->llr);
}

/* LLRs of frame f: scale * 4 EsNo y.  scale 1 is the exact LLR. */
static float *frames_llr(frames_t *F, int f, float scale)
{
    const float *y = F->y + (size_t) f * F->N, g = scale * 4.0f * F->esno;
    for (int i = 0; i < F->N; i++) F->llr[i] = g * y[i];
    return F->llr;
}

/* 1 if any decoded info bit differs from the transmitted one: the record's
 * frame-error definition (parity bits and the decoder's own verdict are not
 * consulted). */
static int frame_error(const frames_t *F, int f, const uint8_t *out)
{
    return memcmp(out, F->ib + (size_t) f * F->K, F->K) != 0;
}

/* (a) sp reproduces legacy.  The record's "context sp vs legacy 0.00 dB on
 * every code" is a statement about frame errors, and that is what holds
 * frame for frame: measured on seeds 1-30 (6000 frames at -3.5 dB, cap 50),
 * the two loops agreed on the outcome of every frame, and their frame-error
 * counts were equal on all 30 seeds.  The full N-bit output is the same
 * wherever both loops converged.  It is not the same on every frame: 5 frames
 * in 6000 differed, each one a frame where at least one loop ran into the cap.
 * The legacy loop tests parity on the signs of the previous pass's extrinsic
 * messages, the context on the hard decisions, so near the cap one may stop a
 * pass before the other (seed 6 frame 27: sp converged at pass 49, legacy ran
 * its 50th pass with 499 of 512 checks satisfied, and one parity bit differs).
 * Seed 1 has no such frame.  Allowing two keeps the test honest about the cap
 * boundary while a wrong message update, which changes many frames, still
 * fails it. */
static void test_sp_reproduces_legacy_at_the_cliff(void)
{
    frames_t F;
    frames_make(&F, CLIFF_CODE, CLIFF_DB, NFRAMES);
    F.l.max_iter = CAP;
    ldpc_ctx_t *c = ldpc_ctx_create(&F.l);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(LDPC_ALG_SP, ldpc_ctx_alg(c));
    uint8_t *o_leg = malloc(F.N), *o_sp = malloc(F.N);
    TEST_ASSERT_NOT_NULL(o_leg); TEST_ASSERT_NOT_NULL(o_sp);
    int fe_leg = 0, fe_sp = 0, outcome_differs = 0, bits_differ = 0, converged_differ = 0;
    for (int f = 0; f < F.nf; f++) {
        float *llr = frames_llr(&F, f, 1.0f);
        int pcc = -1;
        ldpc_stats_t st;
        run_ldpc_decoder_legacy(&F.l, o_leg, llr, &pcc);
        ldpc_ctx_decode(c, llr, o_sp, CAP, 0, &st);
        int e_leg = frame_error(&F, f, o_leg), e_sp = frame_error(&F, f, o_sp);
        fe_leg += e_leg;
        fe_sp += e_sp;
        outcome_differs += e_leg != e_sp;
        if (memcmp(o_leg, o_sp, F.N) != 0) {
            bits_differ++;
            if (pcc == F.M && st.parity_ok) converged_differ++;
        }
    }
    if (verbose)
        printf("  seed %u %s %.1f dB cap %d: frame errors legacy %d sp %d of %d; "
               "frames with any bit differing %d (both converged: %d)\n",
               seed, CLIFF_CODE, (double) CLIFF_DB, CAP, fe_leg, fe_sp, F.nf, bits_differ, converged_differ);
    /* on the cliff: FER between 2.5 % and 30 % (measured 6 % to 16.5 %) */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(5, fe_sp);
    TEST_ASSERT_LESS_OR_EQUAL_INT(60, fe_sp);
    /* Not exact: the legacy loop tests parity on the previous pass's extrinsic
     * signs, the context on its hard decisions, so near the cap the two can end
     * a frame differently (seeds 32 and 83 of 100 each move one frame).  The
     * record's "0.00 dB" is a frame-error-rate statement, gated here as counts. */
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(3, outcome_differs, "sp and legacy disagree on more frames than measured");
    TEST_ASSERT_INT_WITHIN(3, fe_leg, fe_sp);
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(2, bits_differ, "more cap-boundary frames than measured");
    free(o_leg); free(o_sp);
    ldpc_ctx_destroy(c);
    frames_free(&F);
}

/* Frame errors of two context algorithms on the same frames.  Returns the
 * number of frames on which they disagree, and the error count of each. */
static int paired_errors(frames_t *F, ldpc_ctx_t *c, int alg_a, int alg_b, int cap,
                         int *fe_a, int *fe_b)
{
    uint8_t *oa = malloc(F->N), *ob = malloc(F->N);
    TEST_ASSERT_NOT_NULL(oa); TEST_ASSERT_NOT_NULL(ob);
    int disagree = 0;
    *fe_a = *fe_b = 0;
    for (int f = 0; f < F->nf; f++) {
        float *llr = frames_llr(F, f, 1.0f);
        ldpc_ctx_set_alg(c, alg_a, 0.0f);
        ldpc_ctx_decode(c, llr, oa, cap, 0, NULL);
        ldpc_ctx_set_alg(c, alg_b, 0.0f);
        ldpc_ctx_decode(c, llr, ob, cap, 0, NULL);
        int ea = frame_error(F, f, oa), eb = frame_error(F, f, ob);
        *fe_a += ea;
        *fe_b += eb;
        disagree += ea != eb;
    }
    free(oa); free(ob);
    return disagree;
}

/* Tolerances of (b) and (c), from the measurement on seeds 1-30 at -3.5 dB,
 * cap 50, 200 frames: the error-count difference |spt - sp| was at most 4 and
 * |nms16 - nms| at most 2; the number of frames whose outcome differed was at
 * most 5 (spt/sp) and 4 (nms16/nms).  In dB: the committed curve falls by a
 * factor 5.4 between -3.77 and -3.27 dB, so the record's 0.04 dB is a 14 %
 * change of FER, 3 frames of the 22 that seed 1 loses; the pairing noise adds
 * about 2.  The gate is twice the largest measured gap, 6 frames (0.07 dB),
 * and twice the largest measured disagreement, 10 frames. */
#define COUNT_TOL    6
#define DISAGREE_TOL 10

/* (b) spt vs sp: the two-segment phi table against the exact phi0(). */
static void test_spt_tracks_sp(void)
{
    frames_t F;
    frames_make(&F, CLIFF_CODE, CLIFF_DB, NFRAMES);
    ldpc_ctx_t *c = ldpc_ctx_create(&F.l);
    TEST_ASSERT_NOT_NULL(c);
    int fe_sp, fe_spt;
    int dis = paired_errors(&F, c, LDPC_ALG_SP, LDPC_ALG_SPT, CAP, &fe_sp, &fe_spt);
    if (verbose)
        printf("  seed %u: frame errors sp %d spt %d of %d; frames that disagree %d\n",
               seed, fe_sp, fe_spt, F.nf, dis);
    TEST_ASSERT_INT_WITHIN(COUNT_TOL, fe_sp, fe_spt);
    TEST_ASSERT_LESS_OR_EQUAL_INT(DISAGREE_TOL, dis);
    ldpc_ctx_destroy(c);
    frames_free(&F);
}

/* (c) nms16 vs float nms, and the scale invariance of float min-sum.
 *
 * Min-sum only takes minima, signs, sums and one multiplication by the
 * normalisation scale, so scaling every input LLR by a constant scales every
 * message by that constant and changes no sign: the hard decisions, and so
 * the stop iteration, are the same.  This holds bit for bit in float here,
 * at scales 0.37 and 2.5: measured 0 differing bits on seeds 1-8 at -3.5,
 * -3.0, -2.75 and -2.5 dB (6400 frames).  The float path clamps |LLR| at 1e4,
 * far above the 2.5 * 4 EsNo y of these frames.
 *
 * nms16 is not tested this way because it is not scale invariant by design:
 * its messages are int16 in units of 1/16 LLR, so the input scale sets the
 * quantisation step (a clean sample at -3.5 dB is an LLR of 4 EsNo = 1.8, or
 * 29 steps; at 0.37 x it is 11 steps) and the saturation point (32767 / 16 =
 * 2047.9 LLR); its claim is to track float nms at
 * the LLR scale the modem feeds it, which is the first half of this test. */
static void test_nms16_tracks_nms_and_float_nms_is_scale_invariant(void)
{
    frames_t F;
    frames_make(&F, CLIFF_CODE, CLIFF_DB, NFRAMES);
    ldpc_ctx_t *c = ldpc_ctx_create(&F.l);
    TEST_ASSERT_NOT_NULL(c);
    int fe_nms, fe_nms16;
    int dis = paired_errors(&F, c, LDPC_ALG_NMS, LDPC_ALG_NMS16, CAP, &fe_nms, &fe_nms16);
    if (verbose)
        printf("  seed %u: frame errors nms %d nms16 %d of %d; frames that disagree %d\n",
               seed, fe_nms, fe_nms16, F.nf, dis);
    TEST_ASSERT_INT_WITHIN(COUNT_TOL, fe_nms, fe_nms16);
    TEST_ASSERT_LESS_OR_EQUAL_INT(DISAGREE_TOL, dis);

    /* Powers of two only: scaling a float by 2^k is exact, so float min-sum's
     * scale invariance holds bit for bit.  Other factors round differently on
     * a frame here and there (0.37 and 2.5 moved one frame on 5 of 100 seeds). */
    static const float scales[] = {0.25f, 4.0f};
    uint8_t *o1 = malloc(F.N), *o2 = malloc(F.N);
    TEST_ASSERT_NOT_NULL(o1); TEST_ASSERT_NOT_NULL(o2);
    ldpc_ctx_set_alg(c, LDPC_ALG_NMS, 0.0f);
    for (size_t s = 0; s < sizeof scales / sizeof *scales; s++) {
        int frames_differ = 0, iters_differ = 0;
        for (int f = 0; f < F.nf; f++) {
            int it1 = ldpc_ctx_decode(c, frames_llr(&F, f, 1.0f), o1, CAP, 0, NULL);
            int it2 = ldpc_ctx_decode(c, frames_llr(&F, f, scales[s]), o2, CAP, 0, NULL);
            frames_differ += memcmp(o1, o2, F.N) != 0;
            iters_differ += it1 != it2;
        }
        if (verbose)
            printf("  seed %u: nms at LLR scale %.2f: frames differing from scale 1: %d, iteration counts differing: %d\n",
                   seed, (double) scales[s], frames_differ, iters_differ);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, frames_differ, "float min-sum is not LLR-scale invariant");
        TEST_ASSERT_EQUAL_INT(0, iters_differ);
    }
    free(o1); free(o2);
    ldpc_ctx_destroy(c);
    frames_free(&F);
}

/* (d) The LLR scale.  The modem forms LLRs with a per-mode constant, 3 dB for
 * this code (ofdm_mode.c EsNodB), in place of the measured Es/N0: at -3.0 dB
 * that is 4 EsNo y with EsNo four times too large (10^0.6 = 3.98).  Sum-product
 * is not scale invariant, so the over-confident LLRs lose frames that exact
 * LLRs decode.  The record puts the cost at 1.0 dB at FER 1e-1 on this code
 * (committed curve: the legacy loop at cap 100 crosses FER 1e-1 at -2.49 dB
 * with the fixed constant and sp at cap 50 at -3.48 dB with exact LLRs).  At
 * -3.0 dB the
 * fixed curve should therefore sit near FER 0.3, and it does: measured on seeds
 * 1-6, 200 frames, sp at cap 100, exact LLRs lost 0, 1, 2, 1, 0, 2 frames and
 * the fixed constant 62, 57, 72, 63, 70, 55 (seed 12, the worst of seeds 6,
 * 12 and 30 checked as well: 4 and 70).  The gates, 10 and 30, leave a factor
 * of 2.5 and of 1.8 to the worst measurement.
 *
 * The same fixed LLRs through float min-sum lost 0, 1, 2, 1, 1, 4 (seed 12:
 * 6): the record's "nms immune to the LLR scale", at the modem's own
 * constant. */
static void test_fixed_esno_llr_scale_costs_frames(void)
{
    enum { CAP100 = 100 };
    frames_t F;
    frames_make(&F, CLIFF_CODE, SCALE_DB, NFRAMES);
    ldpc_ctx_t *c = ldpc_ctx_create(&F.l);
    TEST_ASSERT_NOT_NULL(c);
    uint8_t *o = malloc(F.N);
    TEST_ASSERT_NOT_NULL(o);
    /* fixed LLR = 4 EsNo_fixed y = exact * (EsNo_fixed / EsNo) */
    const float fixed_scale = powf(10.0f, FIXED_ESNO_DB / 10.0f) / F.esno;
    int fe_exact = 0, fe_fixed = 0, fe_fixed_nms = 0;
    for (int f = 0; f < F.nf; f++) {
        ldpc_ctx_set_alg(c, LDPC_ALG_SP, 0.0f);
        ldpc_ctx_decode(c, frames_llr(&F, f, 1.0f), o, CAP100, 0, NULL);
        fe_exact += frame_error(&F, f, o);
        float *llr = frames_llr(&F, f, fixed_scale);
        ldpc_ctx_decode(c, llr, o, CAP100, 0, NULL);
        fe_fixed += frame_error(&F, f, o);
        ldpc_ctx_set_alg(c, LDPC_ALG_NMS, 0.0f);
        ldpc_ctx_decode(c, llr, o, CAP100, 0, NULL);
        fe_fixed_nms += frame_error(&F, f, o);
    }
    if (verbose)
        printf("  seed %u %s %.1f dB cap %d: frame errors sp exact %d, sp fixed %.0f dB %d, nms fixed %d of %d\n",
               seed, CLIFF_CODE, (double) SCALE_DB, CAP100, fe_exact, (double) FIXED_ESNO_DB, fe_fixed,
               fe_fixed_nms, F.nf);
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(10, fe_exact, "exact LLRs should decode nearly every frame here");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(30, fe_fixed, "the fixed-EsNo constant should cost frames here");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(10, fe_fixed_nms, "min-sum should not care about the LLR scale");
    free(o);
    ldpc_ctx_destroy(c);
    frames_free(&F);
}

/* (e) For every code in ldpc_codes[] (H_2064_516_sparse included): 8 random
 * info words through encode() give codewords [info | parity] that satisfy
 * every check row as ldpc_ctx_row_indices() lays the rows out, including the
 * implied staircase parity columns.  That is the H * c = 0 check the MATLAB
 * suite made on the exported rows, done on the rows the decoder itself uses.
 * The first codeword is also decoded without noise (LLR +-8): one iteration,
 * every check satisfied, and the first K output bits are the info bits, so
 * encoder, graph and decoder agree that the code is systematic in that order.
 * 12 codes, 3 ms measured (ldpc_ctx_create of the 16200-bit code included). */
static void test_every_code_checks_its_codewords(void)
{
    mercury_prng_t p;
    mercury_prng_seed(&p, seed);
    TEST_ASSERT_EQUAL_INT(12, ldpc_codes_num());
    for (int ci = 0; ci < ldpc_codes_num(); ci++) {
        struct LDPC l = ldpc_codes[ci];
        const int N = l.CodeLength, M = l.NumberParityBits, K = N - M;
        unsigned char *ib = malloc(K), *pb = malloc(M);
        int *idx = malloc(sizeof(int) * (size_t) (l.max_row_weight + 2));
        uint8_t *out = malloc(N);
        float *llr = malloc(sizeof(float) * N);
        TEST_ASSERT_NOT_NULL(ib); TEST_ASSERT_NOT_NULL(pb); TEST_ASSERT_NOT_NULL(idx);
        TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_NOT_NULL(llr);
        int unsatisfied = 0;
        for (int w = 0; w < 8; w++) {
            for (int i = 0; i < K; i++) ib[i] = mercury_prng_u31(&p) & 1;
            encode(&l, ib, pb);
            for (int r = 0; r < M; r++) {
                int d = ldpc_ctx_row_indices(&l, r, idx), x = 0;
                TEST_ASSERT_TRUE_MESSAGE(d >= 2 && d <= l.max_row_weight + 2, l.name);
                for (int j = 0; j < d; j++) {
                    int v = idx[j];
                    TEST_ASSERT_TRUE_MESSAGE(v >= 0 && v < N, l.name);
                    x ^= v < K ? ib[v] : pb[v - K];
                }
                unsatisfied += x;
            }
            if (w == 0) {
                for (int i = 0; i < N; i++) llr[i] = (i < K ? ib[i] : pb[i - K]) ? -8.0f : 8.0f;
                ldpc_ctx_t *c = ldpc_ctx_create(&l);
                TEST_ASSERT_NOT_NULL_MESSAGE(c, l.name);
                ldpc_stats_t st;
                int it = ldpc_ctx_decode(c, llr, out, 10, 0, &st);
                TEST_ASSERT_EQUAL_INT_MESSAGE(1, it, l.name);
                TEST_ASSERT_TRUE_MESSAGE(st.parity_ok, l.name);
                TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(ib, out, K, l.name);
                TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(pb, out + K, M, l.name);
                ldpc_ctx_destroy(c);
            }
        }
        if (verbose) printf("  %-18s N %5d K %5d: unsatisfied checks over 8 codewords %d\n", l.name, N, K, unsatisfied);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, unsatisfied, l.name);
        free(ib); free(pb); free(idx); free(out); free(llr);
    }
}

/* (f) The legacy loop halts as soon as the decoded info bits are all zero
 * (SumProduct counts them against a zeroed data[] and stops before it writes
 * *parityCheckCount), so on an all-zero payload it returns after one pass and
 * reports no parity count at all.  A random payload is decoded and reported
 * normally.  The context has no such exit.  Pinned here because
 * docs/LDPC-DECODER.md says the exit is gone: it is gone from the context
 * path only, and tests that go through the legacy loop must use random
 * payloads. */
static void test_legacy_loop_halts_on_all_zero_payload(void)
{
    enum { SENTINEL = -7 };
    frames_t F;
    frames_make(&F, CLIFF_CODE, SCALE_DB, 1);   /* one random frame; noise unused */
    F.l.max_iter = CAP;
    uint8_t *o = malloc(F.N);
    TEST_ASSERT_NOT_NULL(o);
    for (int i = 0; i < F.N; i++) F.llr[i] = 8.0f;   /* the all-zero codeword, no noise */
    int pcc = SENTINEL;
    int it = run_ldpc_decoder_legacy(&F.l, o, F.llr, &pcc);
    TEST_ASSERT_EQUAL_INT(1, it);
    TEST_ASSERT_EQUAL_INT(SENTINEL, pcc);
    ldpc_ctx_t *c = ldpc_ctx_create(&F.l);
    TEST_ASSERT_NOT_NULL(c);
    ldpc_stats_t st;
    it = ldpc_ctx_decode(c, F.llr, o, CAP, 0, &st);
    TEST_ASSERT_EQUAL_INT(1, it);
    TEST_ASSERT_TRUE(st.parity_ok);
    TEST_ASSERT_EQUAL_INT(F.M, st.parity_count);
    /* the random frame, noiseless: legacy decodes it and reports every check */
    for (int i = 0; i < F.N; i++) F.llr[i] = (i < F.K ? F.ib[i] : F.pb[i - F.K]) ? -8.0f : 8.0f;
    pcc = SENTINEL;
    it = run_ldpc_decoder_legacy(&F.l, o, F.llr, &pcc);
    TEST_ASSERT_EQUAL_INT(1, it);
    TEST_ASSERT_EQUAL_INT(F.M, pcc);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(F.ib, o, F.K);
    free(o);
    ldpc_ctx_destroy(c);
    frames_free(&F);
}

int main(void)
{
    verbose = getenv("LDPC_FER_VERBOSE") != NULL;
    if (getenv("LDPC_FER_SEED")) seed = (unsigned) strtoul(getenv("LDPC_FER_SEED"), NULL, 0);
    if (verbose) printf("test_ldpc_fer: mercury_prng seed %u\n", seed);
    UNITY_BEGIN();
    RUN_TEST(test_sp_reproduces_legacy_at_the_cliff);
    RUN_TEST(test_spt_tracks_sp);
    RUN_TEST(test_nms16_tracks_nms_and_float_nms_is_scale_invariant);
    RUN_TEST(test_fixed_esno_llr_scale_costs_frames);
    RUN_TEST(test_every_code_checks_its_codewords);
    RUN_TEST(test_legacy_loop_halts_on_all_zero_payload);
    return UNITY_END();
}
