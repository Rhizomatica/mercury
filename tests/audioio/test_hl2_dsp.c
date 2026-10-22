/*
 * SSB modem tests for the -x hl2 transport (hl2_dsp.c).
 *
 * The filters' passbands and stopbands, which sideband each direction
 * uses (upper = positive frequencies of I + jQ), the opposite-sideband and
 * image rejection, the TX envelope and its limiter, the AGC's level and
 * clip protection, muting while transmitting, and that the result does not
 * depend on how the samples are chunked.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unity.h"
#include "hl2_dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS_IQ    48000
#define FS_AUDIO 8000

void setUp(void)    { }
void tearDown(void) { }

static double db(double x) { return 20.0 * log10(x > 1e-30 ? x : 1e-30); }

/* Unity's double asserts are compiled out here: check by hand and say what
 * was measured. */
#define ASSERT_NEAR(tol, want, got, what) do { \
        char m_[160]; double w_ = (want), g_ = (got); \
        snprintf(m_, sizeof(m_), "%s: %.2f, want %.2f +- %.2f", (what), g_, w_, (double)(tol)); \
        TEST_ASSERT_TRUE_MESSAGE(fabs(g_ - w_) <= (tol), m_); } while (0)
#define ASSERT_BELOW(limit, got, what) do { \
        char m_[160]; double g_ = (got); \
        snprintf(m_, sizeof(m_), "%s: %.2f, want < %.2f", (what), g_, (double)(limit)); \
        TEST_ASSERT_TRUE_MESSAGE(g_ < (limit), m_); } while (0)

/* Amplitude of the tone at hz in a real signal (Goertzel, normalised). */
static double tone_amp_real(const double *x, size_t n, double hz, double fs)
{
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; i++)
    {
        re += x[i] * cos(2.0 * M_PI * hz * (double) i / fs);
        im -= x[i] * sin(2.0 * M_PI * hz * (double) i / fs);
    }
    return 2.0 * sqrt(re * re + im * im) / (double) n;
}

/* Amplitude of the phasor e^{j 2 pi hz t} in interleaved int16 I/Q. */
static double tone_amp_iq(const int16_t *iq, size_t n, double hz, double fs)
{
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; i++)
    {
        double c = cos(2.0 * M_PI * hz * (double) i / fs), s = sin(2.0 * M_PI * hz * (double) i / fs);
        double xi = iq[2 * i] / 32767.0, xq = iq[2 * i + 1] / 32767.0;
        re += xi * c + xq * s;          /* (xi + j xq) e^{-j w i} */
        im += xq * c - xi * s;
    }
    return sqrt(re * re + im * im) / (double) n;
}

void test_lowpass_flat_to_3k_and_70db_down_by_4k6(void)
{
    for (double hz = 0; hz <= 3000; hz += 100)
        ASSERT_NEAR(0.1, 0.0, db(hl2_dsp_lowpass_gain(hz)), "low-pass passband dB");
    for (double hz = 4600; hz <= 24000; hz += 50)
        ASSERT_BELOW(-65.0, db(hl2_dsp_lowpass_gain(hz)), "low-pass stopband dB");
}

void test_bandpass_upper_sideband_only(void)
{
    for (double hz = 200; hz <= 2950; hz += 25)
        ASSERT_NEAR(0.2, 0.0, db(hl2_dsp_bandpass_gain(hz)), "band-pass passband dB");
    for (double hz = -4000; hz <= 50; hz += 25)            /* LSB and DC */
        ASSERT_BELOW(-60.0, db(hl2_dsp_bandpass_gain(hz)), "lower sideband dB");
    for (double hz = 3100; hz <= 4000; hz += 25)
        ASSERT_BELOW(-60.0, db(hl2_dsp_bandpass_gain(hz)), "above the passband dB");
}

/* Run n I/Q pairs through a fresh receiver; returns the 8 kHz output. */
static double *rx_run(const float *iq, size_t n, size_t *nout)
{
    static hl2_rx rx;
    hl2_rx_init(&rx);
    int32_t *out = malloc(sizeof(int32_t) * (n / 6 + HL2_DSP_BLOCK));
    size_t k = hl2_rx_process(&rx, iq, n, false, out);
    double *d = malloc(sizeof(double) * (k ? k : 1));
    for (size_t i = 0; i < k; i++)
        d[i] = out[i] / 2147483648.0;
    free(out);
    *nout = k;
    return d;
}

/* An upper-sideband tone at +1000 Hz and a lower-sideband one at -1700 Hz,
 * equal and weak (-80 dBFS): the audio carries 1000 Hz at the AGC's level,
 * and 1700 Hz more than 60 dB below it. */
