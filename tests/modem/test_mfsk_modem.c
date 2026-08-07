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
static int decode_with_preroll(int preroll_syms, int trail_syms, int chunk,
                               uint8_t **frame_out, uint8_t **out_out)
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
    /* Trailing audio matters as much as leading.  A radio does not stop
     * feeding the decoder when the burst ends -- silence keeps arriving and
     * keeps sliding the window, which is exactly what the ALSA/PulseAudio
     * paths do and what the FIFO harness does NOT (it only moves bytes while
     * someone transmits, so a burst sits in an otherwise empty window).  Feed
     * a burst's worth of trailing audio so the decoder has to hold the burst
     * while the window walks past it. */
    int trail   = trail_syms * nin;
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
        int n = decode_with_preroll(preroll_syms[i], 8, 880, &frame, &out);
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
    int n = decode_with_preroll(2, 8, 880, &frame, &out);
    TEST_ASSERT_EQUAL_INT(bytes, n);
    TEST_ASSERT_EQUAL_MEMORY(frame, out, bytes);
    free(frame); free(out);
}

/* The modulator must not clip its own output.
 *
 * 32-carrier OFDM has a high peak-to-average ratio and MFSK_TXAMP is applied
 * straight to the IFFT output, so a value chosen by eye put the true peak at
 * 42426 against a 32767 rail: mfsk_emit() hard-clipped 45% of every payload and
 * PAPR collapsed to 1.9 dB.  The round-trip test still passed, because TX and
 * RX both saw the same deterministic distortion -- but on the air the burst
 * never decoded.  Assert headroom on the SAMPLES, which is the thing that was
 * actually wrong, and leave room for the operator's TX gain on top. */
/* THE RADIO CASE: a burst arrives in the middle of continuously flowing audio.
 * Silence before it AND after it, so the window keeps sliding once the burst is
 * complete.  On the air (ALSA, PulseAudio) this is the normal situation and the
 * transfer stalls after a single frame; the FIFO harness never exercises it
 * because it carries no idle audio at all. */
void test_mfsk_modem_decodes_burst_in_continuous_audio(void)
{
    static const int trail_syms[] = { 8, 40, 120, 240, 400 };
    int bytes = be->bits_per_frame(ctx) / 8;
    int failures = 0;

    for (unsigned i = 0; i < sizeof(trail_syms)/sizeof(trail_syms[0]); i++)
    {
        be->close(ctx);
        ctx = be->open(MERCURY_MODE_MFSK);
        TEST_ASSERT_NOT_NULL(ctx);

        uint8_t *frame = NULL, *out = NULL;
        int n = decode_with_preroll(40, trail_syms[i], 880, &frame, &out);
        int ok = (n == bytes) && (memcmp(frame, out, (size_t)bytes) == 0);
        if (!ok) failures++;
        printf("  trailing audio %4d symbols (%6.2f s): %s\n", trail_syms[i],
               trail_syms[i] * (double)be->nin(ctx) / 8000.0,
               ok ? "decoded" : "NO DECODE");
        free(frame); free(out);
    }
    TEST_ASSERT_EQUAL_INT(0, failures);
}

/* A SECOND burst, after the window has filled and started sliding.
 *
 * Live, the first burst decodes and every retransmission after it fails: the
 * successes all have a partly-filled window (bf_len 154849), the failures all
 * have a full one (bf_len 211169, i.e. sliding).  Everything the decoder
 * reports looks right on the failures -- preamble found, metric 0.891, payload
 * resident -- and the CRC still fails, which is the signature of the buffer
 * bookkeeping being wrong once samples start being dropped off the front, not
 * of a channel problem.
 *
 * One burst is never enough to catch that.  This feeds two, with enough audio
 * between them to fill and wrap the window. */
