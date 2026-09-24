/* resync_gap_probe — does the payload decoder hear a data burst that follows a
 * control burst by `gap` seconds?
 *
 * The receive side of an ARQ turn: the peer sends a DATAC16 control frame
 * (ACK/TURN), unkeys for the guard, and keys its payload frame.  This station
 * runs a payload decoder over the SAME continuous audio the whole time, as
 * modem.c's dual-plane receiver does, and never transmits in between -- so no
 * TX->RX flush resets anything.
 *
 * Three receivers per trial, all fed identical noisy audio:
 *
 *   alone   the payload burst with nothing before it but noise: the ceiling
 *   plain   control burst, gap, payload burst; the payload decoder left alone
 *   resync  as plain, but freedv_set_sync(UNSYNC) on the payload decoder the
 *           moment the control frame has fully arrived -- what modem.c does
 *           when its control plane decodes a DATAC16 frame (issue #223's fix)
 *
 * If `resync` fails where `plain` and `alone` deliver, the resync costs the
 * burst: UNSYNC zeroes freedv's rxbuf, and the payload decoder needs that
 * buffer refilled with real audio before its preamble search works again.
 *
 * usage: resync_gap_probe [PAYLOAD_MODE] [snr_db] [trials] [gap_s...]
 *        defaults: DATAC17 11 20  0.5 0.9 1.5 2.5 4 6 8
 *
 * Environment:
 *   RESYNC_DELAY_S=s  apply the resync s seconds after the control burst ends,
 *                     as a late-running payload thread would
 *   SHARED_RX=1       one decoder instance for every run instead of a fresh
 *                     one each, to expose state that UNSYNC does not reset
 *   CLIP_S=s          silence the first s seconds of the payload burst, as a
 *                     transmitter that keys late would
 *
 * What it established (DATAC17, 11 dB, 0.9 s gap): a resync that lands before
 * the payload preamble is harmless (10/10); one that lands after the preamble
 * starts kills that burst (0/10).  So an ACK and a data burst can share one
 * keydown only if the peer's resync lands in the gap between them -- which the
 * ARQ_ACK_DATA_GAP_MS silence provides.  Clipping >= 100 ms off the start of a
 * burst kills every mode alike (DATAC16 included).
 *
 * AWGN, SNR3k convention as in acquire_vs_decode.c.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freedv_api.h"

#define FS 8000.0
#define HEADROOM_PEAK 4000.0
#define LEAD_S 3.0              /* noise before the first burst */

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

/* One burst (preamble + frame + postamble) with a valid CRC, peak-normalised
 * to HEADROOM_PEAK.  Returns its length; *ps_out = mean signal power. */
static int make_burst(struct freedv *tx, short *out, double *ps_out)
{
    int nbytes = freedv_get_bits_per_modem_frame(tx) / 8;
    unsigned char *p = malloc((size_t)nbytes);
    for (int i = 0; i < nbytes - 2; i++) p[i] = (unsigned char)(urand() * 256.0);
    unsigned short crc = freedv_gen_crc16(p, nbytes - 2);
    p[nbytes - 2] = (unsigned char)(crc >> 8);
    p[nbytes - 1] = (unsigned char)(crc & 0xff);

    int n = freedv_rawdatapreambletx(tx, out);
    freedv_rawdatatx(tx, out + n, p);
    n += freedv_get_n_tx_modem_samples(tx);
    n += freedv_rawdatapostambletx(tx, out + n);
    free(p);

    double pk = 0.0;
    for (int i = 0; i < n; i++) if (fabs((double)out[i]) > pk) pk = fabs((double)out[i]);
    double g = pk > 0.0 ? HEADROOM_PEAK / pk : 1.0, ps = 0.0;
    for (int i = 0; i < n; i++) { out[i] = (short)lrint(out[i] * g); ps += (double)out[i] * out[i]; }
    *ps_out = ps / n;
    return n;
}

/* Feed `a[0..n)` to rx honouring nin; UNSYNC once when `resync_at` is crossed
 * (-1: never).  Returns 1 if a CRC-clean frame came out at or after `from`. */
static int run_rx(struct freedv *rx, const short *a, int n, int resync_at, int from)
{
    int pos = 0, got = 0, done_resync = 0;
    unsigned char out[4096];
    short chunk[16384];

    freedv_set_sync(rx, FREEDV_SYNC_UNSYNC);
    while (pos < n)
    {
        int nin = freedv_nin(rx);
        if (nin <= 0 || nin > (int)(sizeof(chunk) / sizeof(chunk[0])) || pos + nin > n) break;
        memcpy(chunk, a + pos, sizeof(short) * (size_t)nin);
        pos += nin;
        int nb = (int)freedv_rawdatarx(rx, out, chunk);
        if (nb > 0 && !(freedv_get_rx_status(rx) & FREEDV_RX_BIT_ERRORS) && pos >= from)
            got = 1;
        if (!done_resync && resync_at >= 0 && pos >= resync_at)
        {
            freedv_set_sync(rx, FREEDV_SYNC_UNSYNC);
            done_resync = 1;
        }
    }
    return got;
}

