/* Deterministic unit tests for the channel-busy (occupancy) classifier.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Drives channel_busy_update() with synthetic 512-bin spectra on a virtual
 * clock (no audio, no threads).  Verifies: stays CLEAR on noise; asserts BUSY
 * only after the on-debounce; releases only after the hang time; and does not
 * flap on a borderline signal.
 */
#include "unity.h"
#include "channel_busy.h"

#include <string.h>
#include <math.h>

#define NBINS 512
#define FS    8000            /* bin i centred at i*(FS/2)/NBINS = i*7.8125 Hz  */

static busy_cfg_t   cfg;
static busy_state_t st;
static float        spec[NBINS];

void setUp(void)
{
    cfg = BUSY_CFG_DEFAULT;   /* threshold 10 dB, hyst 3, on 300 ms, hang 1500  */
    channel_busy_init(&st);
}
void tearDown(void) {}

/* Fill the whole spectrum with a flat noise level (dB). */
static void fill_noise(float noise_db)
{
    for (int i = 0; i < NBINS; i++) spec[i] = noise_db;
}

/* Add an in-passband signal (a peak `signal_db` at ~1500 Hz => bin 192). */
static void add_passband_signal(float signal_db)
{
    spec[192] = signal_db;
    spec[191] = signal_db - 2.0f;
    spec[193] = signal_db - 2.0f;
}

/* Feed one spectrum frame; returns true if the debounced state changed. */
static bool feed(uint64_t now_ms, bool *busy_out)
{
    return channel_busy_update(&st, &cfg, spec, NBINS, FS, now_ms, busy_out);
}

/* --- Tests --------------------------------------------------------------- */

/* Flat noise only: never goes BUSY. */
void test_noise_stays_clear(void)
{
    bool busy = true;
    for (uint64_t t = 0; t <= 5000; t += 50) {
        fill_noise(-100.0f);
        bool changed = feed(t, &busy);
        TEST_ASSERT_FALSE(changed);
        TEST_ASSERT_FALSE(busy);
    }
}

