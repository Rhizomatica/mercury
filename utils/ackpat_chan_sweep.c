/* ackpat_chan_sweep — what margin does the pattern ACK have ON A FADING LINK?
 *
 * utils/ackpat_sweep already measures the pattern, but on flat AWGN and on its
 * own signal-RMS/noise-RMS axis.  Neither is comparable with anything else we
 * have: the modes in docs/MODES.md are quoted as SNR3k through
 * common/watterson.c, and the question that matters -- can the pattern carry
 * the in-session ACK where DATAC16 cannot? -- is a question about the reverse
 * path of a REAL link, which fades.
 *
 * So this runs the shipped pattern_ack_tx/pattern_ack_detect through
 * utils/chanutil, the same channel and the same measured-SNR3k axis
 * acquire_vs_decode uses for DATAC16.  The two tables can then be read on one
 * axis, which is the entire point.
 *
 * Three numbers per point, and they do not all pull the same way:
 *
 *   DETECT   a genuine ACK is seen.  Missing one costs a retransmission.
 *   FALSE    noise alone is accepted.  The sender believes a frame landed when
 *            it did not and moves on, leaving the receiver a hole -- worse
 *            than a miss, and the reason a deeper threshold is not free.
 *   CONFUSE  an ACK is read as ACK+TURN or vice versa.  The pattern channel
 *            carries exactly this one bit, so mistaking it is a protocol
 *            error, not a lost frame.
 *
 * FALSE is reported per burst-length window.  The live detector in modem.c
 * slides a 3-burst window and scores it every chunk, so the rate an operator
 * actually sees is this figure times the number of windows in the listen
 * interval -- a per-burst rate that looks tolerable can still be a false ACK
 * every few seconds.  See ARQ_CONNECT_CONFIRM_LISTEN_MS for how long that
 * window is held open.
 *
 * Trials are spaced by an idle gap so the sweep spans a comparable amount of
 * channel time to the DATAC16 sweep it is read against: the pattern burst is
 * 0.64 s against DATAC16's several seconds, and without the gap 50 pattern
 * trials would sample a few fade cycles where 50 DATAC16 trials sample tens,
 * making the pattern's estimate needlessly noisy at the same trial count.
 *
 * Self-check: at high SNR DETECT must be ~100% and FALSE ~0.  If not, the
 * harness is wrong before the design is.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../modem/pattern_ack.h"
#include "chanutil.h"

int main(int argc, char **argv)
{
    const int   trials  = (argc > 1) ? atoi(argv[1]) : 100;
    const float no_hi   = (argc > 2) ? (float)atof(argv[2]) : -4.0f;
    const float no_lo   = (argc > 3) ? (float)atof(argv[3]) : -22.0f;
    const float no_step = (argc > 4) ? (float)atof(argv[4]) : 2.0f;
    const int   chan    = (argc > 5) ? chanutil_preset_from_name(argv[5]) : CHAN_AWGN;

    if (chan < 0) {
        fprintf(stderr, "unknown channel '%s' (awgn|mpg|mpp|mpd)\n", argv[5]);
        return 1;
    }
    if (trials <= 0) {
        fprintf(stderr, "usage: %s [trials] [no_hi] [no_lo] [no_step] [channel]\n",
                argv[0]);
        return 1;
    }

    const int cap = pattern_ack_max_tx_samples();
    int16_t *ack   = malloc((size_t)cap * sizeof(int16_t));
    int16_t *brk   = malloc((size_t)cap * sizeof(int16_t));
    int16_t *work  = malloc((size_t)cap * sizeof(int16_t));
    if (!ack || !brk || !work) return 1;

    const int n_ack = pattern_ack_tx(ack, PATTERN_ACK);
    const int n_brk = pattern_ack_tx(brk, PATTERN_BREAK);
    if (n_ack <= 0 || n_brk <= 0) {
        fprintf(stderr, "pattern tx failed\n");
        return 1;
    }

    /* Peak and RMS of the pattern, printed because they are half the story a
     * bench cannot tell: a constant-envelope tone burst and an OFDM burst that
     * measure the same average power here do NOT come out of a real PA the
     * same, and the pattern is the one that wins that trade. */
    double p = 0; int pk = 0;
    for (int i = 0; i < n_ack; i++) {
        p += (double)ack[i] * ack[i];
        int a = ack[i] < 0 ? -ack[i] : ack[i];
        if (a > pk) pk = a;
    }
    const double rms = sqrt(p / n_ack);

    printf("channel: %s\n", chanutil_preset_name(chan));
    printf("pattern: %d symbols, %d samples, %.0f ms at 8 kHz\n",
           pattern_ack_nsymb(), n_ack, 1000.0 * n_ack / 8000.0);
    printf("pattern level: rms %.0f, peak %d, crest %.1f dB\n",
           rms, pk, 20.0 * log10(pk / rms));
    printf("%d trials per point\n\n", trials);
    printf("   No     SNR3k     DETECT      FALSE      CONFUSE\n");

    for (float no = no_hi; no >= no_lo - 1e-6f; no -= no_step) {
        chanutil_t *c = chanutil_open(chan, no, 1234u);
        if (!c) { fprintf(stderr, "chanutil_open failed\n"); return 1; }

        int hit = 0, fa = 0, confuse = 0;
        double snr_sum = 0; int snr_n = 0;

        for (int t = 0; t < trials; t++) {
            float meas = 0;
            int   isb  = 0;

            /* Alternate ACK and ACK+TURN so both tables are exercised and the
             * discrimination bit is measured, not assumed. */
            const int want_break = (t & 1);
            const int16_t *src   = want_break ? brk : ack;
            const int      n     = want_break ? n_brk : n_ack;

            memcpy(work, src, (size_t)n * sizeof(int16_t));
            chanutil_run(c, work, n, &meas);
            snr_sum += meas; snr_n++;
            if (pattern_ack_detect(work, n, &isb)) {
                hit++;
                if ((isb != 0) != (want_break != 0)) confuse++;
            }

            /* Same channel, same instant in its evolution, no signal. */
            memset(work, 0, (size_t)n * sizeof(int16_t));
            chanutil_run(c, work, n, NULL);
            if (pattern_ack_detect(work, n, &isb)) fa++;

            /* Let the fading process move on between trials. */
            chanutil_advance(c, 4 * 8000);
        }

        printf("  %+5.1f  %+6.2f   %3d/%-3d    %3d/%-3d    %3d/%-3d\n",
               no, snr_n ? (float)(snr_sum / snr_n) : 0.0f,
               hit, trials, fa, trials, confuse, trials);
        fflush(stdout);
        chanutil_close(c);
    }

    free(ack); free(brk); free(work);
    return 0;
}
