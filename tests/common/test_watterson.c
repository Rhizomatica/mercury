/* Watterson channel model — numerical sanity across sample rates.
 *
 * The model is the instrument behind every sensitivity number in this project,
 * so it needs a guard of its own. The Doppler tap is a 2nd-order Butterworth
 * driven by white noise; as fc/fs shrinks its poles crowd the unit circle, and
 * in single precision they can land OUTSIDE it. The filter then diverges
 * instead of fading, and the "measured SNR" becomes the blow-up rather than
 * the signal: at 48 kHz with 1 Hz Doppler this read +65.76 dB where 8 kHz read
 * -8.37 dB. Mercury runs at 8 kHz today, which is why it went unnoticed --
 * it surfaced only when driving a 48 kHz modem through the same channel.
 */
#include "unity.h"
#include "watterson.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* FNV-1a of the channel output for seed 1, two 1 Hz paths, No=-20 at 8 kHz.
 * Captured from the port that replaced rand() with mercury_prng, which was
 * verified byte-identical to the pre-port channel across three presets and
 * three noise levels.  The hash is over the numeric bit-pattern of each float
 * in a canonical byte order (see fnv1a_float below), so it names the same
 * channel on little- and big-endian hosts alike. */
#define GOLDEN_SEED1_HASH 0x4D5ADF92u

/* FNV-1a over the 32-bit bit-pattern of f, most-significant byte first.
 *
 * Hashing the raw in-memory bytes of a float ties the golden checksum to the
 * host byte order: the same IEEE-754 value is laid out LSB-first on little-
 * endian and MSB-first on big-endian, so a big-endian build (e.g. Debian
 * s390x) computed the same channel but a different hash and failed.  Reading
 * the bytes out of the numeric value in a fixed order removes the dependency
 * on how the host stores them while staying sensitive to a single changed bit. */
static void fnv1a_float(uint32_t *sum, float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    for (int k = 3; k >= 0; k--) {
        *sum ^= (unsigned char)(u >> (8 * k));
        *sum *= 16777619u;
    }
}

void setUp(void) {}
void tearDown(void) {}

static float measure(int fs, float doppler_hz, int two_path)
{
    watterson_t w;
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, fs));
    watterson_add_path(&w, 0.0f, doppler_hz, 0.0f, 1.0f);
    if (two_path) watterson_add_path(&w, 2.0f, doppler_hz, 0.0f, 1.0f);
    watterson_set_noise(&w, -20.0f);
    watterson_reset_meas(&w);

    int n = fs * 2;
    COMP *s = malloc(sizeof(COMP) * (size_t)n);
    TEST_ASSERT_NOT_NULL(s);
    for (int i = 0; i < n; i++) {
        s[i].real = 3000.0f * sinf(2.0f * (float)M_PI * 1500.0f * i / fs);
        s[i].imag = 0.0f;
    }
    watterson_process(&w, s, n);

    for (int i = 0; i < n; i++)
        TEST_ASSERT_TRUE_MESSAGE(isfinite(s[i].real) && isfinite(s[i].imag),
                                 "channel produced a non-finite sample");
    float snr = watterson_measured_snr3k(&w);
    free(s);
    watterson_dispose(&w);
    return snr;
}

void test_fading_is_stable_across_sample_rates(void)
{
    /* Same physical channel at every rate, so the measured SNR3k must land in
     * the same ballpark. The band is wide because a 2 s window of slow fading
     * samples only a few fades and scatters by several dB -- it is here to
     * catch divergence, not to pin a value. */
    const int rates[] = { 8000, 16000, 24000, 48000, 96000 };
    const float dopplers[] = { 0.1f, 0.5f, 1.0f, 2.0f };

    for (unsigned r = 0; r < sizeof(rates)/sizeof(rates[0]); r++) {
        for (unsigned d = 0; d < sizeof(dopplers)/sizeof(dopplers[0]); d++) {
            float snr = measure(rates[r], dopplers[d], 1);
            char msg[128];
            snprintf(msg, sizeof msg,
                     "fs=%d doppler=%.1f Hz gave SNR3k=%.2f dB (filter diverged?)",
                     rates[r], dopplers[d], snr);
            TEST_ASSERT_TRUE_MESSAGE(isfinite(snr), msg);
            TEST_ASSERT_TRUE_MESSAGE(snr > -40.0f && snr < 5.0f, msg);
        }
    }
}

