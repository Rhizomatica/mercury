/* MFSK preamble acquisition — pure-C port of v1 cl_ofdm::time_sync_mfsk_corr
 *
 * Non-coherent (envelope) matched-filter preamble detection: two-phase search
 * (coarse 4x-oversampled + fine), per-symbol normalized correlation averaged
 * over the preamble symbols, threshold 0.5. Works below the coherent-OFDM
 * acquisition floor because it needs no phase/frequency lock.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original C++ implementation)
 * Copyright (C) 2026 Rhizomatica (C port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_MFSK_SYNC_H
#define MERCURY_MFSK_SYNC_H

#include <complex.h>
#include "mfsk.h"
#include "mfsk_ofdm.h"

/* Build the preamble time-domain template (preamble_nSymb OFDM symbols) and its
 * per-symbol energy, from the MFSK preamble tones + OFDM framing.
 *   tmpl_out      : caller buffer, >= preamble_nSymb * ofdm_frame_nofdm() samples
 *   sym_energy_out: caller buffer, >= preamble_nSymb doubles
 * Returns the number of template symbols (= m->preamble_nSymb). */
int mfsk_sync_build_template(const mfsk_t *m, const ofdm_frame_t *o,
                             double complex *tmpl_out, double *sym_energy_out);

/* Same, for the postamble tone sequence (for dual-ended acquisition). */
int mfsk_sync_build_postamble_template(const mfsk_t *m, const ofdm_frame_t *o,
                                       double complex *tmpl_out,
                                       double *sym_energy_out);

/* Search baseband for the preamble. rx has rx_len complex samples at
 * interpolation_rate (use 1 for base-rate). Returns the detected start sample
 * offset (metric >= 0.5), or -1 if not found; *out_metric gets the best metric. */
int mfsk_sync_search(const double complex *rx, int rx_len, int interpolation_rate,
                     const double complex *tmpl, const double *sym_energy,
                     int template_nsymb, int Nofdm, int search_start_symb,
                     double *out_metric);

/* Pattern (ACK/BREAK/HAIL) detection: slide the baseband buffer and, for each
 * candidate start, count pattern symbols whose expected hopped tone is the peak
 * bin (per stream). Returns the best matched-symbol count over the buffer;
 * *out_pos gets that start (samples). Detection = return >= match threshold.
 * `tones`/`pattern_len`/`nsymb` come from the mfsk_t (ack_/break_/hail_). */
int mfsk_detect_pattern(const mfsk_t *m, const ofdm_frame_t *o,
                        const double complex *rx, int rx_len,
                        const int *tones, int pattern_len, int nsymb,
                        int *out_pos);

/* Score SEVERAL tone lists over one pass of the buffer.
 *
 * Identical results to calling mfsk_detect_pattern() once per list, at close to
 * the cost of one: the expensive part -- a GI removal, an FFT and a depad for
 * every (candidate start, symbol) pair -- depends only on the buffer, not on
 * which tones are expected, so it is done once and every list scored against
 * the same bins.
 *
 * This matters because the correlator is not cheap.  It was measured consuming
 * 3.5k samp/s against 8k arriving, which is why the ARQ layer only runs it
 * inside bounded windows (see expect_pattern_ack in arq.c).  The shipped
 * ack/break detection was paying that twice over the same samples.
 *
 * scores_out[i] and pos_out[i] (pos_out may be NULL) receive list i's result.
 * All lists share pattern_len and nsymb. */
void mfsk_detect_patterns(const mfsk_t *m, const ofdm_frame_t *o,
                          const double complex *rx, int rx_len,
                          const int *const *tone_lists, int nlists,
                          int pattern_len, int nsymb,
                          int *scores_out, int *pos_out);

/* The same detector, streaming.
 *
 * mfsk_detect_patterns() redoes, for every candidate start, the FFT of every
 * symbol under it -- nsymb FFTs per step -- so it can only afford to run inside
 * short windows.  Every candidate start that lies on the step grid shares its
 * symbols' FFTs with the starts one symbol apart, so the stream keeps each
 * FFT's per-tone energies, once per step, in a ring one pattern long, and
 * scores every list by lookup: one FFT per step for any number of lists.  On
 * the same baseband and grid its scores are bit-identical to the batch
 * detector's (same tie-break metric, summed in the same order).
 *
 * Feed baseband (downmixed, filtered) samples in any chunk sizes.  A list
 * whose score reaches its threshold opens an event; the event is reported,
 * at its best start, once the score has fallen back below the threshold, and
 * the list then holds off for one pattern length.  Positions are absolute
 * sample counts since the last reset. */
#define MFSK_STREAM_MAX_LISTS 24

typedef struct {
    int       list;        /* which tone list */
    long long pos;         /* start sample of the best alignment */
    int       score;       /* matched symbols there */
    double    metric;      /* summed E_target/E_total there */
} mfsk_stream_event_t;

typedef struct mfsk_stream_det mfsk_stream_det_t;

mfsk_stream_det_t *mfsk_stream_det_new(const mfsk_t *m, const ofdm_frame_t *o,
                                       int pattern_len, int nsymb);
void mfsk_stream_det_free(mfsk_stream_det_t *d);
/* Score list idx (0..MFSK_STREAM_MAX_LISTS-1) against tones[pattern_len];
 * threshold <= 0 or tones NULL turns it off.  The list is copied. */
void mfsk_stream_det_set_list(mfsk_stream_det_t *d, int idx, const int *tones, int threshold);
/* Forget all audio and open events (after our own transmission). */
void mfsk_stream_det_reset(mfsk_stream_det_t *d);
/* Feed n samples; up to maxev events are written to ev.  Returns how many. */
int  mfsk_stream_det_push(mfsk_stream_det_t *d, const double complex *bb, int n,
                          mfsk_stream_event_t *ev, int maxev);
/* Best (score, then metric; earliest on a tie) of list idx over every start
 * scored since the reset, thresholds aside -- the batch detector's answer. */
void mfsk_stream_det_best(const mfsk_stream_det_t *d, int idx, int *score, long long *pos);
/* Samples a start needs before it is scored: nsymb symbols. */
int  mfsk_stream_det_span(const mfsk_stream_det_t *d);

#endif /* MERCURY_MFSK_SYNC_H */