void test_rx_demodulates_upper_sideband(void)
{
    const size_t n = FS_IQ * 3;
    float *iq = malloc(sizeof(float) * 2 * n);
    for (size_t i = 0; i < n; i++)
    {
        double t = (double) i / FS_IQ, a = 1e-4;
        iq[2 * i]     = (float)(a * (cos(2 * M_PI * 1000 * t) + cos(2 * M_PI * 1700 * t)));
        iq[2 * i + 1] = (float)(a * (sin(2 * M_PI * 1000 * t) - sin(2 * M_PI * 1700 * t)));
    }
    size_t k;
    double *a = rx_run(iq, n, &k);
    TEST_ASSERT_EQUAL_size_t(n / 6, k);
    const double *tail = a + k - FS_AUDIO;               /* last second */
    double usb = tone_amp_real(tail, FS_AUDIO, 1000, FS_AUDIO);
    double lsb = tone_amp_real(tail, FS_AUDIO, 1700, FS_AUDIO);
    ASSERT_NEAR(1.0, db(HL2_DSP_AGC_TARGET * sqrt(2.0)), db(usb), "USB tone dBFS");
    ASSERT_BELOW(-60.0, db(lsb) - db(usb), "opposite sideband dB");
    free(a);
    free(iq);
}

/* A 1500 Hz tone at half scale: one phasor at +1500 Hz of amplitude 0.5,
 * nothing at -1500 Hz, nor at the interpolation images 8000 +- 1500 Hz. */
void test_tx_modulates_upper_sideband(void)
{
    const size_t n = FS_AUDIO;
    int32_t *in = malloc(sizeof(int32_t) * n);
    int16_t *iq = malloc(sizeof(int16_t) * 2 * 6 * n);
    for (size_t i = 0; i < n; i++)
        in[i] = (int32_t)(0.5 * 2147483647.0 * cos(2 * M_PI * 1500 * (double) i / FS_AUDIO));
    hl2_tx tx;
    hl2_tx_init(&tx);
    hl2_tx_process(&tx, in, n, iq);

    const int16_t *t = iq + 2 * (6 * n / 2);              /* skip the start */
    size_t m = 6 * n / 2;
    double usb = tone_amp_iq(t, m, 1500, FS_IQ);
    ASSERT_NEAR(0.2, db(0.5), db(usb), "USB phasor dBFS");
    ASSERT_BELOW(-60.0, db(tone_amp_iq(t, m, -1500, FS_IQ)) - db(usb), "LSB dB");
    ASSERT_BELOW(-60.0, db(tone_amp_iq(t, m, 6500, FS_IQ)) - db(usb), "image +6500 dB");
    ASSERT_BELOW(-60.0, db(tone_amp_iq(t, m, 9500, FS_IQ)) - db(usb), "image +9500 dB");
    ASSERT_BELOW(-60.0, db(tone_amp_iq(t, m, -6500, FS_IQ)) - db(usb), "image -6500 dB");
    free(iq);
    free(in);
}

/* Two full-scale tones add to twice full scale: the limiter keeps every
 * I/Q sample on or inside the unit circle. */
void test_tx_limits_the_envelope(void)
{
    const size_t n = FS_AUDIO / 2;
    int32_t *in = malloc(sizeof(int32_t) * n);
    int16_t *iq = malloc(sizeof(int16_t) * 2 * 6 * n);
    for (size_t i = 0; i < n; i++)
    {
        double t = (double) i / FS_AUDIO;
        in[i] = (int32_t)(1073741823.0 * (cos(2 * M_PI * 1000 * t) + cos(2 * M_PI * 1100 * t)));
    }
    hl2_tx tx;
    hl2_tx_init(&tx);
    hl2_tx_process(&tx, in, n, iq);
    double peak = 0.0;
    for (size_t i = 0; i < 6 * n; i++)
    {
        double mag = hypot(iq[2 * i], iq[2 * i + 1]);
        peak = mag > peak ? mag : peak;
    }
    TEST_ASSERT_TRUE(peak <= 32768.0);
    TEST_ASSERT_TRUE(peak > 32000.0);
    free(iq);
    free(in);
}

/* TX then RX: a tone comes back as the same tone. */
void test_round_trip_keeps_the_tone(void)
{
    const size_t n = 2 * FS_AUDIO;
    int32_t *in = malloc(sizeof(int32_t) * n);
    int16_t *iq16 = malloc(sizeof(int16_t) * 2 * 6 * n);
    float *iq = malloc(sizeof(float) * 2 * 6 * n);
    for (size_t i = 0; i < n; i++)
        in[i] = (int32_t)(0.3 * 2147483647.0 * sin(2 * M_PI * 1234 * (double) i / FS_AUDIO));
    hl2_tx tx;
    hl2_tx_init(&tx);
    hl2_tx_process(&tx, in, n, iq16);
    for (size_t i = 0; i < 2 * 6 * n; i++)
        iq[i] = (float)(iq16[i] / 32767.0 * 1e-3);          /* -60 dB path */
    size_t k;
    double *a = rx_run(iq, 6 * n, &k);
    const double *tail = a + k - FS_AUDIO;
    double want = tone_amp_real(tail, FS_AUDIO, 1234, FS_AUDIO);
    TEST_ASSERT_TRUE(want > 0.1);
    ASSERT_BELOW(-50.0, db(tone_amp_real(tail, FS_AUDIO, 2766, FS_AUDIO)) - db(want), "mirror dB");
    free(a);
    free(iq);
    free(iq16);
    free(in);
}

