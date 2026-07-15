/* Deterministic unit tests for the MFSK codec (C port of v1's cl_mfsk).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Verifies: mod->demod is lossless with no noise (Gray + tone-hop consistency)
 * across M=4/8/16/32; the soft LLR sign tracks the sent bit at high SNR; and
 * higher M is more power-efficient (lower BER at a fixed Eb/N0) on AWGN —
 * the reason v1 used 32-MFSK.  Fixed PRNG seed -> deterministic.
 */
#include "unity.h"
#include "mfsk.h"
#include "mfsk_ofdm.h"
#include "mfsk_sync.h"
#include "mfsk_ldpc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <complex.h>

#define NC       50
#define NSYM     4000

static mfsk_t m;
static int    bits[NSYM * 5];      /* max bps = nBits(32)=5, 1 stream */
static float  llr[NSYM * 5];
static mfsk_cplx sym[NSYM * NC];

void setUp(void) {}
void tearDown(void) {}

/* deterministic N(0,1) */
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

static int run_awgn(int M, double ebn0_db, int fading)
{
    mfsk_init(&m, M, NC, 1);
    int bps = mfsk_bits_per_symbol(&m);
    int nbits = bps * NSYM;
    for (int i = 0; i < nbits; i++) bits[i] = (urand() < 0.5) ? 0 : 1;
    memset(sym, 0, sizeof(mfsk_cplx) * (size_t)NSYM * NC);
    mfsk_mod(&m, bits, nbits, sym);

    double Es = (double)NC, Eb = Es / m.nBits;
    double N0 = Eb / pow(10.0, ebn0_db / 10.0), sd = sqrt(N0 / 2.0);
    for (int s = 0; s < NSYM; s++)
    {
        double h = 1.0;
        if (fading) { double a = grand(), b = grand(); h = sqrt((a*a + b*b) / 2.0); }
        for (int k = 0; k < NC; k++)
        {
            mfsk_cplx *c = &sym[s * NC + k];
            c->re = h * c->re + sd * grand();
            c->im = h * c->im + sd * grand();
        }
    }
    mfsk_demod(&m, sym, nbits, llr);
    int err = 0;
    for (int i = 0; i < nbits; i++)
        if (((llr[i] < 0) ? 1 : 0) != bits[i]) err++;
    return err; /* bit errors out of nbits */
}

void test_roundtrip_lossless(void)
{
    int Ms[] = {4, 8, 16, 32};
    for (int i = 0; i < 4; i++)
    {
        mfsk_init(&m, Ms[i], NC, 1);
        int bps = mfsk_bits_per_symbol(&m);
        int nbits = bps * NSYM;
        for (int b = 0; b < nbits; b++) bits[b] = (int)(urand() < 0.5);
        memset(sym, 0, sizeof(mfsk_cplx) * (size_t)NSYM * NC);
        mfsk_mod(&m, bits, nbits, sym);
        mfsk_demod(&m, sym, nbits, llr);
        for (int b = 0; b < nbits; b++)
            TEST_ASSERT_EQUAL_INT(bits[b], (llr[b] < 0) ? 1 : 0);
    }
}

void test_bits_per_symbol(void)
{
    mfsk_init(&m, 32, NC, 1);  TEST_ASSERT_EQUAL_INT(5, mfsk_bits_per_symbol(&m));
    mfsk_init(&m, 4,  NC, 1);  TEST_ASSERT_EQUAL_INT(2, mfsk_bits_per_symbol(&m));
    mfsk_init(&m, 8,  NC, 2);  TEST_ASSERT_EQUAL_INT(6, mfsk_bits_per_symbol(&m));
}

void test_higher_M_more_robust_awgn(void)
{
    /* At a fixed Eb/N0 on AWGN, 32-MFSK must beat 2-FSK (power efficiency). */
    int err_m2  = run_awgn(2,  6.0, 0);
    int err_m32 = run_awgn(32, 6.0, 0);
    TEST_ASSERT_TRUE(err_m32 < err_m2);
}

void test_ofdm_framing_roundtrip(void)
{
    /* Nc bins -> zero-pad -> IFFT -> +CP -> -CP -> FFT -> depad recovers bins.
     * (fft is 1/N-normalized, ifft unnormalized, so the pair is identity.) */
    ofdm_frame_t o;
    ofdm_frame_init(&o, 64, 50, 0.25, 0);
    double complex bins[64], pad[64], t[64], cp[80], rmv[64], fftd[64], out[64];
    for (int i = 0; i < o.Nc; i++)
        bins[i] = (urand() - 0.5) + (urand() - 0.5) * I;
    ofdm_zero_padder(&o, bins, pad);
    ofdm_ifft(&o, pad, t);
    ofdm_gi_adder(&o, t, cp);
    ofdm_gi_remover(&o, cp, rmv);
    ofdm_fft(&o, rmv, fftd);
    ofdm_zero_depadder(&o, fftd, out);
    for (int i = 0; i < o.Nc; i++)
        TEST_ASSERT_TRUE(cabs(out[i] - bins[i]) < 1e-9);
}

