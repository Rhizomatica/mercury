/* modem_cpu_probe — burst encode and decode CPU per mode, in CPU-seconds
 * per second of audio, so a rig's headroom reads straight off the table.
 *
 * idle_cpu_probe measures the listening cost; this measures the talking
 * cost: the raw-data encode (preamble, frame, postamble -- the calls
 * modem.c makes), and freedv_rawdatarx in sync on clean bursts at +10 dB,
 * which is the per-frame cost a session pays while data moves.  Sync is
 * kept across frames: the first burst's acquisition is amortised over the
 * run, and what is reported is the steady state.
 *
 * The channel is awgn by default; CPU cost is not an SNR story, the point
 * is a clean, in-sync decode per frame.  Deliveries are counted so a mode
 * that does not decode on this channel says so instead of reporting the
 * CPU of a decoder that never commits a frame.  The MFSK floor is a
 * different modem here (see pattern_probe's cpu mode for its detector).
 *
 * CPU instruments are Linux/macOS only (see utils/Makefile).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "freedv_api.h"
#include "bench_common.h"

#define FS 8000
#define DEFAULT_FRAMES 200
#define DEFAULT_SNR 10.0
#define GAP_SAMPLES (FS / 10) /* 0.1 s of noise between bursts */

static double now_cpu_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* The in-session ladder's modes, floor aside. */
static const struct { const char *name; int mode; } MODES[] = {
  { "DATAC3", FREEDV_MODE_DATAC3 },   { "DATAC1", FREEDV_MODE_DATAC1 },
  { "DATAC17", FREEDV_MODE_DATAC17 }, { "QAM16C2", FREEDV_MODE_QAM16C2 },
  { "DATAC16", FREEDV_MODE_DATAC16 }, { "DATAC15", FREEDV_MODE_DATAC15 },
  { "DATAC4", FREEDV_MODE_DATAC4 },
};

int main(int argc, char **argv) {
  int frames = (argc > 1) ? atoi(argv[1]) : DEFAULT_FRAMES;
  double snr = (argc > 2) ? atof(argv[2]) : DEFAULT_SNR;
  if (frames < 20) {
    fprintf(stderr, "usage: %s [frames] [snr_db]\n"
                    "  frames: bursts per mode (default %d)\n"
                    "  snr_db: the decode channel's SNR3k (default %.0f)\n",
            argv[0], DEFAULT_FRAMES, DEFAULT_SNR);
    return 1;
  }

  printf("modem_cpu_probe: %d frames per mode, decode at %+.1f dB SNR3k awgn, "
         "sync held across frames\n", frames, snr);
  printf("CPU s per audio s (CPU%% of one core); tx = raw-data encode, "
         "rx = rawdatarx in sync\n\n");
  printf("%-8s %8s %9s %8s %8s %8s %10s\n",
         "mode", "bytes/fr", "air_s/fr", "tx_cpu/x", "rx_cpu/x", "tx+rx/x", "delivered");

  bench_rng rng;
  for (unsigned i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
    int mode = MODES[i].mode;
    struct freedv *tx = freedv_open(mode);
    struct freedv *rx = freedv_open(mode);
    if (!tx || !rx) { fprintf(stderr, "freedv_open(%s) failed\n", MODES[i].name); return 1; }
    freedv_set_frames_per_burst(tx, 1);
    freedv_set_frames_per_burst(rx, 1);

    int nbytes = freedv_get_bits_per_modem_frame(tx) / 8;
    int npre   = freedv_get_n_tx_preamble_modem_samples(tx);
    int npost  = freedv_get_n_tx_postamble_modem_samples(tx);
    int ntx    = freedv_get_n_tx_modem_samples(tx);
    int nburst = npre + ntx + npost;
    double air_s = (double)nburst / FS;

    short *burst = malloc(sizeof(short) * (size_t)(nburst + 4));
    short *chunk = malloc(sizeof(short) * (size_t)(8192));
    unsigned char *payload = malloc((size_t)nbytes);
    unsigned char *out = malloc((size_t)nbytes + 8);
    if (!burst || !chunk || !payload || !out) return 1;

    bench_rng_seed(&rng, 1234u + (unsigned)i);

    /* ---- encode: one burst whole, N times ---- */
    double t0 = now_cpu_s();
    for (int f = 0; f < frames; f++) {
      bench_fill_payload(payload, nbytes, &rng);
      int n = bench_make_burst(tx, burst, payload);
      (void)n;
    }
    double tx_cpu = now_cpu_s() - t0;

    /* ---- the decode channel: the last burst at +snr dB in 3 kHz ---- */
    double ps = 0.0;
    for (int j = 0; j < nburst; j++) ps += (double)burst[j] * burst[j];
    ps /= nburst;
    double sigma = sqrt(ps / (pow(10.0, snr / 10.0) * (3000.0 / (FS / 2.0))));

    /* ---- decode: bursts with 0.1 s noise gaps, sync never dropped ---- */
    double rx_cpu = 0.0;
    double audio = 0.0;
    int delivered = 0;
    for (int f = 0; f < frames; f++) {
      bench_fill_payload(payload, nbytes, &rng);
      int n = bench_make_burst(tx, burst, payload);
      t0 = now_cpu_s();
      /* the burst, then the gap, through the nin-chunked receiver */
      int pos = 0;
      while (pos < n + GAP_SAMPLES) {
        int nin = freedv_nin(rx);
        for (int j = 0; j < nin && pos + j < n + GAP_SAMPLES; j++) {
          double s;
          if (pos + j < n) s = (double)burst[pos + j];
          else             s = bench_rng_gauss(&rng) * sigma;
          double v = s + bench_rng_gauss(&rng) * sigma;
          if (v > 32767.0) v = 32767.0;
          if (v < -32768.0) v = -32768.0;
          chunk[j] = (short)v;
        }
        int take = nin < n + GAP_SAMPLES - pos ? nin : n + GAP_SAMPLES - pos;
        int nb = (int)freedv_rawdatarx(rx, out, chunk);
        pos += take;
        if (nb > 0) delivered++;
      }
      rx_cpu += now_cpu_s() - t0;
      audio += (double)(n + GAP_SAMPLES) / FS;
    }

    printf("%-8s %8d %9.3f %8.3f %8.3f %8.3f %10d\n",
           MODES[i].name, nbytes, air_s,
           tx_cpu / audio, rx_cpu / audio, (tx_cpu + rx_cpu) / audio, delivered);
    fflush(stdout);

    free(burst); free(chunk); free(payload); free(out);
    freedv_close(tx); freedv_close(rx);
  }
  return 0;
}
