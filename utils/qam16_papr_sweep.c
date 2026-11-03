/* qam16_papr_sweep — is QAM16C2's unclipped ~10 dB PAPR the reason field
 * radios fall to DATAC17, and does enabling the (already built) Hilbert
 * clipper buy the top rung back on peak-limited transmitters?
 *
 * MODES.md: qam16c2 runs unclipped at PAPR ≈ 10 dB, so a peak-limited rig
 * delivers ~5 dB less average power than the clipped QPSK modes at the same
 * peak — its +15.7 dB bench operating point implicitly assumes headroom real
 * radios do not have.  The clip machinery (clip_en, clip_gain1/gain2) ships
 * in ofdm_mode.c for every mode but is disabled for qam16c2.  This tool
 * measures what happens at EQUAL PEAK POWER -- the peak-limited-radio model
 * by construction: every variant's burst is normalised to one common peak,
 * and SNR3k is derived from the resulting average power, so a variant's PAPR
 * advantage or deficit lands directly on the SNR axis.
 *
 * Per variant it measures, per SNR point:
 *
 *   del      - bursts delivered with CRC intact
 *   acq      - bursts that reached full RX_SYNC
 *   snr_est  - mean freedv modem SNR estimate (synced trials): the carousel's
 *              rung decisions read this number (see #350, OTA-PHASE-A-SNR-
 *              CALIB.md), so a clipped constellation must not bias it
 *
 * and per variant once: the PAPR of the raw transmitter output.
 *
 * Variants are generated IN-CHAIN by freedv_set_tx_clip_config -- the same
 * ofdm_hilbert_clipper path a configured transmitter would run (drive gain,
 * hard clip, TX BPF, peak-restore gain).  The preamble and postamble pass
 * through the same live chain at emit time, so clipping shapes them too.
 * The stock variant is always row one; payloads and noise are re-seeded per
 * variant, so row differences are the clip config, not the channel.
 *
 * Channels: awgn, or any chanutil preset name.  For the MODES.md "MPP"
 * channel (ch simulator, 1 Hz Doppler / 2 ms spread) use ccir-poor, the
 * Watterson preset with those parameters; chanutil's legacy "mpp" Butterworth
 * preset is a different model and NOT the MODES.md MPP.
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
/* Common peak every variant is normalised to before noise is added. */
#define COMMON_PEAK 4000.0

struct mode_def {
  const char *name;
  int freedv_mode;
  float amp_scale; /* the mode's ofdm_mode.c value; kept for grid variants */
};

static const struct mode_def modes[] = {
    {"qam16c2", FREEDV_MODE_QAM16C2, 135E3},
    {"datac17", FREEDV_MODE_DATAC17, 145E3},
    {NULL, 0, 0.0f},
};

static unsigned long long rng = 1357911131517ULL;

static void seed_rng(unsigned long long s) { rng = s; }

static double urand(void) {
  rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
  return (double)((rng >> 11) & 0x1FFFFFFFFFFFFFULL) / (double)0x20000000000000ULL;
}

