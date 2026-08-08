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
#include "chanutil.h"

#define FS        8000.0
#define FC        2000.0
#define NFFT      256
#define NCAR      50
#define GI        0.25
#define MM        32
#define TXAMP     2200.0
#define LPF_TAPS  63
#define LPF_FC    1000.0
#define SUFFIX_MAX 16
#define HEADROOM_PEAK 4000.0        /* AWGN path: fixed signal peak, swept noise */

/* Fading level management.
 *
 * int16 cannot hold signal+noise at fringe SNRs if the signal level is fixed:
 * the noise alone overflows (measured 60-80 % of samples clipped).  So hold the
 * NOISE at a level that comfortably fits -- 4 sigma inside the rail -- and set
 * the SNR by scaling the SIGNAL instead.  The channel's own measured SNR is
 * still what gets reported, so the axis stays honest. */
#define FADE_NO_DBHZ   (-19.0f)      /* -> real noise rms ~6-7k at Fs=8000 */
#define FADE_NOISE_RMS 6800.0

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
static void suffix_from_key(const char *key, int M, int *suffix, int len)
{
    /* FNV-1a as mfsk_set_hail_target does, but rehashed per 6 symbols so the
     * suffix can be longer than the 32 bits of one hash.  A 4-symbol suffix
     * cannot reach a 1-in-10000 false-accept without demanding that all 20
     * symbols match, which costs far more sensitivity than the extra symbols
     * do -- see the table in docs/MFSK-PORT.md. */
    uint32_t hash = 2166136261u;
    for (const char *p = key; *p; p++)
    {
        char c = *p;
        if (c >= 'a' && c <= 'z') c -= 32;
        hash ^= (uint8_t)c;
        hash *= 16777619u;
    }
    uint32_t h = hash;
    for (int i = 0; i < len; i++)
    {
        if (i > 0 && (i % 6) == 0) { h ^= (uint32_t)i; h *= 16777619u; }
        suffix[i] = (int)((h >> ((i % 6) * 5)) & 0x1F) % M;
    }
}

