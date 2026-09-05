/* shortframe_sweep — is a SHORTER coded burst ever a better control frame?
 *
 * This question keeps coming back.  DATAC13, DATAC14 and DATAC18 were each
 * built to shorten the ARQ control plane and each lost to DATAC16 at the
 * fringe (docs/MODES.md).  This tool exists so the next attempt can be settled
 * in minutes instead of a branch, and so the reason is a number rather than a
 * recollection.
 *
 * It measures FER vs Eb/N0 for a named LDPC code carried over the project's
 * non-coherent 32-MFSK modulator/demodulator, on AWGN and on flat Rayleigh at
 * a chosen Doppler.  Eb/N0 rather than SNR is the right axis here: it is
 * bandwidth-independent, so it compares frames of different length and width
 * on equal terms.  Convert with
 *
 *     SNR3k = Eb/N0 * info_bits / (3000 * payload_seconds)
 *
 * Why Eb/N0 answers the question: a frame that carries FEWER bits can be
 * proportionally SHORTER at the same Eb/N0, i.e. free speed.  That is what
 * makes shortening look attractive on AWGN.  Fading is where it dies -- a
 * short burst spans few fades and loses time diversity -- and the size of that
 * loss is exactly what the -f sweep prints.
 *
 * Calibration (do not skip): the model omits acquisition and the passband
 * chain, so it reads optimistic in absolute terms.  Anchor it by running the
 * long-burst case and comparing against a measured floor from docs/MFSK-PORT.md
 * before quoting any absolute SNR.  Relative comparisons (short vs long, code
 * vs code) are the trustworthy output.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "mfsk.h"
#include "mpdecode_core.h"
#include "ldpc_codes.h"

#define NCAR     50
#define MM       32
#define SYMRATE  25.0            /* 40 ms per MFSK symbol */
#define NOSC     16              /* Jakes oscillators     */
#define MAX_LDPC_BITS 8192       /* larger than any vendored codeword */

static unsigned long long rng = 24680135791113ULL;

static double urand(void)
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((rng >> 11) & 0x1FFFFFFFFFFFFFULL) / (double)0x20000000000000ULL;
}