static double gauss(void) {
  double u1 = urand(), u2 = urand();
  if (u1 < 1e-300) u1 = 1e-300;
  return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static const struct mode_def *mode_from_name(const char *s) {
  for (const struct mode_def *m = modes; m->name; m++)
    if (!strcmp(s, m->name)) return m;
  return NULL;
}

/* Generate one transmit burst (preamble + packet + postamble) with the tx
 * handle's current clip config, and report its raw PAPR statistics.  Payload
 * carries a caller-supplied CRC as modem.c does.  Returns the burst length. */
static int make_burst(struct freedv *tx, short *burst, unsigned char *payload,
                      int nbytes, double *papr_db, double *rms, double *pk_out) {
  for (int i = 0; i < nbytes - 2; i++) payload[i] = (unsigned char)(urand() * 256.0);
  unsigned short crc16 = freedv_gen_crc16(payload, nbytes - 2);
  payload[nbytes - 2] = (unsigned char)(crc16 >> 8);
  payload[nbytes - 1] = (unsigned char)(crc16 & 0xff);

  int n = 0;
  n += freedv_rawdatapreambletx(tx, burst + n);
  freedv_rawdatatx(tx, burst + n, payload);
  n += freedv_get_n_tx_modem_samples(tx);
  n += freedv_rawdatapostambletx(tx, burst + n);

  double ps = 0.0, pk = 0.0;
  for (int i = 0; i < n; i++) {
    double s = (double)burst[i];
    ps += s * s;
    if (fabs(s) > pk) pk = fabs(s);
  }
  ps /= n;
  *pk_out = pk;
  *rms = sqrt(ps);
  *papr_db = (ps > 0.0) ? 10.0 * log10(pk * pk / ps) : 0.0;
  return n;
}

/* One burst through the receiver; full-sync and CRC-ok counted as in
 * acquire_vs_decode.  snr_est is collected only on synced trials. */
static void run_burst(struct freedv *rx, const short *samples, int n, int tail,
                      double sigma, unsigned char *out, int *saw_sync,
                      int *got_frame, float *snr_est_sum, int *snr_est_n) {
  freedv_set_sync(rx, FREEDV_SYNC_UNSYNC);
  int pos = 0;
  *saw_sync = 0;
  *got_frame = 0;
  while (pos < n + tail) {
    int nin = freedv_nin(rx);
    short chunk[8192];
    if (nin > (int)(sizeof(chunk) / sizeof(chunk[0]))) break;
    for (int i = 0; i < nin; i++) {
      if (pos + i < n) {
        chunk[i] = samples[pos + i];
      } else {
        double v = sigma * gauss();
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        chunk[i] = (short)lrint(v);
      }
    }
    pos += nin;

    int nb = (int)freedv_rawdatarx(rx, out, chunk);
    int st = freedv_get_rx_status(rx);
    if (st & FREEDV_RX_SYNC) {
      *saw_sync = 1;
      if (snr_est_sum && snr_est_n) {
        int sync_i = 0;
        float se = 0.0f;
        freedv_get_modem_stats(rx, &sync_i, &se);
        *snr_est_sum += se;
        (*snr_est_n)++;
      }
    }
    if (nb > 0 && !(st & FREEDV_RX_BIT_ERRORS)) *got_frame = 1;
  }
}

int main(int argc, char **argv) {
  const char *modename = (argc > 1) ? argv[1] : "qam16c2";
  int trials = (argc > 2) ? atoi(argv[2]) : 20;
  double snr_lo = (argc > 3) ? atof(argv[3]) : 2.0;
  double snr_hi = (argc > 4) ? atof(argv[4]) : 18.0;
  const char *channame = (argc > 5) ? argv[5] : "awgn";
  float g1_list[8], g2_list[8];
  int ng1 = 0, ng2 = 0;
  if (argc > 6) {
    char *copy = strdup(argv[6]), *p = copy;
    while (p && ng1 < 8) {
      char *c = strchr(p, ',');
      if (c) *c = 0;
      g1_list[ng1++] = (float)atof(p);
      p = c ? c + 1 : NULL;
    }
    free(copy);
  }
  if (argc > 7) {
    char *copy = strdup(argv[7]), *p = copy;
    while (p && ng2 < 8) {
      char *c = strchr(p, ',');
      if (c) *c = 0;
      g2_list[ng2++] = (float)atof(p);
      p = c ? c + 1 : NULL;
    }
    free(copy);
  }
  double step = (argc > 8) ? atof(argv[8]) : 1.0;

  const struct mode_def *md = mode_from_name(modename);
  if (md == NULL || trials <= 0 || step <= 0.0) {
    fprintf(stderr,
            "usage: %s [qam16c2|datac17] [trials] [snr_lo] [snr_hi] "
            "[channel] [gain1_list] [gain2_list] [step]\n"
            "  channel: awgn (default), ccir-poor (= MODES.md MPP), or any\n"
            "           chanutil/watterson preset name\n"
            "  gain lists: comma-separated clip_gain1 x clip_gain2 grid to\n"
            "           run besides the stock (unclipped) variant\n",
            argv[0]);
    return 1;
  }

  int chan = CHAN_AWGN;
  if (strcmp(channame, "awgn") != 0) {
    chan = chanutil_preset_from_name(channame);
    if (chan < 0) {
      fprintf(stderr, "unknown channel '%s'\n", channame);
      return 1;
    }
  }

  struct freedv *tx = freedv_open(md->freedv_mode);
  struct freedv *rx = freedv_open(md->freedv_mode);
  if (!tx || !rx) {
    fprintf(stderr, "freedv_open failed\n");
    return 1;
  }
  freedv_set_frames_per_burst(tx, 1);
  freedv_set_frames_per_burst(rx, 1);

  int nbytes = freedv_get_bits_per_modem_frame(tx) / 8;
  int npre = freedv_get_n_tx_preamble_modem_samples(tx);
  int ntx = freedv_get_n_tx_modem_samples(tx);
  int npost = freedv_get_n_tx_postamble_modem_samples(tx);
  int nburst = npre + ntx + npost;
  int tail = nburst;

  short *burst = malloc(sizeof(short) * (size_t)(nburst + 4));
  unsigned char *payload = malloc((size_t)nbytes);
  unsigned char *out = malloc((size_t)nbytes + 8);
  if (!burst || !payload || !out) return 1;

  /* Variant list: stock first, then the gain1 x gain2 grid. */
  int nvar = 1 + ng1 * ng2;
  printf("%s: %d byte frame, burst %.2f s; %d variant(s), channel %s\n", modename,
         nbytes, (double)nburst / FS, nvar,
         (chan == CHAN_AWGN) ? "awgn" : chanutil_preset_name(chan));
  printf("trials %d/point, equal-peak protocol (common peak %.0f)\n\n", trials,
         COMMON_PEAK);

  double papr_stock = 0.0;
  /* Watterson noise convention (common/watterson.c, ch.c-compatible):
   * achieved SNR3k = -No + k, where k depends on the burst's own power
   * through the channel's unit-gain normalisation.  The stock variant's k
   * is calibrated once from its first burst (chanutil_run at No=-30, near
   * noiseless), and every variant is then run at No = k_stock - axis: at
   * the same radio setting each variant's own average power sets its
   * achieved SNR, which the channel MEASURES per burst and the rows report.
   * Same construction as mfsk_channel_sweep.sh's No = -(SNR3k) - n. */
  double k_stock = 0.0;

  for (int iv = 0; iv < nvar; iv++) {
    seed_rng(1357911131517ULL);

    if (iv == 0) {
      printf("== %s stock (tx config untouched)\n", modename);
    } else {
      int ig1 = (iv - 1) / (ng2 ? ng2 : 1);
      int ig2 = (iv - 1) % (ng2 ? ng2 : 1);
      freedv_set_tx_clip_config(tx, true, g1_list[ig1], g2_list[ig2],
                                md->amp_scale);
      printf("== %s clipped: gain1 %.2f gain2 %.2f (amp_scale %.0f)\n", modename,
             (double)g1_list[ig1], (double)g2_list[ig2], (double)md->amp_scale);
    }
    fflush(stdout);

    /* PAPR of the raw transmitter output (first burst; pure scale-invariant).
     * Then normalise EVERY variant to the same common peak: the
     * peak-limited radio model. */
    double papr = 0, rms = 0, pk = 0;
    int n = make_burst(tx, burst, payload, nbytes, &papr, &rms, &pk);
    if (iv == 0) papr_stock = papr;
    /* Calibrate the stock variant's k once (fading channels only): its
     * first burst, equal-peak normalised, through the channel at No=-30
     * (near noiseless); k = meas + 30. */
    if (iv == 0 && chan != CHAN_AWGN) {
      double gnorm = (pk > 0.0) ? (COMMON_PEAK / pk) : 1.0;
      for (int i = 0; i < n; i++)
        burst[i] = (short)lrint((double)burst[i] * gnorm);
      chanutil_t *ck = chanutil_open(chan, -30.0f, 1234u);
      if (!ck) {
        fprintf(stderr, "chanutil_open failed (calibration)\n");
        return 1;
      }
      float m0 = 0.0f;
      chanutil_run(ck, burst, n, &m0);
      chanutil_close(ck);
      /* measured SNR = A - No, so the burst's level constant is
       * A = meas0 + No_ref = meas0 - 30; the axis then maps to
       * No = A - axis.  chanutil's measurement is the true achieved SNR
       * (it verifies to 0.01 dB against direct AWGN), so the mapping is
       * exact for this burst and papr-correct for the others at equal
       * peak. */
      k_stock = (double)m0 - 30.0;
      printf("   noise calibration: A = %.1f dB (axis -> No = A - axis)\n",
             k_stock);
    }
    /* Peak-limited-radio noise model: at the same radio peak the
     * environment adds the SAME noise to every variant, so the clipped
     * variant's higher average power RAISES its achieved SNR.  The axis is
     * the radio setting, expressed as the stock variant's SNR3k; each
     * variant's achieved SNR3k = axis + (papr_stock - papr_variant). */
    double papr_gain_db = papr_stock - papr;
    printf("   PAPR %.2f dB  (raw rms %.0f peak %.0f)%s\n", papr, rms, pk,
           iv ? "" : "   <- axis reference");
    printf("   SNR3k(radio) SNR3k(achieved)  del/trials acq/trials  snr_est(synced)\n");

    for (double snr = snr_lo; snr <= snr_hi + 0.001; snr += step) {
      chanutil_t *ch = NULL;
      if (chan != CHAN_AWGN) {
        /* No = k_stock - axis: every variant sees the same channel noise at
         * the same radio setting; its own average power decides its
         * achieved SNR, which chanutil measures and the row reports. */
        ch = chanutil_open(chan, (float)(k_stock - snr), 1234u);
        if (!ch) {
          fprintf(stderr, "chanutil_open failed\n");
          return 1;
        }
      }

      int delivered = 0, acquired = 0;
      float snr_est_sum = 0.0f;
      int snr_est_n = 0;
      double meas_sum = 0.0;
      int meas_n = 0;
      short *noisy = malloc(sizeof(short) * (size_t)(n + 4));
      if (!noisy) return 1;

      for (int t = 0; t < trials; t++) {
        double papr_t, rms_t, pk_t;
        n = make_burst(tx, burst, payload, nbytes, &papr_t, &rms_t, &pk_t);

        /* Equal-peak normalisation, then the noise sigma of the RADIO
         * SETTING: sigma follows the STOCK variant's power at the axis SNR,
         * so this variant's achieved SNR3k = axis + papr_gain_db.  Under
         * fading, the axis value is the noise density No and the achieved
         * SNR3k is measured by the channel. */
        double ps = rms_t * rms_t;
        double gnorm = (pk_t > 0.0) ? (COMMON_PEAK / pk_t) : 1.0;
        for (int i = 0; i < n; i++) burst[i] = (short)lrint((double)burst[i] * gnorm);
        ps *= gnorm * gnorm;

        double sigma;
        if (chan == CHAN_AWGN) {
          sigma = sqrt(ps / (pow(10.0, (snr + papr_gain_db) / 10.0) *
                             (3000.0 / (FS / 2.0))));
          for (int i = 0; i < n; i++) {
            double v = burst[i] + sigma * gauss();
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            noisy[i] = (short)lrint(v);
          }
        } else {
          /* chanutil adds noise inside the channel at No = k_stock - axis;
           * each variant's higher/lower average power moves its measured
           * SNR by its own papr gain.  The tail after the burst uses the
           * SNR the channel reports for THIS burst. */
          float m = 0.0f;
          chanutil_run(ch, burst, n, &m);
          meas_sum += m;
          meas_n++;
          memcpy(noisy, burst, sizeof(short) * (size_t)n);
          sigma = sqrt(ps / (pow(10.0, m / 10.0) * (3000.0 / (FS / 2.0))));
        }

        int saw, got;
        run_burst(rx, noisy, n, tail, sigma, out, &saw, &got, &snr_est_sum,
                  &snr_est_n);
        if (saw) acquired++;
        if (got) delivered++;
      }
      free(noisy);

      if (ch) chanutil_close(ch);

      if (chan == CHAN_AWGN) {
        printf("   %+5.1f       %+5.1f          %3d/%-3d    %3d/%-3d     %6.1f (%d synced)\n",
               snr, snr + papr_gain_db, delivered, trials, acquired, trials,
               snr_est_n ? snr_est_sum / snr_est_n : 0.0, snr_est_n);
      } else {
        printf("   axis %+5.1f (achieved = meas below)   %3d/%-3d    %3d/%-3d     %6.1f (%d synced)  meas %+6.1f\n",
               snr, delivered, trials, acquired, trials,
               snr_est_n ? snr_est_sum / snr_est_n : 0.0, snr_est_n,
               meas_n ? meas_sum / meas_n : 0.0);
      }
      fflush(stdout);
    }
    printf("\n");
  }

  freedv_close(tx);
  freedv_close(rx);
  free(burst);
  free(payload);
  free(out);
  return 0;
}
