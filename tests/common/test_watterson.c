/* Watterson channel model — numerical sanity across sample rates, the
 * presets, the tap statistics and the legacy pin.
 *
 * The model is the instrument behind every Watterson-engine sensitivity number
 * in this project, so it needs a guard of its own.
 *
 * Stability: the legacy Doppler tap is a 2nd-order Butterworth driven by white
 * noise; as fc/fs shrinks its poles crowd the unit circle, and in single
 * precision they can land OUTSIDE it. The filter then diverges instead of
 * fading, and the "measured SNR" becomes the blow-up rather than the signal:
 * at 48 kHz with 1 Hz Doppler this read +65.76 dB where 8 kHz read -8.37 dB.
 * Mercury runs at 8 kHz today, which is why it went unnoticed -- it surfaced
 * only when driving a 48 kHz modem through the same channel.
 *
 * The default tap is a Gaussian FIR (the spectrum of ITU-R F.1487, which
 * defines the Doppler spread doppler_hz as 2 sigma).  The tests below check
 * its power and width at 8 and 48 kHz, that each named preset delivers the
 * delay its own table row states, that the taps are zero-mean, circular,
 * independent and Rayleigh, that the spectrum's tails and the -10 dB fade
 * rate are the Gaussian's closed-form values (and that the legacy
 * Butterworth's are not), that a fading preset reports the same SNR3k as
 * AWGN alone, that a per-path frequency offset rotates at the set rate, that
 * out-of-range arguments are refused, and that a preset is byte-identical to
 * the same channel built by hand.  Set WATTERSON_TEST_VERBOSE=1 to print the
 * measured statistics.
 */
#include "unity.h"
#include "watterson.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int verbose(void) { return getenv("WATTERSON_TEST_VERBOSE") != NULL; }

/* The statistical tests run on a pinned seed so they are deterministic.  To
 * see how their statistics scatter, rerun them with WATTERSON_TEST_SEED=n
 * (and WATTERSON_TEST_VERBOSE=1 to print the values). */
static unsigned test_seed(unsigned dflt)
{
    const char *s = getenv("WATTERSON_TEST_SEED");
    return s ? (unsigned)strtoul(s, NULL, 10) : dflt;
}

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
 *
 * Known mismatch, for the record (measured 2026-10-12, macOS, Apple clang 21,
 * arm64, FLT_EVAL_METHOD 0): this test reports 0xF0EC0E2F both at release
 * 1.9.18 (f609b8f) and on the channel-model branch, with the same value at
 * -O0 and with -ffp-contract=off (built as
 * `cc <flags> -Itests/unity -Icommon tests/common/test_watterson.c
 * tests/unity/unity.c common/watterson.c common/mercury_prng.c -lm`).  The
 * mismatch therefore predates the channel-model change; its cause has not
 * been identified.  The pin is checked on the Linux CI hosts.
 *
 * test_legacy_preset_is_byte_identical_to_the_hand_built_legacy_channel below
 * shows that the "poor" preset builds exactly this channel, so where this pin
 * holds it also pins the preset table's legacy rows.
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

/* The Gaussian spectrum is the standard's model (ITU-R F.1487): for a path
 * with spread D the tap's Doppler power spectrum is Gaussian with standard
 * deviation D/2, so its autocorrelation is r(tau) = exp(-2 pi^2 (D/2)^2
 * tau^2) and r at tau = 1/(pi D) is exp(-1/2).  Measure the tap through a
 * unit DC input (the output IS the tap) for 400 s: unit power and the
 * standard width.  One 400 s record of a 1 Hz process measures its power
 * with a 1-sigma scatter of sqrt(1 / (2 sqrt(pi) (D/2) T)) = 3.8 %.  Over
 * seeds 1-12 at 8 kHz the power measured 0.936 to 1.061 and r_half 0.597 to
 * 0.627 (1-sigma about 0.01), so the gates in check_tap_stats (0.15 on the
 * power, 0.08 on r_half) are 4 sigma and about 8 sigma. */
static void tap_stats(int fs, float spread_hz, unsigned seed, double *power, double *r_half)
{
    watterson_t w;
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, fs));
    watterson_seed(&w, seed);
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

static void check_tap_stats(int fs, unsigned seed)
{
    double power, r_half;
    tap_stats(fs, 1.0f, seed, &power, &r_half);
    char msg[128];
    snprintf(msg, sizeof msg, "fs=%d seed=%u: E[|tap|^2]=%.4f (want 1), r(1/(pi D))=%.4f (want %.4f)",
             fs, seed, power, r_half, exp(-0.5));
    if (verbose()) printf("  %s\n", msg);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.15, 1.0, power, msg);        /* E[|tap|^2] = 1, one realisation */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.08, exp(-0.5), r_half, msg); /* Gaussian spectrum of std D/2 */
}

void test_gaussian_tap_has_unit_power_and_the_standard_width(void)
{
    check_tap_stats(8000, test_seed(11));
}

/* The same statistics at 48 kHz on an INDEPENDENT realisation (seed + 100).
 * With the same seed the 8 and 48 kHz runs would share every low-rate tap
 * draw (for a 1 Hz spread both run the generator at 64 Hz: 8000/125 and
 * 48000/750) and differ only in the interpolation, so the second run would
 * check nothing the first did not.  Over seeds 101-112 the 48 kHz power
 * measured 0.955 to 1.029 and r_half 0.589 to 0.629. */
void test_gaussian_tap_has_unit_power_and_the_standard_width_at_48_khz(void)
{
    check_tap_stats(48000, test_seed(11) + 100);
}

/* The delay line delivers exactly the configured delay: two static paths and
 * an impulse in give an echo at ceil(delay * fs / 1000) samples.  Up to and
 * including release 1.9.18 it was one sample short; the legacy spectrum keeps
 * that so its pinned realisation holds, and this test pins both behaviours. */
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

/* ---- the presets, through the library's own table -------------------------
 *
 * ITU-R F.1487 gives each test channel as two equal-power paths with a delay
 * and a Doppler spread.  The values themselves are the library's business
 * (common/watterson.c states its sources); what is checked here is that the
 * channel a preset builds delivers the delay its own row states, at 8 and at
 * 48 kHz.  There is no second copy of the table: a test that compares two
 * copies only checks that the copies agree. */

/* Echo position of a unit impulse through a preset, taken through the preset
 * itself (random taps, no AWGN).  Every output sample between the impulse and
 * its echo is exactly zero, because both taps multiply zeros there, and the
 * echo is the second path's complex Gaussian tap, which is never exactly zero
 * however deep the fade.  So the echo is the first non-zero output after
 * sample 0.  Returns -1 if there is none (a single-path preset). */
