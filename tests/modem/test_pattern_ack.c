/* Deterministic tests for the tone-pattern signalling channel, at the
 * passband int16 interface the ARQ layer actually calls.
 *
 * tests/modem/test_mfsk.c covers the correlator on baseband bins.  This covers
 * the layer above it -- modulation to passband, downmix, detection -- because
 * that is what a caller gets, and a round trip through it is the only thing
 * that proves the two ends agree on the geometry.
 *
 * What is asserted, and why each one is here:
 *
 *   round trip       a generated pattern is detected, and ACK is told from
 *                    ACK+TURN.  The discrimination bit is the only bit this
 *                    channel carries; getting it wrong is a protocol error,
 *                    not a lost frame.
 *   noise            noise alone is never accepted.  A false ACK is worse
 *                    than a missed one: the sender believes a frame landed
 *                    and moves on, leaving the receiver a hole.
 *   silence          all-zero input is not a match (degenerate correlation).
 *   offset           the pattern is found when it does not start at sample 0,
 *                    which is the only case that ever occurs live -- the RX
 *                    slides a window over a stream, it does not get handed an
 *                    aligned burst.
 *   short buffer     less than one pattern is refused rather than scored on
 *                    whatever happens to be in the caller's memory.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "pattern_ack.h"
#include "mfsk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* An arbitrary session; the rotation it selects is exercised throughout. */
#define SID 0x37

void setUp(void) {}
void tearDown(void) {}

/* deterministic U(0,1) */
static unsigned long s_rng = 88172645463325252ULL;
static double urand(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return ((s_rng >> 11) + 1.0) / ((1ULL << 53) + 1.0);
}
static double grand(void)
{
    return sqrt(-2.0 * log(urand())) * cos(2.0 * M_PI * urand());
}

void test_geometry_is_sane(void)
{
    const int ns  = pattern_ack_nsymb();
    const int cap = pattern_ack_max_tx_samples();
    TEST_ASSERT_GREATER_THAN_INT(0, ns);
    TEST_ASSERT_GREATER_THAN_INT(0, cap);
    /* 0.64 s at 8 kHz.  Airtime is the reason this channel is worth having,
     * so a change to it should have to walk past a failing test. */
    TEST_ASSERT_EQUAL_INT(5120, cap);
}

void test_roundtrip_ack_and_break(void)
{
    const int cap = pattern_ack_max_tx_samples();
    int16_t *buf  = malloc((size_t)cap * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(buf);

    int is_break = -1;
    int n = pattern_ack_tx(buf, PATTERN_ACK, SID);
    TEST_ASSERT_EQUAL_INT(cap, n);
    TEST_ASSERT_EQUAL_INT(1, pattern_ack_detect(buf, n, SID, &is_break));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, is_break, "plain ACK read as ACK+TURN");

    is_break = -1;
    n = pattern_ack_tx(buf, PATTERN_BREAK, SID);
    TEST_ASSERT_EQUAL_INT(cap, n);
    TEST_ASSERT_EQUAL_INT(1, pattern_ack_detect(buf, n, SID, &is_break));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, is_break, "ACK+TURN read as plain ACK");

    free(buf);
}

