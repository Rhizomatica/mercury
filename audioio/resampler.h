/*
 * Polyphase FIR resampler for the 8 kHz modem <-> sound-card paths.
 *
 * Replaces the old linear-interpolation upsampler (weak anti-imaging) and the
 * bare 1-in-6 decimator (no anti-aliasing at all) with a proper linear-phase
 * low-pass FIR, decomposed polyphase for efficiency and carried statefully
 * across read periods so there are no boundary discontinuities (issue #81).
 *
 * The ratio L is chosen at RUNTIME from the rate the device actually
 * negotiated, not assumed.  Mercury asks for 48 kHz, but a device already
 * locked to another rate by a second client (a shared snd-aloop, say) hands
 * back something else — and resampling by a hardcoded 6 then puts every
 * transmission on the wrong frequency, silently: a 1000 Hz tone measured
 * 166.8 Hz (= 1000/6) out of a device pinned to 8 kHz, at the correct level
 * and spectrally pure.  Nothing about that failure looks like a bug until you
 * count cycles, so the ratio is derived and unsupported rates are refused.
 *
 * Supported device rates are the integer multiples of 8 kHz up to 96 kHz
 * (8/16/24/32/48/96); resampler_ratio_for_rate() reports which.  A caller
 * that gets 0 back must refuse to run that direction rather than transmit
 * off-frequency audio.  44.1 kHz is deliberately NOT supported: it is not an
 * integer multiple of the modem rate and would need a rational (441/80)
 * resampler.
 *
 * Upsample (playback) and downsample (capture) ratios are INDEPENDENT — the
 * two directions can land on different device rates, so they keep separate
 * coefficient tables.
 *
 * All sample data is int32; coefficients are float, accumulation is double,
 * output is clamped.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIOIO_RESAMPLER_H
#define AUDIOIO_RESAMPLER_H

#include <stdint.h>

#define RESAMP_MODEM_FS         8000   /* the modem's native rate            */
#define RESAMP_L                6      /* default ratio: 8 kHz <-> 48 kHz    */
#define RESAMP_L_MAX            12     /* widest supported: 8 kHz <-> 96 kHz */
#define RESAMP_TAPS_PER_PHASE   30
#define RESAMP_NTAPS            (RESAMP_L * RESAMP_TAPS_PER_PHASE)      /* 180 */
#define RESAMP_NTAPS_MAX        (RESAMP_L_MAX * RESAMP_TAPS_PER_PHASE)  /* 360 */

/* Ratio for a negotiated device rate: L such that rate == 8000*L, for
 * 1 <= L <= RESAMP_L_MAX.  Returns 0 if the rate cannot be handled (44100, or
 * anything else that is not an integer multiple of 8 kHz) — callers must treat
 * 0 as fatal for that audio direction. */
int  resampler_ratio_for_rate(int device_rate_hz);

/* Build the coefficient tables.  The two directions are independent; each is
 * idempotent and safe to call again with a different L (it just rebuilds).
 * Call before the matching *_process(). */
void resampler_init_up(int L);     /* 8 kHz -> 8000*L (playback) */
void resampler_init_down(int L);   /* 8000*L -> 8 kHz (capture)  */

/* Back-compatible helper: initialise BOTH directions at the default 1:6. */
void resampler_global_init(void);

/* --- Upsampler: 8 kHz -> 8000*L (anti-imaging) --- */
typedef struct {
    int32_t hist[RESAMP_TAPS_PER_PHASE];  /* last K input samples (8 kHz)  */
} resamp_up_t;

void resamp_up_reset(resamp_up_t *r);

/* Produce n_in*L output samples from n_in input samples (8 kHz).
 * out must hold at least n_in*L int32 samples.  Returns the count. */
int  resamp_up_process(resamp_up_t *r, const int32_t *in, int n_in,
                       int32_t *out);

/* --- Downsampler: 8000*L -> 8 kHz (anti-aliasing) --- */
typedef struct {
    int32_t hist[RESAMP_NTAPS_MAX];       /* last N input samples (device) */
    int     phase;                        /* 0..L-1 decimation phase       */
} resamp_down_t;

void resamp_down_reset(resamp_down_t *r);

/* Consume n_in input samples at the device rate, emit the decimated outputs
 * (8 kHz) to out (must hold at least n_in/L + 1 samples).  Returns the count. */
int  resamp_down_process(resamp_down_t *r, const int32_t *in, int n_in,
                         int32_t *out);

#endif /* AUDIOIO_RESAMPLER_H */