static int preset_echo_lag(const watterson_preset_t *pr, int fs)
{
    enum { N = 512 };                               /* 7 ms at 48 kHz is 336 samples */
    watterson_t w;
    COMP s[N];
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, fs));
    watterson_seed(&w, 5);
    TEST_ASSERT_EQUAL_INT(0, watterson_apply_preset(&w, pr));
    memset(s, 0, sizeof s);
    s[0].real = 1.0f;
    watterson_process(&w, s, N);
    TEST_ASSERT_TRUE_MESSAGE(s[0].real != 0.0f || s[0].imag != 0.0f, "no direct path at sample 0");
    int lag = -1;
    for (int i = 1; i < N; i++)
        if (s[i].real != 0.0f || s[i].imag != 0.0f) { lag = i; break; }
    watterson_dispose(&w);
    return lag;
}

/* Every preset, as watterson_presets() lists it: the first path is at 0 ms,
 * a two-path preset has equal-power paths, and an impulse through the preset
 * comes out at ceil(delay_ms * fs / 1000) samples computed from the preset's
 * OWN delay_ms (one sample less for a legacy preset: the 1.9.18 delay line,
 * kept for its pinned seeds, see test_delay_line_delivers_the_configured_delay).
 * A single-path preset has no echo at all.  The expected lag is derived from
 * the row under test, so a changed delay in the table changes the expectation
 * with it; what cannot change unnoticed is the delay line delivering something
 * other than the row says. */
void test_every_preset_delivers_its_delay_through_the_impulse(void)
{
    int count = 0;
    const watterson_preset_t *all = watterson_presets(&count);
    TEST_ASSERT_NOT_NULL(all);
    TEST_ASSERT_TRUE_MESSAGE(count > 0, "watterson_presets() lists no presets");

    const int rates[] = { 8000, 48000 };
    for (int i = 0; i < count; i++) {
        const watterson_preset_t *pr = &all[i];
        TEST_ASSERT_NOT_NULL_MESSAGE(pr->name, "a preset has no name");
        TEST_ASSERT_TRUE_MESSAGE(pr->npaths == 1 || pr->npaths == 2,
                                 "preset_echo_lag reads the second path's echo; extend it for more paths");
        TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, pr->path[0].delay_ms, pr->name);
        if (pr->npaths == 2) {
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(pr->path[0].gain, pr->path[1].gain, pr->name);              /* equal power */
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(pr->path[0].doppler_hz, pr->path[1].doppler_hz, pr->name);  /* equal spreads */
        }
        for (unsigned r = 0; r < sizeof rates / sizeof rates[0]; r++) {
            int expect = -1;
            if (pr->npaths == 2) {
                expect = (int)ceil((double)pr->path[1].delay_ms * rates[r] / 1000.0);
                if (pr->spectrum == WATTERSON_SPECTRUM_LEGACY) expect -= 1;    /* the 1.9.18 line, kept for its pinned seeds */
            }
            char msg[96];
            snprintf(msg, sizeof msg, "%s (%.2f ms) at %d Hz", pr->name,
                     pr->npaths == 2 ? pr->path[1].delay_ms : 0.0f, rates[r]);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expect, preset_echo_lag(pr, rates[r]), msg);
        }
    }
}

/* Out-of-range arguments are refused with -1 and leave the channel unchanged.
 * The important case is a positive spread below sample_rate / 2^36 (1.2e-7 Hz
 * at 8 kHz, 7e-7 Hz at 48 kHz): before the guard, the interpolation ratio
 * fs / (64 * doppler_hz) overflowed its int cast and the path was built with a
 * meaningless tap filter and no error. */
void test_add_path_rejects_out_of_range_arguments(void)
{
    watterson_t w;
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, 8000));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, 0.0f, 1e-8f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, 0.0f, 1e-30f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, 0.0f, NAN, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, 0.0f, INFINITY, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, 0.0f, -1.0f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, -1.0f, 1.0f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, NAN, 1.0f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, 20000.0f, 1.0f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, 0.0f, 1.0f, NAN, 1.0f));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, 0.0f, 1.0f, 0.0f, 1.5f));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, 0.0f, 1.0f, 0.0f, -0.1f));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, 0.0f, 1.0f, 0.0f, NAN));
    TEST_ASSERT_EQUAL_INT(0, w.num_paths);
    /* the same floor under the legacy spectrum */
    TEST_ASSERT_EQUAL_INT(0, watterson_set_spectrum(&w, WATTERSON_SPECTRUM_LEGACY));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, 0.0f, 1e-8f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(0, w.num_paths);
    watterson_dispose(&w);

    /* Just above the floor is accepted and runs: at 8 kHz a 2e-7 Hz spread
     * needs one tap sample per 6.25e8 output samples, inside the 2^30 limit. */
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, 8000));
    TEST_ASSERT_EQUAL_INT(0, watterson_add_path(&w, 0.0f, 2e-7f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(1, w.num_paths);
    COMP s[256];
    for (int i = 0; i < 256; i++) { s[i].real = 1.0f; s[i].imag = 0.0f; }
    watterson_process(&w, s, 256);
    for (int i = 0; i < 256; i++)
        TEST_ASSERT_TRUE(isfinite(s[i].real) && isfinite(s[i].imag));
    watterson_dispose(&w);

    /* At 48 kHz the floor is six times higher. */
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, 48000));
    TEST_ASSERT_EQUAL_INT(-1, watterson_add_path(&w, 0.0f, 2e-7f, 0.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(0,  watterson_add_path(&w, 0.0f, 1e-6f, 0.0f, 1.0f));
    watterson_dispose(&w);
}

/* Sample the two taps of a two-path preset separately, through the preset
 * itself.  An impulse train with period T longer than the delay d makes the
 * output c*g0 at each impulse and c*g1 d samples later, with exact zeros in
 * between, so the two taps are read off with no filtering or demodulation.
 * c = chan_norm * gain = 0.5 for two equal-power paths.  Fills g0[m] and g1[m]
 * for m < M, i.e. the taps sampled at fs/T. */
static void preset_tap_pair(const char *name, unsigned seed, int fs, int period, int M, COMP *g0, COMP *g1)
{
    watterson_t w;
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, fs));
    watterson_seed(&w, seed);
    TEST_ASSERT_EQUAL_INT(0, watterson_apply_preset(&w, watterson_preset_find(name)));
    TEST_ASSERT_EQUAL_INT(2, w.num_paths);
    int d = w.paths[1].delay_samples;
    TEST_ASSERT_TRUE(d > 0 && d < period);
    enum { BLK = 125 };                             /* impulses per processing block */
    COMP *s = malloc(sizeof(COMP) * (size_t)period * BLK);
    TEST_ASSERT_NOT_NULL(s);
    for (int m0 = 0; m0 < M; m0 += BLK) {
        int nb = (M - m0 < BLK) ? M - m0 : BLK;
        memset(s, 0, sizeof(COMP) * (size_t)period * nb);
        for (int k = 0; k < nb; k++) s[k * period].real = 1.0f;
        watterson_process(&w, s, period * nb);
        for (int k = 0; k < nb; k++) {
            g0[m0 + k] = s[k * period];
            g1[m0 + k] = s[k * period + d];
        }
    }
    free(s);
    watterson_dispose(&w);
}

