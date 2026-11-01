/* acq_tuning_sweep — sweep the burst-acquisition knobs and see where the
 * fringe floor actually moves.
 *
 * docs/MODES.md flags acquisition as "a future tuning candidate
 * (timing_mx_thresh, UW length) independent of the FEC design": below the
 * fringe, preamble acquisition fails long before the decoder does (99 -> 52
 * -> 7 acquired from -9 to -13 dB while sync'd frames still decode 92-99%).
 * The two runtime-reachable knobs are the preamble-correlation acceptance
 * threshold (timing_mx_thresh) and the unique-word error budget
 * (bad_uw_errors); nuwbits is frame structure and NOT touched here.
 *
 * For every (thresh, uw) combination this measures, per SNR point:
 *
 *   acq    - bursts that reached FREEDV_RX_SYNC (signal present)
 *   del    - bursts that delivered a CRC-ok frame
 *   false  - noise-only bursts at the same level that reached FREEDV_RX_SYNC
 *
 * The false column is the control that makes a lower threshold or a looser
 * UW budget honest: whatever the knobs buy in acquisition is only free if
 * the noise-only false-sync rate stays near zero.  The UW check is what
 * separates a real preamble from a lucky timing peak; loosening it trades
 * exactly that separation.
 *
 * Payloads and noise are re-seeded per combination, so every row sees the
 * same bursts: differences between rows are the knobs, not the channel.
 *
 * Knob defaults (thresh 0.45, uw errors 12 of 32 nuwbits for DATAC16) come
 * from ofdm_mode.c; with no arguments overridden this reproduces the stock
 * behaviour measured by utils/acquire_vs_decode.
 *
 * AWGN only, like acquire_vs_decode: fading confounds acquisition loss with
 * payload fade loss, and the clean question belongs on the clean channel.
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
/* Same fixed headroom as acquire_vs_decode. */
#define HEADROOM_PEAK 4000.0

/* Per-mode acquisition-relevant constants from ofdm_mode.c: the stock
 * timing_mx_thresh, stock bad_uw_errors, nuwbits (for context in the
 * output).  nuwbits is reported, never modified. */
struct mode_defaults {
  const char *name;
  int freedv_mode;
  float thresh;
  int uw_errors;
  int nuwbits;
};

