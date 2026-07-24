/* test_burst_selfdescribe.c — codec-layer proof of the windowed-ARQ
 * self-describing burst mechanism (freedv_set_frames_remaining /
 * ofdm_set_packets_remaining).
 *
 * The OFDM burst state machine checks the unique word only during acquisition;
 * once synced its only exit is packet_count >= packetsperburst.  A receiver
 * that acquires with a fixed frames-per-burst therefore mis-handles a burst
 * that carries a DIFFERENT number of frames: a SHORT burst (fewer frames than
 * expected) makes it consume the following keydown's preamble as burst tail.
 *
 * Windowed ARQ makes bursts self-describing: every DATA frame carries how many
 * frames of its keydown remain, and the RX re-anchors the state machine from
 * each decoded frame via freedv_set_frames_remaining().  This test drives the
 * real DATAC3 modem in-process with a mixed burst plan and asserts:
 *   (a) with self-describe, every frame of every burst decodes;
 *   (b) WITHOUT it (fixed frames_per_burst = ceiling), a short-then-long plan
 *       loses frames — the pathology the mechanism fixes.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "freedv_api.h"

#include <stdlib.h>
#include <string.h>

/* Burst plan: frame counts per keydown.  Deliberately mixes a short burst
 * before a long one (the pathology case) and a long before a short. */
#define BURST_CEILING 5
static const int g_plan[] = {2, 4, 3, 5, 1};
#define N_BURSTS ((int)(sizeof(g_plan) / sizeof(g_plan[0])))

static int total_frames(void)
{
    int n = 0;
    for (int i = 0; i < N_BURSTS; i++) n += g_plan[i];
    return n;
}

/* Per-burst sample span recorded by build_stream (for targeted corruption). */
typedef struct { int data_start; int data_end; } burst_span_t;

/* Modulate a burst plan into pcm[]; each frame's payload byte[0] carries the
 * frames-remaining-in-this-burst so the RX can self-describe.  If spans!=NULL,
 * record each burst's DATA-frame sample span (preamble/postamble excluded) so a
 * caller can corrupt one burst's data without touching its preamble.  Returns
 * #samples. */
static int build_stream_plan(struct freedv *tx, const int *plan, int nbursts,
                             short *pcm, int cap, burst_span_t *spans)
{
    int bpf = freedv_get_bits_per_modem_frame(tx) / 8;
    int payload = bpf - 2;                 /* last 2 bytes are CRC16 */
    uint8_t *bytes = malloc(bpf);
    int tx_max = freedv_get_n_tx_modem_samples(tx);
    short *scratch = malloc(sizeof(short) * tx_max);
    int silence = 2 * freedv_get_n_nom_modem_samples(tx);
    int n = 0;

    for (int b = 0; b < nbursts; b++)
    {
        int k = plan[b];
        int np = freedv_rawdatapreambletx(tx, scratch);
        memcpy(pcm + n, scratch, sizeof(short) * np); n += np;

        if (spans) spans[b].data_start = n;
        for (int f = 0; f < k; f++)
        {
            memset(bytes, 0x5a, payload);
            bytes[0] = (uint8_t)(k - 1 - f);        /* frames remaining after this one */
            uint16_t crc = freedv_gen_crc16(bytes, payload);
            bytes[bpf - 2] = crc >> 8; bytes[bpf - 1] = crc & 0xff;
            freedv_rawdatatx(tx, scratch, bytes);
            int ns = freedv_get_n_tx_modem_samples(tx);
            memcpy(pcm + n, scratch, sizeof(short) * ns); n += ns;
        }
        if (spans) spans[b].data_end = n;

        int nps = freedv_rawdatapostambletx(tx, scratch);
        memcpy(pcm + n, scratch, sizeof(short) * nps); n += nps;

        memset(pcm + n, 0, sizeof(short) * silence); n += silence;
        TEST_ASSERT_TRUE(n < cap);
    }
    free(bytes); free(scratch);
    return n;
}

static int build_stream(struct freedv *tx, short *pcm, int cap)
{
    return build_stream_plan(tx, g_plan, N_BURSTS, pcm, cap, NULL);
}

/* Feed the stream through an RX opened at BURST_CEILING frames/burst.
 * self_describe = call freedv_set_frames_remaining(byte[0]) after each decode. */
static int decode_stream(const short *pcm, int nsamp, bool self_describe)
{
    struct freedv *rx = freedv_open(FREEDV_MODE_DATAC3);
    TEST_ASSERT_NOT_NULL(rx);
    freedv_set_frames_per_burst(rx, BURST_CEILING);

    int bpf = freedv_get_bits_per_modem_frame(rx) / 8;
    uint8_t *bytes = malloc(bpf);
    int got = 0, pos = 0;
    int nin = freedv_nin(rx);

    while (pos + nin <= nsamp)
    {
        int nbytes = freedv_rawdatarx(rx, bytes, (short *)(pcm + pos));
        pos += nin;
        nin = freedv_nin(rx);
        if (nbytes > 0)
        {
            got++;
            if (self_describe)
                freedv_set_frames_remaining(rx, bytes[0]);
        }
    }
    free(bytes);
    freedv_close(rx);
    return got;
}