typedef struct {
    double mean_re, mean_im;   /* E[g] */
    double p;                  /* E[|g|^2] */
    double p_re, p_im;         /* E[re^2], E[im^2] */
    double c_iq;               /* E[re * im] */
} tap_moments_t;

static tap_moments_t tap_moments(const COMP *g, int M)
{
    tap_moments_t t = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < M; i++) {
        double re = g[i].real, im = g[i].imag;
        t.mean_re += re; t.mean_im += im;
        t.p_re += re * re; t.p_im += im * im; t.c_iq += re * im;
    }
    t.mean_re /= M; t.mean_im /= M; t.p_re /= M; t.p_im /= M; t.c_iq /= M;
    t.p = t.p_re + t.p_im;
    return t;
}

/* The standard's taps are zero-mean (Rayleigh, no specular component),
 * circular (I and Q of equal power and uncorrelated) and independent between
 * paths.  A DC term in the tap would pass the width and tail gates in this
 * file (a fixed component only adds a line at 0 Hz), so it is checked
 * here, on ccir-poor (1 Hz) for 1200 s at 8 kHz with a pinned seed,
 * the taps sampled every 64 samples (125 Hz, against a spectrum empty above
 * ~1.5 Hz).  Gates are 0.1 on every normalised statistic.
 * Record length: over seeds 1-12 (WATTERSON_TEST_SEED) the largest values at
 * 1200 s are |mean|/rms 0.037, |corr(I,Q)| 0.046, I/Q imbalance 0.037 and
 * inter-path 0.048, so each gate has at least 2x margin; at 300 s the same
 * seeds reached 0.094 on corr(I,Q), too close to call.  The fraction of time
 * below -10 dB is printed here and gated on the 10 Hz record below, where the
 * record holds enough fades to pin it. */
void test_gaussian_preset_taps_are_zero_mean_circular_independent_rayleigh(void)
{
    const int fs = 8000, T = 64, M = 1200 * fs / T;   /* 150000 tap samples per path */
    COMP *g0 = malloc(sizeof(COMP) * (size_t)M), *g1 = malloc(sizeof(COMP) * (size_t)M);
    TEST_ASSERT_NOT_NULL(g0); TEST_ASSERT_NOT_NULL(g1);
    preset_tap_pair("ccir-poor", test_seed(3), fs, T, M, g0, g1);

    tap_moments_t m[2] = { tap_moments(g0, M), tap_moments(g1, M) };
    int below = 0;
    for (int p = 0; p < 2; p++) {
        const COMP *g = p ? g1 : g0;
        double mean_over_rms = hypot(m[p].mean_re, m[p].mean_im) / sqrt(m[p].p);
        double corr_iq       = m[p].c_iq / sqrt(m[p].p_re * m[p].p_im);
        double iq_imbalance  = (m[p].p_re - m[p].p_im) / m[p].p;
        for (int i = 0; i < M; i++)
            if ((double)g[i].real * g[i].real + (double)g[i].imag * g[i].imag < 0.1 * m[p].p) below++;
        char msg[160];
        snprintf(msg, sizeof msg, "path %d: |mean|/rms=%.4f corr(I,Q)=%.4f (P_I-P_Q)/P=%.4f P=%.4f",
                 p, mean_over_rms, corr_iq, iq_imbalance, m[p].p);
        if (verbose()) printf("  %s\n", msg);
        TEST_ASSERT_TRUE_MESSAGE(mean_over_rms < 0.1, msg);
        TEST_ASSERT_TRUE_MESSAGE(fabs(corr_iq) < 0.1, msg);
        TEST_ASSERT_TRUE_MESSAGE(fabs(iq_imbalance) < 0.1, msg);
    }
    /* inter-path: |E[g0 conj(g1)]| / sqrt(P0 P1) */
    double xr = 0.0, xi = 0.0;
    for (int i = 0; i < M; i++) {
        xr += (double)g0[i].real * g1[i].real + (double)g0[i].imag * g1[i].imag;
        xi += (double)g0[i].imag * g1[i].real - (double)g0[i].real * g1[i].imag;
    }
    double cross = hypot(xr / M, xi / M) / sqrt(m[0].p * m[1].p);
    double p_below = (double)below / (2.0 * M);
    char msg[160];
    snprintf(msg, sizeof msg, "inter-path |corr|=%.4f  P(a < -10 dB)=%.4f (Rayleigh 0.0952, not gated here)", cross, p_below);
    if (verbose()) printf("  %s\n", msg);
    TEST_ASSERT_TRUE_MESSAGE(cross < 0.1, msg);
    free(g0); free(g1);
}

/* With unit-power, uncorrelated taps and chan_norm = 1/sqrt(sum gain^2), the
 * summed two-path channel has unit average power gain, which is what keeps
 * SNR3k = -No - 14.82 true under fading.  The sum of two independent complex
 * Gaussian taps is again complex Gaussian, so the summed envelope is Rayleigh
 * and the fraction of time below -10 dB relative to rms is 1 - exp(-0.1) =
 * 0.0952.  DC in, so the output is the summed tap; ccir-flutter (10 Hz) for
 * 600 s at 8 kHz holds some 6000 fade cycles.  Over seeds 1-12 the power gain
 * measures 0.988 to 1.016 (gate 1 +- 0.04) and the -10 dB fraction 0.0943 to
 * 0.0984 (gate 0.0952 +- 0.01). */
