/* offset_acq_sweep -- where does the acquisition search stop following a
 * frequency offset?
 *
 * The question the threshold sweep left open (docs/ACQ-TUNING-FINDINGS.md,
 * "Frequency offset"): the streaming acquisition searches a coarse grid of
 * frequency hypotheses and refines each by a bounded sweep
 * (modem/freedv/ofdm.c, ofdm_sync_search_stream and
 * est_freq_offset_pilot_corr), so the offset it can capture is a property
 * of the SEARCH, not of the acceptance threshold.  This tool measures that
 * property: for each applied offset it reports how often the receiver
 * acquired, and -- the part a threshold sweep cannot see -- what the search
 * estimated the offset to be, and what the acceptance metric was when it
 * said yes or no.
 *
 * The offset is applied with bench_freq_shift_real, the analytic shift that
 * models a mistuned SSB radio; utils/shift_selftest.c checks that
 * instrument against a closed-form tone (it found three defects in it, one
 * of them 6 dB of signal, which is why the earlier offset numbers were
 * wrong).
 *
 * Pairing: the streams are re-seeded per offset point, so every offset sees
 * the same payloads and the same noise -- a row's difference is the offset.
 *
 * Usage:
 *   offset_acq_sweep [MODE] [TRIALS] [SNR] [OFF_LO] [OFF_HI] [OFF_STEP]
 *                    [THRESH] [NOISE_TRIALS]
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_common.h"
#include "freedv_api.h"

#define FS 8000.0
#define HEADROOM_PEAK 4000.0
#define TAIL_SLACK 8192

static bench_rng rngA = {1357911131517ULL}; /* payloads, in-burst noise */
static bench_rng rngB = {1357911131517ULL + 1}; /* tails, control windows */

static double seq_noise(void *ctx) { return bench_seq_next((bench_seq *)ctx); }

static const char *mode_name_of(int freedv_mode) {
  switch (freedv_mode) {
    case FREEDV_MODE_DATAC16:
      return "DATAC16";
    case FREEDV_MODE_DATAC15:
      return "DATAC15";
    case FREEDV_MODE_DATAC14:
      return "DATAC14";
    case FREEDV_MODE_DATAC13:
      return "DATAC13";
    case FREEDV_MODE_DATAC4:
      return "DATAC4";
    case FREEDV_MODE_DATAC3:
      return "DATAC3";
    case FREEDV_MODE_DATAC1:
      return "DATAC1";
    case FREEDV_MODE_DATAC0:
      return "DATAC0";
    case FREEDV_MODE_DATAC17:
      return "DATAC17";
    case FREEDV_MODE_QAM16C2:
      return "QAM16C2";
    default:
      return "?";
  }
}