/* With self-describe, every frame of every burst decodes despite the varying
 * per-keydown frame counts. */
void test_selfdescribe_decodes_all_frames(void)
{
    struct freedv *tx = freedv_open(FREEDV_MODE_DATAC3);
    TEST_ASSERT_NOT_NULL(tx);
    freedv_set_frames_per_burst(tx, BURST_CEILING);

    int cap = 4 * 1024 * 1024;
    short *pcm = malloc(sizeof(short) * cap);
    int nsamp = build_stream(tx, pcm, cap);
    freedv_close(tx);

    int got = decode_stream(pcm, nsamp, /*self_describe=*/true);
    TEST_ASSERT_EQUAL_INT(total_frames(), got);   /* 15/15 */
    free(pcm);
}

/* Control: WITHOUT self-describe (fixed ceiling), the short-first plan loses
 * frames — a short burst overruns into the next keydown's preamble.  Proves the
 * mechanism is load-bearing, not cosmetic. */
void test_without_selfdescribe_loses_frames(void)
{
    struct freedv *tx = freedv_open(FREEDV_MODE_DATAC3);
    freedv_set_frames_per_burst(tx, BURST_CEILING);
    int cap = 4 * 1024 * 1024;
    short *pcm = malloc(sizeof(short) * cap);
    int nsamp = build_stream(tx, pcm, cap);
    freedv_close(tx);

    int got = decode_stream(pcm, nsamp, /*self_describe=*/false);
    TEST_ASSERT_LESS_THAN_INT(total_frames(), got);   /* strictly fewer than 15 */
    free(pcm);
}

/* Partial-burst pathology under LOSS (the real-modem K>1 tail stall).
 *
 * A SHORT burst whose frames ALL fail to decode never re-anchors
 * packetsperburst (no frame is decoded, so freedv_set_frames_remaining is never
 * called) — it stays at the ceiling.  The synced OFDM machine then over-runs by
 * (ceiling - actual_frames) frame-times, eating the FOLLOWING keydown's
 * preamble, so that burst is lost too.  On a real transfer this makes a
 * single-frame tail retransmit un-decodable forever (the 2024/2048 stall).
 *
 * Plan {5, 1, 3}: corrupt the 1-frame burst's data (preamble intact → sync is
 * acquired, then the lone data frame fails).  A correct RX loses only that 1
 * frame and still decodes the following 3-frame burst (got == 5 + 3 == 8).  The
 * over-run bug eats the 3-frame burst's preamble, so got collapses toward 5. */
static const int g_loss_plan[] = {5, 1, 3};
#define N_LOSS_BURSTS ((int)(sizeof(g_loss_plan) / sizeof(g_loss_plan[0])))
#define CORRUPT_BURST 1        /* the short (1-frame) burst */

void test_failed_short_burst_does_not_eat_next_preamble(void)
{
    struct freedv *tx = freedv_open(FREEDV_MODE_DATAC3);
    TEST_ASSERT_NOT_NULL(tx);
    freedv_set_frames_per_burst(tx, BURST_CEILING);

    int cap = 4 * 1024 * 1024;
    short *pcm = malloc(sizeof(short) * cap);
    burst_span_t spans[N_LOSS_BURSTS];
    int nsamp = build_stream_plan(tx, g_loss_plan, N_LOSS_BURSTS, pcm, cap, spans);
    freedv_close(tx);

    /* Corrupt the short burst's DATA frames (leave its preamble intact so the
     * RX still ACQUIRES the burst, then fails to decode its lone frame — the
     * exact real-channel condition that leaves packetsperburst un-re-anchored).
     * Deterministic mid-amplitude noise keeps energy up so sync is held. */
    uint32_t s = 0xC0FFEE;
    for (int i = spans[CORRUPT_BURST].data_start; i < spans[CORRUPT_BURST].data_end; i++)
    {
        s = s * 1103515245u + 12345u;
        pcm[i] = (short)((int)((s >> 16) & 0x3FFF) - 0x2000);   /* +/-8k noise */
    }

    int got = decode_stream(pcm, nsamp, /*self_describe=*/true);
    free(pcm);

    /* Only the corrupted 1-frame burst may be lost; bursts 0 (5) and 2 (3) must
     * both fully decode.  The over-run bug drops burst 2 as well (got ~= 5). */
    int expect = g_loss_plan[0] + g_loss_plan[2];   /* 8 */
    TEST_ASSERT_EQUAL_INT(expect, got);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_selfdescribe_decodes_all_frames);
    RUN_TEST(test_without_selfdescribe_loses_frames);
    RUN_TEST(test_failed_short_burst_does_not_eat_next_preamble);
    return UNITY_END();
}