static const struct mode_defaults modes[] = {
    {"DATAC16", FREEDV_MODE_DATAC16, 0.45f, 12, 32},
    {"DATAC15", FREEDV_MODE_DATAC15, 0.45f, 18, 48},
    {"DATAC13", FREEDV_MODE_DATAC13, 0.45f, 18, 48},
    {"DATAC4",  FREEDV_MODE_DATAC4,  0.50f, 12, 32},
    {NULL, 0, 0.0f, 0, 0},
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

static int mode_from_name(const char *s) {
  for (const struct mode_defaults *m = modes; m->name; m++)
    if (!strcmp(s, m->name)) return m->freedv_mode;
  return -1;
}

static const struct mode_defaults *defaults_from_name(const char *s) {
  for (const struct mode_defaults *m = modes; m->name; m++)
    if (!strcmp(s, m->name)) return m;
  return NULL;
}

/* One burst through the receiver.  samples[] holds the (already
 * noise-added) burst; sigma continues plain noise after it.  Returns saw
 * sync / got frame via the out pointers, mirroring acquire_vs_decode:
 * full RX_SYNC only (TRIAL_SYNC fires on noise), CRC-ok frames only. */
static void run_burst(struct freedv *rx, const short *samples, int n, int tail,
                      double sigma, unsigned char *out, int *saw_sync,
                      int *got_frame) {
  freedv_set_sync(rx, FREEDV_SYNC_UNSYNC);
  int pos = 0;
  *saw_sync = 0;
  *got_frame = 0;
  while (pos < n + tail) {
    int nin = freedv_nin(rx);
    short chunk[8192];
    if (nin > (int)(sizeof(chunk) / sizeof(chunk[0]))) break;
    for (int i = 0; i < nin; i++) {
      if (pos + i < n)
        chunk[i] = samples[pos + i];
      else {
        double v = sigma * gauss();
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        chunk[i] = (short)lrint(v);
      }
    }
    pos += nin;

    int nb = (int)freedv_rawdatarx(rx, out, chunk);
    int st = freedv_get_rx_status(rx);
    if (st & FREEDV_RX_SYNC) *saw_sync = 1;
    if (nb > 0 && !(st & FREEDV_RX_BIT_ERRORS)) *got_frame = 1;
  }
}

/* Build one transmit burst with a CRC'd random payload, normalised to the
 * fixed headroom.  Returns the burst length and, via *sigma_out, the noise
 * sigma that realises snr_db (SNR3k) for it.  Same construction as
 * acquire_vs_decode.  foff_hz mixes the burst off its nominal centre (real
 * carrier, DSB), the way a mistuned radio presents it; noise stays white. */
static int make_burst(struct freedv *tx, short *burst, double snr_db,
                      unsigned char *payload, int nbytes, double *sigma_out,
                      double foff_hz) {
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
    ps += (double)burst[i] * burst[i];
    if (fabs((double)burst[i]) > pk) pk = fabs((double)burst[i]);
  }
  ps /= n;

  double gnorm = (pk > 0.0) ? (HEADROOM_PEAK / pk) : 1.0;
  for (int i = 0; i < n; i++) burst[i] = (short)lrint((double)burst[i] * gnorm);
  ps *= gnorm * gnorm;
  if (foff_hz != 0.0) {
    for (int i = 0; i < n; i++) {
      double v = (double)burst[i] * cos(2.0 * M_PI * foff_hz * (double)i / FS);
      burst[i] = (short)lrint(v);
    }
    /* Power is halved by the real mixer (cos averaging). */
    ps *= 0.5;
  }
  *sigma_out = sqrt(ps / (pow(10.0, snr_db / 10.0) * (3000.0 / (FS / 2.0))));
  return n;
}