void test_noise_is_not_an_ack(void)
{
    const int cap = pattern_ack_max_tx_samples();
    int16_t *buf  = malloc((size_t)cap * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(buf);

    int hits = 0;
    for (int t = 0; t < 20; t++) {
        for (int i = 0; i < cap; i++) {
            double v = 6000.0 * grand();
            if (v >  32767.0) v =  32767.0;
            if (v < -32768.0) v = -32768.0;
            buf[i] = (int16_t)lrint(v);
        }
        int isb = 0;
        if (pattern_ack_detect(buf, cap, SID, &isb)) hits++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits, "noise accepted as a pattern ACK");
    free(buf);
}

void test_silence_is_not_an_ack(void)
{
    const int cap = pattern_ack_max_tx_samples();
    int16_t *buf  = calloc((size_t)cap, sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(buf);
    int isb = 0;
    TEST_ASSERT_EQUAL_INT(0, pattern_ack_detect(buf, cap, SID, &isb));
    free(buf);
}

void test_detected_when_not_sample_aligned(void)
{
    const int cap  = pattern_ack_max_tx_samples();
    const int lead = 1234;                 /* arbitrary, not a symbol multiple */
    int16_t *pat   = malloc((size_t)cap * sizeof(int16_t));
    int16_t *win   = calloc((size_t)(cap + lead), sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pat);
    TEST_ASSERT_NOT_NULL(win);

    int n = pattern_ack_tx(pat, PATTERN_ACK, SID);
    memcpy(win + lead, pat, (size_t)n * sizeof(int16_t));

    int isb = -1;
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, pattern_ack_detect(win, cap + lead, SID, &isb),
                                  "pattern missed when offset in the window");
    TEST_ASSERT_EQUAL_INT(0, isb);
    free(pat); free(win);
}

void test_short_buffer_is_refused(void)
{
    const int cap = pattern_ack_max_tx_samples();
    int16_t *buf  = malloc((size_t)cap * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(buf);
    int n = pattern_ack_tx(buf, PATTERN_ACK, SID);
    int isb = 0;
    TEST_ASSERT_EQUAL_INT(0, pattern_ack_detect(buf, n - 1, SID, &isb));
    TEST_ASSERT_EQUAL_INT(0, pattern_ack_detect(NULL, n, SID, &isb));
    free(buf);
}

/* The two patterns must not be mistakable for one another.
 *
 * This is pure integer arithmetic on the shipped tone tables, so it is exact
 * rather than statistical: it scores every alignment of each pattern against
 * both tone lists and asserts the wrong list never reaches the acceptance
 * threshold.  The detector slides over candidate starts, so a shift of zero is
 * not the only case that matters.
 *
 * Guards the tables and the tone hop together.  Changing either can destroy
 * the separation without changing anything a round-trip test would notice --
 * a pattern still detects itself perfectly while becoming confusable with the
 * other one, and the confusion is a protocol error rather than a lost frame.
 *
 * (Recorded while checking whether these patterns could be rotated per session
 * to give the ACK an identity: they can, but only over half the rotations.
 * The pattern is an 8-tone array sent twice, and a shift of 8 symbols with a
 * hop of 13 lands on +8 mod 32 -- so rotations differing by 8 or 24 alias
 * exactly onto each other and score a full 8 of 16.  See docs/ACK-CHANNEL.md.) */
void test_ack_and_break_are_not_confusable(void)
{
    mfsk_t m;
    mfsk_init(&m, 32, 50, 1);

    const int  ns   = m.ack_pattern_nsymb;
    const int  len  = m.ack_pattern_len;
    const int  hop  = m.tone_hop_step;
    const int  M    = m.M;
    const int *tbl[2] = { m.ack_tones, m.break_tones };
    const int  thr[2] = { m.ack_match_threshold, m.break_match_threshold };

    for (int sent = 0; sent < 2; sent++)
    {
        for (int against = 0; against < 2; against++)
        {
            int best = 0;
            for (int shift = -(ns - 1); shift < ns; shift++)
            {
                int sc = 0;
                for (int p = 0; p < ns; p++)
                {
                    int q = p + shift;
                    if (q < 0 || q >= ns) continue;
                    int tx = (tbl[sent][q % len] + q * hop) % M;
                    int ex = (tbl[against][p % len] + p * hop) % M;
                    if (tx == ex) sc++;
                }
                if (sc > best) best = sc;
            }
            if (sent == against)
                TEST_ASSERT_EQUAL_INT_MESSAGE(ns, best,
                    "a pattern must score perfectly against its own table");
            else
                TEST_ASSERT_LESS_THAN_INT_MESSAGE(thr[against], best,
                    "ACK and ACK+TURN are confusable at some alignment");
        }
    }
    mfsk_deinit(&m);
}

/* A station must not hear another session's ACK.
 *
 * This is the property the rotation exists for, and the failure it prevents is
 * not a lost frame: an ISS that accepts a foreign ACK advances past a frame
 * the IRS never received, and since deliver_rx_checked() drops anything that
 * is not exactly rx_expected -- no NACK, no reordering buffer -- every frame
 * after it is dropped too.  The transfer wedges and the bytes are gone.
 *
 * Exhaustive over the 16 usable rotations and both tone tables, at every
 * alignment, on the shipped tables: integer arithmetic, so this is proof for
 * the noiseless case rather than a sample.  Noise can only add matches to a
 * foreign pattern (a random peak lands on the expected bin with probability
 * 1/M), so the margin this leaves -- threshold minus worst cross-score -- is
 * what stands between a busy channel and a wedged transfer. */
void test_sessions_do_not_hear_each_other(void)
{
    mfsk_t base;
    mfsk_init(&base, 32, 50, 1);
    const int ns  = base.ack_pattern_nsymb;
    const int len = base.ack_pattern_len;
    const int hop = base.tone_hop_step;
    const int M   = base.M;
    const int thr = base.ack_match_threshold < base.break_match_threshold
                    ? base.ack_match_threshold : base.break_match_threshold;

    /* The 16 rotations sessions can select, via the shipped mapping. */
    int rots[16];
    for (int i = 0; i < 16; i++) rots[i] = pattern_ack_rotation((uint8_t)i);

    int worst_cross = 0;
    for (int a = 0; a < 16; a++)
    {
        for (int b = 0; b < 16; b++)
        {
            for (int ta = 0; ta < 2; ta++)
            {
                const int *tbl_a = ta ? base.break_tones : base.ack_tones;
                for (int tb = 0; tb < 2; tb++)
                {
                    const int *tbl_b = tb ? base.break_tones : base.ack_tones;
                    for (int shift = -(ns - 1); shift < ns; shift++)
                    {
                        if (a == b && ta == tb && shift == 0)
                            continue;           /* the wanted match */
                        int sc = 0;
                        for (int p = 0; p < ns; p++)
                        {
                            int q = p + shift;
                            if (q < 0 || q >= ns) continue;
                            int tx = (tbl_a[q % len] + rots[a] + q * hop) % M;
                            int ex = (tbl_b[p % len] + rots[b] + p * hop) % M;
                            if (tx == ex) sc++;
                        }
                        if (sc > worst_cross) worst_cross = sc;
                    }
                }
            }
        }
    }

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(thr, worst_cross,
        "some session's pattern reaches another session's acceptance threshold");
    mfsk_deinit(&base);
}

/* The mapping must never hand out two rotations that alias (differ by 8 or 24
 * mod 32).  Guards the mapping itself rather than its consequences, so a
 * change to it fails here with a readable reason. */
void test_rotation_map_avoids_aliases(void)
{
    for (int i = 0; i < 256; i++)
    {
        for (int j = 0; j < 256; j++)
        {
            int ri = pattern_ack_rotation((uint8_t)i);
            int rj = pattern_ack_rotation((uint8_t)j);
            if (ri == rj) continue;            /* same rotation: fine */
            int d = ((ri - rj) % 32 + 32) % 32;
            TEST_ASSERT_TRUE_MESSAGE(d != 8 && d != 24,
                "rotation map produced an aliasing pair");
        }
    }
}

/* The receive path as the RX loop actually drives it.
 *
 * pattern_ack_detect() proves the correlator; this proves the thing around it,
 * which is where the mistakes live.  Capture arrives in small chunks -- the RX
 * loop hands over RX_DECODE_CHUNK_SAMPLES at a time -- so a burst is split
 * across many pushes and never starts at a window boundary.  A window that
 * only ever looked at one chunk, or that dropped the newest samples instead of
 * the oldest, would still pass every test above.
 *
 * Also asserts the window is CONSUMED on a match: without that, one burst on
 * the air is reported once per chunk for as long as it stays in view, and each
 * report is an ACK that advances the sender's window.
 */
#define CHUNK 160   /* RX_DECODE_CHUNK_SAMPLES */

static void push_silence(pattern_ack_window_t *w, int n, int *hits)
{
    int16_t z[CHUNK];
    memset(z, 0, sizeof(z));
    for (int i = 0; i < n; i += CHUNK)
    {
        int isb = 0;
        if (pattern_ack_window_push(w, z, CHUNK, SID, &isb)) (*hits)++;
    }
}

static void push_burst(pattern_ack_window_t *w, const int16_t *pat, int n,
                       int *hits, int *last_break)
{
    for (int off = 0; off < n; off += CHUNK)
    {
        int take = (n - off < CHUNK) ? (n - off) : CHUNK;
        int isb = 0;
        if (pattern_ack_window_push(w, pat + off, take, SID, &isb))
        {
            (*hits)++;
            if (last_break) *last_break = isb;
        }
    }
}

void test_window_detects_a_chunked_burst(void)
{
    const int cap = pattern_ack_max_tx_samples();
    int16_t *ack = malloc((size_t)cap * sizeof(int16_t));
    int16_t *brk = malloc((size_t)cap * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(ack);
    TEST_ASSERT_NOT_NULL(brk);
    int n_ack = pattern_ack_tx(ack, PATTERN_ACK,   SID);
    int n_brk = pattern_ack_tx(brk, PATTERN_BREAK, SID);

    pattern_ack_window_t w = {0};
    int hits = 0, isb = -1;

    /* Quiet channel first: nothing may fire, and the window must not be primed
     * into a state where the burst that follows is missed. */
    push_silence(&w, cap, &hits);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits, "silence produced an ACK");

    push_burst(&w, ack, n_ack, &hits, &isb);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, hits, "chunked ACK burst not detected exactly once");
    TEST_ASSERT_EQUAL_INT(0, isb);

    /* Keep pushing silence: the window was consumed, so the burst that just
     * matched must not match again as it slides out. */
    push_silence(&w, cap * 2, &hits);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, hits, "one burst reported more than once");

    /* A second, different burst on the same window. */
    isb = -1;
    push_burst(&w, brk, n_brk, &hits, &isb);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, hits, "second burst missed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, isb, "ACK+TURN read as plain ACK");

    pattern_ack_window_free(&w);
    free(ack); free(brk);
}

