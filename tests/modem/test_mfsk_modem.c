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

#include <stdio.h>
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

/* Head-clip recovery: a burst whose PREAMBLE was lost (the fragile head of a
 * half-duplex burst — far end still keyed / AGC settling / T-R turnaround) must
 * still decode by anchoring on the POSTAMBLE at the tail.  Without the postamble
 * fallback this returns 0 (the sole preamble anchor is gone), which stalled the
 * -x sock transfer at the MFSK floor: the ISS's first data burst was clipped by
 * the connect turnaround and lost to a full ACK-timeout retransmit. */
void test_mfsk_modem_preamble_clipped_recovers_via_postamble(void)
{
    int bytes = be->bits_per_frame(ctx) / 8;
    int nin   = be->nin(ctx);                     /* Nofdm */

    uint8_t *frame = malloc(bytes);
    for (int i = 0; i < bytes - 2; i++) frame[i] = (uint8_t)(i * 7 + 3);
    uint16_t crc = freedv_gen_crc16(frame, bytes - 2);
    frame[bytes - 2] = crc >> 8; frame[bytes - 1] = crc & 0xff;

    int cap = be->n_tx_samples(ctx) * 2 + 40000;
    int16_t *dat = calloc(cap, 2), *post = calloc(cap, 2);
    int nd = be->rawdata_tx(ctx, dat, frame);     /* data symbols  */
    int ns = be->postamble_tx(ctx, post);         /* postamble only — NO preamble */

    /* [lead gap | data | postamble | trail gap] — preamble deliberately omitted.
     * Kept within the RX window (rxcap ~ one burst + slack) so data+postamble
     * are both resident when the search fires. */
    int lead = 2 * nin, trail = 6 * nin;
    int total = lead + nd + ns + trail;
    int16_t *pb = calloc(total, 2);
    int p = lead;
    memcpy(pb + p, dat,  (size_t)nd * 2); p += nd;
    memcpy(pb + p, post, (size_t)ns * 2); p += ns;

    uint8_t *out = calloc(bytes, 1);
    int n = feed_and_decode(pb, total, out);
    TEST_ASSERT_EQUAL_INT(bytes, n);              /* recovered despite the clipped head */
    TEST_ASSERT_EQUAL_MEMORY(frame, out, bytes);

    free(frame); free(dat); free(post); free(pb); free(out);
}

/* Feed like the LIVE RX funnel does, not like a tidy test: 880-sample chunks
 * (modem.c's shared capture chunk, not this decoder's nin), and an arbitrary
 * amount of prior audio ahead of the burst.  Returns bytes on the call that
 * decodes, 0 if the whole buffer passes without one. */
static int feed_live_shaped(int16_t *pb, int total, uint8_t *out, int chunk)
{
    int nin = be->nin(ctx);
    /* Mirror rx_decoder_consume_chunk: accumulate, drain in nin-sized pieces,
     * and CARRY THE REMAINDER.  chunk (880) is not a multiple of nin (320), so
     * a loop that just walks the chunk in nin steps would silently discard 240
     * samples of every 880 and corrupt the waveform. */
    int16_t *acc = calloc((size_t)chunk + nin, sizeof(int16_t));
    int held = 0, ret = 0;
    for (int off = 0; off + chunk <= total && !ret; off += chunk)
    {
        memcpy(acc + held, &pb[off], (size_t)chunk * sizeof(int16_t));
        held += chunk;
        while (held >= nin)
        {
            int n = be->rawdata_rx(ctx, out, acc);
            held -= nin;
            if (held > 0) memmove(acc, acc + nin, (size_t)held * sizeof(int16_t));
            if (n > 0) { ret = n; break; }
        }
    }
    free(acc);
    return ret;
}

/* Build [preroll | preamble | data | postamble | trail] and try to decode.
 * preroll_syms is the prior audio ahead of the burst, in OFDM symbols. */
