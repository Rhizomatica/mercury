/*
 * Offline resampler tests — validate the polyphase 8 kHz <-> 48 kHz resampler
 * without a sound card: boundary continuity (issue #81), anti-imaging on
 * upsample, anti-aliasing on downsample, and passband flatness.  Spectral
 * levels are measured with the Goertzel algorithm at exact DFT bins.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unity.h"
#include "resampler.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FS8   8000
#define FS48  48000
#define AMP   1.0e9      /* well below full scale, no clamping */

/* Goertzel magnitude (tone amplitude estimate) at exact bin freq f. */
static double goertzel(const int32_t *x, int n, int fs, double f)
{
    double w = 2.0 * M_PI * f / fs;
    double coeff = 2.0 * cos(w);
    double s0, s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; i++) {
        s0 = (double)x[i] + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    if (power < 0) power = 0;
    return 2.0 * sqrt(power) / n;   /* ~tone amplitude */
}

static double db(double num, double den) { return 20.0 * log10(num / den + 1e-300); }

void setUp(void)    { resampler_global_init(); }
void tearDown(void) { }

/* ---- Upsample: image rejection ---- */
/* A 2400 Hz tone (top of the modem band) upsampled to 48 kHz images at
 * 8000-2400 = 5600 Hz.  Linear interpolation left that image only ~14 dB
 * down; the FIR must push it well below the radio's needs. */
void test_upsample_image_rejection(void)
{
    const int n8 = 4000;
    int32_t *in = malloc(sizeof(int32_t) * n8);
    int32_t *out = malloc(sizeof(int32_t) * n8 * RESAMP_L);
    for (int i = 0; i < n8; i++)
        in[i] = (int32_t)(AMP * sin(2.0 * M_PI * 2400.0 * i / FS8));

    resamp_up_t r; resamp_up_reset(&r);
    int no = resamp_up_process(&r, in, n8, out);

    /* discard filter startup transient */
    int skip = RESAMP_NTAPS;
    double fund  = goertzel(out + skip, no - skip, FS48, 2400.0);
    double image = goertzel(out + skip, no - skip, FS48, 5600.0);
    double rej = db(image, fund);
    TEST_ASSERT_TRUE_MESSAGE(rej < -45.0, "upsample image must be >45 dB down");

    free(in); free(out);
}

/* ---- Upsample: passband flatness ---- */
void test_upsample_passband_flat(void)
{
    const int n8 = 4000;
    int32_t *in = malloc(sizeof(int32_t) * n8);
    int32_t *out = malloc(sizeof(int32_t) * n8 * RESAMP_L);
    for (int i = 0; i < n8; i++)
        in[i] = (int32_t)(AMP * sin(2.0 * M_PI * 1500.0 * i / FS8));

    resamp_up_t r; resamp_up_reset(&r);
    int no = resamp_up_process(&r, in, n8, out);

    int skip = RESAMP_NTAPS;
    double out_amp = goertzel(out + skip, no - skip, FS48, 1500.0);
    /* unity passband gain: output tone amplitude ~= input amplitude */
    double g = db(out_amp, AMP);
    TEST_ASSERT_TRUE_MESSAGE(g > -1.0 && g < 1.0, "1500 Hz passband must be flat (+-1 dB)");

    free(in); free(out);
}

/* ---- Downsample: alias rejection ---- */
/* A 5600 Hz tone at 48 kHz would fold to |5600-8000| = 2400 Hz when decimated
 * to 8 kHz.  The bare decimator had NO filter, so it aliased at full level;
 * the FIR must reject it. */
void test_downsample_alias_rejection(void)
{
    const int n48 = 24000;
    int32_t *in = malloc(sizeof(int32_t) * n48);
    int32_t *out = malloc(sizeof(int32_t) * (n48 / RESAMP_L + 2));
    for (int i = 0; i < n48; i++)
        in[i] = (int32_t)(AMP * sin(2.0 * M_PI * 5600.0 * i / FS48));

    resamp_down_t r; resamp_down_reset(&r);
    int no = resamp_down_process(&r, in, n48, out);

    int skip = RESAMP_NTAPS / RESAMP_L;
    double alias = goertzel(out + skip, no - skip, FS8, 2400.0);
    double rej = db(alias, AMP);
    TEST_ASSERT_TRUE_MESSAGE(rej < -45.0, "downsample alias must be >45 dB down");

    free(in); free(out);
}

