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

#endif /* MERCURY_MFSK_SYNC_H */