int main(int argc, char **argv) {
  const char *modename = (argc > 1) ? argv[1] : "DATAC16";
  int trials = (argc > 2) ? atoi(argv[2]) : 20;
  double snr_lo = (argc > 3) ? atof(argv[3]) : -13.0;
  double snr_hi = (argc > 4) ? atof(argv[4]) : -8.0;
  int noise_trials = (argc > 5) ? atoi(argv[5]) : trials;
  /* Optional 6th/7th args: comma-separated knob lists, e.g.
   *   acq_tuning_sweep DATAC16 20 -13 -11 40 0.45,0.35,0.25 12,10,8
   * Defaults below. */
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

  const struct mode_defaults *md = defaults_from_name(modename);
  int mode = mode_from_name(modename);
  if (mode < 0 || trials <= 0 || noise_trials < 0) {
    fprintf(stderr,
            "usage: %s [MODE] [trials] [snr_lo] [snr_hi] [noise_trials] "
            "[thresh_list] [uw_list]\n"
            "  MODE: DATAC16 (default), DATAC15, DATAC13, DATAC4\n"
            "  thresh_list/uw_list: comma-separated, e.g. 0.45,0.35,0.25 and "
            "12,10,8\n"
            "  Sweeps timing_mx_thresh x bad_uw_errors around the ofdm_mode.c\n"
            "  defaults; prints acq/del per SNR and a noise-only false-sync\n"
            "  control per row.\n",
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

  int nbytes = freedv_get_bits_per_modem_frame(tx) / 8;
  int npre = freedv_get_n_tx_preamble_modem_samples(tx);
  int ntx = freedv_get_n_tx_modem_samples(tx);
  int npost = freedv_get_n_tx_postamble_modem_samples(tx);
  int nburst = npre + ntx + npost;
  int tail = nburst;
  short *burst = malloc(sizeof(short) * (size_t)(nburst + 4));
  unsigned char *payload = malloc((size_t)nbytes);
  unsigned char *out = malloc((size_t)nbytes + 8);
  short *noise_only = malloc(sizeof(short) * (size_t)(nburst + 4));
  if (!burst || !payload || !out || !noise_only) return 1;

  printf("%s: %d byte frame, burst %.2f s; defaults thresh %.2f, uw %d/%d\n",
         modename, nbytes, (double)nburst / FS, (double)md->thresh, md->uw_errors,
         md->nuwbits);
  printf("trials %d/snr point, %d noise-only/snr point; identical bursts per "
         "combo (seeded)\n\n",
         trials, noise_trials);

  /* Sweep thresholds below the stock value; the stock value is the first
   * row, so every table contains the calibrated baseline.  UW budget:
   * default stock, looser (in case the UW is the binding constraint), and
   * tighter (to buy a lower threshold back without false syncs), capped at
   * nuwbits/2 so the UW still rejects something. */
  if (nthresh == 0) {
    float defaults[8] = {md->thresh, 0.30f, 0.20f, 0.10f, 0.05f};
    memcpy(thresh_list, defaults, sizeof(defaults));
    nthresh = 5;
  }
  if (nuw == 0) {
    uw_list[0] = md->uw_errors;
    uw_list[1] = md->uw_errors + 3;
    uw_list[2] = md->uw_errors - 4;
    uw_list[3] = md->uw_errors / 2;
    nuw = 4;
  }
  while (nuw > 0 && uw_list[nuw - 1] > md->nuwbits / 2) nuw--;
  if (nuw == 0) {
    fprintf(stderr, "every UW candidate exceeds nuwbits/2\n");
    return 1;
  }

  printf("thresh uw      ");
  for (double s = snr_lo; s <= snr_hi + 0.01; s += 1.0) printf("| %-13.0f ", s);
  printf("\n");
  printf("       (nuw)   ");
  for (double s = snr_lo; s <= snr_hi + 0.01; s += 1.0) printf("| acq/del/false ");
  printf("\n");

  for (int it = 0; it < nthresh; it++) {
    for (int iu = 0; iu < nuw; iu++) {
      freedv_set_timing_mx_thresh(rx, thresh_list[it]);
      freedv_set_bad_uw_errors(rx, uw_list[iu]);

      printf("%5.2f  %2d/%-2d   ", (double)thresh_list[it], uw_list[iu],
             md->nuwbits);
      fflush(stdout);

      /* Re-seed per combo: identical payloads and noise on every row. */
      seed_rng(1357911131517ULL + 7919ULL * iu);

      for (double snr = snr_lo; snr <= snr_hi + 0.01; snr += 1.0) {
        int acquired = 0, delivered = 0;
        double sigma = 0.0;
        for (int t = 0; t < trials; t++) {
          int n = make_burst(tx, burst, snr, payload, nbytes, &sigma, foff_hz);
          for (int i = 0; i < n; i++) {
            double v = burst[i] + sigma * gauss();
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            burst[i] = (short)lrint(v);
          }
          int saw, got;
          run_burst(rx, burst, n, tail, sigma, out, &saw, &got);
          if (saw) acquired++;
          if (got) delivered++;
        }

        /* Noise-only control at the same level: nburst samples of plain
         * noise, nothing else.  Any RX_SYNC here is a false alarm. */
        int false_sync = 0;
        for (int t = 0; t < noise_trials; t++) {
          for (int i = 0; i < nburst; i++)
            noise_only[i] = (short)lrint(sigma * gauss());
          int saw, got;
          run_burst(rx, noise_only, nburst, 0, sigma, out, &saw, &got);
          if (saw) false_sync++;
        }

        printf("| %3d/%-3d/%-4d ", acquired, delivered, false_sync);
        fflush(stdout);
      }
      printf("\n");
    }
  }

  freedv_close(tx);
  freedv_close(rx);
  free(burst);
  free(payload);
  free(out);
  free(noise_only);
  return 0;
}
