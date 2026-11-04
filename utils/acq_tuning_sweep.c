/* acq_tuning_sweep — sweep the burst-acquisition knobs and see where the
 * fringe floor actually moves.
 *
 * docs/MODES.md flags acquisition as a tuning candidate: below the fringe,
 * preamble acquisition fails long before the decoder does (99 -> 52 -> 7
 * acquired from -9 to -13 dB while sync'd frames still decode 92-99%).  The
 * two runtime-reachable knobs are the preamble-correlation acceptance
 * threshold (timing_mx_thresh) and the unique-word error budget
 * (bad_uw_errors); nuwbits is frame structure and NOT touched here.
 *
 * For every (thresh, uw) combination this measures, per SNR point:
 *
 *   acq    - bursts that reached FREEDV_RX_SYNC (signal present)
 *   del    - bursts that delivered a CRC-ok frame
 *   false  - noise-only bursts, over the SAME window as a signal trial
 *            (burst + tail), that reached FREEDV_RX_SYNC
 *
 * The false column is the control that makes a lower threshold or a looser
 * UW budget honest: whatever the knobs buy in acquisition is only free if
 * the noise-only false-sync rate stays near zero.
 *
 * Pairing (the property PR #455's review found broken and this file now
 * implements through utils/bench_common.h):
 *
 *   - Payload bytes and in-burst noise come from stream A: a fixed number
 *     of draws per trial, independent of the receiver.
 *   - The tail after each burst is a pre-generated sequence from stream B
 *     with a fixed draw count per trial -- NOT drawn on the fly, because
 *     the receiver's nin sequence (and therefore the draw count) depends on
 *     the knob under test.  On the fly drawing is what silently put every
 *     row on a different channel after the first burst.
 *   - The noise-only control draws its window from stream B the same way,
 *     with the int16 clamp the signal paths always had.
 *   - Every knob combination is seeded identically, so rows see the same
 *     payloads and the same noise: row differences are the knobs.
 *
 * Offset: bench_freq_shift_real applies a true analytic shift (the way a
 * mistuned SSB radio presents the burst); multiplying the real samples by
 * cos() is DSB and produces two half-amplitude images -- a different
 * signal, not a shift.
 *
 * AWGN only by default, like acquire_vs_decode: fading confounds
 * acquisition loss with payload fade loss; the clean question belongs on
 * the clean channel.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freedv_api.h"
#include "bench_common.h"

#define FS 8000.0
#define HEADROOM_PEAK 4000.0
#define TAIL_SLACK 8192 /* RX loop chunk bound; keeps every seq finite */

/* nuwbits per mode, for the UW-budget sanity cap and for context in the
 * output.  Frame structure, so unlike the knobs these cannot drift. */
static int nuwbits_of(int freedv_mode) {
  switch (freedv_mode) {
    case FREEDV_MODE_DATAC16:
    case FREEDV_MODE_DATAC4:
      return 32;
    case FREEDV_MODE_DATAC15:
    case FREEDV_MODE_DATAC13:
      return 48;
    default:
      return 0;
  }
}

static bench_rng rngA = {1357911131517ULL}; /* payloads, in-burst noise */
static bench_rng rngB = {1357911131517ULL + 1}; /* tails, control windows */

static double seq_noise(void *ctx) { return bench_seq_next((bench_seq *)ctx); }

static const char *mode_name_of(int freedv_mode) {
  switch (freedv_mode) {
    case FREEDV_MODE_DATAC16:
      return "DATAC16";
    case FREEDV_MODE_DATAC15:
      return "DATAC15";
    case FREEDV_MODE_DATAC13:
      return "DATAC13";
    case FREEDV_MODE_DATAC4:
      return "DATAC4";
    default:
      return "?";
  }
}

