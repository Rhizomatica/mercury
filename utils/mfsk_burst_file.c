/* utils/mfsk_burst_file.c -- an MFSK burst through a channel tool, by file.
 *
 *   mfsk_burst_file tx <mode> <seed> <burst.raw> <padded.raw>
 *   mfsk_burst_file rx <mode> <seed> <received.raw>        (prints 1 or 0)
 *
 * tx writes one CRC-valid 100-byte frame (payload from <seed>) as 8 kHz int16:
 * the burst alone (for the channel tool to measure its SNR) and with 0.5 s of
 * silence before and 4 s after (to decode).  rx decodes a received file with
 * the real MFSK backend and says whether that frame came back.  <mode> is a
 * Mercury mode number: 100 (MFSK) or 101 (MFSK16).  utils/mfsk_channel_sweep.sh
 * drives it through codec2's ch or the Watterson model.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "modem_backend.h"
#include "modem_mfsk.h"
#include "freedv_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void make_frame(uint8_t *f, int n, int seed)
{
    srand((unsigned)seed);
    for (int i = 0; i < n - 2; i++) f[i] = (uint8_t)(rand() & 255);
    uint16_t c = freedv_gen_crc16(f, n - 2);
    f[n - 2] = (uint8_t)(c >> 8); f[n - 1] = (uint8_t)(c & 0xff);
}

int main(int argc, char **argv)
{
    if (argc < 5 || (strcmp(argv[1], "tx") && strcmp(argv[1], "rx")) || (!strcmp(argv[1], "tx") && argc < 6)) {
        fprintf(stderr, "usage: %s tx <mode> <seed> <burst.raw> <padded.raw> | rx <mode> <seed> <received.raw>\n", argv[0]);
        return 2;
    }
    const modem_backend_t *be = &modem_backend_mfsk;
    void *h = be->open(atoi(argv[2]));
    if (!h) { fprintf(stderr, "cannot open mode %s\n", argv[2]); return 2; }
    be->configure(h, 1, 0);
    int fb = be->bits_per_frame(h) / 8;
    uint8_t frame[256];
    make_frame(frame, fb, atoi(argv[3]));
    if (!strcmp(argv[1], "tx")) {
        int16_t *a = calloc(8000 * 60, sizeof(int16_t)), *z = calloc(8000 * 4, sizeof(int16_t));
        int n = 0;
        n += be->preamble_tx(h, a + n);
        n += be->rawdata_tx(h, a + n, frame);
        n += be->postamble_tx(h, a + n);
        FILE *f = fopen(argv[4], "wb");
        if (!f) return 2;
        fwrite(a, sizeof(int16_t), (size_t)n, f); fclose(f);
        if (!(f = fopen(argv[5], "wb"))) return 2;
        fwrite(z, sizeof(int16_t), 4000, f); fwrite(a, sizeof(int16_t), (size_t)n, f);
        fwrite(z, sizeof(int16_t), 8000 * 4, f); fclose(f);
        return 0;
    }
    FILE *f = fopen(argv[4], "rb");
    if (!f) return 2;
    int16_t buf[4096];
    uint8_t out[256];
    int got = 0;
    for (;;) {
        int k = be->nin(h);
        if (fread(buf, sizeof(int16_t), (size_t)k, f) != (size_t)k) break;
        if (be->rawdata_rx(h, out, buf) > 0 && !memcmp(out, frame, (size_t)fb)) { got = 1; break; }
    }
    fclose(f);
    printf("%d\n", got);
    return 0;
}
