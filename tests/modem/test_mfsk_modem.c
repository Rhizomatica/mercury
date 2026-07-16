/* MFSK modem-backend round-trip: TX a frame through the vtable, assemble the
 * passband burst, feed it to the RX in nin-sized chunks (as the modem RX funnel
 * does), and require the exact frame back — CRC-gated, like FreeDV. Also checks
 * that pure noise never false-decodes.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "modem_mfsk.h"
#include "freedv_api.h"

#include <stdlib.h>
#include <string.h>

static const modem_backend_t *be;
static void *ctx;

void setUp(void)   { be = &modem_backend_mfsk; ctx = be->open(MERCURY_MODE_MFSK); TEST_ASSERT_NOT_NULL(ctx); }
void tearDown(void){ be->close(ctx); }

/* deterministic PRNG */
static unsigned long s_rng = 0x9E3779B97F4A7C15ULL;
static int coin(void){ s_rng ^= s_rng<<13; s_rng ^= s_rng>>7; s_rng ^= s_rng<<17; return (int)(s_rng & 1); }

/* Feed a passband buffer to rawdata_rx in nin-sized chunks; return bytes on the
 * call that decodes a frame (0 if none over the whole buffer). */
static int feed_and_decode(int16_t *pb, int total, uint8_t *out)
{
    int nin = be->nin(ctx);
    for (int off = 0; off + nin <= total; off += nin)
    {
        int n = be->rawdata_rx(ctx, out, &pb[off]);
        if (n > 0) return n;
    }
    return 0;
}

void test_mfsk_modem_roundtrip(void)
{
    int bytes = be->bits_per_frame(ctx) / 8;      /* 100 */
    int ndata = be->n_tx_samples(ctx);
    TEST_ASSERT_EQUAL_INT(100, bytes);

    /* Build a frame: 98 payload bytes + CRC16 in the last 2 (as send_modulated_data does). */
    uint8_t *frame = malloc(bytes);
    for (int i = 0; i < bytes - 2; i++) frame[i] = (uint8_t)(i * 5 + 1);
    uint16_t crc = freedv_gen_crc16(frame, bytes - 2);
    frame[bytes - 2] = crc >> 8; frame[bytes - 1] = crc & 0xff;

    /* TX preamble + data + postamble into one passband buffer, with silence gaps. */
    int cap = ndata * 2 + 40000;
    int16_t *pre = calloc(cap, 2), *dat = calloc(cap, 2), *post = calloc(cap, 2);
    int np = be->preamble_tx(ctx, pre);
    int nd = be->rawdata_tx(ctx, dat, frame);
    int ns = be->postamble_tx(ctx, post);
    TEST_ASSERT_EQUAL_INT(ndata, nd);

    int gap = 1200;
    int total = gap + np + nd + ns + gap;
    int16_t *pb = calloc(total, 2);
    int p = gap;
    memcpy(pb + p, pre,  (size_t)np * 2); p += np;
    memcpy(pb + p, dat,  (size_t)nd * 2); p += nd;
    memcpy(pb + p, post, (size_t)ns * 2); p += ns;

    uint8_t *out = calloc(bytes, 1);
    int n = feed_and_decode(pb, total, out);
    TEST_ASSERT_EQUAL_INT(bytes, n);              /* decoded, CRC-valid */
    TEST_ASSERT_EQUAL_MEMORY(frame, out, bytes);  /* exact bytes back */

    free(frame); free(pre); free(dat); free(post); free(pb); free(out);
}

void test_mfsk_modem_noise_no_false_decode(void)
{
    int bytes = be->bits_per_frame(ctx) / 8;
    int total = be->n_tx_samples(ctx) + 8000;
    int16_t *pb = malloc((size_t)total * 2);
    for (int i = 0; i < total; i++) pb[i] = (int16_t)((coin() ? 1 : -1) * (400 + (int)(s_rng % 400)));
    uint8_t *out = calloc(bytes, 1);
    int n = feed_and_decode(pb, total, out);
    TEST_ASSERT_EQUAL_INT(0, n);                  /* noise never yields a CRC-valid frame */
    free(pb); free(out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mfsk_modem_roundtrip);
    RUN_TEST(test_mfsk_modem_noise_no_false_decode);
    return UNITY_END();
}
