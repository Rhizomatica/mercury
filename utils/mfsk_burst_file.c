/* utils/mfsk_burst_file.c -- an MFSK burst through a channel tool, by file.
 *
 *   mfsk_burst_file tx <mode> <seed> <burst.raw> <padded.raw>
 *   mfsk_burst_file rx <mode> <seed> <received.raw>        (prints 1 or 0)
 *   mfsk_burst_file bpf <lo_hz> <hi_hz> <in.raw> <out.raw>
 *
 * tx writes one CRC-valid frame (payload from <seed>) as 8 kHz int16: the
 * burst alone (for the channel tool to measure its SNR) and with 0.5 s of
 * silence before and 4 s after (to decode).  rx decodes a received file with
 * the real MFSK backend and says whether that frame came back.  <mode> is a
 * Mercury MFSK mode number: 100 (the floor), 102 or 103 (the 500 Hz
 * geometries).  bpf is a receiver's narrow filter: a windowed-sinc band-pass,
 * -6 dB at lo and hi, about 150 Hz from there to the stop band.
 * utils/mfsk_channel_sweep.sh drives the three through codec2's ch or the
 * Watterson model.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "modem_backend.h"
#include "modem_mfsk.h"
#include "freedv_api.h"
#include <math.h>
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

#define BPF_TAPS 201

static int bpf(double lo, double hi, const char *in, const char *out)
{
    double h[BPF_TAPS];
    for (int i = 0; i < BPF_TAPS; i++) {
        int k = i - BPF_TAPS / 2;
        double a = 2.0 * M_PI * hi / 8000.0, b = 2.0 * M_PI * lo / 8000.0;
        double v = k == 0 ? (a - b) / M_PI : (sin(a * k) - sin(b * k)) / (M_PI * k);
        h[i] = v * (0.54 - 0.46 * cos(2.0 * M_PI * i / (BPF_TAPS - 1)));
    }
    FILE *f = fopen(in, "rb"), *g = fopen(out, "wb");
    if (!f || !g) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f) / 2;
    rewind(f);
    int16_t *x = malloc((size_t)n * 2), *y = malloc((size_t)n * 2);
    if (!x || !y || fread(x, 2, (size_t)n, f) != (size_t)n) return 2;
    for (long i = 0; i < n; i++) {
        double a = 0.0;
        for (int k = 0; k < BPF_TAPS; k++) {
            long j = i - k + BPF_TAPS / 2;
            if (j >= 0 && j < n) a += h[k] * x[j];
        }
        y[i] = (int16_t)lrint(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
    }
    fwrite(y, 2, (size_t)n, g);
    fclose(f); fclose(g);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 6 && !strcmp(argv[1], "bpf"))
        return bpf(atof(argv[2]), atof(argv[3]), argv[4], argv[5]);
    if (argc < 5 || (strcmp(argv[1], "tx") && strcmp(argv[1], "rx")) || (!strcmp(argv[1], "tx") && argc < 6)) {
        fprintf(stderr, "usage: %s tx <mode> <seed> <burst.raw> <padded.raw> | rx <mode> <seed> <received.raw>"
                        " | bpf <lo> <hi> <in.raw> <out.raw>\n", argv[0]);
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
