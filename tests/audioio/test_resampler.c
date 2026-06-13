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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_upsample_image_rejection);
    RUN_TEST(test_upsample_passband_flat);
    RUN_TEST(test_downsample_alias_rejection);
    RUN_TEST(test_downsample_passband_flat);
    RUN_TEST(test_upsample_chunking_invariant);
    RUN_TEST(test_downsample_chunking_invariant);
    return UNITY_END();
}