void test_mfsk_modem_decodes_second_burst_after_window_wraps(void)
{
    int bytes = be->bits_per_frame(ctx) / 8;
    int nin   = be->nin(ctx);
    int cap   = be->n_tx_samples(ctx) + 40000;

    uint8_t *frame = malloc(bytes);
    for (int i = 0; i < bytes - 2; i++) frame[i] = (uint8_t)(i * 23 + 9);
    uint16_t crc = freedv_gen_crc16(frame, bytes - 2);
    frame[bytes - 2] = crc >> 8; frame[bytes - 1] = crc & 0xff;

    int16_t *pre = calloc(cap,2), *dat = calloc(cap,2), *post = calloc(cap,2);
    int np = be->preamble_tx(ctx, pre);
    int nd = be->rawdata_tx(ctx, dat, frame);
    int ns = be->postamble_tx(ctx, post);
    int blen = np + nd + ns;

    /* [burst][idle][burst][idle] — the second must decode too. */
    int idle  = 400 * nin;               /* 16 s, comfortably wraps the window */
    int total = blen + idle + blen + idle;
    int16_t *pb = calloc((size_t)total, 2);
    int p = 0;
    for (int rep = 0; rep < 2; rep++)
    {
        memcpy(pb + p, pre,  (size_t)np * 2); p += np;
        memcpy(pb + p, dat,  (size_t)nd * 2); p += nd;
        memcpy(pb + p, post, (size_t)ns * 2); p += ns;
        for (int i = 0; i < idle; i++) pb[p + i] = (int16_t)(coin() ? 9 : -9);
        p += idle;
    }

    /* Count how many of the two bursts decode. */
    int chunk = 880, held = 0, decodes = 0;
    int16_t *acc = calloc((size_t)chunk + nin, 2);
    uint8_t *out = calloc((size_t)bytes, 1);
    for (int off = 0; off + chunk <= total; off += chunk)
    {
        memcpy(acc + held, &pb[off], (size_t)chunk * 2);
        held += chunk;
        while (held >= nin)
        {
            if (be->rawdata_rx(ctx, out, acc) > 0) decodes++;
            held -= nin;
            if (held > 0) memmove(acc, acc + nin, (size_t)held * 2);
        }
    }
    printf("  bursts decoded: %d of 2\n", decodes);
    free(acc); free(out); free(frame); free(pre); free(dat); free(post); free(pb);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, decodes,
        "a burst after the RX window wrapped did not decode");
}

void test_mfsk_modem_decodes_burst_behind_a_failed_anchor(void)
{
    /* A located preamble whose payload is resident but fails CRC must NOT stay
     * cached as the anchor.  The decoder keeps the anchor to avoid re-running
     * the correlation over the whole window, so settling on a dead one blinds it
     * until that anchor slides out -- long enough, with a two-burst window, to
     * lose the real burst sitting behind it.  This matters more the lower the
     * accept threshold goes, since weak-signal detection admits more bad
     * anchors by construction.
     *
     * Neither burst carries a postamble: the postamble fallback is a second,
     * full-window correlation that would otherwise mask the defect here, and on
     * a real half-duplex link the burst tail is not guaranteed either. */
    int bytes = be->bits_per_frame(ctx) / 8;
    int nin   = be->nin(ctx);
    int cap   = be->n_tx_samples(ctx) + 40000;

    uint8_t *frame = malloc(bytes);
    for (int i = 0; i < bytes - 2; i++) frame[i] = (uint8_t)(i * 17 + 5);
    uint16_t crc = freedv_gen_crc16(frame, bytes - 2);
    frame[bytes - 2] = crc >> 8; frame[bytes - 1] = crc & 0xff;

    int16_t *pre = calloc(cap,2), *dat = calloc(cap,2);
    int np = be->preamble_tx(ctx, pre);
    int nd = be->rawdata_tx(ctx, dat, frame);

    int gap   = 4 * nin;
    int trail = 12 * nin;                 /* room for the real payload to become resident */
    int total = (np + nd) * 2 + gap + trail;
    int16_t *pb = calloc((size_t)total, 2);
    int p = 0;

    /* decoy: a genuine preamble with garbage where the payload belongs */
    memcpy(pb + p, pre, (size_t)np * 2); p += np;
    for (int i = 0; i < nd; i++) pb[p + i] = (int16_t)(coin() ? 2200 : -2200);
    p += nd;
    for (int i = 0; i < gap; i++) pb[p + i] = (int16_t)(coin() ? 9 : -9);
    p += gap;

    /* the real burst, close enough behind that the decoy anchor is still in view */
    memcpy(pb + p, pre, (size_t)np * 2); p += np;
    memcpy(pb + p, dat, (size_t)nd * 2); p += nd;
    for (int i = 0; i < trail; i++) pb[p + i] = (int16_t)(coin() ? 9 : -9);

    int chunk = 880, held = 0, match = 0;
    int16_t *acc = calloc((size_t)chunk + nin, 2);
    uint8_t *out = calloc((size_t)bytes, 1);
    for (int off = 0; off + chunk <= total; off += chunk)
    {
        memcpy(acc + held, &pb[off], (size_t)chunk * 2);
        held += chunk;
        while (held >= nin)
        {
            if (be->rawdata_rx(ctx, out, acc) > 0 &&
                memcmp(out, frame, (size_t)bytes) == 0) match++;
            held -= nin;
            if (held > 0) memmove(acc, acc + nin, (size_t)held * 2);
        }
    }
    printf("  payload-match=%d\n", match);
    free(acc); free(out); free(frame); free(pre); free(dat); free(pb);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, match,
        "burst behind a failed anchor was lost -- is the dead anchor still cached?");
}

