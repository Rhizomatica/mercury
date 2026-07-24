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

/* Modulate the whole plan into pcm[]; each frame's payload byte[0] carries the
 * frames-remaining-in-this-burst so the RX can self-describe. Returns #samples. */
static int build_stream(struct freedv *tx, short *pcm, int cap)
{
    int bpf = freedv_get_bits_per_modem_frame(tx) / 8;
    int payload = bpf - 2;                 /* last 2 bytes are CRC16 */
    uint8_t *bytes = malloc(bpf);
    /* n_tx_modem_samples is a whole data frame (~3.2 s for DATAC3); preamble
     * and postamble are shorter, so this sizes the TX scratch for all three. */
    int tx_max = freedv_get_n_tx_modem_samples(tx);
    short *scratch = malloc(sizeof(short) * tx_max);
    int silence = 2 * freedv_get_n_nom_modem_samples(tx);
    int n = 0;

    for (int b = 0; b < N_BURSTS; b++)
    {
        int k = g_plan[b];
        int np = freedv_rawdatapreambletx(tx, scratch);
        memcpy(pcm + n, scratch, sizeof(short) * np); n += np;

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

        int nps = freedv_rawdatapostambletx(tx, scratch);
        memcpy(pcm + n, scratch, sizeof(short) * nps); n += nps;

        memset(pcm + n, 0, sizeof(short) * silence); n += silence;
        TEST_ASSERT_TRUE(n < cap);
    }
    free(bytes); free(scratch);
    return n;
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

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_selfdescribe_decodes_all_frames);
    RUN_TEST(test_without_selfdescribe_loses_frames);
    return UNITY_END();
}