/* ---- Downsample: passband flatness ---- */
void test_downsample_passband_flat(void)
{
    const int n48 = 24000;
    int32_t *in = malloc(sizeof(int32_t) * n48);
    int32_t *out = malloc(sizeof(int32_t) * (n48 / RESAMP_L + 2));
    for (int i = 0; i < n48; i++)
        in[i] = (int32_t)(AMP * sin(2.0 * M_PI * 1500.0 * i / FS48));

    resamp_down_t r; resamp_down_reset(&r);
    int no = resamp_down_process(&r, in, n48, out);

    int skip = RESAMP_NTAPS / RESAMP_L;
    double out_amp = goertzel(out + skip, no - skip, FS8, 1500.0);
    double g = db(out_amp, AMP);
    TEST_ASSERT_TRUE_MESSAGE(g > -1.0 && g < 1.0, "1500 Hz passband must be flat (+-1 dB)");

    free(in); free(out);
}

/* ---- Continuity: whole vs period-chunked output must be identical ---- */
/* This is the issue #81 invariant generalised to the FIR: feeding the same
 * continuous signal in one shot or split into read-periods produces the same
 * output, i.e. period boundaries are invisible. */
void test_upsample_chunking_invariant(void)
{
    const int n8 = 8000, period = 160;
    int32_t *in = malloc(sizeof(int32_t) * n8);
    int32_t *whole = malloc(sizeof(int32_t) * n8 * RESAMP_L);
    int32_t *chunk = malloc(sizeof(int32_t) * n8 * RESAMP_L);
    for (int i = 0; i < n8; i++)
        in[i] = (int32_t)(AMP * sin(2.0 * M_PI * 1000.0 * i / FS8));

    resamp_up_t r1; resamp_up_reset(&r1);
    resamp_up_process(&r1, in, n8, whole);

    resamp_up_t r2; resamp_up_reset(&r2);
    int o = 0;
    for (int p = 0; p * period < n8; p++)
        o += resamp_up_process(&r2, in + p * period, period, chunk + o);

    for (int k = 0; k < n8 * RESAMP_L; k++)
        TEST_ASSERT_EQUAL_INT32(whole[k], chunk[k]);

    free(in); free(whole); free(chunk);
}

void test_downsample_chunking_invariant(void)
{
    const int n48 = 48000, period = 960;
    int32_t *in = malloc(sizeof(int32_t) * n48);
    int32_t *whole = malloc(sizeof(int32_t) * (n48 / RESAMP_L + 2));
    int32_t *chunk = malloc(sizeof(int32_t) * (n48 / RESAMP_L + 2));
    for (int i = 0; i < n48; i++)
        in[i] = (int32_t)(AMP * sin(2.0 * M_PI * 1000.0 * i / FS48));

    resamp_down_t r1; resamp_down_reset(&r1);
    int ow = resamp_down_process(&r1, in, n48, whole);

    resamp_down_t r2; resamp_down_reset(&r2);
    int oc = 0;
    for (int p = 0; p * period < n48; p++)
        oc += resamp_down_process(&r2, in + p * period, period, chunk + oc);

    TEST_ASSERT_EQUAL_INT(ow, oc);
    for (int k = 0; k < ow; k++)
        TEST_ASSERT_EQUAL_INT32(whole[k], chunk[k]);

    free(in); free(whole); free(chunk);
}


/* ---- Runtime ratio selection ---- */
/* Mercury asks for 48 kHz but takes what the device gives.  Resampling by the
 * ratio we WANTED rather than the one we GOT transmits off-frequency with no
 * other symptom, so the mapping itself is worth pinning. */
void test_ratio_for_rate_supported(void)
{
    TEST_ASSERT_EQUAL_INT(1,  resampler_ratio_for_rate(8000));
    TEST_ASSERT_EQUAL_INT(2,  resampler_ratio_for_rate(16000));
    TEST_ASSERT_EQUAL_INT(3,  resampler_ratio_for_rate(24000));
    TEST_ASSERT_EQUAL_INT(4,  resampler_ratio_for_rate(32000));
    TEST_ASSERT_EQUAL_INT(6,  resampler_ratio_for_rate(48000));
    TEST_ASSERT_EQUAL_INT(12, resampler_ratio_for_rate(96000));
}

void test_ratio_for_rate_rejected(void)
{
    /* 44.1 kHz is not an integer multiple of 8 kHz — it needs a rational
     * (441/80) resampler, so it must be refused, not approximated. */
    TEST_ASSERT_EQUAL_INT(0, resampler_ratio_for_rate(44100));
    TEST_ASSERT_EQUAL_INT(0, resampler_ratio_for_rate(22050));
    TEST_ASSERT_EQUAL_INT(0, resampler_ratio_for_rate(192000));  /* L=24 > max */
    TEST_ASSERT_EQUAL_INT(0, resampler_ratio_for_rate(0));
    TEST_ASSERT_EQUAL_INT(0, resampler_ratio_for_rate(-48000));
}

