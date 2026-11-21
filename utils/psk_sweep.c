/* psk_sweep -- delivery of a modem/psk level against the S/N of a steady
 * carrier at the transmitter's peak, as rung_sweep does for the OFDM modes:
 *
 *   psk_sweep <level> <trials> <lo> <hi> <step> [channel] [seed]
 *
 * level: psk3, psk6, psk8.  Each trial puts one frame between random
 * stretches of silence (so the receiver must find it, not just decode it)
 * and through AWGN or a chanutil preset (ccir-moderate, ccir-poor, ...).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_common.h"
#include "chanutil.h"
#include "../modem/psk/mpsk.h"

#define FS 8000.0
#define COMMON_PEAK 4000.0
#define TONE_POWER (COMMON_PEAK * COMMON_PEAK / 2.0)

static bench_rng rng;

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: %s <psk3|psk6|psk8> <trials> <lo> <hi> <step> [channel] [seed]\n"
                        "  the axis is the S/N in 3 kHz of a steady carrier at the transmitter's peak\n", argv[0]);
        return 1;
    }
    int lv = -1;
    for (int i = 0; i < mpsk_nlevels; i++) if (!strcmp(argv[1], mpsk_levels[i].name)) lv = i;
    int trials = atoi(argv[2]);
    double lo = atof(argv[3]), hi = atof(argv[4]), step = atof(argv[5]);
    const char *channame = argc > 6 ? argv[6] : "awgn";
    unsigned long long seed = argc > 7 ? strtoull(argv[7], NULL, 10) : 1357911131517ULL;
    int chan = CHAN_AWGN;
    if (strcmp(channame, "awgn") && (chan = chanutil_preset_from_name(channame)) < 0) {
        fprintf(stderr, "unknown channel '%s'\n", channame);
        return 1;
    }
    mpsk_t *tx = lv >= 0 ? mpsk_open(lv) : NULL, *rx = lv >= 0 ? mpsk_open(lv) : NULL;
    if (!tx || !rx) { fprintf(stderr, "cannot open level '%s'\n", argv[1]); return 1; }

    int nb = mpsk_frame_bytes(tx), nf = mpsk_tx_samples(tx);
    int pad = (int)FS;                                 /* up to 1 s each side */
    int nbuf = nf + 2 * pad;
    double secs = (double)(nf - 2 * MPSK_SPS) / FS;    /* the frame's airtime */
    double rate = (nb - 2) * 8.0 / secs;
    int16_t *burst = malloc(sizeof(int16_t) * (size_t)nf), *buf = malloc(sizeof(int16_t) * (size_t)nbuf);
    uint8_t *pay = malloc((size_t)nb), *got = malloc((size_t)nb);

    /* How far under the carrier the level goes out, and for fading the
     * channel's level constant, as rung_sweep measures them. */
    bench_rng_seed(&rng, seed);
    double under = 0.0, a_tone = 0.0;
    for (int i = 0; i < 8; i++) {
        for (int b = 0; b < nb; b++) pay[b] = (uint8_t)(bench_rng_uniform(&rng) * 256.0);
        int n = mpsk_modulate(tx, pay, burst, COMMON_PEAK);
        double p = 0.0;
        for (int j = 0; j < n; j++) p += (double)burst[j] * burst[j];
        p /= n;
        double u = 10.0 * log10(TONE_POWER / p);
        under += u / 8.0;
        if (chan != CHAN_AWGN) {
            chanutil_t *ck = chanutil_open(CHAN_AWGN, -30.0f, 1234u);
            float m0 = 0.0f;
            chanutil_run(ck, burst, n, &m0);
            chanutil_close(ck);
            a_tone += ((double)m0 - 30.0 + u) / 8.0;
        }
    }
    printf("%s: %d B payload, frame %.2f s, %.0f bit/s, %.1f dB under the carrier, channel %s, %d trials\n",
           argv[1], nb - 2, secs, rate, under, chan == CHAN_AWGN ? "awgn" : chanutil_preset_name(chan), trials);
    printf("  carrier S/N   delivered   acquired   raw est.   goodput\n");

    for (double s = lo; s <= hi + 0.001; s += step) {
        bench_rng_seed(&rng, seed + 1);
        chanutil_t *ch = NULL;
        if (chan != CHAN_AWGN && !(ch = chanutil_open(chan, (float)(a_tone - s), 1234u))) return 1;
        int del = 0, acq = 0, nest = 0;
        double est = 0.0;
        for (int t = 0; t < trials; t++) {
            for (int b = 0; b < nb; b++) pay[b] = (uint8_t)(bench_rng_uniform(&rng) * 256.0);
            int n = mpsk_modulate(tx, pay, burst, COMMON_PEAK);
            int off = (int)(bench_rng_uniform(&rng) * pad);
            if (getenv("OFFSTEP")) off -= off % atoi(getenv("OFFSTEP"));
            memset(buf, 0, sizeof(int16_t) * (size_t)nbuf);
            memcpy(buf + off, burst, sizeof(int16_t) * (size_t)n);
            double sigma;
            if (chan == CHAN_AWGN) {
                sigma = sqrt(TONE_POWER / (pow(10.0, s / 10.0) * (3000.0 / (FS / 2.0))));
            } else {
                float mm = 0.0f;
                chanutil_run(ch, buf + off, n, &mm);
                double p = 0.0;
                for (int j = 0; j < n; j++) p += 0;      /* the channel added the noise inside the frame */
                (void)p;
                sigma = sqrt(TONE_POWER / (pow(10.0, s / 10.0) * (3000.0 / (FS / 2.0))));
                /* noise outside the frame too, so the search sees noise there */
                for (int j = 0; j < nbuf; j++)
                    if (j < off || j >= off + n) {
                        double v = buf[j] + sigma * bench_rng_gauss(&rng);
                        buf[j] = (int16_t)lrint(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                    }
                sigma = 0.0;
            }
            if (sigma > 0.0)
                for (int j = 0; j < nbuf; j++) {
                    double v = buf[j] + sigma * bench_rng_gauss(&rng);
                    buf[j] = (int16_t)lrint(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
            int found = 0;
            float snr = 0.0f;
            int ok = mpsk_demod(rx, buf, nbuf, got, &found, &snr);
            ok = ok && !memcmp(got, pay, (size_t)nb);
            if (getenv("PSKDBG") && found) {
                static uint8_t cw[8192];
                int N = mpsk_debug_codeword(tx, pay, cw);
                const float *L = mpsk_debug_llr(rx);
                int e = 0; double ma = 0;
                for (int i = 0; i < N; i++) { if ((L[i] < 0) != (cw[i] != 0)) e++; ma += fabs(L[i]); }
                char em[256];
                mpsk_debug_errmap(rx, cw, em, sizeof em);
                double chi = 0;
                int dt = mpsk_debug_lock(rx, off, &chi);
                fprintf(stderr, "snr %+.0f trial %d ok %d rawBER %.4f mean|llr| %.2f est %.1f lock %+d chi %.0f %s\n", s, t, ok, (double)e / N, ma / N, snr, dt, chi, em);
            }
            acq += found;
            del += ok;
            if (found) { est += snr; nest++; }
        }
        if (ch) chanutil_close(ch);
        printf("  %+7.1f      %3d/%-3d     %3d/%-3d    %6.1f   %6.0f\n", s, del, trials, acq, trials,
               nest ? est / nest : 0.0, rate * del / trials);
        fflush(stdout);
    }
    mpsk_close(tx);
    mpsk_close(rx);
    return 0;
}