void test_two_path_preset_keeps_unit_average_power_and_rayleigh_depth(void)
{
    const int fs = 8000, secs = 600;
    watterson_t w;
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, fs));
    watterson_seed(&w, test_seed(7));
    TEST_ASSERT_EQUAL_INT(0, watterson_apply_preset(&w, watterson_preset_find("ccir-flutter")));
    COMP  *s  = malloc(sizeof(COMP) * (size_t)fs);
    float *pw = malloc(sizeof(float) * (size_t)fs * secs);     /* |out|^2 per sample */
    TEST_ASSERT_NOT_NULL(s); TEST_ASSERT_NOT_NULL(pw);
    double p = 0.0;
    for (int sec = 0; sec < secs; sec++) {
        for (int i = 0; i < fs; i++) { s[i].real = 1.0f; s[i].imag = 0.0f; }
        watterson_process(&w, s, fs);
        for (int i = 0; i < fs; i++) {
            pw[(size_t)sec * fs + i] = s[i].real * s[i].real + s[i].imag * s[i].imag;
            p += pw[(size_t)sec * fs + i];
        }
    }
    p /= (double)fs * secs;
    long below = 0;
    for (size_t i = 0; i < (size_t)fs * secs; i++) if (pw[i] < 0.1 * p) below++;
    double p_below = (double)below / ((double)fs * secs);
    free(s); free(pw);
    watterson_dispose(&w);
    char msg[128];
    snprintf(msg, sizeof msg, "ccir-flutter mean power gain %.4f, P(a < -10 dB)=%.4f (Rayleigh 0.0952)", p, p_below);
    if (verbose()) printf("  %s\n", msg);
    TEST_ASSERT_TRUE_MESSAGE(fabs(p - 1.0) < 0.04, msg);
    TEST_ASSERT_TRUE_MESSAGE(fabs(p_below - (1.0 - exp(-0.1))) < 0.01, msg);
}

/* ---- the Doppler spectrum's tails and the fade rate -----------------------
 *
 * Both tests below measure one path's tap through a unit DC input at 8 kHz
 * (the output IS the tap, as in tap_stats) and work on the tap decimated to
 * 64 Hz: the complex mean of each block of 125 consecutive samples.  64 Hz is
 * 64 x the 1 Hz spread, which keeps everything the Gaussian process has (its
 * spectrum is empty above 3 sigma = 1.5 Hz) in a record of 57 600 points for
 * 900 s instead of 7.2 million.  The block mean is a lowpass with its first
 * null at 64 Hz; its power response is 0.9992 at 1 Hz, 0.9982 at 1.5 Hz and
 * 0.9928 at 3 Hz, so it moves no tail fraction below by more than its own
 * 0.7 %.  The first 5 s are discarded: the legacy IIR is primed with 1000
 * draws (0.125 s at 8 kHz), short against its 0.23 s time constant, while
 * the Gaussian FIR starts in its steady state and loses nothing. */

/* One path of the given spectrum and spread through a unit DC input, the tap
 * decimated by block means.  Writes nblk decimated samples after skipping
 * settle_blk blocks. */
static void decimated_tap(int spectrum, float spread_hz, unsigned seed, int fs, int block,
                          int settle_blk, int nblk, COMP *out)
{
    enum { PER_CALL = 64 };                         /* blocks per watterson_process() call */
    watterson_t w;
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, fs));
    watterson_seed(&w, seed);
    TEST_ASSERT_EQUAL_INT(0, watterson_set_spectrum(&w, spectrum));
    TEST_ASSERT_EQUAL_INT(0, watterson_add_path(&w, 0.0f, spread_hz, 0.0f, 1.0f));
    COMP *s = malloc(sizeof(COMP) * (size_t)block * PER_CALL);
    TEST_ASSERT_NOT_NULL(s);
    for (int b0 = -settle_blk; b0 < nblk; b0 += PER_CALL) {
        int nb = (nblk - b0 < PER_CALL) ? nblk - b0 : PER_CALL;
        for (int i = 0; i < block * nb; i++) { s[i].real = 1.0f; s[i].imag = 0.0f; }
        watterson_process(&w, s, block * nb);
        for (int k = 0; k < nb; k++) {
            if (b0 + k < 0) continue;               /* still settling */
            double re = 0.0, im = 0.0;
            for (int i = 0; i < block; i++) { re += s[k * block + i].real; im += s[k * block + i].imag; }
            out[b0 + k].real = (float)(re / block);
            out[b0 + k].imag = (float)(im / block);
        }
    }
    free(s);
    watterson_dispose(&w);
}