void test_single_static_path_is_rate_invariant(void)
{
    /* One static path is the clean cross-rate invariant: the tap is a
     * constant, so an identical channel must measure identically whatever the
     * sample rate. This is what makes an 8 kHz result comparable with a 48 kHz
     * one, so it is worth pinning tightly -- measured agreement is 0.04 dB.
     *
     * Deliberately NOT two paths: summing a delayed copy adds a multipath
     * interference term that genuinely does depend on the rate (~1.5 dB here),
     * which would make this a test of interference rather than of the model's
     * rate handling. */
    float a = measure(8000, 0.0f, 0), b = measure(48000, 0.0f, 0);
    TEST_ASSERT_TRUE(isfinite(a) && isfinite(b));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, a, b);
}

/* A fixed seed must name a fixed channel, FOREVER.
 *
 * That is the whole value of seeding: a fading result quoted in a commit
 * message has to be replayable, on this machine and on someone else's.  The
 * realisation depends not only on the generator but on the ORDER and COUNT of
 * the draws -- watterson_add_path() consumes some to estimate tap_norm before
 * a single sample is processed -- so a refactor that merely moves a draw
 * silently invalidates every recorded measurement without failing anything.
 *
 * Hence a golden checksum rather than a statistical assertion.  If this test
 * fails, the channel changed: either fix the change, or accept it deliberately
 * and re-measure everything that was pinned to a seed.
 *
 * The checksum is over the raw sample bytes, so it only means anything where
 * float expressions are evaluated in float.  On i386 the x87 unit keeps
 * intermediates in 80-bit registers, so the same source produces different
 * low bits -- and different ones again at a different optimisation level,
 * which is why Debian's i386 build and a local -m32 build disagreed with each
 * other as well as with this value (issue #299).  FLT_EVAL_METHOD says
 * exactly that: 0 means each expression is evaluated in its own type, 2 means
 * everything goes through long double.  Where it is not 0 the golden value is
 * not a meaningful assertion, so say so instead of failing.  Building i386
 * with -mfpmath=sse also makes it 0, and then the value matches amd64 exactly.
 */
void test_a_pinned_seed_reproduces_a_fixed_realisation(void)
{
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
    TEST_IGNORE_MESSAGE("excess floating-point precision (FLT_EVAL_METHOD != 0): "
                        "a bit-exact channel checksum is not reproducible here; "
                        "build with -mfpmath=sse to check it on this target");
#endif
    const int   fs = 8000;
    const int   n  = fs;
    watterson_t w;
    uint32_t    sum = 0;

    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, fs));
    watterson_seed(&w, 1);
    /* the golden hash is the Butterworth-shaped channel this test shipped with */
    TEST_ASSERT_EQUAL_INT(0, watterson_set_spectrum(&w, WATTERSON_SPECTRUM_LEGACY));
    watterson_add_path(&w, 0.0f, 1.0f, 0.0f, 0.7f);
    watterson_add_path(&w, 2.0f, 1.0f, 0.0f, 0.7f);
    watterson_set_noise(&w, -20.0f);
    watterson_reset_meas(&w);

    COMP *s = malloc(sizeof(COMP) * (size_t)n);
    TEST_ASSERT_NOT_NULL(s);
    for (int i = 0; i < n; i++) {
        s[i].real = 3000.0f * sinf(2.0f * (float)M_PI * 1500.0f * i / fs);
        s[i].imag = 0.0f;
    }
    watterson_process(&w, s, n);

    /* FNV-1a over each sample's float bit-patterns: sensitive to a single
     * changed bit, independent of host byte order (see fnv1a_float). */
    sum = 2166136261u;
    for (int i = 0; i < n; i++) {
        fnv1a_float(&sum, s[i].real);
        fnv1a_float(&sum, s[i].imag);
    }
    free(s);

    TEST_ASSERT_EQUAL_HEX32(GOLDEN_SEED1_HASH, sum);

    /* And the seed must be reported back, so a run can say what it used. */
    TEST_ASSERT_EQUAL_UINT(1, watterson_get_seed(&w));
    watterson_dispose(&w);
}

/* The Gaussian spectrum is the standard's model (ITU-R F.1487 / CCIR 520):
 * for a path with spread D the tap's Doppler power spectrum is Gaussian with
 * standard deviation D/2, so its autocorrelation is r(tau) = exp(-2 pi^2
 * (D/2)^2 tau^2) and r at tau = 1/(pi D) is exp(-1/2).  Measure the tap
 * through a unit DC input (the output IS the tap): unit power, the standard
 * width, and the same answers at 8 and 48 kHz. */
