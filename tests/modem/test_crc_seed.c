/*
 * Seeded data-frame CRC: a frame carries its session in the CRC16
 *
 * The carousel ARQ puts no session id in its frames.  The sender XORs the
 * frame CRC with a per-session seed instead, and the receiver checks against
 * its seed: another session's frames -- or plain ones, when not admitted --
 * fail like a bad frame and are never delivered.  The control decoder admits
 * plain frames too (CALL, ACCEPT, broadcast) and reports which kind it got.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "freedv_api.h"

void setUp(void) {}
void tearDown(void) {}

/* One clean burst of `mode` carrying a CRC XORed with tx_seed, into a
 * receiver with rx_seed / accept_plain.  Returns 1 plain, 2 seeded, 0 lost. */
static int roundtrip(int mode, uint16_t tx_seed, uint16_t rx_seed, int accept_plain)
{
    struct freedv *tx = freedv_open(mode), *rx = freedv_open(mode);
    freedv_set_frames_per_burst(tx, 1);
    freedv_set_frames_per_burst(rx, 1);
    freedv_set_crc_seed(rx, rx_seed, accept_plain);

    int nbytes = freedv_get_bits_per_modem_frame(tx) / 8;
    uint8_t *p = malloc((size_t)nbytes);
    for (int i = 0; i < nbytes - 2; i++) p[i] = (uint8_t)(i * 37 + 11);
    uint16_t crc = freedv_gen_crc16(p, nbytes - 2) ^ tx_seed;
    p[nbytes - 2] = (uint8_t)(crc >> 8);
    p[nbytes - 1] = (uint8_t)crc;

    int lead = 8000, cap = lead + freedv_get_n_tx_preamble_modem_samples(tx) +
               freedv_get_n_tx_modem_samples(tx) + freedv_get_n_tx_postamble_modem_samples(tx) + 16000;
    short *a = calloc((size_t)cap, sizeof(short));
    int n = lead;
    n += freedv_rawdatapreambletx(tx, a + n);
    freedv_rawdatatx(tx, a + n, p);
    n += freedv_get_n_tx_modem_samples(tx);
    n += freedv_rawdatapostambletx(tx, a + n);

    int got = 0, pos = 0, idle = 0;
    uint8_t out[4096];
    while (pos < cap && !got) {
        int nin = freedv_nin(rx);
        if (nin < 0 || pos + nin > cap || (nin == 0 && ++idle > 64)) break;
        if (nin > 0) idle = 0;
        size_t nb = freedv_rawdatarx(rx, out, a + pos);
        pos += nin;
        if (nb > 0 && !(freedv_get_rx_status(rx) & FREEDV_RX_BIT_ERRORS)) {
            TEST_ASSERT_EQUAL_MEMORY(p, out, nbytes - 2);
            got = freedv_get_rx_crc_seeded(rx) ? 2 : 1;
        }
    }
    free(a); free(p);
    freedv_close(tx); freedv_close(rx);
    return got;
}

static void test_plain_frames_as_before(void)
{
    TEST_ASSERT_EQUAL_INT(1, roundtrip(FREEDV_MODE_DATAC16, 0, 0, 0));
}

static void test_seeded_frame_needs_the_seed(void)
{
    TEST_ASSERT_EQUAL_INT(2, roundtrip(FREEDV_MODE_DATAC3, 0xBEEF, 0xBEEF, 0));
    TEST_ASSERT_EQUAL_INT(0, roundtrip(FREEDV_MODE_DATAC3, 0xBEEF, 0, 0));      /* plain receiver */
    TEST_ASSERT_EQUAL_INT(0, roundtrip(FREEDV_MODE_DATAC3, 0xBEEF, 0x1234, 0)); /* another session */
}

static void test_seeded_receiver_drops_plain_unless_admitted(void)
{
    TEST_ASSERT_EQUAL_INT(0, roundtrip(FREEDV_MODE_DATAC16, 0, 0xBEEF, 0));
    TEST_ASSERT_EQUAL_INT(1, roundtrip(FREEDV_MODE_DATAC16, 0, 0xBEEF, 1));
    TEST_ASSERT_EQUAL_INT(2, roundtrip(FREEDV_MODE_DATAC16, 0xBEEF, 0xBEEF, 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_plain_frames_as_before);
    RUN_TEST(test_seeded_frame_needs_the_seed);
    RUN_TEST(test_seeded_receiver_drops_plain_unless_admitted);
    return UNITY_END();
}