int main(int argc, char **argv)
{
    const char *pname = argc > 1 ? argv[1] : "DATAC17";
    double snr  = argc > 2 ? atof(argv[2]) : 11.0;
    int trials  = argc > 3 ? atoi(argv[3]) : 20;
    double gaps_def[] = {0.5, 0.9, 1.5, 2.5, 4.0, 6.0, 8.0};
    int ngaps = argc > 4 ? argc - 4 : (int)(sizeof(gaps_def) / sizeof(gaps_def[0]));
    /* RESYNC_DELAY_S: apply the resync this long after the control burst
     * ends, as a late-running payload thread would. */
    int resync_delay = getenv("RESYNC_DELAY_S") ? (int)(atof(getenv("RESYNC_DELAY_S")) * FS) : 0;
    int shared = getenv("SHARED_RX") != NULL;
    int clip = getenv("CLIP_S") ? (int)(atof(getenv("CLIP_S")) * FS) : 0;
    int pmode = mode_from_name(pname);
    if (pmode < 0 || trials <= 0) { fprintf(stderr, "usage: %s [MODE] [snr] [trials] [gap_s...]\n", argv[0]); return 1; }

    struct freedv *ctx = freedv_open(FREEDV_MODE_DATAC16);
    struct freedv *ptx = freedv_open(pmode);
    struct freedv *prx = freedv_open(pmode);
    if (!ctx || !ptx || !prx) { fprintf(stderr, "freedv_open failed\n"); return 1; }
    freedv_set_frames_per_burst(ctx, 1);
    freedv_set_frames_per_burst(ptx, 1);
    freedv_set_frames_per_burst(prx, 1);

    int cmax = freedv_get_n_tx_preamble_modem_samples(ctx) + freedv_get_n_tx_modem_samples(ctx) +
               freedv_get_n_tx_postamble_modem_samples(ctx);
    int pmax = freedv_get_n_tx_preamble_modem_samples(ptx) + freedv_get_n_tx_modem_samples(ptx) +
               freedv_get_n_tx_postamble_modem_samples(ptx);
    int lead = (int)(LEAD_S * FS), maxgap = 0;
    for (int g = 0; g < ngaps; g++)
    {
        double gs = argc > 4 ? atof(argv[4 + g]) : gaps_def[g];
        if ((int)(gs * FS) > maxgap) maxgap = (int)(gs * FS);
    }
    int cap = lead + cmax + maxgap + pmax + 2 * pmax;
    short *cb = malloc(sizeof(short) * (size_t)cmax), *pb = malloc(sizeof(short) * (size_t)pmax);
    short *mix = malloc(sizeof(short) * (size_t)cap), *solo = malloc(sizeof(short) * (size_t)cap);

    printf("payload %s (burst %.2f s) after DATAC16 (burst %.2f s), SNR3k %+.1f dB, %d trials\n",
           pname, pmax / FS, cmax / FS, snr, trials);
    printf("   gap_s    alone    plain   resync\n");

    for (int g = 0; g < ngaps; g++)
    {
        double gs = argc > 4 ? atof(argv[4 + g]) : gaps_def[g];
        int gap = (int)(gs * FS), a_ok = 0, p_ok = 0, r_ok = 0;

        for (int t = 0; t < trials; t++)
        {
            double psc, psp;
            int nc = make_burst(ctx, cb, &psc);
            int np = make_burst(ptx, pb, &psp);
            /* CLIP_S: silence the first CLIP_S seconds of the payload burst, as
             * a transmitter that keys late would. */
            for (int i = 0; i < clip && i < np; i++) pb[i] = 0;
            /* One noise level for the whole stream, set by the payload burst:
             * the station hears both bursts from the same peer at the same
             * level, so give them the same power first. */
            double gc = sqrt(psp / psc);
            for (int i = 0; i < nc; i++) cb[i] = (short)lrint(cb[i] * gc);
            double sigma = sqrt(psp / (pow(10.0, snr / 10.0) * (3000.0 / (FS / 2.0))));

            int pstart = lead + nc + gap, n = pstart + np + 2 * pmax;
            for (int i = 0; i < n; i++)
            {
                double v = sigma * gauss(), s = v;
                if (i >= lead && i < lead + nc) v += cb[i - lead];
                if (i >= pstart && i < pstart + np) { v += pb[i - pstart]; s += pb[i - pstart]; }
                mix[i]  = (short)lrint(fmax(-32768.0, fmin(32767.0, v)));
                solo[i] = (short)lrint(fmax(-32768.0, fmin(32767.0, s)));
            }

            /* A fresh decoder per run unless SHARED_RX is set: UNSYNC does not
             * return a freedv instance to its initial state, so one run can
             * leak into the next. */
            struct freedv *r1 = shared ? prx : freedv_open(pmode);
            struct freedv *r2 = shared ? prx : freedv_open(pmode);
            struct freedv *r3 = shared ? prx : freedv_open(pmode);
            freedv_set_frames_per_burst(r1, 1);
            freedv_set_frames_per_burst(r2, 1);
            freedv_set_frames_per_burst(r3, 1);
            a_ok += run_rx(r1, solo, n, -1, pstart);
            p_ok += run_rx(r2, mix, n, -1, pstart);
            r_ok += run_rx(r3, mix, n, lead + nc + resync_delay, pstart);
            if (!shared) { freedv_close(r1); freedv_close(r2); freedv_close(r3); }
        }
        printf("  %6.2f   %3d/%-3d  %3d/%-3d  %3d/%-3d\n", gs, a_ok, trials, p_ok, trials, r_ok, trials);
        fflush(stdout);
    }
    return 0;
}
