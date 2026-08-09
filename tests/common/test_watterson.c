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
#include <math.h>
#include <stdlib.h>

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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fading_is_stable_across_sample_rates);
    RUN_TEST(test_single_static_path_is_rate_invariant);
    return UNITY_END();
}