/* True single-sideband frequency shift of a real passband signal: form the
 * analytic signal with a Hilbert FIR, rotate, take the real part. Multiplying
 * by a cosine would make two sidebands, which is not what a mistuned radio
 * does. */
static void shift_hz(int16_t *x, int n, double df, double fs)
{
    enum { HT = 101 };
    double h[HT];
    for (int i = 0; i < HT; i++) {
        int k = i - HT / 2;
        if (k == 0 || (k % 2) == 0) { h[i] = 0.0; continue; }
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * i / (HT - 1));
        h[i] = (2.0 / (M_PI * k)) * w;
    }
    double *q = calloc((size_t)n, sizeof(double));
    for (int i = 0; i < n; i++) {
        double acc = 0.0;
        for (int j = 0; j < HT; j++) {
            int idx = i - HT / 2 + j;
            if (idx >= 0 && idx < n) acc += h[j] * (double)x[idx];
        }
        q[i] = acc;
    }
    for (int i = 0; i < n; i++) {
        double ph = 2.0 * M_PI * df * (double)i / fs;
        double v  = (double)x[i] * cos(ph) - q[i] * sin(ph);
        if (v >  32767.0) v =  32767.0;
        if (v < -32768.0) v = -32768.0;
        x[i] = (int16_t)lrint(v);
    }
    free(q);
}

