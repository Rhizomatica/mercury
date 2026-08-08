/* acquire_vs_decode — for a freedv mode, is the fringe floor the PREAMBLE
 * DETECTOR or the DECODER?
 *
 * The distinction decides where robustness work should go, and it is easy to
 * assert from an energy budget and get wrong: a frame's implied Eb/N0 looks
 * damningly high until you subtract cyclic prefix, pilot carriers,
 * channel-estimation loss and short-code loss, each worth a dB or more.  This
 * measures it instead.
 *
 * Per burst it records two independent outcomes:
 *
 *   acquired  - the demodulator reached FREEDV_RX_SYNC at some point
 *   delivered - a frame came out with the CRC intact
 *
 * If delivered == acquired all the way down, the mode is acquisition-limited:
 * everything that syncs decodes, and a better code buys nothing.  If bursts
 * acquire but fail the CRC, it is decode-limited and the code (or the LLRs
 * feeding it) is the lever.  docs/MFSK-PORT.md established the former for the
 * MFSK mode by exactly this test; nothing had established it either way for
 * the DATAC OFDM modes.
 *
 * AWGN only, and deliberately so: fading confounds the two (a fade during the
 * preamble costs acquisition, a fade during the payload costs the decode), so
 * the clean question is asked on the clean channel first.
 *
 * SNR is reported as SNR3k, the project's convention: noise power measured in
 * a 3 kHz reference bandwidth, i.e. for real samples at fs, the full-band
 * noise variance scaled by 3000/(fs/2).
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

/* Peak the burst is normalised to before noise is added, leaving the rest of
 * the int16 range as noise headroom.  ~18 dB below full scale. */
#define HEADROOM_PEAK 4000.0

static unsigned long long rng = 1357911131517ULL;

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
    if (!strcmp(s, "DATAC16")) return FREEDV_MODE_DATAC16;
    if (!strcmp(s, "DATAC15")) return FREEDV_MODE_DATAC15;
    if (!strcmp(s, "DATAC13")) return FREEDV_MODE_DATAC13;
    if (!strcmp(s, "DATAC4"))  return FREEDV_MODE_DATAC4;
    if (!strcmp(s, "DATAC3"))  return FREEDV_MODE_DATAC3;
    if (!strcmp(s, "DATAC1"))  return FREEDV_MODE_DATAC1;
    if (!strcmp(s, "DATAC0"))  return FREEDV_MODE_DATAC0;
    return -1;
}