/* A foreign session's burst must not be reported through the window either --
 * the same property as test_sessions_do_not_hear_each_other, but exercised
 * through the code the RX loop runs rather than through the tone tables. */
void test_window_ignores_another_session(void)
{
    const int cap = pattern_ack_max_tx_samples();
    int16_t *pat = malloc((size_t)cap * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pat);

    /* A session whose rotation differs from SID's. */
    uint8_t other = SID ^ 0x01;
    TEST_ASSERT_NOT_EQUAL(pattern_ack_rotation(SID), pattern_ack_rotation(other));
    int n = pattern_ack_tx(pat, PATTERN_ACK, other);

    pattern_ack_window_t w = {0};
    int hits = 0;
    push_burst(&w, pat, n, &hits, NULL);
    push_silence(&w, cap, &hits);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits, "heard another session's ACK");

    pattern_ack_window_free(&w);
    free(pat);
}

/* A burst that was never matched must not be reported later, once the caller
 * has started waiting for a DIFFERENT ACK.
 *
 * This is the failure mode that makes the window stateful rather than a plain
 * buffer.  The window holds over a second of audio.  If a burst arrives but is
 * never matched -- it fell outside the gate, or the gate closed first -- it is
 * still sitting there when the caller opens the next ACK window, and the next
 * scan will find it.  The sender then treats it as the ACK for the frame now
 * outstanding, advances past a frame the peer never received, and the transfer
 * wedges: deliver_rx_checked() drops everything that follows.
 *
 * So the caller resets on arming, and this asserts the reset actually clears
 * what is held rather than only the bookkeeping around it. */
