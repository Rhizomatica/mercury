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

#endif /* MERCURY_MFSK_SYNC_H */