static double gauss(void)
{
    double u1 = urand(), u2 = urand();
    if (u1 < 1e-300) u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static double osc_ph[NOSC], osc_ph2[NOSC];

static void fade_new_realisation(void)
{
    for (int i = 0; i < NOSC; i++)
    {
        osc_ph[i]  = 2.0 * M_PI * urand();
        osc_ph2[i] = 2.0 * M_PI * urand();
    }
}

/* Flat Rayleigh (Jakes sum-of-sinusoids), one complex gain per symbol.
 *
 * Deliberately flat: a real 2 ms delay spread makes the 1 kHz MFSK band
 * frequency-selective, so tones fade semi-independently and the true channel
 * is kinder.  If a frame survives here it survives there. */
static void fade_gain(int n, double fd, double *gr, double *gi)
{
    double t = (double)n / SYMRATE;
    double sr = 0.0, si = 0.0;
    for (int i = 0; i < NOSC; i++)
    {
        double alpha = 2.0 * M_PI * (i + 0.5) / (4.0 * NOSC);
        double wd    = 2.0 * M_PI * fd * cos(alpha);
        sr += cos(wd * t + osc_ph[i]);
        si += cos(wd * t + osc_ph2[i]);
    }
    double s = 1.0 / sqrt((double)NOSC);
    *gr = sr * s;
    *gi = si * s;
}

static void usage(const char *me)
{
    fprintf(stderr,
        "usage: %s [-c code] [-t trials] [-f doppler_hz] [-r reps]\n"
        "  -c  LDPC code name (default H_128_256_5; try HRA_112_112,\n"
        "      HRA_56_56, H_1024_2048_4f for a long-burst reference)\n"
        "  -t  trials per point (default 300)\n"
        "  -f  Rayleigh Doppler in Hz; 0 = AWGN (default 0)\n"
        "  -r  transmit the codeword r times (crude time diversity)\n", me);
}

int main(int argc, char **argv)
{
    const char *codename = "H_128_256_5";
    int    trials = 300;
    double fd     = 0.0;
    int    reps   = 1;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)      codename = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) trials   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) fd       = atof(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) reps     = atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }
    if (trials <= 0 || reps <= 0) { usage(argv[0]); return 1; }

    struct LDPC ldpc;
    if (ldpc_codes_find((char *)codename) < 0)
    {
        fprintf(stderr, "unknown code '%s'; available:\n", codename);
        ldpc_codes_list();
        return 1;
    }
    ldpc_codes_setup(&ldpc, (char *)codename);
    int    K    = ldpc.NumberRowsHcols;
    int    N    = ldpc.CodeLength;
    double rate = (double)K / (double)N;

    mfsk_t m;
    mfsk_init(&m, MM, NCAR, 1);
    int bps      = m.nBits;
    int nsym     = ((N + bps - 1) / bps) * reps;
    int tot_bits = nsym * bps;

    int *perm = malloc(sizeof(int) * (size_t)tot_bits);
    if (!perm) return 1;
    mfsk_interleave_init(perm, tot_bits);

    double payload_s = nsym / SYMRATE;
    printf("%s x%d over %d-MFSK, %s: K=%d N=%d, %d sym = %.2f s payload\n",
           codename, reps, MM,
           fd > 0.0 ? "flat Rayleigh" : "AWGN",
           K, N, nsym, payload_s);
    if (fd > 0.0) printf("  Doppler %.2f Hz (~%.1f fades across the burst)\n",
                         fd, fd * payload_s);
    printf("  convert: SNR3k = Eb/N0 + 10*log10(%d / (3000 * %.2f)) "
           "= Eb/N0 %+.2f dB\n",
           K, payload_s, 10.0 * log10((double)K / (3000.0 * payload_s)));
    printf("\n   Eb/N0(dB)   FER\n");

    mfsk_cplx *sym   = malloc(sizeof(mfsk_cplx) * (size_t)nsym * NCAR);
    int       *cbits = malloc(sizeof(int)   * (size_t)tot_bits);
    float     *llr   = malloc(sizeof(float) * (size_t)tot_bits);
    float     *acc   = malloc(sizeof(float) * (size_t)N);
    if (!sym || !cbits || !llr || !acc) return 1;

    for (double ebn0_db = 0.0; ebn0_db <= 30.01; ebn0_db += 1.0)
    {
        int fails = 0;
        for (int t = 0; t < trials; t++)
        {
            unsigned char ibits[MAX_LDPC_BITS], pbits[MAX_LDPC_BITS];
            for (int i = 0; i < K; i++) ibits[i] = (urand() < 0.5) ? 0 : 1;
            encode(&ldpc, ibits, pbits);

            for (int i = 0; i < tot_bits; i++)
            {
                int ci = i % N;
                cbits[perm[i]] = (ci < K) ? ibits[ci] : pbits[ci - K];
            }
            mfsk_mod(&m, cbits, tot_bits, sym);

            /* Read the emitted energy back rather than assuming mfsk_mod's
             * internal scaling, so the Eb/N0 axis cannot drift with it. */
            double es = 0.0;
            for (int i = 0; i < nsym * NCAR; i++)
                es += sym[i].re * sym[i].re + sym[i].im * sym[i].im;
            es /= nsym;

            double ebn0  = pow(10.0, ebn0_db / 10.0);
            double n0    = es * reps / (ebn0 * bps * rate);
            double sigma = sqrt(n0 / 2.0);

            if (fd > 0.0) fade_new_realisation();
            for (int s = 0; s < nsym; s++)
            {
                double gr = 1.0, gi = 0.0;
                if (fd > 0.0) fade_gain(s, fd, &gr, &gi);
                for (int c = 0; c < NCAR; c++)
                {
                    mfsk_cplx *v = &sym[s * NCAR + c];
                    double re = v->re * gr - v->im * gi;
                    double im = v->re * gi + v->im * gr;
                    v->re = re + sigma * gauss();
                    v->im = im + sigma * gauss();
                }
            }

            mfsk_demod(&m, sym, tot_bits, llr);
            for (int i = 0; i < N; i++)        acc[i] = 0.0f;
            for (int i = 0; i < tot_bits; i++) acc[i % N] += llr[perm[i]];

            uint8_t out[MAX_LDPC_BITS];
            int it = 0;
            run_ldpc_decoder(&ldpc, out, acc, &it);
            if (memcmp(out, ibits, (size_t)K) != 0) fails++;
        }
        double fer = (double)fails / trials;
        printf("      %4.1f     %.3f\n", ebn0_db, fer);
        if (fer <= 0.01) break;
    }

    free(sym); free(cbits); free(llr); free(acc); free(perm);
    return 0;
}
