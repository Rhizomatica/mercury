/* hail_suffix_sweep — can a DIRECTED pattern replace a coded ACCEPT?
 *
 * The ACCEPT frame spends 3.74 s of DATAC16 telling the caller something it
 * very nearly already knows: it chose the session id and it dialled the
 * station, so the only genuinely new field is the bandwidth token.  A
 * Welch-Costas pattern plus a few session-derived suffix symbols could say the
 * same thing in well under a second, and -- being a correlation against an
 * expected sequence rather than a decode -- it does not pay the
 * energy-per-bit price that sinks every short coded control frame
 * (docs/MODES.md, "Fourth attempt").
 *
 * mfsk_set_hail_target() already does exactly this for directed hailing.  The
 * open question is not whether it works but what it costs, and there are two
 * competing failure modes with one knob between them:
 *
 *   MISS  - the caller fails to see a genuine ACCEPT.  Costs a retry (~8 s),
 *           and if it keeps happening the connect is worse than DATAC16's.
 *   FALSE - the caller accepts a pattern meant for somebody else, or noise.
 *           Costs a phantom connection, which is much worse than a retry.
 *
 * A coded ACCEPT gets its selectivity free from the CRC and the DST-CRC16
 * check.  A pattern has to buy it from the suffix and the match threshold, so
 * this sweeps both: detection rate vs SNR against a matched suffix, and the
 * false-accept rate against a mismatched one, at every threshold.
 *
 * Self-check: at high SNR a matched suffix must detect ~always and a
 * mismatched suffix must never be accepted at the operating threshold.  If
 * either fails, the harness is wrong before the design is.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <stdint.h>

#include "mfsk.h"
#include "mfsk_sync.h"
#include "mfsk_ofdm.h"

#define FS        8000.0
#define FC        2000.0
#define NFFT      256
#define NCAR      50
#define GI        0.25
#define MM        32
#define TXAMP     2200.0
#define LPF_TAPS  63
#define LPF_FC    1000.0
#define SUFFIX_LEN 4

static unsigned long long rng = 99887766554433ULL;

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

static void mklpf(double *h, double fc)
{
    double sum = 0.0;
    for (int i = 0; i < LPF_TAPS; i++)
    {
        int k = i - LPF_TAPS / 2;
        double s = (k == 0) ? (2.0 * fc / FS)
                            : sin(2.0 * M_PI * fc * k / FS) / (M_PI * k);
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * i / (LPF_TAPS - 1));
        h[i] = s * w;
        sum += h[i];
    }
    for (int i = 0; i < LPF_TAPS; i++) h[i] /= sum;
}

/* Same FNV-1a derivation mfsk_set_hail_target uses, over an arbitrary key so
 * the suffix can be bound to (session id, callsigns, bandwidth token). */
static void suffix_from_key(const char *key, int M, int *suffix)
{
    uint32_t hash = 2166136261u;
    for (const char *p = key; *p; p++)
    {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        hash ^= (uint8_t)c;
        hash *= 16777619u;
    }
    for (int i = 0; i < SUFFIX_LEN; i++)
        suffix[i] = (int)((hash >> (i * 5)) & 0x1F) % M;
}