static void tap_stats(int fs, float spread_hz, double *power, double *r_half)
{
    watterson_t w;
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, fs));
    watterson_seed(&w, 11);
    watterson_add_path(&w, 0.0f, spread_hz, 0.0f, 1.0f);
    int n = fs * 400;                                   /* 400 s: ~200 cycles of a 0.5 Hz process */
    COMP *s = malloc(sizeof(COMP) * (size_t)n);
    TEST_ASSERT_NOT_NULL(s);
    for (int i = 0; i < n; i++) { s[i].real = 1.0f; s[i].imag = 0.0f; }
    watterson_process(&w, s, n);
    int lag = (int)lrint((double)fs / (M_PI * (double)spread_hz));
    double p = 0.0, r = 0.0;
    for (int i = 0; i < n; i++) p += (double)s[i].real * s[i].real + (double)s[i].imag * s[i].imag;
    for (int i = 0; i + lag < n; i++) r += (double)s[i].real * s[i + lag].real + (double)s[i].imag * s[i + lag].imag;
    *power = p / n;
    *r_half = (r / (n - lag)) / (p / n);
    free(s);
    watterson_dispose(&w);
}

void test_gaussian_tap_has_unit_power_and_the_standard_width(void)
{
    double power, r_half;
    tap_stats(8000, 1.0f, &power, &r_half);
    TEST_ASSERT_FLOAT_WITHIN(0.15, 1.0, power);                 /* E[|tap|^2] = 1, one realisation */
    TEST_ASSERT_FLOAT_WITHIN(0.08, exp(-0.5), r_half);          /* Gaussian spectrum of std D/2 */
}

void test_gaussian_tap_is_the_same_process_at_48_khz(void)
{
    double p8, r8, p48, r48;
    tap_stats(8000, 1.0f, &p8, &r8);
    tap_stats(48000, 1.0f, &p48, &r48);
    TEST_ASSERT_FLOAT_WITHIN(0.08, exp(-0.5), r48);
    TEST_ASSERT_FLOAT_WITHIN(0.25, p8, p48);
}

/* The delay line delivers exactly the configured delay: two static paths and
 * an impulse in give an echo at ceil(delay * fs / 1000) samples.  Up to
 * v1.9.17 it was one sample short; the legacy spectrum keeps that so its
 * pinned realisation holds, and this test pins both behaviours. */
static int echo_lag(int spectrum, float delay_ms)
{
    watterson_t w;
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, 8000));
    TEST_ASSERT_EQUAL_INT(0, watterson_set_spectrum(&w, spectrum));
    watterson_add_path(&w, 0.0f, 0.0f, 0.0f, 0.7071f);
    watterson_add_path(&w, delay_ms, 0.0f, 0.0f, 0.7071f);
    COMP s[64]; memset(s, 0, sizeof s); s[0].real = 1.0f;
    watterson_process(&w, s, 64);
    int lag = -1;
    for (int i = 1; i < 64; i++) if (fabsf(s[i].real) > 0.1f) { lag = i; break; }
    watterson_dispose(&w);
    return lag;
}

void test_delay_line_delivers_the_configured_delay(void)
{
    TEST_ASSERT_EQUAL_INT(4,  echo_lag(WATTERSON_SPECTRUM_GAUSSIAN, 0.5f));    /* 0.5 ms at 8 kHz */
    TEST_ASSERT_EQUAL_INT(16, echo_lag(WATTERSON_SPECTRUM_GAUSSIAN, 2.0f));
    TEST_ASSERT_EQUAL_INT(1,  echo_lag(WATTERSON_SPECTRUM_GAUSSIAN, 0.1f));    /* a one-sample line is a delay of one */
    TEST_ASSERT_EQUAL_INT(15, echo_lag(WATTERSON_SPECTRUM_LEGACY, 2.0f));      /* the old line, kept for its pinned seed */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fading_is_stable_across_sample_rates);
    RUN_TEST(test_single_static_path_is_rate_invariant);
    RUN_TEST(test_a_pinned_seed_reproduces_a_fixed_realisation);
    RUN_TEST(test_gaussian_tap_has_unit_power_and_the_standard_width);
    RUN_TEST(test_gaussian_tap_is_the_same_process_at_48_khz);
    RUN_TEST(test_delay_line_delivers_the_configured_delay);
    return UNITY_END();
}
