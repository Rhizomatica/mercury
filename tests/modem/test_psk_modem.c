/* PSK modem-backend round trip: a frame through the vtable, its audio fed to
 * the receiver in nin-sized chunks (as the modem's RX funnel does), the exact
 * frame back; the session CRC seed; noise never decodes.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "modem_psk.h"
#include "freedv_api.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const modem_backend_t *be;
static void *ctx;

void setUp(void)    { be = &modem_backend_psk; ctx = be->open(MERCURY_MODE_PSK3); TEST_ASSERT_NOT_NULL(ctx); }
void tearDown(void) { be->close(ctx); }

static uint64_t s_rng = 0x9E3779B97F4A7C15ULL;
static double uni(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (double)(s_rng >> 11) / 9007199254740992.0; }
static double gauss(void) { return sqrt(-2.0 * log(uni() + 1e-300)) * cos(2.0 * M_PI * uni()); }

/* A frame as the modem layer builds it: payload, then its CRC16 XOR seed. */
static int make_frame(uint8_t *f, uint16_t seed)
{
    int bytes = be->bits_per_frame(ctx) / 8;
    for (int i = 0; i < bytes - 2; i++) f[i] = (uint8_t)(i * 7 + 3);
    uint16_t crc = freedv_gen_crc16(f, bytes - 2) ^ seed;
    f[bytes - 2] = (uint8_t)(crc >> 8);
    f[bytes - 1] = (uint8_t)crc;
    return bytes;
}

/* One frame between silences, with white noise of the given rms. */
static int16_t *air(const uint8_t *f, double noise, int *total)
{
    int n = be->n_tx_samples(ctx), lead = 3000, tail = 4000;
    int16_t *tx = calloc((size_t)n, sizeof(int16_t)), *pb = calloc((size_t)(lead + n + tail), sizeof(int16_t));
    TEST_ASSERT_EQUAL_INT(0, be->preamble_tx(ctx, tx));
    TEST_ASSERT_EQUAL_INT(n, be->rawdata_tx(ctx, tx, f));
    TEST_ASSERT_EQUAL_INT(0, be->postamble_tx(ctx, tx));
    for (int i = 0; i < lead + n + tail; i++) {
        double v = (i >= lead && i < lead + n ? tx[i - lead] : 0) + noise * gauss();
        pb[i] = (int16_t)lrint(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    free(tx);
    *total = lead + n + tail;
    return pb;
}

static int feed(const int16_t *pb, int total, uint8_t *out)
{
    int nin = be->nin(ctx);
    for (int off = 0; off + nin <= total; off += nin) {
        int n = be->rawdata_rx(ctx, out, &pb[off]);
        if (n > 0) return n;
    }
    return 0;
}

void test_psk_roundtrip(void)
{
    uint8_t f[256], got[256];
    int bytes = make_frame(f, 0), total;
    TEST_ASSERT_EQUAL_INT(135, bytes);
    int16_t *pb = air(f, 300.0, &total);
    TEST_ASSERT_EQUAL_INT(bytes, feed(pb, total, got));
    TEST_ASSERT_EQUAL_MEMORY(f, got, (size_t)bytes);
    int sync = 0; float snr = -99.0f;
    be->get_stats(ctx, &sync, &snr);
    TEST_ASSERT_TRUE(snr > 10.0f);
    free(pb);
}

/* The peak the codec2 modes go out at, and no more. */
void test_psk_peak(void)
{
    uint8_t f[256];
    make_frame(f, 0);
    int n = be->n_tx_samples(ctx);
    int16_t *tx = calloc((size_t)n, sizeof(int16_t));
    be->rawdata_tx(ctx, tx, f);
    int pk = 0;
    for (int i = 0; i < n; i++) if (abs(tx[i]) > pk) pk = abs(tx[i]);
    TEST_ASSERT_INT_WITHIN(2, 16384, pk);
    free(tx);
}

/* A seeded receiver takes its session's frames and refuses another's. */
void test_psk_crc_seed(void)
{
    uint8_t f[256], got[256];
    int total;
    be->set_crc_seed(ctx, 0x5A5A, 0);
    make_frame(f, 0x5A5A);
    int16_t *pb = air(f, 300.0, &total);
    TEST_ASSERT_EQUAL_INT(135, feed(pb, total, got));
    TEST_ASSERT_TRUE(be->rx_crc_seeded(ctx));
    free(pb);

    be->unsync(ctx);
    make_frame(f, 0x1234);
    pb = air(f, 300.0, &total);
    TEST_ASSERT_EQUAL_INT(0, feed(pb, total, got));
    free(pb);
}

void test_psk_noise_never_decodes(void)
{
    int total = 8000 * 60;
    int16_t *pb = malloc(sizeof(int16_t) * (size_t)total);
    for (int i = 0; i < total; i++) pb[i] = (int16_t)lrint(3000.0 * gauss());
    uint8_t got[256];
    TEST_ASSERT_EQUAL_INT(0, feed(pb, total, got));
    free(pb);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_psk_roundtrip);
    RUN_TEST(test_psk_peak);
    RUN_TEST(test_psk_crc_seed);
    RUN_TEST(test_psk_noise_never_decodes);
    return UNITY_END();
}
