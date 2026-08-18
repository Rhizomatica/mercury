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
 * Two engines live here.
 *
 * The INTEGER one (resampler_ratio_for_rate, resamp_up_*, resamp_down_*)
 * handles device rates that are integer multiples of 8 kHz, and is the path
 * every ordinary sound card takes.
 *
 * The RATIONAL one (resamp_rat_*) handles any supported rate by converting
 * L/M in lowest terms, which is what the 44.1 kHz family needs: 44100 -> 8000
 * is 80/441, and 11025/22050/88200/176400 all reduce to the same M = 441 with
 * a different L.  Windows and macOS have no equivalent of ALSA's plug layer,
 * so a card left at 44.1 kHz in the OS sound settings simply could not be
 * used before (issue #193).
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

#include <stdbool.h>
#include <stdint.h>

#define RESAMP_MODEM_FS         8000   /* the modem's native rate            */
#define RESAMP_L                6      /* default ratio: 8 kHz <-> 48 kHz    */
#define RESAMP_L_MAX            12     /* widest INTEGER ratio: 8 kHz <-> 96 kHz */

/* Widest out/in ratio ANY engine can produce, integer or rational: 8 kHz ->
 * 192 kHz is 24.  Callers that must size a buffer before the device rate is
 * known (the playback scratch is allocated before open()) have to use this,
 * not RESAMP_L_MAX -- the rational engine is reached precisely when the ratio
 * exceeds what the integer one handles, so sizing by RESAMP_L_MAX would be
 * too small for exactly the rates that need it.
 * resampler_ratio_max_covers_all_rates() in the tests pins the two together. */
#define RESAMP_RATIO_MAX        24

/* resamp_rat_max_out() can exceed n_in*ratio by this much: the ratio is not an
 * integer, so a block can carry one extra output, plus one for the output that
 * may already be due when the block starts.  A buffer sized from
 * RESAMP_RATIO_MAX alone is short by exactly this. */
#define RESAMP_RAT_OUT_SLACK    2
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
/* The history is a circular buffer stored DOUBLED: every sample is written at
 * both `w` and `w + K`, so the K-sample window ending at the newest sample is
 * always a contiguous descending run and the filter loop needs no modulo and
 * no per-sample shuffling.  Costs K extra words; saves shifting the whole
 * history once per input sample. */
typedef struct {
    int32_t hist[2 * RESAMP_TAPS_PER_PHASE];
    int     w;                            /* index of the newest sample    */
} resamp_up_t;

void resamp_up_reset(resamp_up_t *r);

/* Produce n_in*L output samples from n_in input samples (8 kHz).
 * out must hold at least n_in*L int32 samples.  Returns the count. */
int  resamp_up_process(resamp_up_t *r, const int32_t *in, int n_in,
                       int32_t *out);

/* --- Downsampler: 8000*L -> 8 kHz (anti-aliasing) --- */
/* Doubled circular history, as above.  This is the direction that matters:
 * it runs on every captured sample at the DEVICE rate, so at 48 kHz the old
 * shift moved 180 words 48000 times a second. */
typedef struct {
    int32_t hist[2 * RESAMP_NTAPS_MAX];
    int     w;                            /* index of the newest sample    */
    int     phase;                        /* 0..L-1 decimation phase       */
} resamp_down_t;

void resamp_down_reset(resamp_down_t *r);

/* Consume n_in input samples at the device rate, emit the decimated outputs
 * (8 kHz) to out (must hold at least n_in/L + 1 samples).  Returns the count. */
int  resamp_down_process(resamp_down_t *r, const int32_t *in, int n_in,
                         int32_t *out);

/* ======================================================================
 * Rational resampler: any in_rate -> any out_rate among the supported set
 * ====================================================================== */

/* True if the modem can drive a device at this rate, by either engine. */
bool resampler_rate_supported(int device_rate_hz);

/* Conversion ratio out/in as L/M in lowest terms.  Returns false if either
 * rate is unsupported.  M == 1 means the integer engine can do it. */
bool resampler_rational_for(int in_rate_hz, int out_rate_hz, int *L, int *M);

/* Opaque: the coefficient table is sized from the ratio, so it is heap
 * allocated rather than a worst-case static (44.1 kHz capture needs ~53 KB,
 * which has no business on a thread stack). */
typedef struct resamp_rat resamp_rat_t;

/* NULL if the pair is unsupported or out of memory. */
resamp_rat_t *resamp_rat_create(int in_rate_hz, int out_rate_hz);
void          resamp_rat_free(resamp_rat_t *r);
void          resamp_rat_reset(resamp_rat_t *r);

/* Upper bound on the outputs n_in inputs can produce, for buffer sizing.
 * The true count varies by one between calls: the ratio is not an integer,
 * so a fixed n_in*L/M would be wrong on some blocks. */
int  resamp_rat_max_out(const resamp_rat_t *r, int n_in);

/* Streaming: state carries across calls, so block boundaries are not
 * discontinuities.  Returns the number of samples written to out. */
int  resamp_rat_process(resamp_rat_t *r, const int32_t *in, int n_in,
                        int32_t *out);

/* Ratio actually in use, for logging and tests. */
void resamp_rat_ratio(const resamp_rat_t *r, int *L, int *M);

/* One decimation-filter coefficient.  Exposed so a test can reimplement the
 * FIR and prove the fast history representation did not change any output
 * sample; not part of the audio path.  Returns 0 for out-of-range t. */
float resampler_down_tap(int t);

#endif /* AUDIOIO_RESAMPLER_H */