int main(int argc, char **argv)
{
    const char *modename = (argc > 1) ? argv[1] : "DATAC16";
    int    trials  = (argc > 2) ? atoi(argv[2]) : 100;
    double snr_lo  = (argc > 3) ? atof(argv[3]) : -14.0;
    double snr_hi  = (argc > 4) ? atof(argv[4]) : -4.0;
    /* Optional 5th arg: a fading preset.  Under fading the delivered SNR is an
     * OUTPUT of the channel, so the sweep axis becomes noise density and the
     * achieved SNR3k is measured and reported per row. */
    int    chan    = (argc > 5) ? chanutil_preset_from_name(argv[5]) : CHAN_AWGN;
    if (chan < 0) { fprintf(stderr, "unknown channel '%s' (awgn|mpg|mpp|mpd)\n", argv[5]); return 1; }

    int mode = mode_from_name(modename);
    if (mode < 0 || trials <= 0)
    {
        fprintf(stderr, "usage: %s [MODE] [trials] [snr_lo] [snr_hi]\n"
                        "  MODE: DATAC0/1/3/4/13/15/16 (default DATAC16)\n", argv[0]);
        return 1;
    }

    struct freedv *tx = freedv_open(mode);
    struct freedv *rx = freedv_open(mode);
    if (!tx || !rx) { fprintf(stderr, "freedv_open failed\n"); return 1; }
    freedv_set_frames_per_burst(tx, 1);
    freedv_set_frames_per_burst(rx, 1);

    int nbytes  = freedv_get_bits_per_modem_frame(tx) / 8;
    int ntx     = freedv_get_n_tx_modem_samples(tx);
    int npre    = freedv_get_n_tx_preamble_modem_samples(tx);
    int npost   = freedv_get_n_tx_postamble_modem_samples(tx);
    int nburst  = npre + ntx + npost;

    printf("%s: %d byte frame, burst %d samples (%.2f s at %.0f Hz)\n",
           modename, nbytes, nburst, nburst / FS, FS);
    printf("channel: %s\n", chanutil_preset_name(chan));
    printf("acquired = reached FREEDV_RX_SYNC;  delivered = frame out with CRC ok\n\n");
    if (chan == CHAN_AWGN)
        printf("  SNR3k    acquired   delivered   decoded|acquired\n");
    else
        printf("   No     SNR3k(meas)  acquired   delivered\n");

    short *burst = malloc(sizeof(short) * (size_t)(nburst + 4));
    unsigned char *payload = malloc((size_t)nbytes);
    unsigned char *out     = malloc((size_t)nbytes + 8);
    if (!burst || !payload || !out) return 1;

    for (double snr = snr_lo; snr <= snr_hi + 0.01; snr += 1.0)
    {
        int acquired = 0, delivered = 0;
        double snr_meas_sum = 0.0; int snr_meas_cnt = 0;
        long clipped = 0, tot_samp = 0;

        for (int t = 0; t < trials; t++)
        {
            /* The raw-data API does not generate the CRC: the caller owns the
             * last two bytes of the frame, exactly as modem.c does.  Without
             * this every burst decodes and every burst is then rejected, which
             * looks precisely like a mode that never delivers. */
            for (int i = 0; i < nbytes - 2; i++)
                payload[i] = (unsigned char)(urand() * 256.0);
            unsigned short crc16 = freedv_gen_crc16(payload, nbytes - 2);
            payload[nbytes - 2] = (unsigned char)(crc16 >> 8);
            payload[nbytes - 1] = (unsigned char)(crc16 & 0xff);

            int n = 0;
            n += freedv_rawdatapreambletx(tx, burst + n);
            freedv_rawdatatx(tx, burst + n, payload);
            n += ntx;
            n += freedv_rawdatapostambletx(tx, burst + n);

            /* Signal power over the burst, then the noise sigma that puts the
             * 3 kHz-referenced SNR where we want it. */
            double ps = 0.0, pk = 0.0;
            for (int i = 0; i < n; i++)
            {
                ps += (double)burst[i] * burst[i];
                if (fabs((double)burst[i]) > pk) pk = fabs((double)burst[i]);
            }
            ps /= n;
            /* SNR3k = ps / (sigma^2 * 3000/(FS/2)) */
            double sigma = sqrt(ps / (pow(10.0, snr / 10.0) * (3000.0 / (FS / 2.0))));

            /* HEADROOM.  freedv emits DATAC16 at rms ~8000 / peak ~16400, so at
             * fringe SNRs -- where the noise rms is a couple of times the
             * signal -- signal+noise runs straight into the int16 rail.
             * Measured 23 % of samples clipped, which quietly delivered
             * -7.1 dB when -9.0 was asked for, and cost more in distortion
             * than the extra SNR gave back.
             *
             * Normalise the burst to a fixed low level and leave the rest of
             * the int16 range for noise.  Deliberately NOT a gain derived from
             * the noise level: that feeds back (the gain changes the noise
             * setting, which changes the gain) and pinned the measured SNR
             * regardless of what was asked for. */
            {
                double gnorm = (pk > 0.0) ? (HEADROOM_PEAK / pk) : 1.0;
                for (int i = 0; i < n; i++)
                    burst[i] = (short)lrint((double)burst[i] * gnorm);
                ps    *= gnorm * gnorm;
                sigma *= gnorm;
            }

            if (chan == CHAN_AWGN)
            {
                for (int i = 0; i < n; i++)
                {
                    double v = burst[i] + sigma * gauss();
                    if (v >  32767.0) v =  32767.0;
                    else if (v < -32768.0) v = -32768.0;
                    else goto no_clip;
                    clipped++;
                no_clip:
                    burst[i] = (short)lrint(v);
                }
            }
            else
            {
                /* `snr` is the noise density No here; the channel adds its own
                 * noise on top of the already-normalised burst. */
                float meas = 0.0f;
                chanutil_fade((int16_t *)burst, n, chan, (float)snr,
                              (unsigned)(t + 1), &meas);
                for (int i = 0; i < n; i++)
                    if (burst[i] >= 32767 || burst[i] <= -32768) clipped++;
                snr_meas_sum += meas;
                snr_meas_cnt++;
                /* The tail is plain noise at a comparable level. */
                sigma = sqrt(ps / (pow(10.0, meas / 10.0) * (3000.0 / (FS / 2.0))));
            }

            /* Feed the burst plus a tail of pure noise, honouring nin. */
            freedv_set_sync(rx, FREEDV_SYNC_UNSYNC);
            int pos = 0, saw_sync = 0, got = 0;
            int tail = nburst;                 /* let sync/decode complete */
            while (pos < n + tail)
            {
                int nin = freedv_nin(rx);
                short chunk[8192];
                if (nin > (int)(sizeof(chunk) / sizeof(chunk[0]))) break;
                for (int i = 0; i < nin; i++)
                {
                    if (pos + i < n) chunk[i] = burst[pos + i];
                    else {
                        double v = sigma * gauss();
                        if (v >  32767.0) v =  32767.0;
                        if (v < -32768.0) v = -32768.0;
                        chunk[i] = (short)lrint(v);
                    }
                }
                pos += nin;

                int nb = (int)freedv_rawdatarx(rx, out, chunk);
                int st = freedv_get_rx_status(rx);
                /* Full sync only.  TRIAL_SYNC fires on noise at deep SNR, so
                 * counting it reports false alarms as acquisitions and makes
                 * the mode look acquisition-limited when it is not. */
                if (st & FREEDV_RX_SYNC) saw_sync = 1;
                if (nb > 0 && !(st & FREEDV_RX_BIT_ERRORS)) got = 1;
            }

            tot_samp += n;
            if (saw_sync) acquired++;
            if (got)      delivered++;
        }

        if (chan == CHAN_AWGN)
        {
            printf("  %+5.1f     %3d/%-3d     %3d/%-3d      %s  clip %.2f%%\n",
                   snr, acquired, trials, delivered, trials,
                   acquired ? (delivered == acquired ? "1.00 (all)" : "") : "-",
                   tot_samp ? 100.0 * (double)clipped / (double)tot_samp : 0.0);
            if (delivered != acquired && acquired)
                printf("            ^ %d burst(s) acquired but failed the CRC\n",
                       acquired - delivered);
        }
        else
        {
            printf("  %+6.1f    %+6.2f     %3d/%-3d     %3d/%-3d   clip %.2f%%\n",
                   snr, snr_meas_cnt ? snr_meas_sum / snr_meas_cnt : 0.0,
                   acquired, trials, delivered, trials,
                   tot_samp ? 100.0 * (double)clipped / (double)tot_samp : 0.0);
        }
    }

    freedv_close(tx);
    freedv_close(rx);
    free(burst); free(payload); free(out);
    return 0;
}