void test_mfsk_modem_decodes_with_dial_offset(void)
{
    /* A mistuned radio shifts every tone. Beyond half a subcarrier (15.6 Hz
     * here) the nominal FFT bins see nothing and the mode goes deaf: measured
     * on the pre-fix decoder, 12 Hz decoded 10/10 and 16 Hz decoded 0/10.
     * Acquisition therefore has to search frequency as well as time.
     *
     * 47 Hz is deliberately 1.5 subcarriers -- an offset that defeats both the
     * original decoder AND a whole-bin-only hypothesis grid, since it sits
     * exactly between two whole-bin hypotheses. */
    const double offsets[] = { 0.0, 16.0, 47.0, 94.0 };

    for (unsigned oi = 0; oi < sizeof(offsets)/sizeof(offsets[0]); oi++)
    {
        be->close(ctx);
        ctx = be->open(MERCURY_MODE_MFSK);
        TEST_ASSERT_NOT_NULL(ctx);
        if (be->configure) be->configure(ctx, 1, 0);

        int bytes = be->bits_per_frame(ctx) / 8;
        int nin   = be->nin(ctx);
        int cap   = be->n_tx_samples(ctx) + 40000;

        uint8_t *frame = malloc(bytes);
        for (int i = 0; i < bytes - 2; i++) frame[i] = (uint8_t)(i * 31 + 7);
        uint16_t crc = freedv_gen_crc16(frame, bytes - 2);
        frame[bytes - 2] = crc >> 8; frame[bytes - 1] = crc & 0xff;

        int16_t *pre = calloc(cap,2), *dat = calloc(cap,2), *post = calloc(cap,2);
        int np = be->preamble_tx(ctx, pre);
        int nd = be->rawdata_tx(ctx, dat, frame);
        int ns = be->postamble_tx(ctx, post);

        int lead = 2 * nin, trail = 8 * nin;
        int total = lead + np + nd + ns + trail;
        int16_t *pb = calloc((size_t)total, 2);
        int p = lead;
        memcpy(pb + p, pre,  (size_t)np * 2); p += np;
        memcpy(pb + p, dat,  (size_t)nd * 2); p += nd;
        memcpy(pb + p, post, (size_t)ns * 2);

        if (offsets[oi] != 0.0) shift_hz(pb, total, offsets[oi], 8000.0);

        int chunk = 880, held = 0, match = 0;
        int16_t *acc = calloc((size_t)chunk + nin, 2);
        uint8_t *out = calloc((size_t)bytes, 1);
        for (int off = 0; off + chunk <= total; off += chunk) {
            memcpy(acc + held, &pb[off], (size_t)chunk * 2);
            held += chunk;
            while (held >= nin) {
                if (be->rawdata_rx(ctx, out, acc) > 0 &&
                    memcmp(out, frame, (size_t)bytes) == 0) match++;
                held -= nin;
                if (held > 0) memmove(acc, acc + nin, (size_t)held * 2);
            }
        }
        printf("  dial offset %+6.1f Hz (%.2f bins): %s\n", offsets[oi],
               offsets[oi] / (8000.0 / 256.0), match ? "decoded" : "LOST");
        free(acc); free(out); free(frame); free(pre); free(dat); free(post); free(pb);

        char msg[96];
        snprintf(msg, sizeof msg, "burst lost at a %.0f Hz dial offset", offsets[oi]);
        TEST_ASSERT_TRUE_MESSAGE(match >= 1, msg);
    }
}

void test_mfsk_modem_tx_does_not_clip(void)
{
    int bytes = be->bits_per_frame(ctx) / 8;
    int cap   = be->n_tx_samples(ctx) + 40000;

    uint8_t *frame = malloc(bytes);
    for (int i = 0; i < bytes - 2; i++) frame[i] = (uint8_t)(i * 13 + 7);
    uint16_t crc = freedv_gen_crc16(frame, bytes - 2);
    frame[bytes - 2] = crc >> 8; frame[bytes - 1] = crc & 0xff;

    int16_t *pre = calloc(cap, 2), *dat = calloc(cap, 2), *post = calloc(cap, 2);
    int np = be->preamble_tx(ctx, pre);
    int nd = be->rawdata_tx(ctx, dat, frame);
    int ns = be->postamble_tx(ctx, post);

    const int16_t *parts[3] = { pre, dat, post };
    const int      lens[3]  = { np,  nd,  ns  };
    for (int p = 0; p < 3; p++)
    {
        long clipped = 0;
        int  peak = 0;
        for (int i = 0; i < lens[p]; i++)
        {
            int v = abs(parts[p][i]);
            if (v > peak) peak = v;
            if (v >= 32700) clipped++;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)clipped,
            "MFSK TX clipped its own samples: lower MFSK_TXAMP");
        /* >=6 dB of headroom so a +6 dB operator gain still fits. */
        TEST_ASSERT_TRUE_MESSAGE(peak <= 16384,
            "MFSK TX peak leaves no headroom for the operator's TX gain");
    }
    free(frame); free(pre); free(dat); free(post);
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
    RUN_TEST(test_mfsk_modem_decodes_burst_in_continuous_audio);
    RUN_TEST(test_mfsk_modem_decodes_second_burst_after_window_wraps);
    RUN_TEST(test_mfsk_modem_decodes_burst_behind_a_failed_anchor);
    RUN_TEST(test_mfsk_modem_decodes_with_dial_offset);
    RUN_TEST(test_mfsk_modem_tx_does_not_clip);
    RUN_TEST(test_mfsk_modem_noise_no_false_decode);
    return UNITY_END();
}