/* In-place radix-2 FFT of n complex points (n a power of two). */
static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {            /* bit-reversal permutation */
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double wr = cos(-2.0 * M_PI / len), wi = sin(-2.0 * M_PI / len);
        for (int i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                double vr = re[b] * cr - im[b] * ci, vi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] += vr;        im[a] += vi;
                double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

/* Welch power spectrum of nz complex samples: Hann-windowed segments of
 * WELCH_N points, half overlapping, periodograms averaged, normalised to unit
 * total power.  Bin k is centred on bin_freq(k): k * fsd / WELCH_N up to the
 * Nyquist bin, negative frequencies above it.  Returns the segment count. */
enum { WELCH_N = 1024 };                            /* 16 s at 64 Hz: 1/16 Hz bins */

static double bin_freq(int k, double fsd)
{
    return (k < WELCH_N / 2 ? k : k - WELCH_N) * fsd / WELCH_N;
}

static int welch(const COMP *z, int nz, double *S)
{
    double re[WELCH_N], im[WELCH_N], win[WELCH_N];
    for (int n = 0; n < WELCH_N; n++) win[n] = 0.5 * (1.0 - cos(2.0 * M_PI * n / WELCH_N));
    for (int k = 0; k < WELCH_N; k++) S[k] = 0.0;
    int K = 0;
    for (int start = 0; start + WELCH_N <= nz; start += WELCH_N / 2, K++) {
        for (int n = 0; n < WELCH_N; n++) {
            re[n] = win[n] * z[start + n].real;
            im[n] = win[n] * z[start + n].imag;
        }
        fft(re, im, WELCH_N);
        for (int k = 0; k < WELCH_N; k++) S[k] += re[k] * re[k] + im[k] * im[k];
    }
    double total = 0.0;
    for (int k = 0; k < WELCH_N; k++) total += S[k];
    for (int k = 0; k < WELCH_N; k++) S[k] /= total;
    return K;
}

/* Fraction of the (unit) power at |f| > f0.  A bin is fsd / WELCH_N wide and
 * centred on a multiple of that width, so 1 Hz, 1.5 Hz and 3 Hz are all bin
 * centres: the bin straddling the boundary is counted by the share of its
 * width that lies beyond f0 (one half when f0 is its centre). */
static double power_beyond(const double *S, double fsd, double f0)
{
    double sum = 0.0, bw = fsd / WELCH_N;
    for (int k = 0; k < WELCH_N; k++) {
        double part = (fabs(bin_freq(k, fsd)) + 0.5 * bw - f0) / bw;
        if (part > 1.0) part = 1.0;
        if (part > 0.0) sum += part * S[k];
    }
    return sum;
}

/* rms width of a unit-power spectrum: sqrt(sum f^2 S).  A Gaussian's sigma
 * (measured 0.491 to 0.512 Hz for sigma 0.5 over seeds 1-12).  A second-order
 * Butterworth's cutoff, less what the record cannot hold: 2.8 % of its
 * f^2-weighted power lies above 32 x the cutoff, where the 64 Hz record ends,
 * and the block mean attenuates the top of the band, so a 1 Hz cutoff
 * measures 0.960 to 0.993 Hz over seeds 101-112. */
static double rms_width(const double *S, double fsd)
{
    double m2 = 0.0;
    for (int k = 0; k < WELCH_N; k++) { double f = bin_freq(k, fsd); m2 += f * f * S[k]; }
    return sqrt(m2);
}

/* The instrument before the measurement: a tone at bin 5 of a 64 Hz record
 * must come out of the FFT in bin 5 and nowhere else, and a Hann-windowed
 * tone must spread into no more than its two neighbours (the Hann window's
 * transform is three lines: 1/2, -1/4, -1/4). */
void test_the_fft_behind_the_spectrum_test_puts_a_tone_in_its_bin(void)
{
    double re[WELCH_N], im[WELCH_N];
    for (int n = 0; n < WELCH_N; n++) {
        re[n] = cos(2.0 * M_PI * 5.0 * n / WELCH_N);
        im[n] = sin(2.0 * M_PI * 5.0 * n / WELCH_N);
    }
    fft(re, im, WELCH_N);
    for (int k = 0; k < WELCH_N; k++) {
        double p = re[k] * re[k] + im[k] * im[k];
        if (k == 5) TEST_ASSERT_TRUE_MESSAGE(fabs(p / ((double)WELCH_N * WELCH_N) - 1.0) < 1e-9, "tone power is not N^2 in its bin");
        else        TEST_ASSERT_TRUE_MESSAGE(p < 1e-12, "tone leaked out of its bin");
    }
    COMP z[WELCH_N];
    double S[WELCH_N];
    for (int n = 0; n < WELCH_N; n++) {
        z[n].real = (float)cos(2.0 * M_PI * 5.0 * n / WELCH_N);
        z[n].imag = (float)sin(2.0 * M_PI * 5.0 * n / WELCH_N);
    }
    TEST_ASSERT_EQUAL_INT(1, welch(z, WELCH_N, S));
    /* single-precision input: the leakage floor is about 1e-13 of the power */
    TEST_ASSERT_TRUE_MESSAGE(fabs(S[4] + S[5] + S[6] - 1.0) < 1e-6, "Hann-windowed tone spread past its neighbours");
    TEST_ASSERT_TRUE_MESSAGE(fabs(S[5] - 2.0 / 3.0) < 1e-6, "centre bin is not (1/2)^2 / ((1/2)^2 + 2 (1/4)^2)");
}

/* What separates the Gaussian spectrum from the Butterworth is its tail.  A
 * Gaussian of standard deviation sigma holds 0.27 % of its power beyond
 * 3 sigma and 4.55 % beyond 2 sigma (the ITU-R spread); a second-order
 * Butterworth, |H|^2 = 1 / (1 + (f/fc)^4), holds 1.11 % beyond 3 fc and
 * 21.95 % beyond fc.  So a Gaussian path of 1 Hz spread (sigma 0.5 Hz) is
 * measured against 1.5 Hz and 1 Hz, and a legacy path with its cutoff at
 * 1 Hz against 3 Hz and 1 Hz: the legacy reference is the cutoff the path
 * was built with, not its measured rms width (printed under verbose; it
 * measures 0.96 to 0.99 Hz, see rms_width).
 *
 * Method: 900 s of tap after a 5 s settle, decimated to 64 Hz (the section
 * comment above), Welch spectrum from 111 Hann-windowed 16 s segments at 50 %
 * overlap, 1/16 Hz bins, then the fraction of power beyond each boundary
 * (power_beyond counts the straddling bin by halves).  The Hann window's
 * leakage is irrelevant here: at 24 bins from the peak its sidelobes are
 * some 90 dB down, while the Gaussian's tail at 1.5 Hz is 19.5 dB down.
 *
 * Gates: Gaussian beyond 3 sigma under 0.6 %, i.e. twice the Gaussian's
 * value and half the Butterworth's; Gaussian beyond the spread 4.55 +- 2.0
 * points; legacy beyond 3 fc over 0.9 % and beyond fc over 15 %.  On seed 13
 * the Gaussian measures 0.273 % and 4.68 % (rms width 0.5010 Hz); on seed
 * 113 the legacy measures 1.081 % and 21.66 % (rms width 0.9775 Hz).  Over
 * seeds 1-12 the Gaussian ranged 0.255 to 0.302 % beyond 3 sigma and 4.45 to
 * 4.99 % beyond the spread (rms width 0.491 to 0.512 Hz); over seeds 101-112
 * the legacy 1.04 to 1.14 % and 20.6 to 22.4 % (rms width 0.960 to 0.993
 * Hz).  The Gaussian's beyond-the-spread figure sits 0.1 to 0.2 points above
 * the analytic 4.55 % because the Hann window's main lobe (two bins wide)
 * smears a little of the peak outward; the gate has ten times that room. */
void test_spectrum_tails_separate_the_gaussian_from_the_butterworth(void)
{
    const int fs = 8000, block = 125, secs = 900, settle_s = 5;
    const double fsd = (double)fs / block;          /* 64 Hz */
    const int nblk = secs * fs / block;             /* 57 600 decimated samples */
    COMP   *z = malloc(sizeof(COMP) * (size_t)nblk);
    double *S = malloc(sizeof(double) * WELCH_N);
    TEST_ASSERT_NOT_NULL(z); TEST_ASSERT_NOT_NULL(S);

    const struct { int spectrum; const char *name; unsigned seed; double sigma; } cases[] = {
        { WATTERSON_SPECTRUM_GAUSSIAN, "gaussian 1 Hz spread", test_seed(13),       0.5 },   /* sigma = spread / 2 */
        { WATTERSON_SPECTRUM_LEGACY,   "legacy 1 Hz cutoff",   test_seed(13) + 100, 1.0 },   /* the cutoff itself */
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        decimated_tap(cases[c].spectrum, 1.0f, cases[c].seed, fs, block, settle_s * fs / block, nblk, z);
        int K = welch(z, nblk, S);
        double width   = rms_width(S, fsd);
        double beyond3 = power_beyond(S, fsd, 3.0 * cases[c].sigma);
        double beyondD = power_beyond(S, fsd, 1.0);                     /* beyond the 1 Hz spread / cutoff */
        char msg[200];
        snprintf(msg, sizeof msg,
                 "%s seed=%u %d s fs=%d: %d segments, rms width %.4f Hz, beyond 3 x %.1f Hz %.3f %%, beyond 1 Hz %.2f %%",
                 cases[c].name, cases[c].seed, secs, fs, K, width, cases[c].sigma, 100.0 * beyond3, 100.0 * beyondD);
        if (verbose()) printf("  %s\n", msg);
        if (cases[c].spectrum == WATTERSON_SPECTRUM_GAUSSIAN) {
            TEST_ASSERT_TRUE_MESSAGE(beyond3 < 0.006, msg);                 /* Gaussian 0.27 %, Butterworth 1.11 % */
            TEST_ASSERT_TRUE_MESSAGE(fabs(beyondD - 0.0455) < 0.020, msg);  /* Gaussian 4.55 % */
        } else {
            TEST_ASSERT_TRUE_MESSAGE(beyond3 > 0.009, msg);                 /* Butterworth 1.11 % */
            TEST_ASSERT_TRUE_MESSAGE(beyondD > 0.15, msg);                  /* Butterworth 21.95 % */
        }
    }
    free(z); free(S);
}

/* Downward crossings of the level 10 dB below the record's rms (rms from the
 * record's own mean power) in n samples of the decimated tap. */
static long fade_count(const COMP *z, int n)
{
    double p = 0.0;
    for (int i = 0; i < n; i++) p += (double)z[i].real * z[i].real + (double)z[i].imag * z[i].imag;
    double thr2 = 0.1 * p / n;                       /* |z|^2 at -10 dB below the mean power */
    long fades = 0;
    int above = ((double)z[0].real * z[0].real + (double)z[0].imag * z[0].imag) >= thr2;
    for (int i = 1; i < n; i++) {
        int a = ((double)z[i].real * z[i].real + (double)z[i].imag * z[i].imag) >= thr2;
        if (above && !a) fades++;
        above = a;
    }
    return fades;
}

/* Rice's level-crossing rate of a Rayleigh envelope at rho = -10 dB for a
 * Doppler spectrum of rms width sigma: 2 sqrt(pi) sigma rho exp(-rho^2),
 * rho = 10^(-10/20).  It depends on the spectrum only through its rms
 * width.  For the Gaussian that is 0.507 x the spread per second. */
static double rice_rate(double sigma_hz)
{
    double rho = pow(10.0, -10.0 / 20.0);
    return 2.0 * sqrt(M_PI) * sigma_hz * rho * exp(-rho * rho);
}

/* The -10 dB fade rate against Rice's closed form.  A Gaussian path of 1 Hz
 * spread (sigma 0.5 Hz) fades below -10 dB 0.507 times a second, so a 900 s
 * record holds 456 fades.  On seed 17 it measures 0.5067 /s, 456 fades.
 *
 * The legacy Butterworth path with its cutoff at 1 Hz is measured against
 * the Rice rate for its OWN rms width, read from the same record (0.9760 Hz
 * on seed 117, so 0.9900 /s and 891 fades; it measures 0.9444 /s, 850 fades,
 * -4.6 %).  The cutoff is not the reference because the count runs 4 %
 * under the Rice rate for it (seeds 101-112: -1.6 to -8.3 %), for two
 * reasons.  The record's width is 2 % below the cutoff (see rms_width).  And
 * the Butterworth's f^-4 tail makes small, quick excursions near the
 * threshold: the continuous-time formula counts them, a 64 Hz count misses
 * those that come and go within one sample.  Against its measured width the
 * same seeds run -4.5 to 0.0 %, 2.1 % under on average, where the Gaussian
 * (no such tail) runs -1.2 to +6.7 %, 2.0 % over.
 *
 * The legacy path must also fade at least 1.7 x as fast as the Gaussian path
 * of the same number.  By the standard's definition the legacy "1 Hz" is a
 * 2 Hz spread, and it fades about twice as fast as CCIR Poor: 1.86 x on seed
 * 117 (0.9444 / 0.5072), 1.83 to 1.97 x over seeds 101-112.
 *
 * The count must be taken on the decimated envelope, not on the 8 kHz
 * samples.  A crossing count belongs to a band-limited process: the Gaussian
 * tap at 64 x its spread carries nothing but the fading, while the 8 kHz
 * legacy tap also carries the Butterworth's f^-4 tail out to 4 kHz, and a
 * tone-based measurement would carry the channel noise.  Either chatters
 * across the threshold whenever the envelope sits near it and adds
 * crossings that are not fades.  (For the Gaussian path the 8 kHz tap is a
 * linear interpolation of the 64 Hz one, so counting there would only cost
 * 125 x the samples for the same count.)
 *
 * Tolerance: the fades of one record are not independent events (a fade
 * cannot start before the last has ended), so the count scatters by less
 * than Poisson's sqrt(N).  0.74 sqrt(N) is used for 1 sigma; over 100 seeds
 * the count scatters by 0.71 sqrt(N) for the Gaussian and 0.79 sqrt(N) for
 * the legacy path, so the gate is about 3 sigma.  The gate is
 * 3 x 0.74 / sqrt(N): 10.4 % for the Gaussian's 456 fades and 7.4 % for the
 * legacy's 891. */
void test_fade_rate_matches_the_rice_closed_form(void)
{
    const int fs = 8000, block = 125, secs = 900, settle_s = 5;
    const double fsd = (double)fs / block;           /* 64 Hz */
    const int nblk = secs * fs / block;              /* 57 600 decimated samples */
    const double tol_sd = 0.74;                      /* 1-sigma count scatter, in units of sqrt(N) */
    COMP   *z = malloc(sizeof(COMP) * (size_t)nblk);
    double *S = malloc(sizeof(double) * WELCH_N);
    TEST_ASSERT_NOT_NULL(z); TEST_ASSERT_NOT_NULL(S);

    const struct { int spectrum; const char *name; unsigned seed; } cases[] = {
        { WATTERSON_SPECTRUM_GAUSSIAN, "gaussian 1 Hz spread", test_seed(17)       },
        { WATTERSON_SPECTRUM_LEGACY,   "legacy 1 Hz cutoff",   test_seed(17) + 100 },
    };
    const double rice_gaussian = rice_rate(0.5);     /* 0.507 /s: the Gaussian's reference, sigma = spread / 2 */
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        int legacy = cases[c].spectrum == WATTERSON_SPECTRUM_LEGACY;
        decimated_tap(cases[c].spectrum, 1.0f, cases[c].seed, fs, block, settle_s * fs / block, nblk, z);
        long   n     = fade_count(z, nblk);
        double rate  = (double)n / secs;
        double sigma = 0.5;                          /* Gaussian: sigma = spread / 2 */
        if (legacy) { welch(z, nblk, S); sigma = rms_width(S, fsd); }   /* legacy: its own rms width */
        double rice = rice_rate(sigma);
        double tol  = 3.0 * tol_sd / sqrt(rice * secs);
        char msg[220];
        snprintf(msg, sizeof msg,
                 "%s seed=%u %d s fs=%d: %.4f fades/s (%ld fades), Rice for %ssigma %.4f Hz %.4f /s (%+.1f %%, tol %.1f %%)",
                 cases[c].name, cases[c].seed, secs, fs, rate, n, legacy ? "its measured " : "", sigma, rice,
                 100.0 * (rate - rice) / rice, 100.0 * tol);
        if (verbose()) printf("  %s\n", msg);
        TEST_ASSERT_TRUE_MESSAGE(fabs(rate - rice) / rice < tol, msg);
        if (legacy) {
            snprintf(msg, sizeof msg, "legacy 1 Hz fades %.2f x as fast as the Gaussian 1 Hz spread's Rice rate (want about 2, gate 1.7)",
                     rate / rice_gaussian);
            if (verbose()) printf("  %s\n", msg);
            TEST_ASSERT_TRUE_MESSAGE(rate > 1.7 * rice_gaussian, msg);
        }
    }
    free(z); free(S);
}