int main(int argc, char **argv)
{
    int    trials = (argc > 1) ? atoi(argv[1]) : 200;
    int    sufflen= (argc > 2) ? atoi(argv[2]) : 8;
    double snr_lo = (argc > 3) ? atof(argv[3]) : -20.0;
    double snr_hi = (argc > 4) ? atof(argv[4]) : -6.0;
    /* Optional channel preset.  Under fading the axis becomes noise density
     * and the achieved SNR3k is measured per row, exactly as in
     * acquire_vs_decode -- so the two instruments can be read side by side. */
    int    chan   = (argc > 5) ? chanutil_preset_from_name(argv[5]) : CHAN_AWGN;
    /* Search-window width in bursts.  3 mirrors the RX loop and is what the
     * false-alarm numbers need; it also costs ~9x the correlation work, which
     * is unaffordable on a fading sweep.  Detection rate barely depends on it,
     * so a fading run may use 1 -- but then do NOT quote its false-alarm
     * column, which is why the header says which was used. */
    int    winmult = (argc > 6) ? atoi(argv[6]) : 3;
    if (winmult < 1) winmult = 1;
    if (chan < 0) { fprintf(stderr, "unknown channel (awgn|mpg|mpp|mpd)\n"); return 1; }
    if (trials <= 0 || sufflen <= 0 || sufflen > SUFFIX_MAX)
    { fprintf(stderr, "usage: %s [trials] [suffix_len] [snr_lo] [snr_hi]\n", argv[0]); return 1; }

    mfsk_t m;
    ofdm_frame_t o;
    mfsk_init(&m, MM, NCAR, 1);
    ofdm_frame_init(&o, NFFT, NCAR, GI, 0);
    int Nofdm = ofdm_frame_nofdm(&o);
    double w = 2.0 * M_PI * FC / FS;
    double lpf[LPF_TAPS];
    mklpf(lpf, LPF_FC);

    int base_ns  = m.ack_pattern_nsymb;          /* plain pattern, as shipped */
    int dir_ns   = base_ns + sufflen;            /* directed = pattern+suffix */
    int nsamp    = dir_ns * Nofdm;
    /* Production slides the correlator over ~3 bursts of mostly noise
     * (see the pattern detector in modem.c), so search over the same span:
     * a one-burst window understates both the search difficulty and the
     * false-alarm rate. */
    int nwin     = nsamp * winmult;

    printf("M=%d  plain pattern %d sym (%.0f ms, threshold %d/%d)\n",
           m.M, base_ns, base_ns * Nofdm * 1000.0 / FS,
           m.ack_match_threshold, base_ns);
    printf("      directed      %d sym (%.0f ms) = pattern + %d suffix symbols\n\n",
           dir_ns, dir_ns * Nofdm * 1000.0 / FS, sufflen);

    /* Expected tone list for OUR session, and for somebody else's. */
    int tones_mine[64], tones_other[64];
    int suf_mine[SUFFIX_MAX], suf_other[SUFFIX_MAX];
    suffix_from_key("PU2UIT-2>PU2UIT-3:42:2300", m.M, suf_mine, sufflen);
    for (int s = 0; s < base_ns; s++)
        tones_mine[s] = tones_other[s] = m.ack_tones[s % m.ack_pattern_len];
    for (int s = 0; s < sufflen; s++)
        tones_mine[base_ns + s] = suf_mine[s];

    /* Thresholds worth testing, all out of dir_ns.  Anything <= base_ns can be
     * met by the SHARED Costas prefix alone, so the suffix contributes no
     * selectivity there -- which includes hail's own
     * hail_match_threshold + MFSK_HAIL_SUFFIX_LEN. */
    /* Only thresholds ABOVE base_ns force suffix matches; at or below it the
     * shared Costas prefix alone satisfies the detector and the suffix
     * addresses nobody. */
    int nthr = 6;
    int thr[6];
    for (int i = 0; i < nthr; i++)
    {
        thr[i] = base_ns + 1 + i;
        if (thr[i] > dir_ns) thr[i] = dir_ns;
    }
    int thr_op = base_ns + 4;                    /* the >=4-suffix operating point */
    if (thr_op > dir_ns) thr_op = dir_ns;

    printf("channel: %s   search window %d burst(s)%s\n",
           chanutil_preset_name(chan), winmult,
           winmult < 3 ? "  [narrow: false-alarm column NOT comparable]" : "");
    printf("%s |", chan == CHAN_AWGN ? "  SNR3k " : "  No/meas");
    for (int i = 0; i < nthr; i++) printf("  detect@%2d ", thr[i]);
    printf("| false-accept vs RANDOM sessions at thr=%d\n", thr_op);

    printf("  (thresholds above %d force suffix matches; %d = all symbols)\n",
           base_ns, dir_ns);
    mfsk_cplx *bins = calloc((size_t)dir_ns * NCAR, sizeof(mfsk_cplx));
    int16_t   *pb   = malloc(sizeof(int16_t) * (size_t)nsamp);
    double complex *bb = malloc(sizeof(double complex) * (size_t)nwin);
    double complex *bf = malloc(sizeof(double complex) * (size_t)nwin);
    if (!bins || !pb || !bb || !bf) return 1;

    for (double snr = snr_lo; snr <= snr_hi + 0.01; snr += 1.0)
    {
        int hit[6] = {0}, wrong = 0, noise_fa = 0;
        long clipped = 0, tot_samp = 0;
        double snr_meas_sum = 0.0; int snr_meas_cnt = 0;

        /* One warmed channel per SNR point, shared by every burst -- see the
         * note in acquire_vs_decode. */
        chanutil_t *ch = NULL;
        if (chan != CHAN_AWGN)
        {
            ch = chanutil_open(chan, FADE_NO_DBHZ, 1234u);
            if (!ch) { fprintf(stderr, "chanutil_open failed\n"); return 1; }
        }

        for (int t = 0; t < trials; t++)
        {
            /* A fresh random peer session each trial: the false-accept rate
             * we care about is against the population of other sessions, not
             * against one arbitrary key. */
            char okey[64];
            snprintf(okey, sizeof(okey), "%c%c%d%c%c>%c%c%d:%d:%d",
                     'A' + (int)(urand() * 26), 'A' + (int)(urand() * 26),
                     (int)(urand() * 10),
                     'A' + (int)(urand() * 26), 'A' + (int)(urand() * 26),
                     'A' + (int)(urand() * 26), 'A' + (int)(urand() * 26),
                     (int)(urand() * 10), (int)(urand() * 128),
                     (int)(urand() * 4));
            suffix_from_key(okey, m.M, suf_other, sufflen);
            for (int s2 = 0; s2 < sufflen; s2++)
                tones_other[base_ns + s2] = suf_other[s2];

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

            double ps = 0.0, pkv = 0.0;
            for (int i = 0; i < written; i++)
            {
                ps += (double)pb[i] * pb[i];
                if (fabs((double)pb[i]) > pkv) pkv = fabs((double)pb[i]);
            }
            ps /= written;

            /* HEADROOM, same reason as acquire_vs_decode: the pattern leaves
             * mfsk_pattern_tx at ~15.5k peak, and at fringe SNRs the noise rms
             * is several times the signal, so signal+noise clips against the
             * int16 rail and the delivered SNR quietly rises while distortion
             * eats the gain.  Scale to a fixed peak first. */
            if (pkv > 0.0)
            {
                double gnorm = HEADROOM_PEAK / pkv;
                for (int i = 0; i < written; i++)
                    pb[i] = (int16_t)lrint((double)pb[i] * gnorm);
                ps *= gnorm * gnorm;
            }
            double sigma = sqrt(ps / (pow(10.0, snr / 10.0) * (3000.0 / (FS / 2.0))));

            if (chan != CHAN_AWGN)
            {
                /* Scale the burst so that, against the fixed channel noise, the
                 * 3 kHz SNR lands on the requested value. */
                double want_rms = FADE_NOISE_RMS * sqrt(3000.0 / (FS / 2.0))
                                  * pow(10.0, snr / 20.0);
                double cur_rms  = sqrt(ps);
                if (cur_rms > 0.0)
                {
                    double gs2 = want_rms / cur_rms;
                    for (int i = 0; i < written; i++)
                        pb[i] = (int16_t)lrint((double)pb[i] * gs2);
                    ps *= gs2 * gs2;
                }
                /* Fade the burst itself; the surrounding window stays plain
                 * noise at the level the channel actually delivered. */
                float meas = 0.0f;
                chanutil_run(ch, pb, written, &meas);
                snr_meas_sum += meas; snr_meas_cnt++;
                sigma = sqrt(ps / (pow(10.0, meas / 10.0) * (3000.0 / (FS / 2.0))));
            }

            /* Random placement inside the window, as on the air. */
            int off = (int)(urand() * (double)(nwin - written));

            for (int pass = 0; pass < 2; pass++)
            {
                /* pass 0: signal + noise.  pass 1: noise only (false alarm). */
                for (int i = 0; i < nwin; i++)
                {
                    int k2 = i - off;
                    int in_burst = (pass == 0 && k2 >= 0 && k2 < written);
                    /* Under fading the burst ALREADY carries the channel's own
                     * AWGN (chanutil_fade added it), so adding sigma on top of
                     * it here would count the noise twice and read ~3 dB
                     * pessimistic.  Outside the burst there is no channel
                     * output, so the window is filled with noise at the level
                     * the channel actually delivered. */
                    double v;
                    if (in_burst)
                        v = (double)pb[k2] + (chan == CHAN_AWGN ? sigma * gauss() : 0.0);
                    else
                        v = sigma * gauss();
                    double ph = w * (double)i;
                    bb[i] = 2.0 * v * cos(ph) + I * 2.0 * v * sin(ph);
                }
                for (int i = 0; i < nwin; i++)
                {
                    double complex a = 0;
                    for (int k = 0; k < LPF_TAPS; k++)
                    {
                        int j = i - k + LPF_TAPS / 2;
                        if (j >= 0 && j < nwin) a += lpf[k] * bb[j];
                    }
                    bf[i] = a;
                }

                if (pass == 0)
                {
                    for (int i = 0; i < written; i++)
                        if (pb[i] >= 32767 || pb[i] <= -32768) clipped++;
                    tot_samp += written;
                }

                int pos = -1;
                int s_mine  = mfsk_detect_pattern(&m, &o, bf, nwin,
                                                  tones_mine, dir_ns, dir_ns, &pos);
                int s_other = mfsk_detect_pattern(&m, &o, bf, nwin,
                                                  tones_other, dir_ns, dir_ns, &pos);
                if (pass == 0)
                {
                    for (int i = 0; i < nthr; i++)
                        if (s_mine >= thr[i]) hit[i]++;
                    if (s_other >= thr_op) wrong++;
                }
                else
                {
                    if (s_mine >= thr_op) noise_fa++;
                }
            }
        }

        if (ch) chanutil_close(ch);
        printf("  %+5.1f |", chan == CHAN_AWGN ? snr
               : (snr_meas_cnt ? snr_meas_sum / snr_meas_cnt : 0.0));
        for (int i = 0; i < nthr; i++)
            printf("   %3d/%-3d ", hit[i], trials);
        printf("|  %3d/%-4d wrong, %d noise, clip %.2f%%\n", wrong, trials, noise_fa,
               tot_samp ? 100.0 * (double)clipped / (double)tot_samp : 0.0);
    }

    free(bins); free(pb); free(bb); free(bf);
    return 0;
}
