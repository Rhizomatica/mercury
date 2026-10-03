/* burst_train_probe — does ONE payload decoder hear N bursts of the same mode
 * sent back to back in one keydown, and how much gap do they need?
 *
 * The carousel ARQ (tests/sim/carousel_bench.c) sends a round as N separate
 * bursts -- each preamble + frame + postamble -- in one keydown, to a receiver
 * whose single payload decoder is bound to that mode for the whole round.
 * Each burst stands alone on the air, but the decoder sees them as one
 * continuous stream: the question is whether it re-acquires on burst i+1's
 * preamble right after burst i's postamble.
 *
 * Per trial, on one channel realisation:
 *
 *   train  one decoder over the whole keydown, as the receiver runs
 *   alone  each burst on its own -- lead-in noise, the burst, noise -- through
 *          the same channel (the next stretch of the same fading process),
 *          each with a fresh decoder: the ceiling
 *
 * If train falls short of alone, the train itself costs frames (and which
 * positions fail says how).
 *
 * What it established (20-60 trials, 6-burst trains):
 *   - with no gap the decoder misses bursts after the first: DATAC15 72/120
 *     at -5 dB AWGN (alone 120/120), QAM16C2 82/120 on MPP (alone 104/120);
 *   - at 100 ms every mode matches isolated bursts on AWGN, but QAM16C2 on
 *     MPP still loses ~6 % (297/360 against 316/360);
 *   - at 200 ms trains match isolated bursts within noise on AWGN, MPP and
 *     MPD (QAM16C2 324/328, DATAC15 335/335), which is the carousel's gap.
 *   Re-opening or UNSYNCing the decoder after each frame changes nothing: the
 *   burst state machine already returns to search after a packet.
 *
 * usage: burst_train_probe [MODE] [level] [trials] [N] [gap_s...]
 *        defaults: DATAC3 5 20 6  0 0.1 0.3 1.0
 *
 * Environment:
 *   RX_VERBOSE=n           the train decoder's freedv verbosity (acquisition
 *                          decisions on stderr at 2)
 *   RX_AFTER=keep|resync|reopen  what the train's decoder does after each
 *                          frame it decodes: nothing (default), UNSYNC, or
 *                          close and open a fresh instance
 *   CHAN=AWGN|MPG|MPP|MPD  channel (default AWGN).  On AWGN `level` is the
 *                          SNR3k in dB; on the fading presets it is the noise
 *                          density No in dB/Hz for common/watterson.c, and
 *                          the achieved SNR3k is printed.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freedv_api.h"
#include "chanutil.h"

#define FS 8000.0
#define HEADROOM_PEAK 4000.0
#define LEAD_S 2.0              /* noise before the first burst */
#define MAX_N 32

static unsigned long long rng = 88172645463325252ULL;

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

static int mode_from_name(const char *s)
{
    static const struct { const char *n; int m; } t[] = {
        {"DATAC16", FREEDV_MODE_DATAC16}, {"DATAC15", FREEDV_MODE_DATAC15}, {"DATAC4", FREEDV_MODE_DATAC4},
        {"DATAC3", FREEDV_MODE_DATAC3},   {"DATAC1", FREEDV_MODE_DATAC1},
        {"DATAC17", FREEDV_MODE_DATAC17}, {"QAM16C2", FREEDV_MODE_QAM16C2},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(s, t[i].n)) return t[i].m;
    return -1;
}

/* One burst (preamble + frame + postamble) carrying `p` (CRC appended),
 * peak-normalised to HEADROOM_PEAK.  Returns its length; *ps = mean power. */
static int make_burst(struct freedv *tx, unsigned char *p, int nbytes, short *out, double *ps)
{
    unsigned short crc = freedv_gen_crc16(p, nbytes - 2);
    p[nbytes - 2] = (unsigned char)(crc >> 8);
    p[nbytes - 1] = (unsigned char)(crc & 0xff);
    int n = freedv_rawdatapreambletx(tx, out);
    freedv_rawdatatx(tx, out + n, p);
    n += freedv_get_n_tx_modem_samples(tx);
    n += freedv_rawdatapostambletx(tx, out + n);
    double pk = 0.0;
    for (int i = 0; i < n; i++) if (fabs((double)out[i]) > pk) pk = fabs((double)out[i]);
    double g = pk > 0.0 ? HEADROOM_PEAK / pk : 1.0, acc = 0.0;
    for (int i = 0; i < n; i++) { out[i] = (short)lrint(out[i] * g); acc += (double)out[i] * out[i]; }
    *ps = acc / n;
    return n;
}