/* The SNR3k calibration must not depend on fading.  watterson_measured_snr3k()
 * divides the accumulated faded-signal power by the accumulated noise power,
 * and chan_norm holds a two-path preset at unit average power gain, so a
 * fading preset must report the same SNR3k as one static unit-gain path for
 * the same tone, noise density and record.  An error in chan_norm, in the tap
 * power or in the accumulators would show here as a dB offset (a missing
 * 1/sqrt(2) would be 3 dB).  The difference between the two reports is the
 * mean power gain of the fading record in dB.  Its 1-sigma scatter is
 * sqrt(1 / (2 sqrt(pi) sigma T)): 0.093 dB at 120 s and 0.030 dB at 1200 s
 * for ccir-flutter (sigma = 5 Hz).  Measured over seeds 1-12 at 1200 s the
 * difference is -0.030 to +0.049 dB, so the 0.1 dB gate is above 3 sigma.
 * At 120 s the same seeds reached 0.15 dB, so a 0.1 dB gate at 120 s would
 * be a 1-sigma gate.  A slow preset would need hours of record for the same
 * resolution, which is why the fast preset is used. */
void test_fading_preset_reports_the_same_snr3k_as_awgn_alone(void)
{
    const int fs = 8000, secs = 1200;
    const unsigned seed = test_seed(7);
    COMP *tone = malloc(sizeof(COMP) * (size_t)fs), *s = malloc(sizeof(COMP) * (size_t)fs);
    TEST_ASSERT_NOT_NULL(tone); TEST_ASSERT_NOT_NULL(s);
    for (int i = 0; i < fs; i++) {              /* one second of the 1500 Hz tone: 1500 whole cycles */
        tone[i].real = 3000.0f * sinf(2.0f * (float)M_PI * 1500.0f * i / fs);
        tone[i].imag = 0.0f;
    }
    float snr[2];
    for (int k = 0; k < 2; k++) {
        watterson_t w;
        TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, fs));
        watterson_seed(&w, seed);
        if (k == 0) TEST_ASSERT_EQUAL_INT(0, watterson_add_path(&w, 0.0f, 0.0f, 0.0f, 1.0f));   /* AWGN only */
        else        TEST_ASSERT_EQUAL_INT(0, watterson_apply_preset(&w, watterson_preset_find("ccir-flutter")));
        watterson_set_noise(&w, -20.0f);
        watterson_reset_meas(&w);
        for (int sec = 0; sec < secs; sec++) {
            memcpy(s, tone, sizeof(COMP) * (size_t)fs);
            watterson_process(&w, s, fs);
        }
        snr[k] = watterson_measured_snr3k(&w);
        watterson_dispose(&w);
    }
    free(tone); free(s);
    char msg[128];
    snprintf(msg, sizeof msg, "SNR3k: one static path %.3f dB, ccir-flutter %.3f dB, difference %+.3f dB (gate 0.1)",
             snr[0], snr[1], snr[1] - snr[0]);
    if (verbose()) printf("  %s\n", msg);
    TEST_ASSERT_TRUE_MESSAGE(fabsf(snr[1] - snr[0]) < 0.1f, msg);
}

