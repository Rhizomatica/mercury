/* Deterministic unit tests for the tone-pattern signalling channel.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Verifies the OFDM framing round-trip, that the preamble and postamble
 * templates acquire (including well below the coherent-OFDM threshold), that
 * noise alone is never accepted as either, and that the ACK / ACK+TURN
 * patterns are detected and told apart.
 *
 * The codec tests that used to live here went with the MFSK data waveform.
 * Fixed PRNG seed -> deterministic.
 */
#include "unity.h"
#include "mfsk.h"
#include "mfsk_ofdm.h"
#include "mfsk_sync.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <complex.h>

#define NC       50

static mfsk_t m;

void setUp(void) {}
void tearDown(void) {}

/* deterministic U(0,1) */
static unsigned long s_rng = 88172645463325252ULL;
static double urand(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return ((s_rng >> 11) + 1.0) / ((1ULL << 53) + 1.0);
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

void test_weak_preamble_acquisition(void)
{
    /* The acceptance threshold -- not the LDPC -- set the MFSK fringe floor.
     * The per-symbol statistic is SNR_sym/(1+SNR_sym), so the original 0.5 gate
     * demanded SNR_sym >= 0 dB and every code rate from 1/2 to 1/16 produced an
     * identical FER curve, because nothing weaker ever reached the decoder.
     * Plant a preamble well under that gate: it must be found, and pure noise at
     * the same level must not be. */
    mfsk_init(&m, 32, 50, 1);
    ofdm_frame_t o; ofdm_frame_init(&o, 64, 50, 0.25, 0);
    int Nofdm = ofdm_frame_nofdm(&o);
    double complex tmpl[8 * 128]; double sym_e[8];
    int P = mfsk_sync_build_template(&m, &o, tmpl, sym_e);

    int gap = 3 * Nofdm, true_off = gap;
    int L = gap + P * Nofdm + gap;
    static double complex rx[64 * 128];

    /* (urand()-0.5) has variance 1/12 per component, so the complex noise
     * variance is n^2/6; pick n for a per-symbol SNR of 0.25 (~ -6 dB), which
     * lands the correlation metric near 0.2. */
    const double target_snr = 0.25;
    double n = sqrt(6.0 * sym_e[0] / (Nofdm * target_snr));

    int found = 0; double weak_metric = 1.0;
    for (int t = 0; t < 10; t++)
    {
        for (int i = 0; i < L; i++)
            rx[i] = n * (urand() - 0.5) + n * (urand() - 0.5) * I;
        for (int i = 0; i < P * Nofdm; i++) rx[true_off + i] += tmpl[i];
        double metric = 0;
        int off = mfsk_sync_search(rx, L, 1, tmpl, sym_e, P, Nofdm, 0, &metric);
        if (off >= 0 && abs(off - true_off) <= Nofdm)
        {
            found++;
            if (metric < weak_metric) weak_metric = metric;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found >= 8,
        "weak preamble not acquired -- has the accept threshold been raised?");
    TEST_ASSERT_TRUE_MESSAGE(weak_metric < 0.5,
        "test signal was not actually weak: the old 0.5 gate would have taken it");

    /* Same noise, no signal: the lower threshold must not invent a preamble. */
    int falsehits = 0;
    for (int t = 0; t < 10; t++)
    {
        for (int i = 0; i < L; i++)
            rx[i] = n * (urand() - 0.5) + n * (urand() - 0.5) * I;
        double metric = 0;
        if (mfsk_sync_search(rx, L, 1, tmpl, sym_e, P, Nofdm, 0, &metric) >= 0)
            falsehits++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, falsehits, "false preamble detected in noise");
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

void test_pattern_detect(void)
{
    /* Plant an ACK tone pattern in a noise buffer; detector must (a) match ~all
     * symbols at the right offset, (b) score pure noise below threshold, and
     * (c) not confuse the BREAK pattern for ACK (order-aware discrimination). */
    mfsk_init(&m, 32, 50, 1);
    ofdm_frame_t o; ofdm_frame_init(&o, 64, 50, 0.25, 0);
    int Nofdm = ofdm_frame_nofdm(&o);
    int ns = m.ack_pattern_nsymb;                 /* 16 for M=32 */

    /* build the ACK burst time-domain (freq bins -> OFDM symbols) */
    static mfsk_cplx abins[48 * 50];
    mfsk_generate_ack_pattern(&m, abins);
    int gap = 3 * Nofdm, true_off = gap, L = gap + ns * Nofdm + gap;
    static double complex rx[64 * 128 * 20];
    double n = 0.02;
    for (int i = 0; i < L; i++) rx[i] = n * (urand() - 0.5) + n * (urand() - 0.5) * I;
    for (int s = 0; s < ns; s++)
    {
        double complex bins[64], pad[64], t[64], cp[80];
        for (int k = 0; k < o.Nc; k++)
            bins[k] = abins[s * m.Nc + k].re + abins[s * m.Nc + k].im * I;
        ofdm_zero_padder(&o, bins, pad);
        ofdm_ifft(&o, pad, t);
        ofdm_gi_adder(&o, t, cp);
        for (int j = 0; j < Nofdm; j++) rx[true_off + s * Nofdm + j] += cp[j];
    }

    int pos = -2;
    int matched = mfsk_detect_pattern(&m, &o, rx, L, m.ack_tones,
                                      m.ack_pattern_len, ns, &pos);
    TEST_ASSERT_TRUE(matched >= m.ack_match_threshold);   /* detected */
    TEST_ASSERT_TRUE(abs(pos - true_off) <= Nofdm);       /* at the ACK */

    /* pure noise -> below threshold */
    for (int i = 0; i < L; i++) rx[i] = n * (urand() - 0.5) + n * (urand() - 0.5) * I;
    int m2 = mfsk_detect_pattern(&m, &o, rx, L, m.ack_tones,
                                 m.ack_pattern_len, ns, &pos);
    TEST_ASSERT_TRUE(m2 < m.ack_match_threshold);

    /* a BREAK burst must NOT be mistaken for ACK (different tone sequence) */
    static mfsk_cplx bbins[48 * 50];
    mfsk_generate_break_pattern(&m, bbins);
    for (int i = 0; i < L; i++) rx[i] = n * (urand() - 0.5) + n * (urand() - 0.5) * I;
    for (int s = 0; s < ns; s++)
    {
        double complex bins[64], pad[64], t[64], cp[80];
        for (int k = 0; k < o.Nc; k++)
            bins[k] = bbins[s * m.Nc + k].re + bbins[s * m.Nc + k].im * I;
        ofdm_zero_padder(&o, bins, pad);
        ofdm_ifft(&o, pad, t);
        ofdm_gi_adder(&o, t, cp);
        for (int j = 0; j < Nofdm; j++) rx[true_off + s * Nofdm + j] += cp[j];
    }
    int m3 = mfsk_detect_pattern(&m, &o, rx, L, m.ack_tones,
                                 m.ack_pattern_len, ns, &pos);
    TEST_ASSERT_TRUE(m3 < m.ack_match_threshold);         /* not an ACK */
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ofdm_framing_roundtrip);
    RUN_TEST(test_preamble_acquisition);
    RUN_TEST(test_weak_preamble_acquisition);
    RUN_TEST(test_postamble);
    RUN_TEST(test_pattern_detect);
    return UNITY_END();
}