/* Weak noise for 2 s, then a tone 60 dB stronger: no output sample comes
 * near full scale, and the level settles back to the target. */
void test_agc_never_clips_a_sudden_signal(void)
{
    const size_t n = FS_IQ * 4;
    float *iq = malloc(sizeof(float) * 2 * n);
    srand(7);
    for (size_t i = 0; i < n; i++)
    {
        double t = (double) i / FS_IQ;
        double ni = (rand() / (double) RAND_MAX - 0.5) * 2e-5;
        double nq = (rand() / (double) RAND_MAX - 0.5) * 2e-5;
        double a = i < n / 2 ? 0.0 : 1e-2;
        iq[2 * i]     = (float)(ni + a * cos(2 * M_PI * 800 * t));
        iq[2 * i + 1] = (float)(nq + a * sin(2 * M_PI * 800 * t));
    }
    size_t k;
    double *a = rx_run(iq, n, &k);
    double peak = 0.0, sq = 0.0;
    for (size_t i = 0; i < k; i++)
        peak = fabs(a[i]) > peak ? fabs(a[i]) : peak;
    for (size_t i = k - FS_AUDIO / 2; i < k; i++)
        sq += a[i] * a[i];
    ASSERT_BELOW(0.95, peak, "peak output");
    ASSERT_NEAR(1.0, db(HL2_DSP_AGC_TARGET), db(sqrt(sq / (FS_AUDIO / 2))), "settled RMS dBFS");
    free(a);
    free(iq);
}

/* While transmitting the receiver outputs silence and the AGC holds. */
void test_rx_mute_outputs_silence_and_holds_gain(void)
{
    static hl2_rx rx;
    hl2_rx_init(&rx);
    const size_t n = FS_IQ / 2;
    float *iq = malloc(sizeof(float) * 2 * n);
    int32_t *out = malloc(sizeof(int32_t) * (n / 6 + HL2_DSP_BLOCK));
    for (size_t i = 0; i < 2 * n; i++)
        iq[i] = 0.5f;                                   /* own TX leaking in */
    float g = rx.gain;
    size_t k = hl2_rx_process(&rx, iq, n, true, out);
    TEST_ASSERT_EQUAL_size_t(n / 6, k);
    for (size_t i = 0; i < k; i++)
        TEST_ASSERT_EQUAL_INT32(0, out[i]);
    TEST_ASSERT_EQUAL_FLOAT(g, rx.gain);
    free(out);
    free(iq);
}

/* One sample at a time or a packet (126) at a time: identical output. */
void test_chunking_does_not_change_the_output(void)
{
    const size_t n = 126 * 40;
    float *iq = malloc(sizeof(float) * 2 * n);
    for (size_t i = 0; i < n; i++)
    {
        iq[2 * i]     = (float)(1e-3 * cos(0.1 * (double) i) + 1e-4 * sin(0.013 * (double) i));
        iq[2 * i + 1] = (float)(1e-3 * sin(0.1 * (double) i));
    }
    static hl2_rx r1, r2;
    hl2_rx_init(&r1);
    hl2_rx_init(&r2);
    int32_t o1[n / 6 + HL2_DSP_BLOCK], o2[n / 6 + HL2_DSP_BLOCK];
    size_t k1 = 0, k2 = 0;
    for (size_t i = 0; i < n; i++)
        k1 += hl2_rx_process(&r1, iq + 2 * i, 1, false, o1 + k1);
    for (size_t i = 0; i < n; i += 126)
        k2 += hl2_rx_process(&r2, iq + 2 * i, 126, false, o2 + k2);
    TEST_ASSERT_EQUAL_size_t(k1, k2);
    TEST_ASSERT_EQUAL_INT32_ARRAY(o1, o2, k1);

    int32_t in[300];
    for (int i = 0; i < 300; i++)
        in[i] = (int32_t)(1e9 * sin(0.37 * i));
    int16_t t1[2 * 6 * 300], t2[2 * 6 * 300];
    hl2_tx x1, x2;
    hl2_tx_init(&x1);
    hl2_tx_init(&x2);
    for (int i = 0; i < 300; i++)
        hl2_tx_process(&x1, in + i, 1, t1 + 12 * i);
    hl2_tx_process(&x2, in, 300, t2);
    TEST_ASSERT_EQUAL_INT16_ARRAY(t1, t2, 2 * 6 * 300);
    free(iq);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lowpass_flat_to_3k_and_70db_down_by_4k6);
    RUN_TEST(test_bandpass_upper_sideband_only);
    RUN_TEST(test_rx_demodulates_upper_sideband);
    RUN_TEST(test_tx_modulates_upper_sideband);
    RUN_TEST(test_tx_limits_the_envelope);
    RUN_TEST(test_round_trip_keeps_the_tone);
    RUN_TEST(test_agc_never_clips_a_sudden_signal);
    RUN_TEST(test_rx_mute_outputs_silence_and_holds_gain);
    RUN_TEST(test_chunking_does_not_change_the_output);
    return UNITY_END();
}