int main(int argc, char **argv) {
  const char *modename = (argc > 1) ? argv[1] : "DATAC16";
  int trials = (argc > 2) ? atoi(argv[2]) : 20;
  double snr_lo = (argc > 3) ? atof(argv[3]) : -13.0;
  double snr_hi = (argc > 4) ? atof(argv[4]) : -8.0;
  int noise_trials = (argc > 5) ? atoi(argv[5]) : trials;
  /* Optional 6th/7th args: comma-separated knob lists, e.g.
   *   acq_tuning_sweep DATAC16 100 -13 -9 100 0.45,0.40,0.35 12
   * Optional 8th: frequency offset in Hz (analytic shift). */
  float thresh_list[8];
  int uw_list[8];
  int nthresh = 0, nuw = 0;
  if (argc > 6) {
    char *copy = strdup(argv[6]), *p = copy;
    while (p && nthresh < 8) {
      char *c = strchr(p, ',');
      if (c) *c = 0;
      thresh_list[nthresh++] = (float)atof(p);
      p = c ? c + 1 : NULL;
    }
    free(copy);
  }
  if (argc > 7) {
    char *copy = strdup(argv[7]), *p = copy;
    while (p && nuw < 8) {
      char *c = strchr(p, ',');
      if (c) *c = 0;
      uw_list[nuw++] = atoi(p);
      p = c ? c + 1 : NULL;
    }
    free(copy);
  }
  double foff_hz = (argc > 8) ? atof(argv[8]) : 0.0;

  int mode = bench_mode_from_name(modename);
  int nuwbits = (mode >= 0) ? nuwbits_of(mode) : 0;
  if (mode < 0 || trials <= 0 || noise_trials < 0 || nuwbits == 0) {
    fprintf(stderr,
            "usage: %s [MODE] [trials] [snr_lo] [snr_hi] [noise_trials] "
            "[thresh_list] [uw_list] [foff_hz]\n"
            "  MODE: DATAC16 (default), DATAC15, DATAC13, DATAC4\n"
            "  thresh_list/uw_list: comma-separated, e.g. 0.45,0.40,0.35 and "
            "12,10,8\n"
            "  Sweeps timing_mx_thresh x bad_uw_errors; prints acq/del per SNR\n"
            "  and a same-window noise-only false-sync control per row.\n",
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

  /* The stock knobs are read from the opened modem, not restated here: the
   * values live in ofdm_mode.c and change when the default changes. */
  float stock_thresh = freedv_get_timing_mx_thresh(rx);
  int stock_uw = freedv_get_bad_uw_errors(rx);

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

  /* Defaults: stock threshold first (the calibrated baseline), then a
   * moderate step down, then progressively lower.  UW: stock, looser,
   * tighter -- all below nuwbits/2 (a budget over half the UW cannot
   * reject noise, which sits at ~50% bit errors).  Lists are compacted in
   * order. */
  if (nthresh == 0) {
    float defaults[8] = {stock_thresh, 0.30f, 0.20f, 0.10f, 0.05f};
    memcpy(thresh_list, defaults, sizeof(defaults));
    nthresh = 5;
  }
  if (nuw == 0) {
    uw_list[0] = stock_uw;
    uw_list[1] = stock_uw + 3;
    uw_list[2] = stock_uw - 4;
    uw_list[3] = stock_uw / 2;
    nuw = 4;
  }
  int kept = 0;
  for (int i = 0; i < nuw; i++)
    if (uw_list[i] <= nuwbits / 2) uw_list[kept++] = uw_list[i];
  nuw = kept;
  if (nuw == 0) {
    fprintf(stderr, "every UW candidate exceeds nuwbits/2\n");
    return 1;
  }

  printf("%s: %d byte frame, burst %.2f s; stock knobs thresh %.2f, uw %d/%d\n",
         mode_name_of(mode), nbytes, (double)nburst / FS, (double)stock_thresh,
         stock_uw, nuwbits);
  printf("trials %d/snr point, %d noise-only/snr point (same window as a "
         "signal trial)\n",
         trials, noise_trials);
  if (foff_hz != 0.0) printf("frequency offset %+.0f Hz (analytic shift)\n", foff_hz);
  printf("streams A/B re-seeded per combination: identical bursts on every "
         "row\n\n");

  printf("thresh uw      ");
  for (double s = snr_lo; s <= snr_hi + 0.001; s += 1.0) printf("| %-13.0f ", s);
  printf("\n");
  printf("       (nuw)   ");
  for (double s = snr_lo; s <= snr_hi + 0.001; s += 1.0) printf("| acq/del/false ");
  printf("\n");

  bench_seq tailseq = {0};
  bench_seq ctrlseq = {0};

  for (int it = 0; it < nthresh; it++) {
    for (int iu = 0; iu < nuw; iu++) {
      freedv_set_timing_mx_thresh(rx, thresh_list[it]);
      freedv_set_bad_uw_errors(rx, uw_list[iu]);

      printf("%5.2f  %2d/%-2d   ", (double)thresh_list[it], uw_list[iu],
             nuwbits);
      fflush(stdout);

      /* Identical per combination: same seed on both streams, fixed draw
       * counts per trial.  Rows differ by the knobs, nothing else. */
      bench_rng_seed(&rngA, 1357911131517ULL);
      bench_rng_seed(&rngB, 1357911131517ULL + 1);

      for (double snr = snr_lo; snr <= snr_hi + 0.001; snr += 1.0) {
        int acquired = 0, delivered = 0;
        double sigma = 0.0;
        int n = 0;

        for (int t = 0; t < trials; t++) {
          bench_fill_payload(payload, nbytes, &rngA);
          n = bench_make_burst(tx, burst, payload);

          double ps = 0.0, pk = 0.0;
          for (int i = 0; i < n; i++) {
            ps += (double)burst[i] * burst[i];
            if (fabs((double)burst[i]) > pk) pk = fabs((double)burst[i]);
          }
          ps /= n;
          sigma = sqrt(ps / (pow(10.0, snr / 10.0) * (3000.0 / (FS / 2.0))));

          double gnorm = (pk > 0.0) ? (HEADROOM_PEAK / pk) : 1.0;
          for (int i = 0; i < n; i++)
            burst[i] = (short)lrint((double)burst[i] * gnorm);
          ps *= gnorm * gnorm;
          sigma *= gnorm;

          /* True shift, before noise: same power, single copy at fc+f. */
          bench_freq_shift_real(burst, n, foff_hz, FS);

          for (int i = 0; i < n; i++) {
            double v = burst[i] + sigma * bench_rng_gauss(&rngA);
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            burst[i] = (short)lrint(v);
          }

          bench_seq_init(&tailseq, &rngB, tail + TAIL_SLACK);
          int saw, got;
          bench_run_burst(rx, burst, n, tail, sigma, out, seq_noise, &tailseq,
                          &saw, &got);
          bench_seq_free(&tailseq);
          if (saw) acquired++;
          if (got) delivered++;
        }

        int false_sync = 0;
        for (int t = 0; t < noise_trials; t++) {
          bench_seq_init(&ctrlseq, &rngB, n + tail + TAIL_SLACK);
          int saw;
          bench_run_noise_only(rx, sigma, n, tail, &ctrlseq, &saw);
          bench_seq_free(&ctrlseq);
          if (saw) false_sync++;
        }

        printf("| %3d/%-3d/%-4d ", acquired, delivered, false_sync);
        fflush(stdout);
      }
      printf("\n");
    }
  }

  bench_seq_free(&tailseq);
  bench_seq_free(&ctrlseq);
  freedv_close(tx);
  freedv_close(rx);
  free(burst);
  free(payload);
  free(out);
  return 0;
}
