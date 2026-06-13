/*
 * Polyphase FIR resampler for the 8 kHz modem <-> 48 kHz sound-card paths.
 *
 * Replaces the old linear-interpolation upsampler (weak anti-imaging) and the
 * bare 1-in-6 decimator (no anti-aliasing at all) with a proper linear-phase
 * low-pass FIR, decomposed polyphase for efficiency and carried statefully
 * across read periods so there are no boundary discontinuities (issue #81).
 *
 * Fixed for the 8 kHz <-> 48 kHz, L=M=6 case.  All sample data is int32;
 * coefficients are float, accumulation is double, output is clamped.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIOIO_RESAMPLER_H
#define AUDIOIO_RESAMPLER_H

#include <stdint.h>

#define RESAMP_L                6      /* 8 kHz <-> 48 kHz ratio          */
#define RESAMP_TAPS_PER_PHASE   30
#define RESAMP_NTAPS            (RESAMP_L * RESAMP_TAPS_PER_PHASE)  /* 180 */

/* Build the shared coefficient tables.  Idempotent; call once before use
 * (thread-safe to call repeatedly from init paths — it just recomputes). */
void resampler_global_init(void);

/* --- Upsampler: 8 kHz -> 48 kHz (anti-imaging) --- */
typedef struct {
    int32_t hist[RESAMP_TAPS_PER_PHASE];  /* last K input samples (8 kHz)  */
} resamp_up_t;

void resamp_up_reset(resamp_up_t *r);

/* Produce n_in*6 output samples (48 kHz) from n_in input samples (8 kHz).
 * out must hold at least n_in*RESAMP_L int32 samples.  Returns the count. */
int  resamp_up_process(resamp_up_t *r, const int32_t *in, int n_in,
                       int32_t *out);

/* --- Downsampler: 48 kHz -> 8 kHz (anti-aliasing) --- */
typedef struct {
    int32_t hist[RESAMP_NTAPS];           /* last N input samples (48 kHz) */
    int     phase;                        /* 0..L-1 decimation phase       */
} resamp_down_t;

void resamp_down_reset(resamp_down_t *r);

/* Consume n_in input samples (48 kHz), emit the decimated outputs (8 kHz) to
 * out (must hold at least n_in/6 + 1 samples).  Returns the number emitted. */
int  resamp_down_process(resamp_down_t *r, const int32_t *in, int n_in,
                         int32_t *out);

#endif /* AUDIOIO_RESAMPLER_H */
