/* mfsk_floor_sweep — is the MFSK floor limited by acquisition or by the code?
 *
 * The MFSK rung carries 100 bytes in a ~13.5 s keydown, and a fringe link
 * lives there, so that cycle sets the throughput floor.  Shortening the burst
 * by puncturing parity would raise it (utils/mfsk_puncture_sweep measures the
 * coding cost: 13% shorter for about 1 dB), but only if the code HAS a dB to
 * spare over acquisition.
 *
 * docs/MFSK-PORT.md established the qualitative answer -- `delivered ==
 * acquired` at every code rate -- and that is exactly why the quantity is
 * missing: when decode never fails first, a plain sweep cannot say by how much
 * it wins.
 *
 * So this sweeps twice:
 *
 *   BOTH   noise over the whole burst.  Acquisition and decode both exposed;
 *          this is the operating floor and should reproduce MFSK-PORT.
 *   DATA   noise over the payload only, preamble and postamble left clean.
 *          Acquisition then always succeeds, so the frame errors that remain
 *          are the code's own -- the decode-only floor.
 *
 * The gap between them is the slack, in dB, and that is the budget any
 * puncturing scheme has to spend from.
 *
 * Self-check: at high SNR both columns must deliver everything, and DATA must
 * never be worse than BOTH at the same SNR.  If either fails the harness is
 * wrong before the design is.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../modem/modem_backend.h"
#include "../modem/modem_mfsk.h"
#include "chanutil.h"
#include "freedv_api.h"

static int tx_burst(const modem_backend_t *be, void *ctx,
                    const uint8_t *frame, int16_t *out, int *data_start,
                    int *data_end)
{
    int n = 0;
    n += be->preamble_tx(ctx, out + n);
    *data_start = n;
    n += be->rawdata_tx(ctx, out + n, frame);
    *data_end = n;
    n += be->postamble_tx(ctx, out + n);
    return n;
}

/* Feed a burst to the demod; returns 1 if a frame came out. */
static int rx_burst(const modem_backend_t *be, void *ctx,
                    const int16_t *pb, int n, uint8_t *bytes, int nbytes)
{
    int pos = 0, got = 0;
    while (pos < n) {
        int want = be->nin(ctx);
        if (want <= 0) break;
        if (pos + want > n) break;
        int r = be->rawdata_rx(ctx, bytes, pb + pos);
        pos += want;
        if (r >= nbytes) { got = 1; break; }
    }
    return got;
}

int main(int argc, char **argv)
{
    const int trials = (argc > 1) ? atoi(argv[1]) : 20;
    const modem_backend_t *be = &modem_backend_mfsk;

    void *tx = be->open(MERCURY_MODE_MFSK);
    void *rx = be->open(MERCURY_MODE_MFSK);
    if (!tx || !rx) { fprintf(stderr, "backend open failed\n"); return 1; }
    be->configure(tx, 1, 0);
    be->configure(rx, 1, 0);

    const int nbytes = be->bits_per_frame(tx) / 8;
    const int cap    = be->n_tx_samples(tx) + 8192;

    uint8_t *frame = malloc((size_t)nbytes);
    uint8_t *out   = malloc((size_t)nbytes + 16);
    int16_t *clean = malloc((size_t)cap * sizeof(int16_t));
    int16_t *work  = malloc((size_t)cap * sizeof(int16_t));
    if (!frame || !out || !clean || !work) return 1;
    /* The demod returns bytes only on a valid CRC16, and the frame contract
     * puts that CRC in the last two bytes -- the TX path in modem.c appends it,
     * so an instrument that does not will decode perfectly and report nothing. */
    for (int i = 0; i < nbytes - 2; i++) frame[i] = (uint8_t)(i * 37 + 11);
    {
        uint16_t crc = freedv_gen_crc16(frame, nbytes - 2);
        frame[nbytes - 2] = (uint8_t)(crc >> 8);
        frame[nbytes - 1] = (uint8_t)(crc & 0xff);
    }

    int ds = 0, de = 0;
    int n = tx_burst(be, tx, frame, clean, &ds, &de);
    /* Trailing silence: the RX slides a window, so the last symbols only leave
     * it once more samples have arrived.  Without this the burst is never
     * completed and nothing is ever delivered. */
    const int burst_n = n;
    memset(clean + n, 0, (size_t)(cap - n) * sizeof(int16_t));
    n = cap;

    printf("MFSK burst: %d samples (%.2f s), payload %d B, data span [%d,%d)\n",
           burst_n, burst_n / 8000.0, nbytes, ds, de);
    printf("%d trials per point.  BOTH = noise over the whole burst (the\n"
           "operating floor); DATA = payload only, preamble kept clean, which\n"
           "isolates the code.\n\n", trials);
    printf("   No     SNR3k     BOTH      DATA-only\n");

    const float no_hi   = (argc > 2) ? (float)atof(argv[2]) : -6.0f;
    const float no_lo   = (argc > 3) ? (float)atof(argv[3]) : -24.0f;
    const float no_step = (argc > 4) ? (float)atof(argv[4]) : 1.0f;
    for (float no = no_hi; no >= no_lo - 1e-6f; no -= no_step) {
        int ok_both = 0, ok_data = 0;
        float snr3k = 0;

        for (int t = 0; t < trials; t++) {
            /* whole burst through the channel */
            memcpy(work, clean, (size_t)n * sizeof(int16_t));
            chanutil_fade(work, n, 0, no, (unsigned)(1000 + t), &snr3k);
            if (rx_burst(be, rx, work, n, out, nbytes) &&
                memcmp(out, frame, (size_t)nbytes) == 0) ok_both++;

            /* payload only: preamble and postamble stay clean */
            memcpy(work, clean, (size_t)n * sizeof(int16_t));
            chanutil_fade(work + ds, de - ds, 0, no, (unsigned)(2000 + t), NULL);
            if (rx_burst(be, rx, work, n, out, nbytes) &&
                memcmp(out, frame, (size_t)nbytes) == 0) ok_data++;
        }
        printf("  %+5.1f  %+6.2f   %3d/%-3d    %3d/%-3d\n",
               no, snr3k, ok_both, trials, ok_data, trials);
    }

    free(frame); free(out); free(clean); free(work);
    be->close(tx); be->close(rx);
    return 0;
}
