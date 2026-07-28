/*
 * 24-bit packed PCM conversion tests — pins the byte layout and the scaling
 * of audioio/pcm24.h without a sound card.
 *
 * This is the format ALSA/PulseAudio expose for 24-bit cards
 * (SND_PCM_FORMAT_S24_3LE).  Getting the sign extension or the byte order
 * wrong does not crash: it puts distorted or inverted audio on the air, which
 * is exactly the failure mode that has bitten this file before.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unity.h"
#include "pcm24.h"

#include <stdint.h>
#include <math.h>

void setUp(void)    { }
void tearDown(void) { }

/* ---- byte layout is little-endian on every host ---- */

void test_read_is_little_endian(void)
{
    const uint8_t p[3] = {0x78, 0x56, 0x34};        /* 0x345678 */
    TEST_ASSERT_EQUAL_INT32(0x345678 * 256, pcm24_rd_le(p));
}

void test_write_is_little_endian(void)
{
    uint8_t p[3] = {0, 0, 0};
    pcm24_wr_le(p, 0x345678 * 256);
    TEST_ASSERT_EQUAL_UINT8(0x78, p[0]);
    TEST_ASSERT_EQUAL_UINT8(0x56, p[1]);
    TEST_ASSERT_EQUAL_UINT8(0x34, p[2]);
}

/* ---- sign handling: bit 23 is the sign, not a magnitude bit ---- */

void test_read_negative_sign_extends(void)
{
    const uint8_t minus_one[3] = {0xFF, 0xFF, 0xFF};   /* 24-bit -1 */
    TEST_ASSERT_EQUAL_INT32(-256, pcm24_rd_le(minus_one));

    const uint8_t most_negative[3] = {0x00, 0x00, 0x80}; /* -2^23 */
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, pcm24_rd_le(most_negative));
}

void test_read_largest_positive(void)
{
    const uint8_t p[3] = {0xFF, 0xFF, 0x7F};          /* +2^23 - 1 */
    TEST_ASSERT_EQUAL_INT32(2147483392, pcm24_rd_le(p));   /* (2^23-1)*256 */
}

void test_write_negative(void)
{
    uint8_t p[3];
    pcm24_wr_le(p, -256);                              /* -> 24-bit -1 */
    TEST_ASSERT_EQUAL_UINT8(0xFF, p[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, p[1]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, p[2]);

    pcm24_wr_le(p, INT32_MIN);                         /* -> -2^23 */
    TEST_ASSERT_EQUAL_UINT8(0x00, p[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, p[1]);
    TEST_ASSERT_EQUAL_UINT8(0x80, p[2]);
}

/* ---- round trip: write then read must return the value, to 24-bit
 *      resolution (the low 8 bits cannot survive a 24-bit device) ---- */

void test_round_trip_preserves_value(void)
{
    const int32_t cases[] = {
        0, 256, -256, 65536, -65536,
        1073741824, -1073741824,          /* +/- half scale */
        2147483392, INT32_MIN,            /* extremes representable in 24 bit */
        123456 * 256, -123456 * 256,
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        uint8_t p[3];
        pcm24_wr_le(p, cases[i]);
        TEST_ASSERT_EQUAL_INT32(cases[i], pcm24_rd_le(p));
    }
}

/* Values that are not multiples of 256 lose only their low 8 bits, and the
 * error must stay under one 24-bit LSB — never wrap or flip sign. */
void test_round_trip_truncates_within_one_lsb(void)
{
    for (int32_t base = -2100000000; base < 2100000000; base += 37129151) {
        for (int k = 0; k < 256; k += 37) {
            int64_t vin = (int64_t)base + k;
            if (vin > INT32_MAX) break;
            uint8_t p[3];
            pcm24_wr_le(p, (int32_t)vin);
            int64_t out = pcm24_rd_le(p);
            TEST_ASSERT_TRUE_MESSAGE(out <= vin ? (vin - out) < 256 : (out - vin) < 256,
                                     "24-bit round trip error exceeded 1 LSB");
            /* sign must never invert */
            if (vin > 255)  TEST_ASSERT_TRUE(out >= 0);
            if (vin < -255) TEST_ASSERT_TRUE(out <= 0);
        }
    }
}

/* A full-scale sine must survive the round trip with its shape intact — the
 * property that actually matters on the air. */
void test_round_trip_sine_is_undistorted(void)
{
    for (int i = 0; i < 4096; i++) {
        double phase = 2.0 * M_PI * 1000.0 * i / 8000.0;
        int32_t in = (int32_t)(2000000000.0 * sin(phase));
        uint8_t p[3];
        pcm24_wr_le(p, in);
        int32_t out = pcm24_rd_le(p);
        int64_t err = (int64_t)in - out;
        if (err < 0) err = -err;
        TEST_ASSERT_TRUE_MESSAGE(err < 256, "sine sample distorted beyond 1 LSB");
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_read_is_little_endian);
    RUN_TEST(test_write_is_little_endian);
    RUN_TEST(test_read_negative_sign_extends);
    RUN_TEST(test_read_largest_positive);
    RUN_TEST(test_write_negative);
    RUN_TEST(test_round_trip_preserves_value);
    RUN_TEST(test_round_trip_truncates_within_one_lsb);
    RUN_TEST(test_round_trip_sine_is_undistorted);
    return UNITY_END();
}