void test_preamble_acquisition(void)
{
    /* Plant the preamble template in a noise buffer at a known offset; the
     * matched-filter search must find it (high SNR) and reject pure noise. */
    mfsk_init(&m, 32, 50, 1);
    ofdm_frame_t o; ofdm_frame_init(&o, 64, 50, 0.25, 0);
    int Nofdm = ofdm_frame_nofdm(&o);
    double complex tmpl[8 * 128]; double sym_e[8];
    int P = mfsk_sync_build_template(&m, &o, tmpl, sym_e);

    int gap = 3 * Nofdm, true_off = gap;
    int L = gap + P * Nofdm + gap;
    double complex rx[64 * 128];
    double n = 0.02;                       /* small noise -> high SNR */
    for (int i = 0; i < L; i++) rx[i] = n * (urand() - 0.5) + n * (urand() - 0.5) * I;
    for (int i = 0; i < P * Nofdm; i++) rx[true_off + i] += tmpl[i];

    double metric = 0;
    int off = mfsk_sync_search(rx, L, 1, tmpl, sym_e, P, Nofdm, 0, &metric);
    TEST_ASSERT_TRUE(off >= 0);                       /* detected */
    TEST_ASSERT_TRUE(abs(off - true_off) <= Nofdm);   /* at the right place */
    TEST_ASSERT_TRUE(metric > 0.5);

    /* pure noise -> no detection */
    for (int i = 0; i < L; i++) rx[i] = n * (urand() - 0.5) + n * (urand() - 0.5) * I;
    int off2 = mfsk_sync_search(rx, L, 1, tmpl, sym_e, P, Nofdm, 0, &metric);
    TEST_ASSERT_EQUAL_INT(-1, off2);
}

void test_ldpc_encode_decode(void)
{
    /* Encoder produces valid codewords (H*c=0) and noiseless LLRs decode back
     * to the info bits (converged). */
    int info[MFSK_LDPC_K], coded[MFSK_LDPC_N], out[MFSK_LDPC_K];
    float llr[MFSK_LDPC_N];
    for (int i = 0; i < MFSK_LDPC_K; i++) info[i] = (int)(urand() < 0.5);
    mfsk_ldpc_encode(info, coded);

    for (int c = 0; c < MFSK_LDPC_P; c++)
    {
        int par = 0;
        for (int j = 0; j < MFSK_LDPC_CWIDTH; j++)
        {
            int v = mfsk_ldpc_QCmatrixC[c][j];
            if (v >= 0) par ^= coded[v];
        }
        TEST_ASSERT_EQUAL_INT(0, par);            /* H*c = 0 */
    }

    for (int i = 0; i < MFSK_LDPC_N; i++) llr[i] = coded[i] ? -10.0f : 10.0f;
    int conv = mfsk_ldpc_decode(llr, out, 50);
    TEST_ASSERT_TRUE(conv);
    for (int i = 0; i < MFSK_LDPC_K; i++)
        TEST_ASSERT_EQUAL_INT(info[i], out[i]);
}

void test_postamble(void)
{
    /* Postamble tones are distinct from the preamble, and its own template
     * acquires (dual-ended acquisition support). */
    mfsk_init(&m, 32, 50, 1);
    int differ = 0;
    for (int i = 0; i < m.preamble_nSymb; i++)
        if (m.postamble_tones[i] != m.preamble_tones[i]) differ = 1;
    TEST_ASSERT_TRUE(differ);

    ofdm_frame_t o; ofdm_frame_init(&o, 64, 50, 0.25, 0);
    int Nofdm = ofdm_frame_nofdm(&o);
    double complex tmpl[8 * 128]; double se[8];
    int P = mfsk_sync_build_postamble_template(&m, &o, tmpl, se);
    int gap = 3 * Nofdm, true_off = gap, L = gap + P * Nofdm + gap;
    double complex rx[64 * 128];
    for (int i = 0; i < L; i++) rx[i] = 0.02 * (urand() - 0.5) + 0.02 * (urand() - 0.5) * I;
    for (int i = 0; i < P * Nofdm; i++) rx[true_off + i] += tmpl[i];
    double metric = 0;
    int off = mfsk_sync_search(rx, L, 1, tmpl, se, P, Nofdm, 0, &metric);
    TEST_ASSERT_TRUE(off >= 0);
    TEST_ASSERT_TRUE(abs(off - true_off) <= Nofdm);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_lossless);
    RUN_TEST(test_bits_per_symbol);
    RUN_TEST(test_higher_M_more_robust_awgn);
    RUN_TEST(test_ofdm_framing_roundtrip);
    RUN_TEST(test_preamble_acquisition);
    RUN_TEST(test_ldpc_encode_decode);
    RUN_TEST(test_postamble);
    return UNITY_END();
}
