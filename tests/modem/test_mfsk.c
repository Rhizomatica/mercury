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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_lossless);
    RUN_TEST(test_bits_per_symbol);
    RUN_TEST(test_higher_M_more_robust_awgn);
    RUN_TEST(test_ofdm_framing_roundtrip);
    return UNITY_END();
}
