/* bench_common — shared machinery for the modem bench tools.
 *
 * One place for the RNG discipline, burst construction, the receiver loop
 * and the analytic frequency shift, so the tools stay calibrated together
 * instead of growing private copies that drift (see PR #455 review,
 * points 1-4 and 9).
 *
 * The RNG discipline that makes "identical bursts per row" actually true:
 *
 *   - Stream A (bench_rng) produces the payload bytes and the in-burst
 *     noise: a FIXED number of draws per trial, independent of the
 *     receiver.
 *   - The tail after the burst is NOT drawn on the fly.  Its length in
 *     draws would depend on the receiver's nin sequence, which depends on
 *     the knob under test -- the flaw the review found, where rows with
 *     different thresholds silently drifted onto different channels.  The
 *     tail comes from bench_seq: a pre-generated sequence with a fixed
 *     draw count per trial, read back in order no matter how the receiver
 *     chunks the audio.
 *   - Every knob combination is seeded identically.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_BENCH_COMMON_H
#define MERCURY_BENCH_COMMON_H

#include <stddef.h>

#include "freedv_api.h"

/* ---- RNG: two independent xorshift64* streams per tool ---- */

typedef struct { unsigned long long s; } bench_rng;

void bench_rng_seed(bench_rng *r, unsigned long long seed);
double bench_rng_uniform(bench_rng *r); /* [0, 1) */
double bench_rng_gauss(bench_rng *r);   /* Box-Muller; consumes 2 uniforms */

/* ---- A fixed-length pre-generated noise sequence ----
 *
 * bench_seq_init draws `len` gaussian samples (unit sigma) from `rng` and
 * holds them; bench_seq_next reads them back in order without drawing.
 * Feed `len` >= tail + 8192 so the RX loop's final chunk (nin is bounded
 * by the 8192-sample chunk buffer) never runs past the end. */

typedef struct {
  double *x;
  int len;
  int pos;
} bench_seq;

void bench_seq_init(bench_seq *s, bench_rng *rng, int len);
void bench_seq_free(bench_seq *s);
double bench_seq_next(bench_seq *s);

/* Callback form, for tools whose noise discipline predates this header
 * (acquire_vs_decode draws its tail from its own global stream and its
 * numbers must not move by a sample).  Returns one unit-sigma sample. */
typedef double (*bench_noise_fn)(void *ctx);

/* ---- Burst construction ----
 *
 * bench_fill_payload draws (nbytes - 2) payload bytes from `rng` and
 * writes the CRC16 after them, as modem.c does -- a fixed draw count per
 * trial.  bench_make_burst emits preamble + payload + postamble and
 * returns the burst length. */

void bench_fill_payload(unsigned char *payload, int nbytes, bench_rng *rng);
int bench_make_burst(struct freedv *tx, short *burst,
                     const unsigned char *payload);

/* ---- The receiver loop ----
 *
 * Feeds the burst plus `tail` samples of unit-sigma noise (from `noise`),
 * honouring nin, counting full RX_SYNC as "acquired" (TRIAL_SYNC fires on
 * noise and must not count) and CRC-ok frames as "delivered".  Clamps
 * generated samples to int16. */

void bench_run_burst(struct freedv *rx, const short *samples, int n, int tail,
                     double sigma, unsigned char *out, bench_noise_fn noise,
                     void *noise_ctx, int *saw_sync, int *got_frame);

/* Pure-noise control: feeds exactly n + tail samples of noise from `seq`
 * (clamped) and counts full RX_SYNC -- any sync here is a false alarm.  The
 * window matches the signal trials' exactly. */
void bench_run_noise_only(struct freedv *rx, double sigma, int n, int tail,
                          bench_seq *seq, int *saw_sync);

/* ---- Analytic frequency shift ----
 *
 * Shifts a real int16 passband burst by foff_hz the way a mistuned SSB
 * radio presents it: analytic signal (FFT, negative bins zeroed, zero-
 * padded against circular wrap), complex mix by exp(j*2*pi*f*t), real
 * part.  Multiplying the real burst by cos() instead produces two
 * half-amplitude images at fc +- f -- a different signal, and the reason
 * the first offset sweep had to be redone.  No-op at foff_hz == 0. */
void bench_freq_shift_real(short *buf, int n, double foff_hz, double fs);

/* ---- Mode lookup shared by the tools ---- */

int bench_mode_from_name(const char *s); /* -1 if unknown */

#endif /* MERCURY_BENCH_COMMON_H */