/* Noise (AWGN at `level` for mean burst power ps) or the fading channel. */
static void apply_channel(chanutil_t *chan, short *a, int n, double level, double ps, float *snr3k)
{
    if (chan) { chanutil_run(chan, a, n, snr3k); return; }
    double sigma = sqrt(ps / (pow(10.0, level / 10.0) * (3000.0 / (FS / 2.0))));
    for (int i = 0; i < n; i++)
        a[i] = (short)lrint(fmax(-32768.0, fmin(32767.0, a[i] + sigma * gauss())));
    *snr3k = (float)level;
}

enum { AFTER_KEEP, AFTER_RESYNC, AFTER_REOPEN };
static int rx_after = AFTER_KEEP;

/* Decode a[0..n) with a fresh decoder; mark which of the N payloads came out
 * with a clean CRC. */
static void run_rx(int mode, const short *a, int n, unsigned char pay[][4096], int nbytes, int N, int *got)
{
    struct freedv *rx = freedv_open(mode);
    freedv_set_frames_per_burst(rx, 1);
    if (N > 1 && getenv("RX_VERBOSE")) freedv_set_verbose(rx, atoi(getenv("RX_VERBOSE")));
    unsigned char out[4096];
    short chunk[16384];
    int pos = 0, idle = 0;
    while (pos < n)
    {
        /* nin == 0 is not the end: after a postamble the demod rewinds into
         * its history and wants to be called again with no new samples (as
         * modem.c does).  Stopping there lost every burst after the first. */
        int nin = freedv_nin(rx);
        if (nin < 0 || nin > (int)(sizeof(chunk) / sizeof(chunk[0])) || pos + nin > n) break;
        if (nin == 0 && ++idle > 64) break;
        if (nin > 0) idle = 0;
        memcpy(chunk, a + pos, sizeof(short) * (size_t)nin);
        pos += nin;
        int nb = (int)freedv_rawdatarx(rx, out, chunk);
        if (nb > 0 && !(freedv_get_rx_status(rx) & FREEDV_RX_BIT_ERRORS))
        {
            for (int k = 0; k < N; k++)
                if (!memcmp(out, pay[k], (size_t)(nbytes - 2))) got[k] = 1;
            if (rx_after == AFTER_RESYNC)
                freedv_set_sync(rx, FREEDV_SYNC_UNSYNC);
            else if (rx_after == AFTER_REOPEN)
            {
                freedv_close(rx);
                rx = freedv_open(mode);
                freedv_set_frames_per_burst(rx, 1);
            }
        }
    }
    freedv_close(rx);
}