/* A tone must come back at the SAME frequency at every supported ratio.
 * This is the regression for the measured failure: with a hardcoded 1:6, a
 * device at 8 kHz turned a 1000 Hz tone into 166.8 Hz. */
static void assert_roundtrip_preserves_tone(int L)
{
    const int fs_dev = FS8 * L;
    const double f    = 1000.0;
    const int n8      = 8000;                 /* 1 s of modem audio */
    int32_t *in   = malloc(sizeof(int32_t) * n8);
    int32_t *up   = malloc(sizeof(int32_t) * (size_t)n8 * L);
    int32_t *down = malloc(sizeof(int32_t) * (n8 + 8));
    TEST_ASSERT_NOT_NULL(in); TEST_ASSERT_NOT_NULL(up); TEST_ASSERT_NOT_NULL(down);

    for (int i = 0; i < n8; i++)
        in[i] = (int32_t)(AMP * sin(2.0 * M_PI * f * i / FS8));

    resampler_init_up(L);
    resampler_init_down(L);

    resamp_up_t u; resamp_up_reset(&u);
    int nu = resamp_up_process(&u, in, n8, up);
    TEST_ASSERT_EQUAL_INT(n8 * L, nu);

    resamp_down_t d; resamp_down_reset(&d);
    int nd = resamp_down_process(&d, up, nu, down);
    TEST_ASSERT_INT_WITHIN(2, n8, nd);

    /* Skip the filter transient at both ends, then confirm the tone is at f
     * and that no strong component sits at the frequency a wrong ratio would
     * have produced (f/L or f*L). */
    int skip = RESAMP_NTAPS_MAX / L + 64;
    int nmeas = nd - 2 * skip;
    TEST_ASSERT_TRUE(nmeas > 1000);
    double at_f = goertzel(down + skip, nmeas, FS8, f);
    TEST_ASSERT_TRUE_MESSAGE(at_f > 0.5 * AMP, "tone lost or attenuated");
    if (L > 1) {
        double wrong = goertzel(down + skip, nmeas, FS8, f / L);
        TEST_ASSERT_TRUE_MESSAGE(db(wrong, at_f) < -40.0,
                                 "energy at f/L — ratio mismatch");
    }
    (void)fs_dev;
    free(in); free(up); free(down);
}

void test_roundtrip_ratio_1(void)  { assert_roundtrip_preserves_tone(1);  }
void test_roundtrip_ratio_2(void)  { assert_roundtrip_preserves_tone(2);  }
void test_roundtrip_ratio_4(void)  { assert_roundtrip_preserves_tone(4);  }
void test_roundtrip_ratio_6(void)  { assert_roundtrip_preserves_tone(6);  }
void test_roundtrip_ratio_12(void) { assert_roundtrip_preserves_tone(12); }

/* L == 1 (device already at the modem rate) must be a bit-exact pass-through:
 * filtering there would only add delay and droop. */
void test_ratio_1_is_passthrough(void)
{
    const int n = 512;
    int32_t in[512], out[512];
    for (int i = 0; i < n; i++)
        in[i] = (int32_t)(AMP * sin(2.0 * M_PI * 1000.0 * i / FS8));

    resampler_init_up(1);
    resamp_up_t u; resamp_up_reset(&u);
    TEST_ASSERT_EQUAL_INT(n, resamp_up_process(&u, in, n, out));
    TEST_ASSERT_EQUAL_INT32_ARRAY(in, out, n);

    resampler_init_down(1);
    resamp_down_t d; resamp_down_reset(&d);
    TEST_ASSERT_EQUAL_INT(n, resamp_down_process(&d, in, n, out));
    TEST_ASSERT_EQUAL_INT32_ARRAY(in, out, n);
}

/* Playback and capture can land on DIFFERENT device rates; the two directions
 * keep separate tables, so setting one must not disturb the other. */