void test_reset_discards_a_stale_burst(void)
{
    const int cap = pattern_ack_max_tx_samples();
    int16_t *pat = malloc((size_t)cap * sizeof(int16_t));
    TEST_ASSERT_NOT_NULL(pat);
    int n = pattern_ack_tx(pat, PATTERN_ACK, SID);

    pattern_ack_window_t w = {0};
    int hits = 0;

    /* Offset the scan cadence by half a burst first, so that the scan which
     * falls during the burst sees only part of it and finds nothing.  The
     * burst then sits COMPLETE in the window with no further scan due -- which
     * is the state the gate closing leaves behind on a real miss. */
    push_silence(&w, cap / 2, &hits);
    for (int off = 0; off < n; off += CHUNK)
    {
        int take = (n - off < CHUNK) ? (n - off) : CHUNK;
        int isb = 0;
        if (pattern_ack_window_push(&w, pat + off, take, SID, &isb)) hits++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits, "test setup: burst was matched too early");

    /* The caller now starts waiting for a different ACK. */
    pattern_ack_window_reset(&w);

    /* Nothing but silence follows.  The stale burst must be gone. */
    push_silence(&w, cap * 3, &hits);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits,
        "a stale burst was reported as the next frame's ACK");

    pattern_ack_window_free(&w);
    free(pat);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_geometry_is_sane);
    RUN_TEST(test_roundtrip_ack_and_break);
    RUN_TEST(test_noise_is_not_an_ack);
    RUN_TEST(test_silence_is_not_an_ack);
    RUN_TEST(test_detected_when_not_sample_aligned);
    RUN_TEST(test_short_buffer_is_refused);
    RUN_TEST(test_ack_and_break_are_not_confusable);
    RUN_TEST(test_sessions_do_not_hear_each_other);
    RUN_TEST(test_rotation_map_avoids_aliases);
    RUN_TEST(test_window_detects_a_chunked_burst);
    RUN_TEST(test_window_ignores_another_session);
    RUN_TEST(test_reset_discards_a_stale_burst);
    return UNITY_END();
}