int main(int argc, char **argv)
{
    const char *pname = argc > 1 ? argv[1] : "DATAC3";
    double level = argc > 2 ? atof(argv[2]) : 5.0;
    int trials   = argc > 3 ? atoi(argv[3]) : 20;
    int N        = argc > 4 ? atoi(argv[4]) : 6;
    double gaps_def[] = {0.0, 0.1, 0.3, 1.0};
    int ngaps = argc > 5 ? argc - 5 : (int)(sizeof(gaps_def) / sizeof(gaps_def[0]));
    const char *cname = getenv("CHAN") ? getenv("CHAN") : "AWGN";
    const char *after = getenv("RX_AFTER") ? getenv("RX_AFTER") : "keep";
    rx_after = !strcmp(after, "resync") ? AFTER_RESYNC : !strcmp(after, "reopen") ? AFTER_REOPEN : AFTER_KEEP;
    int preset = chanutil_preset_from_name(cname);
    int mode = mode_from_name(pname);
    if (mode < 0 || trials <= 0 || N < 1 || N > MAX_N || preset < 0)
    {
        fprintf(stderr, "usage: %s [MODE] [snr_db|No] [trials] [N] [gap_s...]  (CHAN=AWGN|MPG|MPP|MPD)\n", argv[0]);
        return 1;
    }

    struct freedv *tx = freedv_open(mode);
    freedv_set_frames_per_burst(tx, 1);
    int nbytes = freedv_get_bits_per_modem_frame(tx) / 8;
    int bmax = freedv_get_n_tx_preamble_modem_samples(tx) + freedv_get_n_tx_modem_samples(tx) +
               freedv_get_n_tx_postamble_modem_samples(tx);
    double maxgap = 0.0;
    for (int g = 0; g < ngaps; g++)
    {
        double gs = argc > 5 ? atof(argv[5 + g]) : gaps_def[g];
        if (gs > maxgap) maxgap = gs;
    }
    int lead = (int)(LEAD_S * FS);
    int cap = lead + N * (bmax + (int)(maxgap * FS)) + 2 * bmax;
    short *mix = malloc(sizeof(short) * (size_t)cap);
    short *solo = malloc(sizeof(short) * (size_t)(lead + 3 * bmax));
    static short burst[MAX_N][65536];
    int blen[MAX_N];
    double bps[MAX_N];
    static unsigned char pay[MAX_N][4096];
    if (bmax > 65536) { fprintf(stderr, "burst too long\n"); return 1; }
    chanutil_t *chan = preset != CHAN_AWGN ? chanutil_open(preset, (float)level, 1234u) : NULL;
    if (preset != CHAN_AWGN && !chan) { fprintf(stderr, "chanutil_open failed\n"); return 1; }

    printf("%s x%d, burst %.2f s, %s %s %.1f, %d trials, train decoder after a frame: %s\n", pname, N,
           bmax / FS, cname, preset == CHAN_AWGN ? "SNR3k" : "No", level, trials, after);
    printf("   gap_s   train   alone   per-position (train/alone)   SNR3k\n");

    for (int g = 0; g < ngaps; g++)
    {
        double gs = argc > 5 ? atof(argv[5 + g]) : gaps_def[g];
        int gap = (int)(gs * FS), tr_tot = 0, al_tot = 0;
        int tr_pos[MAX_N] = {0}, al_pos[MAX_N] = {0};
        double snr_acc = 0.0;
        for (int t = 0; t < trials; t++)
        {
            int n = lead;
            double ps = 0.0;
            memset(mix, 0, sizeof(short) * (size_t)cap);
            for (int k = 0; k < N; k++)
            {
                for (int i = 0; i < nbytes - 2; i++) pay[k][i] = (unsigned char)(urand() * 256.0);
                blen[k] = make_burst(tx, pay[k], nbytes, burst[k], &bps[k]);
                ps += bps[k];
                if (k) n += gap;
                memcpy(mix + n, burst[k], sizeof(short) * (size_t)blen[k]);
                n += blen[k];
            }
            n += 2 * bmax;
            ps /= N;
            float snr3k;
            apply_channel(chan, mix, n, level, ps, &snr3k);
            snr_acc += snr3k;

            int got_t[MAX_N] = {0};
            run_rx(mode, mix, n, pay, nbytes, N, got_t);
            for (int k = 0; k < N; k++)
            {
                int got_a[1] = {0}, m = lead + blen[k] + 2 * bmax;
                float dummy;
                memset(solo, 0, sizeof(short) * (size_t)m);
                memcpy(solo + lead, burst[k], sizeof(short) * (size_t)blen[k]);
                apply_channel(chan, solo, m, level, bps[k], &dummy);
                run_rx(mode, solo, m, pay + k, nbytes, 1, got_a);
                tr_pos[k] += got_t[k]; tr_tot += got_t[k];
                al_pos[k] += got_a[0]; al_tot += got_a[0];
            }
        }
        printf("  %6.2f  %3d/%-3d %3d/%-3d  ", gs, tr_tot, trials * N, al_tot, trials * N);
        for (int k = 0; k < N; k++) printf(" %d/%d", tr_pos[k], al_pos[k]);
        printf("   %+.1f\n", snr_acc / trials);
        fflush(stdout);
    }
    if (chan) chanutil_close(chan);
    return 0;
}