void test_up_and_down_ratios_are_independent(void)
{
    resampler_init_up(2);
    resampler_init_down(6);

    const int n8 = 2048;
    int32_t *in  = malloc(sizeof(int32_t) * n8);
    int32_t *up  = malloc(sizeof(int32_t) * (size_t)n8 * 2);
    for (int i = 0; i < n8; i++)
        in[i] = (int32_t)(AMP * sin(2.0 * M_PI * 1000.0 * i / FS8));

    resamp_up_t u; resamp_up_reset(&u);
    TEST_ASSERT_EQUAL_INT(n8 * 2, resamp_up_process(&u, in, n8, up));   /* still 1:2 */

    int32_t *dn = malloc(sizeof(int32_t) * (n8 / 6 + 4));
    resamp_down_t d; resamp_down_reset(&d);
    int nd = resamp_down_process(&d, in, n8, dn);                        /* still 6:1 */
    TEST_ASSERT_INT_WITHIN(2, n8 / 6, nd);

    free(in); free(up); free(dn);
    resampler_global_init();   /* restore the default for later tests */
}

/* ---- circular history must be bit-identical to the naive shift ----
 *
 * The history used to be shifted one word at a time for every input sample --
 * at a 48 kHz device rate, 180 words moved 48000 times a second to feed a
 * filter that fires once every 6 samples.  It is now a doubled circular
 * buffer.  That is a pure representation change and MUST NOT alter a single
 * output sample: these are audio paths feeding a demodulator, and a resampler
 * that quietly rounds differently is far worse than a slow one.
 *
 * Floating-point addition is not associative, so the fast path deliberately
 * walks the taps in the same order the shift version did.  The references
 * below are the original implementations, kept verbatim. */

static int ref_down_process(int L, const int32_t *in, int n_in, int32_t *out,
                            int32_t *hist, int *phase)
{
    const int ntaps = L * RESAMP_TAPS_PER_PHASE;
    int o = 0;
    for (int i = 0; i < n_in; i++) {
        for (int t = ntaps - 1; t > 0; t--)
            hist[t] = hist[t - 1];
        hist[0] = in[i];
        if (++(*phase) >= L) {
            *phase = 0;
            double acc = 0.0;
            for (int t = 0; t < ntaps; t++)
                acc += (double)resampler_down_tap(t) * (double)hist[t];
            double v = acc;
            if (v >  2147483647.0) v =  2147483647.0;
            if (v < -2147483648.0) v = -2147483648.0;
            out[o++] = (int32_t)v;
        }
    }
    return o;
}

void test_circular_history_matches_naive_shift(void)
{
    const int rates[] = { 16000, 24000, 48000, 96000 };

    for (unsigned k = 0; k < sizeof(rates) / sizeof(rates[0]); k++)
    {
        int L = resampler_ratio_for_rate(rates[k]);
        TEST_ASSERT_GREATER_THAN_INT(0, L);
        resampler_init_down(L);

        resamp_down_t fast;
        resamp_down_reset(&fast);

        static int32_t ref_hist[2 * RESAMP_NTAPS_MAX];
        memset(ref_hist, 0, sizeof(ref_hist));
        int ref_phase = 0;

        /* Several chunks, so the comparison also covers state carried across
         * call boundaries -- the whole reason the history exists. */
        uint32_t rng = 0xC0FFEEu;
        for (int chunk = 0; chunk < 8; chunk++)
        {
            int n = 137 + chunk * 53;          /* deliberately not a multiple of L */
            static int32_t in[4096], a[4096], b[4096];
            for (int i = 0; i < n; i++) {
                rng = rng * 1664525u + 1013904223u;
                in[i] = (int32_t)((int)(rng >> 8) % 2000000) - 1000000;
            }

            int na = resamp_down_process(&fast, in, n, a);
            int nb = ref_down_process(L, in, n, b, ref_hist, &ref_phase);

            TEST_ASSERT_EQUAL_INT(nb, na);
            for (int i = 0; i < na; i++)
                TEST_ASSERT_EQUAL_INT32_MESSAGE(b[i], a[i],
                    "circular history changed an output sample");
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_upsample_image_rejection);
    RUN_TEST(test_upsample_passband_flat);
    RUN_TEST(test_downsample_alias_rejection);
    RUN_TEST(test_downsample_passband_flat);
    RUN_TEST(test_upsample_chunking_invariant);
    RUN_TEST(test_downsample_chunking_invariant);
    RUN_TEST(test_ratio_for_rate_supported);
    RUN_TEST(test_ratio_for_rate_rejected);
    RUN_TEST(test_roundtrip_ratio_1);
    RUN_TEST(test_roundtrip_ratio_2);
    RUN_TEST(test_roundtrip_ratio_4);
    RUN_TEST(test_roundtrip_ratio_6);
    RUN_TEST(test_roundtrip_ratio_12);
    RUN_TEST(test_ratio_1_is_passthrough);
    RUN_TEST(test_up_and_down_ratios_are_independent);
    RUN_TEST(test_circular_history_matches_naive_shift);
    return UNITY_END();
}
