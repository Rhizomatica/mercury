/* The control plane's codec (modem_ctl.c): fed one stream, it returns a frame
 * of the control mode and a short control frame, each exactly, CRC-gated, and
 * transmits the control mode.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "modem_ctl.h"
#include "modem_psk.h"
#include "freedv_api.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const modem_backend_t *be = &modem_backend_ctl, *pk = &modem_backend_psk;
static void *ctx, *psk;

void setUp(void)    { ctx = be->open(FREEDV_MODE_DATAC16); psk = pk->open(MERCURY_MODE_PSKC); TEST_ASSERT_NOT_NULL(ctx); TEST_ASSERT_NOT_NULL(psk); be->configure(ctx, 1, 0); }
void tearDown(void) { be->close(ctx); pk->close(psk); }

static uint64_t s_rng = 0x9E3779B97F4A7C15ULL;
static double uni(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (double)(s_rng >> 11) / 9007199254740992.0; }
static double gauss(void) { return sqrt(-2.0 * log(uni() + 1e-300)) * cos(2.0 * M_PI * uni()); }

static void frame(uint8_t *f, int bytes, int k)
{
    for (int i = 0; i < bytes - 2; i++) f[i] = (uint8_t)(i * 11 + k);
    uint16_t crc = freedv_gen_crc16(f, bytes - 2);
    f[bytes - 2] = (uint8_t)(crc >> 8);
    f[bytes - 1] = (uint8_t)crc;
}

/* Append codec's burst for f, then silence, to pb at *n. */
static void burst(const modem_backend_t *b, void *c, const uint8_t *f, int16_t *pb, int *n)
{
    int16_t *tmp = malloc(sizeof(int16_t) * 2 * (size_t)b->n_tx_samples(c) + 65536);
    int k = b->preamble_tx(c, tmp);
    k += b->rawdata_tx(c, tmp + k, f);
    k += b->postamble_tx(c, tmp + k);
    memcpy(pb + *n, tmp, sizeof(int16_t) * (size_t)k);
    *n += k + 8000;
    free(tmp);
}

void test_ctl_hears_both(void)
{
    int bytes = be->bits_per_frame(ctx) / 8;
    TEST_ASSERT_EQUAL_INT(bytes, pk->bits_per_frame(psk) / 8);
    uint8_t a[64], b[64], got[64];
    frame(a, bytes, 1);
    frame(b, bytes, 2);
    int cap = 8000 * 20, n = 4000;
    int16_t *pb = calloc((size_t)cap, sizeof(int16_t));
    burst(be, ctx, a, pb, &n);           /* the control mode, from the wrapper itself */
    burst(pk, psk, b, pb, &n);           /* the short control frame */
    for (int i = 0; i < cap; i++) {
        double v = pb[i] * 0.5 + 200.0 * gauss();
        pb[i] = (int16_t)lrint(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    int seen_a = 0, seen_b = 0;
    for (int off = 0; off < cap;) {
        int nin = be->nin(ctx);
        if (off + nin > cap) break;
        int k = be->rawdata_rx(ctx, got, pb + off);
        off += nin;
        /* each frame as the mode it came on; what TX reads stays the control mode */
        if (k == bytes && !memcmp(got, a, (size_t)bytes)) { seen_a++; TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC16, be->rx_mode(ctx)); }
        if (k == bytes && !memcmp(got, b, (size_t)bytes)) { seen_b++; TEST_ASSERT_EQUAL_INT(MERCURY_MODE_PSKC, be->rx_mode(ctx)); }
        TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC16, be->get_mode(ctx));
    }
    free(pb);
    TEST_ASSERT_EQUAL_INT(1, seen_a);
    TEST_ASSERT_EQUAL_INT(1, seen_b);
}

/* A short frame cut by a flush (our keydown starting part-way into it) is
 * dropped, not completed by the audio after it as a fresh frame: decoded
 * 10/10 at every cut from 50 to 90 % before the flush reached the short
 * frame's receiver (#629 review).  The control mode's rxbuf stays. */
void test_ctl_flush_drops_a_cut_short_frame(void)
{
    TEST_ASSERT_NOT_NULL(be->flush);
    int bytes = be->bits_per_frame(ctx) / 8;
    uint8_t b[64], got[64];
    frame(b, bytes, 3);
    int cap = 8000 * 12, n = 4000;
    int16_t *pb = calloc((size_t)cap, sizeof(int16_t));
    burst(pk, psk, b, pb, &n);
    int len = n - 8000 - 4000;
    for (int pct = 50; pct <= 90; pct += 10) {
        int cut = 4000 + len * pct / 100, seen = 0, flushed = 0;
        void *c = be->open(FREEDV_MODE_DATAC16);
        be->configure(c, 1, 0);
        for (int off = 0; off < cap;) {
            int nin = be->nin(c);
            if (off + nin > cap) break;
            int16_t chunk[8192];
            for (int i = 0; i < nin; i++) {
                double v = (off + i < cut ? pb[off + i] * 0.5 : 0.0) + 200.0 * gauss();
                chunk[i] = (int16_t)lrint(v);
            }
            if (!flushed && off >= cut) { be->flush(c); flushed = 1; }
            int k = be->rawdata_rx(c, got, chunk);
            off += nin;
            if (k == bytes && !memcmp(got, b, (size_t)bytes)) seen++;
        }
        be->close(c);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, seen, "a cut short frame decoded after the flush");
    }
    free(pb);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ctl_hears_both);
    RUN_TEST(test_ctl_flush_drops_a_cut_short_frame);
    return UNITY_END();
}