/* A strong in-band signal asserts BUSY, but only after on_debounce_ms. */
void test_signal_asserts_busy_after_debounce(void)
{
    bool busy = false;

    /* Seed the noise floor first (a few quiet frames). */
    for (uint64_t t = 0; t < 500; t += 50) { fill_noise(-100.0f); feed(t, &busy); }
    TEST_ASSERT_FALSE(busy);

    /* Signal 25 dB over floor appears at t=500; on_debounce is 300 ms. */
    bool became_busy = false;
    uint64_t busy_at = 0;
    for (uint64_t t = 500; t <= 1200; t += 50) {
        fill_noise(-100.0f);
        add_passband_signal(-75.0f);   /* 25 dB above the -100 floor */
        if (feed(t, &busy) && busy) { became_busy = true; busy_at = t; break; }
    }
    TEST_ASSERT_TRUE(became_busy);
    /* First crossing at t=500 starts debounce; BUSY asserts >= 300 ms later. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(800, busy_at);
}

/* Once BUSY, a brief signal drop does NOT immediately clear (hang time). */
void test_busy_holds_through_hang(void)
{
    bool busy = false;
    /* Drive to BUSY. */
    for (uint64_t t = 0; t <= 1500; t += 50) {
        fill_noise(-100.0f);
        if (t >= 400) add_passband_signal(-70.0f);
        feed(t, &busy);
    }
    TEST_ASSERT_TRUE(busy);

    /* Signal gone from t=1500; hang is 1500 ms => must stay BUSY until ~3000. */
    bool cleared_early = false;
    for (uint64_t t = 1550; t < 2900; t += 50) {
        fill_noise(-100.0f);
        bool changed = feed(t, &busy);
        if (changed && !busy) { cleared_early = true; break; }
    }
    TEST_ASSERT_FALSE(cleared_early);
    TEST_ASSERT_TRUE(busy);

    /* After the full hang elapses it finally clears. */
    bool cleared = false;
    for (uint64_t t = 2900; t < 3600; t += 50) {
        fill_noise(-100.0f);
        if (feed(t, &busy) && !busy) { cleared = true; break; }
    }
    TEST_ASSERT_TRUE(cleared);
    TEST_ASSERT_FALSE(busy);
}

/* A signal that hovers right at the threshold must not rapidly flap. */
void test_borderline_does_not_flap(void)
{
    bool busy = false;
    int transitions = 0;

    for (uint64_t t = 0; t <= 8000; t += 50) {
        fill_noise(-100.0f);
        /* Oscillate the peak around ~threshold (floor -100 + ~10 dB). */
        float peak = (((t / 50) % 2) == 0) ? -90.5f : -89.5f;
        add_passband_signal(peak);
        if (feed(t, &busy)) transitions++;
    }
    /* Hysteresis + debounce/hang must keep this from chattering every frame. */
    TEST_ASSERT_LESS_THAN_INT(4, transitions);
}

/* Out-of-passband energy (below 300 Hz / above 2700 Hz) is ignored. */
void test_out_of_band_energy_ignored(void)
{
    bool busy = true;
    for (uint64_t t = 0; t <= 3000; t += 50) {
        fill_noise(-100.0f);
        spec[5]   = -50.0f;   /* ~39 Hz  — below passband  */
        spec[400] = -50.0f;   /* ~3125 Hz — above passband */
        feed(t, &busy);
    }
    TEST_ASSERT_FALSE(busy);
}

/* --- Realistic noise ------------------------------------------------------
 * The tests above use a flat "noise" spectrum, every bin at the same level --
 * which is how a classifier comparing the loudest bin with the quietest passed
 * them while reporting BUSY on almost every real empty channel: one FFT bin of
 * noise is an exponential variable, and across the passband the loudest sits
 * tens of dB above the quietest.  Each bin here is drawn independently, which
 * is harsher than a real (Hann-windowed, correlated) FFT. */

static unsigned long long rng_state = 88172645463325252ULL;
static double urand(void)
{
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((double)((rng_state >> 11) & 0x1FFFFFFFFFFFFFULL) + 0.5) / (double)0x20000000000000ULL;
}

/* Exponentially distributed bin powers around a mean of noise_db, plus an
 * optional wideband signal `sig_db` above the noise across 500-2500 Hz. */
static void fill_real_noise(float noise_db, float sig_db)
{
    for (int i = 0; i < NBINS; i++) {
        double p = -log(urand());                         /* mean 1 */
        double f = i * (FS / 2.0) / NBINS;
        if (sig_db > -900.0f && f >= 500.0 && f <= 2500.0)
            p += pow(10.0, sig_db / 10.0);
        spec[i] = noise_db + (float)(10.0 * log10(p));
    }
}

void test_real_noise_stays_clear(void)
{
    for (int seed = 1; seed <= 20; seed++) {
        channel_busy_init(&st);
        rng_state = 88172645463325252ULL * (unsigned long long)seed;
        bool busy = false;
        for (uint64_t t = 0; t <= 120000; t += 50) {
            fill_real_noise(-100.0f, -999.0f);
            feed(t, &busy);
            TEST_ASSERT_FALSE_MESSAGE(busy, "an empty channel read as BUSY");
        }
    }
}

/* A wideband signal (a digital mode) well above the noise asserts BUSY while
 * it lasts and releases once it ends. */
void test_real_wideband_signal_asserts_and_releases(void)
{
    bool busy = false, was_busy = false;
    for (uint64_t t = 0; t < 20000; t += 50) { fill_real_noise(-100.0f, -999.0f); feed(t, &busy); }
    TEST_ASSERT_FALSE(busy);
    for (uint64_t t = 20000; t < 30000; t += 50) {
        fill_real_noise(-100.0f, 12.0f);
        feed(t, &busy);
        if (t >= 21000) {
            TEST_ASSERT_TRUE_MESSAGE(busy, "missed a +12 dB wideband signal");
            was_busy = true;
        }
    }
    TEST_ASSERT_TRUE(was_busy);
    bool cleared = false;
    for (uint64_t t = 30000; t < 40000; t += 50) {
        fill_real_noise(-100.0f, -999.0f);
        if (feed(t, &busy) && !busy) cleared = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(cleared, "BUSY never released after the signal ended");
    TEST_ASSERT_FALSE(busy);
}

/* Digital silence -- a radio's capture while it transmits -- is no audio, not
 * a quiet channel.  Measuring it dragged the floor to ~-220 dB, after which
 * ordinary noise read as a 100 dB signal and BUSY latched for 80 minutes on a
 * field station. */
void test_digital_silence_does_not_latch_busy(void)
{
    bool busy = false;
    for (uint64_t t = 0; t < 10000; t += 50) { fill_real_noise(-100.0f, -999.0f); feed(t, &busy); }
    for (uint64_t t = 10000; t < 13000; t += 50) { fill_noise(-232.0f); feed(t, &busy); }
    for (uint64_t t = 13000; t < 60000; t += 50) {
        fill_real_noise(-100.0f, -999.0f);
        feed(t, &busy);
        TEST_ASSERT_FALSE_MESSAGE(busy, "noise after digital silence read as BUSY");
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_noise_stays_clear);
    RUN_TEST(test_signal_asserts_busy_after_debounce);
    RUN_TEST(test_busy_holds_through_hang);
    RUN_TEST(test_borderline_does_not_flap);
    RUN_TEST(test_out_of_band_energy_ignored);
    RUN_TEST(test_real_noise_stays_clear);
    RUN_TEST(test_real_wideband_signal_asserts_and_releases);
    RUN_TEST(test_digital_silence_does_not_latch_busy);
    return UNITY_END();
}