/* Relative error of a path's frequency offset: DC in through one static path
 * with freq_hz set, the output is exp(j 2 pi f n / fs) and the slope of its
 * unwrapped phase is f.  Also returns the largest deviation of |out| from 1. */
static double freq_offset_error(int fs, float f_hz, double secs, double *mag_err)
{
    watterson_t w;
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&w, fs));
    TEST_ASSERT_EQUAL_INT(0, watterson_add_path(&w, 0.0f, 0.0f, f_hz, 1.0f));
    enum { BLK = 4096 };
    COMP s[BLK];
    long n = (long)(secs * fs), done = 0;
    double phi = 0.0, prev = 0.0, worst = 0.0;
    int first = 1;
    while (done < n) {
        int nb = (n - done < BLK) ? (int)(n - done) : BLK;
        for (int i = 0; i < nb; i++) { s[i].real = 1.0f; s[i].imag = 0.0f; }
        watterson_process(&w, s, nb);
        for (int i = 0; i < nb; i++) {
            double a = atan2((double)s[i].imag, (double)s[i].real);
            if (!first) {
                double d = a - prev;
                while (d >  M_PI) d -= 2.0 * M_PI;
                while (d < -M_PI) d += 2.0 * M_PI;
                phi += d;
            }
            first = 0;
            prev = a;
            double mag = hypot((double)s[i].real, (double)s[i].imag);
            if (fabs(mag - 1.0) > worst) worst = fabs(mag - 1.0);
        }
        done += nb;
    }
    watterson_dispose(&w);
    *mag_err = worst;
    double f_meas = phi / (2.0 * M_PI * (double)(n - 1) / fs);
    return f_meas / (double)f_hz - 1.0;
}

/* Per-path frequency offset (the model's only frequency mechanism: the tool's
 * --freq is a whole-signal rotation before the channel).  The phase is
 * accumulated in float, so a small offset at a high rate carries a bias: the
 * 0.1 Hz / 48 kHz case gets 0.5 %, the rest 0.01 %. */
void test_per_path_frequency_offset_rotates_at_the_set_rate(void)
{
    const struct { int fs; float f; double tol; } cases[] = {
        { 8000,   1.0f, 1e-4 }, { 8000,  75.0f, 1e-4 }, { 8000, -75.0f, 1e-4 },
        { 48000,  1.0f, 1e-4 }, { 48000, 75.0f, 1e-4 }, { 48000,  0.1f, 5e-3 },
    };
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        double mag_err;
        double err = freq_offset_error(cases[k].fs, cases[k].f, 20.0, &mag_err);
        char msg[128];
        snprintf(msg, sizeof msg, "fs=%d f=%.1f Hz: relative error %+.3e, max | |out|-1 | = %.2e",
                 cases[k].fs, cases[k].f, err, mag_err);
        if (verbose()) printf("  %s\n", msg);
        TEST_ASSERT_TRUE_MESSAGE(fabs(err) < cases[k].tol, msg);
        TEST_ASSERT_TRUE_MESSAGE(mag_err < 1e-4, msg);
    }
}