int main(int argc, char **argv) {
  const char *modename = (argc > 1) ? argv[1] : "DATAC16";
  int trials = (argc > 2) ? atoi(argv[2]) : 20;
  double snr = (argc > 3) ? atof(argv[3]) : -9.0;
  double off_lo = (argc > 4) ? atof(argv[4]) : -70.0;
  double off_hi = (argc > 5) ? atof(argv[5]) : 70.0;
  double off_step = (argc > 6) ? atof(argv[6]) : 1.0;
  int noise_trials = (argc > 8) ? atoi(argv[8]) : 0;
  /* Optional 9th: the coarse search span in Hz (0 = stock, +-50).
   * Optional 10th: the coarse grid step in Hz (0 = stock, 5).
   * Optional 11th: the fine stage's half-window in Hz (0 = derive). */
  double span = (argc > 9) ? atof(argv[9]) : 0.0;
  double step_hz = (argc > 10) ? atof(argv[10]) : 0.0;
  double fine_hz = (argc > 11) ? atof(argv[11]) : 0.0;

  int mode = bench_mode_from_name(modename);
  if (mode < 0 || trials <= 0 || off_step <= 0.0) {
    fprintf(stderr,
            "usage: %s [MODE] [TRIALS] [SNR] [OFF_LO] [OFF_HI] [OFF_STEP] "
            "[THRESH] [NOISE_TRIALS]\n",
            argv[0]);
    return 1;
  }

  struct freedv *tx = freedv_open(mode);
  struct freedv *rx = freedv_open(mode);
  if (!tx || !rx) {
    fprintf(stderr, "freedv_open failed\n");
    return 1;
  }
  freedv_set_frames_per_burst(tx, 1);
  freedv_set_frames_per_burst(rx, 1);

  float stock_thresh = freedv_get_timing_mx_thresh(rx);
  if (argc > 7) freedv_set_timing_mx_thresh(rx, (float)atof(argv[7]));
  float thresh = freedv_get_timing_mx_thresh(rx);
  if (span > 0.0) {
    freedv_set_freq_search_range(rx, (float)-span, (float)span);
    printf("coarse frequency search span set to +-%.0f Hz (stock +-50)\n",
           span);
  }
  if (step_hz > 0.0) {
    freedv_set_freq_search_step(rx, (float)step_hz);
    printf("coarse grid step set to %.1f Hz (stock 5)\n", step_hz);
  }
  if (fine_hz > 0.0) {
    freedv_set_freq_search_fine(rx, (float)fine_hz);
    printf("fine window set to +-%.1f Hz (stock derived: +-3)\n", fine_hz);
  }

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

  printf("%s: %d byte frame, burst %.2f s; threshold %.2f (stock %.2f)\n",
         mode_name_of(mode), nbytes, (double)nburst / FS, (double)thresh,
         (double)stock_thresh);
  printf("%d trials per offset, SNR %.1f dB AWGN, offsets %.2f..%.2f step %.2f "
         "Hz\n",
         trials, snr, off_lo, off_hi, off_step);
  printf("the shift moves whole FFT bins, so 'applied' is the offset the "
         "instrument really used\n\n");
  printf("  off   applied    acq  del     t_mx   c_err   f_err   snr_est\n");
  printf("                (per trial: acq/del, mean over synced trials)\n");

  bench_seq tailseq = {0};
  bench_seq ctrlseq = {0};
  int false_sync = 0;
  double last_good = 0.0, first_bad = 0.0;
  int have_edge = 0;
  double sigma_last = 0.0;

  for (double off = off_lo; off <= off_hi + 1e-9; off += off_step) {
    /* Identical channel per offset: same payloads, same noise draws. */
    bench_rng_seed(&rngA, 1357911131517ULL);
    bench_rng_seed(&rngB, 1357911131517ULL + 1);

    int acquired = 0, delivered = 0, seen = 0;
    double t_mx_sum = 0.0, c_err_sum = 0.0, f_err_sum = 0.0, snr_sum = 0.0;
    double applied = 0.0;

    for (int t = 0; t < trials; t++) {
      bench_fill_payload(payload, nbytes, &rngA);
      int n = bench_make_burst(tx, burst, payload);

      double ps = 0.0, pk = 0.0;
      for (int i = 0; i < n; i++) {
        ps += (double)burst[i] * burst[i];
        if (fabs((double)burst[i]) > pk) pk = fabs((double)burst[i]);
      }
      ps /= n;
      double sigma = sqrt(ps / (pow(10.0, snr / 10.0) * (3000.0 / (FS / 2.0))));

      double gnorm = (pk > 0.0) ? (HEADROOM_PEAK / pk) : 1.0;
      for (int i = 0; i < n; i++)
        burst[i] = (short)lrint((double)burst[i] * gnorm);
      ps *= gnorm * gnorm;
      sigma *= gnorm;

      if (off != 0.0) {
        applied = bench_freq_shift_applied_hz(n, off, FS);
        bench_freq_shift_real(burst, n, off, FS);
      }

      for (int i = 0; i < n; i++) {
        double v = burst[i] + sigma * bench_rng_gauss(&rngA);
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        burst[i] = (short)lrint(v);
      }
      sigma_last = sigma;

      bench_seq_init(&tailseq, &rngB, tail + TAIL_SLACK);
      int saw, got;
      bench_sync_info info = {0};
      bench_run_burst_ex(rx, burst, n, tail, sigma, out, seq_noise, &tailseq,
                         &saw, &got, NULL, NULL, &info);
      bench_seq_free(&tailseq);
      if (saw) acquired++;
      if (got) delivered++;
      if (info.seen) {
        seen++;
        t_mx_sum += info.timing_mx;
        c_err_sum += (double)info.coarse_foff_hz - applied;
        f_err_sum += (double)info.foff_hz - applied;
        snr_sum += info.snr_est;
      }
    }

    /* The capture edge: the last offset that still acquires reliably. */
    int good = (acquired * 10 >= trials * 9);
    if (good) {
      last_good = off;
      have_edge = 1;
    } else if (have_edge && first_bad == 0.0) {
      first_bad = off;
    }

    printf("%+6.1f  %+7.3f  %3d/%3d  %5d/%-3d  ", off, applied, acquired, trials,
           delivered, trials);
    if (seen > 0) {
      printf("%6.3f  %+6.2f  %+6.2f  %6.2f\n", t_mx_sum / seen,
             c_err_sum / seen, f_err_sum / seen, snr_sum / seen);
    } else {
      printf("%6s  %6s  %6s  %6s\n", "-", "-", "-", "-");
    }
    fflush(stdout);
  }

  /* One noise-only control for the row: the offset does not change the
   * noise, so one window per run is enough. */
  if (noise_trials > 0) {
    bench_rng_seed(&rngB, 1357911131517ULL + 1);
    for (int t = 0; t < noise_trials; t++) {
      bench_seq_init(&ctrlseq, &rngB, nburst + tail + TAIL_SLACK);
      int saw;
      bench_run_noise_only(rx, sigma_last, nburst, tail, &ctrlseq, &saw);
      bench_seq_free(&ctrlseq);
      if (saw) false_sync++;
    }
  }

  printf("\nnoise-only control: %d/%d windows declared sync\n", false_sync,
         noise_trials);
  if (have_edge)
    printf("capture edge: last offset acquiring 9/10 or better was %+.1f Hz\n",
           last_good);
  if (first_bad != 0.0)
    printf("first offset below that was %+.1f Hz\n", first_bad);

  bench_seq_free(&tailseq);
  bench_seq_free(&ctrlseq);
  freedv_close(tx);
  freedv_close(rx);
  free(burst);
  free(payload);
  free(out);
  return 0;
}
