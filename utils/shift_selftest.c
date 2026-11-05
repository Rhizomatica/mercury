/* shift_selftest -- is the analytic frequency shift what it claims to be?
 *
 * The offset study rests on one instrument: bench_freq_shift_real, the
 * analytic shift that models a mistuned SSB radio.  The first version of
 * the offset sweep (docs/ACQ-TUNING-FINDINGS.md) was re-measured with it
 * after the DSB mistake, and it reported that +-20 Hz destroys acquisition
 * at every threshold measured.  Before any of that is trusted again, the
 * shift has to be checked against a reference that cannot be wrong: a pure
 * tone, whose shifted form is known in closed form.
 *
 * Four checks:
 *   1. the output tone's frequency is the input's plus the applied offset;
 *   2. the output amplitude equals the input's -- an SSB mix preserves
 *      power, and a half-amplitude copy is 6 dB of SNR that a sweep which
 *      calibrates its noise before the shift never accounts for;
 *   3. the samples match the closed-form reference for that tone, sample
 *      for sample;
 *   4. the offset asked for is close to the offset applied (the shift
 *      moves whole FFT bins, so it is quantised to fs/nfft).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_common.h"

#define FS 8000.0
#define TONE_A 8000.0
#define TONE_F0 1000.0
#define TONE_N 8000

static int failures = 0;

static void check(int ok, const char *what, const char *detail) {
  printf("  %-4s %-44s %s\n", ok ? "PASS" : "FAIL", what, detail);
  if (!ok) failures++;
}

/* Frequency of the strongest tone in [f_lo, f_hi], by DFT peak with a
 * parabolic refinement on the magnitude.  No window: the test signal is a
 * single tone, so the peak is exact and the grid sets the resolution. */
static double tone_freq(const short *buf, int n, double fs, double f_lo,
                         double f_hi) {
  double best_f = f_lo, best_mag = -1.0, prev_mag = 0.0, next_mag = 0.0;
  double step = 0.05;
  int at = -1;
  for (double f = f_lo; f <= f_hi + 1e-9; f += step) {
    double w = 2.0 * M_PI * f / fs;
    double c = cos(w), s = sin(w);
    double cr = 1.0, ci = 0.0, re = 0.0, im = 0.0;
    for (int i = 0; i < n; i++) {
      re += buf[i] * cr;
      im += buf[i] * ci;
      double nr = cr * c + ci * s;
      ci = ci * c - cr * s;
      cr = nr;
    }
    double mag = sqrt(re * re + im * im);
    if (mag > best_mag) {
      best_mag = mag;
      best_f = f;
      at++;
    }
    if (at == 1) prev_mag = mag;
    if (at == 2) next_mag = mag;
  }
  if (at >= 2 && prev_mag > 0 && next_mag > 0) {
    double denom = prev_mag - 2.0 * best_mag + next_mag;
    if (fabs(denom) > 1e-12) best_f += step * 0.5 * (prev_mag - next_mag) / denom;
  }
  return best_f;
}

static double rms(const short *buf, int n) {
  double s = 0.0;
  for (int i = 0; i < n; i++) s += (double)buf[i] * buf[i];
  return sqrt(s / n);
}

int main(void) {
  printf("shift_selftest: bench_freq_shift_real against a closed-form tone\n");

  const double f_shift = 20.0;
  short *in = malloc(sizeof(short) * TONE_N);
  short *out = malloc(sizeof(short) * TONE_N);
  short *ref = malloc(sizeof(short) * TONE_N);
  if (!in || !out || !ref) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }

  const double phi = 0.3;
  for (int i = 0; i < TONE_N; i++)
    in[i] = (short)lrint(TONE_A * cos(2.0 * M_PI * TONE_F0 * i / FS + phi));

  double applied = bench_freq_shift_applied_hz(TONE_N, f_shift, FS);
  memcpy(out, in, sizeof(short) * TONE_N);
  bench_freq_shift_real(out, TONE_N, f_shift, FS);

  /* 1. frequency */
  {
    double f = tone_freq(out, TONE_N, FS, TONE_F0 + f_shift - 40.0,
                          TONE_F0 + f_shift + 40.0);
    double err = f - (TONE_F0 + applied);
    char d[128];
    snprintf(d, sizeof(d), "%.3f Hz (want %.3f, applied offset %.3f)", f,
             TONE_F0 + applied, applied);
    check(fabs(err) < 0.05, "output frequency is input + applied offset", d);
  }

  /* 2. amplitude and power */
  {
    double ri = rms(in, TONE_N), ro = rms(out, TONE_N);
    double ratio = ro / ri;
    char d[128];
    snprintf(d, sizeof(d), "rms ratio %.4f (want 1.0000, %.2f dB)", ratio,
             20.0 * log10(ratio));
    check(fabs(ratio - 1.0) < 0.01, "amplitude preserved", d);
  }

  /* 3. sample-for-sample against the closed form.  The edges are excluded:
   * the zero padding is finite, so the FFT's wrap-around leaves a little
   * energy at the buffer's ends. */
  {
    double maxdiff = 0.0, sumsq = 0.0;
    int cnt = 0;
    for (int i = 1024; i < TONE_N - 1024; i++) {
      double v = TONE_A * cos(2.0 * M_PI * (TONE_F0 + applied) * i / FS + phi);
      double d = fabs(v - (double)out[i]);
      if (d > maxdiff) maxdiff = d;
      sumsq += d * d;
      cnt++;
    }
    char d[128];
    snprintf(d, sizeof(d), "max |diff| %.4f, rms %.4f (int16 step is 1.0)",
             maxdiff, sqrt(sumsq / (cnt > 0 ? cnt : 1)));
    /* Two independently rounded copies of the same real value differ by at
     * most one LSB, plus the FFT's own rounding: 2.0 is the scale. */
    check(maxdiff < 2.0, "samples match the closed-form shifted tone", d);
    (void)ref;
  }

  /* 4. asked-for offset vs applied offset */
  {
    char d[128];
    snprintf(d, sizeof(d), "asked %+.2f Hz, applied %+.4f Hz (bin %.4f Hz)",
             f_shift, applied, FS / 32768.0);
    check(fabs(applied - f_shift) < 0.25, "applied offset tracks the request", d);
  }

  printf("%s\n", failures ? "shift_selftest: FAILURES" : "shift_selftest: all checks pass");
  free(in);
  free(out);
  free(ref);
  return failures ? 1 : 0;
}
