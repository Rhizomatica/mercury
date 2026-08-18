/* ackpat_sweep — how much margin does the pattern ACK actually have?
 *
 * The in-session ACK is a Welch-Costas tone burst occupying 640 ms of a 1.05 s
 * keydown, and that keydown is the largest single component of the 2.47 s
 * turnaround every data frame pays (docs/ARQ-FRAME-SIZING.md).  For the 32-MFSK
 * geometry it is an 8-tone array sent TWICE, accepted at 8 matched symbols, so
 * the obvious saving is to send it once: ~13% off the turnaround, ~4% end to
 * end.
 *
 * Whether that is affordable depends on how much margin the pattern has, and
 * margin here is not spare capacity.  The ACK is the reverse path: a link that
 * carries data forward but loses ACKs stalls exactly as dead as one that
 * carries nothing, and this project has already been bitten by that asymmetry.
 * So measure the margin before spending it.
 *
 * Two failure modes, and they pull opposite ways:
 *
 *   MISS   the sender does not see a genuine ACK.  Costs a retransmission and
 *          a step down the ladder: expensive, recoverable.
 *   FALSE  noise is accepted as an ACK.  The sender believes a frame landed
 *          when it did not and moves on, leaving the receiver a hole.  Worse.
 *
 * Drives the shipped mfsk_pattern_tx/mfsk_pattern_detect, not a reimplementation,
 * so what it reports is what the modem does.
 *
 * Self-check: at high SNR detection must be ~100%, and noise alone must almost
 * never be accepted.  If either fails the harness is wrong before the design is.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../modem/modem_mfsk.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double rng_gauss(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0);
    double u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

int main(int argc, char **argv)
{
    const int trials = (argc > 1) ? atoi(argv[1]) : 300;
    const int nsym   = mfsk_pattern_nsymb();
    const int cap    = mfsk_pattern_max_tx_samples();

    int16_t *clean = malloc((size_t)cap * sizeof(int16_t));
    int16_t *buf   = malloc((size_t)cap * sizeof(int16_t));
    if (!clean || !buf) return 1;

    int n = mfsk_pattern_tx(clean, 0);        /* 0 = plain ACK */
    if (n <= 0) { fprintf(stderr, "pattern tx failed\n"); return 1; }

    double p = 0;
    for (int i = 0; i < n; i++) p += (double)clean[i] * clean[i];
    const double srms = sqrt(p / n);

    printf("shipped ACK pattern: %d symbols, %d samples, %.0f ms at 8 kHz\n",
           nsym, n, 1000.0 * n / 8000.0);
    printf("trials per point: %d\n\n", trials);
    printf("   SNR    detect   false-accept   detect(one repetition)\n");
    printf("                                   [2nd half replaced by noise:\n");
    printf("                                    a PESSIMISTIC bound, since it\n");
    printf("                                    still needs 8 of 8 to match]\n");

    for (double snr = 6; snr >= -30; snr -= 2) {
        const double nrms = srms / pow(10.0, snr / 20.0);
        int hit = 0, fa = 0, half = 0;
        srand(20260818);
        for (int t = 0; t < trials; t++) {
            for (int i = 0; i < n; i++) {
                double s = clean[i] + nrms * rng_gauss();
                if (s >  32767.0) s =  32767.0;
                if (s < -32768.0) s = -32768.0;
                buf[i] = (int16_t)lrint(s);
            }
            int isb = 0;
            if (mfsk_pattern_detect(buf, n, &isb)) hit++;

            for (int i = 0; i < n; i++) {
                double s = nrms * rng_gauss();
                if (s >  32767.0) s =  32767.0;
                if (s < -32768.0) s = -32768.0;
                buf[i] = (int16_t)lrint(s);
            }
            if (mfsk_pattern_detect(buf, n, &isb)) fa++;

            /* One repetition: signal in the first half only.  The detector
             * still scores 16 symbols at threshold 8, so every one of the 8
             * signal symbols must match -- stricter than a purpose-built
             * 8-symbol pattern would be, hence a lower bound on how well one
             * repetition could do. */
            for (int i = 0; i < n; i++) {
                double sig = (i < n / 2) ? clean[i] : 0.0;
                double s = sig + nrms * rng_gauss();
                if (s >  32767.0) s =  32767.0;
                if (s < -32768.0) s = -32768.0;
                buf[i] = (int16_t)lrint(s);
            }
            if (mfsk_pattern_detect(buf, n, &isb)) half++;
        }
        printf("  %+5.1f  %6.1f%%   %6.1f%%        %6.1f%%\n",
               snr, 100.0 * hit / trials, 100.0 * fa / trials,
               100.0 * half / trials);
    }
    free(clean); free(buf);
    return 0;
}