int main(int argc, char **argv)
{
    int    trials = (argc > 1) ? atoi(argv[1]) : 200;
    double snr_lo = (argc > 2) ? atof(argv[2]) : -22.0;
    double snr_hi = (argc > 3) ? atof(argv[3]) : -6.0;
    if (trials <= 0) { fprintf(stderr, "usage: %s [trials] [snr_lo] [snr_hi]\n", argv[0]); return 1; }

    mfsk_t m;
    ofdm_frame_t o;
    mfsk_init(&m, MM, NCAR, 1);
    ofdm_frame_init(&o, NFFT, NCAR, GI, 0);
    int Nofdm = ofdm_frame_nofdm(&o);
    double w = 2.0 * M_PI * FC / FS;
    double lpf[LPF_TAPS];
    mklpf(lpf, LPF_FC);

    int base_ns  = m.ack_pattern_nsymb;          /* plain pattern, as shipped */
    int dir_ns   = base_ns + SUFFIX_LEN;         /* directed = pattern+suffix */
    int nsamp    = dir_ns * Nofdm;

    printf("M=%d  plain pattern %d sym (%.0f ms, threshold %d/%d)\n",
           m.M, base_ns, base_ns * Nofdm * 1000.0 / FS,
           m.ack_match_threshold, base_ns);
    printf("      directed      %d sym (%.0f ms) = pattern + %d suffix symbols\n\n",
           dir_ns, dir_ns * Nofdm * 1000.0 / FS, SUFFIX_LEN);

    /* Expected tone list for OUR session, and for somebody else's. */
    int tones_mine[64], tones_other[64], suf_mine[SUFFIX_LEN], suf_other[SUFFIX_LEN];
    suffix_from_key("PU2UIT-2>PU2UIT-3:42:2300", m.M, suf_mine);
    suffix_from_key("DL9ABC>W1XYZ:17:500",       m.M, suf_other);
    for (int s = 0; s < base_ns; s++)
        tones_mine[s] = tones_other[s] = m.ack_tones[s % m.ack_pattern_len];
    for (int s = 0; s < SUFFIX_LEN; s++)
    {
        tones_mine[base_ns + s]  = suf_mine[s];
        tones_other[base_ns + s] = suf_other[s];
    }

    /* Thresholds worth testing, all out of dir_ns.  Anything <= base_ns can be
     * met by the SHARED Costas prefix alone, so the suffix contributes no
     * selectivity there -- which includes hail's own
     * hail_match_threshold + SUFFIX_LEN. */
    int nthr = 6;
    int thr[6] = { m.hail_match_threshold + SUFFIX_LEN, base_ns,
                   base_ns + 1, base_ns + 2, base_ns + 3, base_ns + 4 };

    printf("  SNR3k |");
    for (int i = 0; i < nthr; i++) printf("  detect@%2d ", thr[i]);
    printf("|  false-accept (wrong/noise) at thr=%d and thr=%d\n", thr[0], thr[3]);

    mfsk_cplx *bins = calloc((size_t)dir_ns * NCAR, sizeof(mfsk_cplx));
    int16_t   *pb   = malloc(sizeof(int16_t) * (size_t)nsamp);
    double complex *bb = malloc(sizeof(double complex) * (size_t)nsamp);
    double complex *bf = malloc(sizeof(double complex) * (size_t)nsamp);
    if (!bins || !pb || !bb || !bf) return 1;

    for (double snr = snr_lo; snr <= snr_hi + 0.01; snr += 2.0)
    {
        int hit[6] = {0}, wrong = 0, wrong_hi = 0, noise_fa = 0;

        for (int t = 0; t < trials; t++)
        {
            /* --- transmit our directed pattern --- */
            memset(bins, 0, (size_t)dir_ns * NCAR * sizeof(mfsk_cplx));
            double amp = sqrt((double)NCAR);
            for (int s = 0; s < dir_ns; s++)
            {
                int tone = (tones_mine[s] + s * m.tone_hop_step) % m.M;
                bins[s * NCAR + m.stream_offsets[0] + tone].re = amp;
            }

            int written = 0;
            long tx_n = 0;
            for (int s = 0; s < dir_ns; s++)
            {
                double complex fb[NCAR], pad[NFFT], tt[NFFT], cp[NFFT + 128];
                for (int k = 0; k < NCAR; k++)
                    fb[k] = bins[s * NCAR + k].re + bins[s * NCAR + k].im * I;
                ofdm_zero_padder(&o, fb, pad);
                ofdm_ifft(&o, pad, tt);
                ofdm_gi_adder(&o, tt, cp);
                for (int n = 0; n < Nofdm; n++)
                {
                    double ph = w * (double)tx_n++;
                    double v = TXAMP * (creal(cp[n]) * cos(ph) + cimag(cp[n]) * sin(ph));
                    if (v > 32767.0) v = 32767.0; else if (v < -32768.0) v = -32768.0;
                    pb[written++] = (int16_t)lrint(v);
                }
            }

            double ps = 0.0;
            for (int i = 0; i < written; i++) ps += (double)pb[i] * pb[i];
            ps /= written;
            double sigma = sqrt(ps / (pow(10.0, snr / 10.0) * (3000.0 / (FS / 2.0))));

            for (int pass = 0; pass < 2; pass++)
            {
                /* pass 0: signal + noise.  pass 1: noise only (false alarm). */
                for (int i = 0; i < written; i++)
                {
                    double v = (pass == 0 ? (double)pb[i] : 0.0) + sigma * gauss();
                    double ph = w * (double)i;
                    bb[i] = 2.0 * v * cos(ph) + I * 2.0 * v * sin(ph);
                }
                for (int i = 0; i < written; i++)
                {
                    double complex a = 0;
                    for (int k = 0; k < LPF_TAPS; k++)
                    {
                        int j = i - k + LPF_TAPS / 2;
                        if (j >= 0 && j < written) a += lpf[k] * bb[j];
                    }
                    bf[i] = a;
                }

                int pos = -1;
                int s_mine  = mfsk_detect_pattern(&m, &o, bf, written,
                                                  tones_mine, dir_ns, dir_ns, &pos);
                int s_other = mfsk_detect_pattern(&m, &o, bf, written,
                                                  tones_other, dir_ns, dir_ns, &pos);
                if (pass == 0)
                {
                    for (int i = 0; i < nthr; i++)
                        if (s_mine >= thr[i]) hit[i]++;
                    if (s_other >= thr[0]) wrong++;        /* at hail's own threshold */
                    if (s_other >= thr[3]) wrong_hi++;     /* at a suffix-forcing one */
                }
                else
                {
                    if (s_mine >= thr[0]) noise_fa++;
                }
            }
        }

        printf("  %+5.1f |", snr);
        for (int i = 0; i < nthr; i++)
            printf("   %3d/%-3d ", hit[i], trials);
        printf("|  %3d/%-3d and %3d/%-3d wrong-session, %d noise\n",
               wrong, trials, wrong_hi, trials, noise_fa);
    }

    free(bins); free(pb); free(bb); free(bf);
    return 0;
}