/* One second of the 1500 Hz test tone at 8 kHz through w with No = -20 dB/Hz
 * (AWGN on, as the pinned-hash test runs).  The caller frees the result. */
static COMP *run_tone_1s(watterson_t *w)
{
    const int fs = 8000, n = fs;
    COMP *s = malloc(sizeof(COMP) * (size_t)n);
    TEST_ASSERT_NOT_NULL(s);
    for (int i = 0; i < n; i++) {
        s[i].real = 3000.0f * sinf(2.0f * (float)M_PI * 1500.0f * i / fs);
        s[i].imag = 0.0f;
    }
    watterson_set_noise(w, -20.0f);
    watterson_reset_meas(w);
    watterson_process(w, s, n);
    return s;
}

/* A named preset must build EXACTLY the channel its row describes: the same
 * draws in the same order as watterson_add_path() by hand, so the output is
 * byte-identical for the same seed.  This is an in-process comparison rather
 * than a second pinned hash, because a pinned hash fails on this Mac for a
 * cause not yet established (see
 * test_a_pinned_seed_reproduces_a_fixed_realisation above).  "poor" is the
 * channel of the pinned-hash test, so where that pin holds it covers the
 * preset table too; the preset must also leave the caller's spectrum
 * selection alone. */
void test_legacy_preset_is_byte_identical_to_the_hand_built_legacy_channel(void)
{
    watterson_t a, b;
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&a, 8000));
    watterson_seed(&a, 1);
    TEST_ASSERT_EQUAL_INT(0, watterson_apply_preset(&a, watterson_preset_find("poor")));
    TEST_ASSERT_EQUAL_INT(WATTERSON_SPECTRUM_LEGACY, a.paths[0].spectrum);
    TEST_ASSERT_EQUAL_INT(WATTERSON_SPECTRUM_GAUSSIAN, a.spectrum);   /* caller's selection restored */

    TEST_ASSERT_EQUAL_INT(0, watterson_init(&b, 8000));
    watterson_seed(&b, 1);
    TEST_ASSERT_EQUAL_INT(0, watterson_set_spectrum(&b, WATTERSON_SPECTRUM_LEGACY));
    TEST_ASSERT_EQUAL_INT(0, watterson_add_path(&b, 0.0f, 1.0f, 0.0f, 0.7f));
    TEST_ASSERT_EQUAL_INT(1, watterson_add_path(&b, 2.0f, 1.0f, 0.0f, 0.7f));

    COMP *sa = run_tone_1s(&a), *sb = run_tone_1s(&b);
    TEST_ASSERT_EQUAL_MEMORY(sb, sa, sizeof(COMP) * 8000);
    free(sa); free(sb);
    watterson_dispose(&a); watterson_dispose(&b);
}

void test_gaussian_preset_is_byte_identical_to_the_hand_built_channel(void)
{
    watterson_t a, b, c;
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&a, 8000));
    watterson_seed(&a, 1);
    TEST_ASSERT_EQUAL_INT(0, watterson_apply_preset(&a, watterson_preset_find("ccir-poor")));
    TEST_ASSERT_EQUAL_INT(WATTERSON_SPECTRUM_GAUSSIAN, a.paths[0].spectrum);

    TEST_ASSERT_EQUAL_INT(0, watterson_init(&b, 8000));
    watterson_seed(&b, 1);
    TEST_ASSERT_EQUAL_INT(0, watterson_add_path(&b, 0.0f, 1.0f, 0.0f, 0.70710678f));
    TEST_ASSERT_EQUAL_INT(1, watterson_add_path(&b, 2.0f, 1.0f, 0.0f, 0.70710678f));

    /* and the legacy channel with the same numbers is a different one */
    TEST_ASSERT_EQUAL_INT(0, watterson_init(&c, 8000));
    watterson_seed(&c, 1);
    TEST_ASSERT_EQUAL_INT(0, watterson_set_spectrum(&c, WATTERSON_SPECTRUM_LEGACY));
    TEST_ASSERT_EQUAL_INT(0, watterson_add_path(&c, 0.0f, 1.0f, 0.0f, 0.70710678f));
    TEST_ASSERT_EQUAL_INT(1, watterson_add_path(&c, 2.0f, 1.0f, 0.0f, 0.70710678f));

    COMP *sa = run_tone_1s(&a), *sb = run_tone_1s(&b), *sc = run_tone_1s(&c);
    TEST_ASSERT_EQUAL_MEMORY(sb, sa, sizeof(COMP) * 8000);
    TEST_ASSERT_TRUE_MESSAGE(memcmp(sa, sc, sizeof(COMP) * 8000) != 0,
                             "Gaussian and legacy spectra produced the same output");
    free(sa); free(sb); free(sc);
    watterson_dispose(&a); watterson_dispose(&b); watterson_dispose(&c);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fading_is_stable_across_sample_rates);
    RUN_TEST(test_single_static_path_is_rate_invariant);
    RUN_TEST(test_a_pinned_seed_reproduces_a_fixed_realisation);
    RUN_TEST(test_gaussian_tap_has_unit_power_and_the_standard_width);
    RUN_TEST(test_gaussian_tap_has_unit_power_and_the_standard_width_at_48_khz);
    RUN_TEST(test_delay_line_delivers_the_configured_delay);
    RUN_TEST(test_every_preset_delivers_its_delay_through_the_impulse);
    RUN_TEST(test_add_path_rejects_out_of_range_arguments);
    RUN_TEST(test_gaussian_preset_taps_are_zero_mean_circular_independent_rayleigh);
    RUN_TEST(test_two_path_preset_keeps_unit_average_power_and_rayleigh_depth);
    RUN_TEST(test_the_fft_behind_the_spectrum_test_puts_a_tone_in_its_bin);
    RUN_TEST(test_spectrum_tails_separate_the_gaussian_from_the_butterworth);
    RUN_TEST(test_fade_rate_matches_the_rice_closed_form);
    RUN_TEST(test_fading_preset_reports_the_same_snr3k_as_awgn_alone);
    RUN_TEST(test_per_path_frequency_offset_rotates_at_the_set_rate);
    RUN_TEST(test_legacy_preset_is_byte_identical_to_the_hand_built_legacy_channel);
    RUN_TEST(test_gaussian_preset_is_byte_identical_to_the_hand_built_channel);
    return UNITY_END();
}