static int decode_with_preroll(int preroll_syms, int chunk, uint8_t **frame_out,
                               uint8_t **out_out)
{
    int bytes = be->bits_per_frame(ctx) / 8;
    int nin   = be->nin(ctx);
    int ndata = be->n_tx_samples(ctx);

    uint8_t *frame = malloc(bytes);
    for (int i = 0; i < bytes - 2; i++) frame[i] = (uint8_t)(i * 11 + 5);
    uint16_t crc = freedv_gen_crc16(frame, bytes - 2);
    frame[bytes - 2] = crc >> 8; frame[bytes - 1] = crc & 0xff;

    int cap = ndata * 2 + 40000;
    int16_t *pre = calloc(cap, 2), *dat = calloc(cap, 2), *post = calloc(cap, 2);
    int np = be->preamble_tx(ctx, pre);
    int nd = be->rawdata_tx(ctx, dat, frame);
    int ns = be->postamble_tx(ctx, post);

    int preroll = preroll_syms * nin;
    int trail   = 8 * nin;
    int total   = preroll + np + nd + ns + trail;
    int16_t *pb = calloc((size_t)total, 2);
    /* Prior audio is quiet but not digitally silent — a real receiver is never
     * fed exact zeros, and an all-zero preroll would flatter the sync search. */
    for (int i = 0; i < preroll; i++) pb[i] = (int16_t)(coin() ? 12 : -12);
    int p = preroll;
    memcpy(pb + p, pre,  (size_t)np * 2); p += np;
    memcpy(pb + p, dat,  (size_t)nd * 2); p += nd;
    memcpy(pb + p, post, (size_t)ns * 2); p += ns;

    uint8_t *out = calloc(bytes, 1);
    int n = feed_live_shaped(pb, total, out, chunk);

    free(pre); free(dat); free(post); free(pb);
    *frame_out = frame; *out_out = out;
    return n;
}

/* THE LIVE CASE.  On the air a burst never arrives 2 symbols into the receiver's
 * window: the IRS has been listening for seconds — through the CALL, its own
 * ACCEPT, the turnaround — so the data burst lands after a long stretch of prior
 * audio.  Measured on loopsim, that put the preamble at sample 101085 of a
 * 107520-sample window needing 204765, so it was detected (metric 0.889 against
 * a 0.168 noise floor) and could never be demodulated: fits=0, forever, and the
 * transfer delivered 0 bytes while -x sock (virtual clock, no deadline) passed.
 *
 * Residency is the contract being pinned here: a burst that has fully arrived
 * must be decodable regardless of how much audio preceded it. */
void test_mfsk_modem_decodes_after_long_preroll(void)
{
    /* Sweep the prior-audio length across and beyond one RX window.  A burst
     * that has fully arrived must decode at every one of these: on the air the
     * IRS has no control over how long it listened before the ISS keyed. */
    static const int preroll_syms[] = { 2, 40, 120, 240, 320, 400, 640 };
    int bytes = be->bits_per_frame(ctx) / 8;
    int failures = 0;

    for (unsigned i = 0; i < sizeof(preroll_syms)/sizeof(preroll_syms[0]); i++)
    {
        /* Fresh decoder per point: state must not carry between bursts. */
        be->close(ctx);
        ctx = be->open(MERCURY_MODE_MFSK);
        TEST_ASSERT_NOT_NULL(ctx);

        uint8_t *frame = NULL, *out = NULL;
        int n = decode_with_preroll(preroll_syms[i], 880, &frame, &out);
        int ok = (n == bytes) && (memcmp(frame, out, (size_t)bytes) == 0);
        if (!ok) failures++;
        printf("  preroll %4d symbols (%6.2f s): %s\n", preroll_syms[i],
               preroll_syms[i] * (double)be->nin(ctx) / 8000.0,
               ok ? "decoded" : "NO DECODE");
        free(frame); free(out);
    }
    TEST_ASSERT_EQUAL_INT(0, failures);
}

/* Same burst, negligible preroll: isolates residency from everything else.  If
 * this passes while the test above fails, the waveform and the decoder are fine
 * and only the window bookkeeping is wrong. */
void test_mfsk_modem_decodes_with_short_preroll(void)
{
    uint8_t *frame = NULL, *out = NULL;
    int bytes = be->bits_per_frame(ctx) / 8;
    int n = decode_with_preroll(2, 880, &frame, &out);
    TEST_ASSERT_EQUAL_INT(bytes, n);
    TEST_ASSERT_EQUAL_MEMORY(frame, out, bytes);
    free(frame); free(out);
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
    RUN_TEST(test_mfsk_modem_preamble_clipped_recovers_via_postamble);
    RUN_TEST(test_mfsk_modem_decodes_with_short_preroll);
    RUN_TEST(test_mfsk_modem_decodes_after_long_preroll);
    RUN_TEST(test_mfsk_modem_noise_no_false_decode);
    return UNITY_END();
}
