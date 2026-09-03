/* mfsk_puncture_sweep — can the MFSK floor say the same thing in less airtime?
 *
 * The MFSK rung carries 100 bytes in a 13.5 s keydown, and on a fringe link the
 * session lives there, so its cycle time sets the throughput floor.  All five
 * ported v1 codes are N=1600 (docs/MFSK-PORT.md), so the rate ladder trades
 * payload, never airtime -- and that document also measured that every rate
 * delivers identically down to the acquisition floor: `delivered == acquired`.
 * The code is not the limit; acquisition is.
 *
 * If the code has slack, the useful way to spend it is a SHORTER burst rather
 * than a bigger payload.  Puncturing parity bits from the existing, verified
 * 8/16 code does exactly that: same 100 bytes, fewer transmitted symbols, no
 * new matrix to design or trust.  It also cuts exposure to the residual
 * frequency offset and timing drift that MFSK-PORT flags as the untested risk
 * over a ~13 s frame -- a shorter frame is less smeared, not more.
 *
 * What this measures is only the CODING cost: frame error rate against Es/N0
 * for several puncture depths, with punctured positions handed to the decoder
 * as zero LLR (no information), which is what a receiver that never saw them
 * has.  It deliberately says nothing about acquisition; the floor is set there,
 * and that has to be measured with the whole modem afterwards.
 *
 * Self-check: at high Es/N0 every depth must decode everything, and the
 * unpunctured column must be the best of them at every point.  If either fails
 * the harness is wrong before the design is.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../modem/mfsk_ldpc.h"

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
    const int trials = (argc > 1) ? atoi(argv[1]) : 200;
    const mfsk_ldpc_code_t *c = &mfsk_ldpc_8_16;
    const int K = c->K, N = c->N, P = N - K;

    /* Puncture every stride-th parity bit; stride 0 = none.  Spreading them
     * evenly matters: the parity part is an accumulator chain, so removing a
     * contiguous run would cut it rather than thin it. */
    const int strides[] = { 0, 8, 6, 4, 3, 2 };
    const int nvar = (int)(sizeof(strides)/sizeof(strides[0]));

    int  *info  = malloc((size_t)K * sizeof(int));
    int  *coded = malloc((size_t)N * sizeof(int));
    int  *out   = malloc((size_t)K * sizeof(int));
    float *llr  = malloc((size_t)N * sizeof(float));

    printf("code %s: N=%d K=%d (%d parity), %d trials/point\n",
           c->name ? c->name : "8/16", N, K, P, trials);
    printf("32-MFSK carries 5 bits/symbol at 25 symbols/s => %.2f s of data per frame\n\n",
           N / 5.0 / 25.0);

    printf("                 ");
    for (int v = 0; v < nvar; v++) {
        int punc = strides[v] ? P / strides[v] : 0;
        printf("  1/%-2s ", strides[v] ? (char[]){'0'+strides[v],0} : "-");
        (void)punc;
    }
    printf("\n  Es/N0   FER:");
    for (int v = 0; v < nvar; v++) {
        int punc = strides[v] ? P / strides[v] : 0;
        int tx = N - punc;
        printf(" %4.1fs", tx / 5.0 / 25.0);
    }
    printf("   <- data airtime\n");

    for (double esn0 = 7.0; esn0 >= 1.0; esn0 -= 0.25) {
        double sigma = pow(10.0, -esn0 / 20.0);
        printf("  %+5.1f dB   ", esn0);
        for (int v = 0; v < nvar; v++) {
            int stride = strides[v];
            int errs = 0;
            srand(4242);
            for (int t = 0; t < trials; t++) {
                for (int i = 0; i < K; i++) info[i] = rand() & 1;
                mfsk_ldpc_encode(c, info, coded);
                for (int i = 0; i < N; i++) {
                    /* BPSK: bit 0 -> +1.  LLR convention: >0 favours bit 0. */
                    double x = coded[i] ? -1.0 : +1.0;
                    double y = x + sigma * rng_gauss();
                    llr[i] = (float)(2.0 * y / (sigma * sigma));
                }
                if (stride) {
                    for (int i = K; i < N; i++)
                        if (((i - K) % stride) == 0) llr[i] = 0.0f;  /* never sent */
                }
                mfsk_ldpc_decode(c, llr, out, 30);
                if (memcmp(info, out, (size_t)K * sizeof(int)) != 0) errs++;
            }
            printf(" %4.0f%%", 100.0 * errs / trials);
        }
        printf("\n");
    }
    free(info); free(coded); free(out); free(llr);
    return 0;
}
