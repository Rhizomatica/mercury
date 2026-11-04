/* idle_cpu_probe — what does a false sync cost, per threshold?
 *
 * Mercury listens on DATAC16 whenever it is idle, so the acquisition
 * threshold runs against noise all day.  Lowering it (PR #455) buys
 * acquisition at the fringe and raises the noise-only false-sync rate;
 * every false sync costs a UW check and, when it passes, a full LDPC
 * decode attempt on garbage.  On a Raspberry Pi 4 that decode cost is the
 * price of the setting, and the review asked for it measured before the
 * constant merges.
 *
 * Protocol: feed the receiver AUDIO_SECONDS of continuous noise as fast as
 * the machine can chew it and count CPU seconds over the run (the
 * rxcost_bench/resampler_bench convention: CPU time per unit of work).
 * Reporting is per audio-second, i.e. per real-time second on the radio:
 *
 *   CPU%      - percent of one core while listening in real time
 *   syncs     - false RX_SYNC edges (a rising edge is one decode attempt
 *               that passed the UW check)
 *   frames    - frames the decoder emitted on those false syncs
 *
 * False-sync statistics are level-invariant -- timing_mx is a normalised
 * correlation, so its noise-only distribution does not depend on the noise
 * scale (absent int16 quantisation) -- so one receive level is run and the
 * level is printed for the record.  The knobs are read and set through the
 * same setters/getters as the acquisition sweep; stock values come from the
 * opened modem, so the "stock" row is always what this build would do.
 *
 * CPU instruments are Linux/macOS only (see utils/Makefile).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "freedv_api.h"
#include "bench_common.h"

#define FS 8000
#define NOISE_SIGMA 3000.0 /* receive level for the record; see above */

static double now_cpu_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double now_wall_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
  const char *modename = (argc > 1) ? argv[1] : "DATAC16";
  double audio_seconds = (argc > 2) ? atof(argv[2]) : 600.0;
  float thresh_list[8];
  int nthresh = 0;
  if (argc > 3) {
    char *copy = strdup(argv[3]), *p = copy;
    while (p && nthresh < 8) {
      char *c = strchr(p, ',');
      if (c) *c = 0;
      thresh_list[nthresh++] = (float)atof(p);
      p = c ? c + 1 : NULL;
    }
    free(copy);
  }

  int mode = bench_mode_from_name(modename);
  if (mode < 0 || audio_seconds < 10.0) {
    fprintf(stderr,
            "usage: %s [MODE] [audio_seconds] [thresh_list]\n"
            "  MODE: DATAC16 (default)\n"
            "  Feeds continuous noise and counts CPU seconds, false syncs\n"
            "  and decode attempts, per threshold.  Run on the target\n"
            "  hardware (the numbers are per audio-second, so a Pi and a\n"
            "  desktop produce directly comparable tables).\n",
            argv[0]);
    return 1;
  }

  struct freedv *probe = freedv_open(mode);
  if (!probe) return 1;
  freedv_set_frames_per_burst(probe, 1);
  float stock_thresh = freedv_get_timing_mx_thresh(probe);
  freedv_close(probe);

  if (nthresh == 0) {
    /* stock, then the settings the acquisition study proposes or tests */
    float d[8] = {stock_thresh, 0.40f, 0.35f, 0.30f, 0.25f};
    memcpy(thresh_list, d, sizeof(d));
    nthresh = 5;
  }

  int nbytes = 0;
  long total_samples = (long)(audio_seconds * FS);
  bench_rng rng = {1357911131517ULL + 2};

  printf("%s idle, %.0f s of noise per setting (sigma %.0f); CPU is process\n"
         "time, CPU%% is that per real-time audio second on one core\n\n",
         modename, audio_seconds, NOISE_SIGMA);
  printf("  thresh  CPU%%   cpu s   syncs   syncs/h  frames  frames/h\n");

  for (int it = 0; it < nthresh; it++) {
    struct freedv *rx = freedv_open(mode);
    if (!rx) return 1;
    freedv_set_frames_per_burst(rx, 1);
    freedv_set_timing_mx_thresh(rx, thresh_list[it]);

    double cpu0 = now_cpu_s(), wall0 = now_wall_s();
    long fed = 0, sync_edges = 0, frames = 0;
    int was_sync = 0;
    bench_seq seq;
    bench_seq_init(&seq, &rng, 1 << 16); /* 64k-sample noise page, refilled */

    while (fed < total_samples) {
      int nin = freedv_nin(rx);
      short chunk[8192];
      if (nin > (int)(sizeof(chunk) / sizeof(chunk[0]))) break;
      for (int i = 0; i < nin; i++) {
        double v = NOISE_SIGMA * bench_seq_next(&seq);
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        chunk[i] = (short)lrint(v);
      }
      if (seq.pos + nin >= seq.len) { /* refill the page deterministically */
        bench_seq_free(&seq);
        bench_seq_init(&seq, &rng, 1 << 16);
      }
      fed += nin;

      int nb = (int)freedv_rawdatarx(rx, NULL, chunk);
      if (nb > 0) frames++;
      int st = freedv_get_rx_status(rx);
      int is_sync = (st & FREEDV_RX_SYNC) != 0;
      if (is_sync && !was_sync) sync_edges++;
      was_sync = is_sync;
    }

    double cpu = now_cpu_s() - cpu0;
    double audio = (double)fed / FS;
    bench_seq_free(&seq);
    freedv_close(rx);

    printf("  %5.2f  %5.1f  %6.1f  %6ld  %7.0f  %6ld  %8.0f\n",
           (double)thresh_list[it], 100.0 * cpu / audio, cpu, sync_edges,
           sync_edges / audio * 3600.0, frames, frames / audio * 3600.0);
    fflush(stdout);
    (void)wall0;
  }

  (void)nbytes;
  return 0;
}
