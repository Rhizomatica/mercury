/*---------------------------------------------------------------------------*\

  FILE........: codec2_ofdm.h
  AUTHORS.....: David Rowe & Steve Sampson
  DATE CREATED: June 2017

  External user references to the modem library.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2017 David Rowe

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2.1, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#ifndef CODEC2_OFDM_H
#define CODEC2_OFDM_H

/* Includes */

#include <stdbool.h>
#include <stdint.h>

#include "comp.h"
#include "modem_stats.h"

/* Defines */

#ifdef __cplusplus
extern "C" {
#endif

#define OFDM_PEAK 16384 /* peak level of OFDM TX signal */
#define OFDM_CLIP \
  (32767 *        \
   0.35) /* experimentally derived constant to reduce PAPR to about 8dB */

#define UN_SYNC 0 /* Used with the ofdm_set_sync() */
#define AUTO_SYNC 1
#define MANUAL_SYNC 2

#define AUTO_PHASE_EST 0
#define LOCKED_PHASE_EST 1

#define LOW_BW 0
#define HIGH_BW 1

struct OFDM_CONFIG;
struct OFDM;

/* create and destroy modem states */

struct OFDM *ofdm_create(const struct OFDM_CONFIG *config);
void ofdm_destroy(struct OFDM *);
void ofdm_init_mode(char mode[], struct OFDM_CONFIG *config);

/* signal processing */

void ofdm_mod(struct OFDM *, COMP *, const int *);
void ofdm_demod(struct OFDM *, int *, COMP *);
void ofdm_demod_shorts(struct OFDM *, int *, short *, float);
int ofdm_sync_search(struct OFDM *, COMP *);
int ofdm_sync_search_shorts(struct OFDM *, short *, float);
void ofdm_sync_state_machine(struct OFDM *, uint8_t *);
void ofdm_sync_state_machine2(struct OFDM *, uint8_t *);

/* getters */

struct OFDM_CONFIG *ofdm_get_config_param(struct OFDM *ofdm);
int ofdm_get_nin(struct OFDM *);
int ofdm_get_samples_per_frame(struct OFDM *ofdm);
int ofdm_get_samples_per_packet(struct OFDM *ofdm);
int ofdm_get_max_samples_per_frame(struct OFDM *ofdm);
int ofdm_get_bits_per_frame(struct OFDM *ofdm);
int ofdm_get_bits_per_packet(struct OFDM *ofdm);
int ofdm_get_phase_est_bandwidth_mode(struct OFDM *ofdm);

/* option setters */

void ofdm_set_verbose(struct OFDM *, int);
void ofdm_set_timing_enable(struct OFDM *, bool);
void ofdm_set_foff_est_enable(struct OFDM *, bool);
void ofdm_set_phase_est_enable(struct OFDM *, bool);
void ofdm_set_phase_est_bandwidth_mode(struct OFDM *ofdm, int val);
void ofdm_set_off_est_hz(struct OFDM *, float);

/* Enable/disable the FFT-accelerated burst acquisition search (default on).
 * Both paths compute the same joint timing/frequency argmax; this exists so
 * the two can be compared directly, and so a station can fall back if the
 * fast path is ever suspected.  See est_timing_and_freq_fft() in ofdm.c. */
void ofdm_set_acq_fft_enable(struct OFDM *, bool);
/* Acquisition tuning: the preamble-correlation acceptance threshold and the
 * unique-word error budget.  Per-mode defaults live in ofdm_mode.c; these
 * exist so bench tools can sweep them (see utils/acq_tuning_sweep.c). */
void ofdm_set_timing_mx_thresh(struct OFDM *, float);
void ofdm_set_bad_uw_errors(struct OFDM *, int);
float ofdm_get_timing_mx_thresh(struct OFDM *);
int ofdm_get_bad_uw_errors(struct OFDM *);
/* Acquisition telemetry for the offset study: the acceptance metric, the
 * coarse frequency estimate (grid hypothesis + refinement) and the tracked
 * estimate.  Read-only. */
float ofdm_get_timing_mx(struct OFDM *);
float ofdm_get_coarse_foff_est_hz(struct OFDM *);
float ofdm_get_foff_est_hz(struct OFDM *);
/* The span of the coarse frequency search (stock +-50 Hz).  Sets how far
 * from the nominal carrier the acquisition can capture; wider costs one
 * correlation pass per 5 Hz of span.  Bench-facing. */
void ofdm_set_freq_search_range(struct OFDM *, float fmin, float fmax);
/* The coarse grid's step (Hz) and the fine stage's half-width (Hz, 0 =
 * derive from the step).  A wider span for less CPU, if the fine window
 * still covers the coarse grid's uncertainty.  Bench-facing. */
void ofdm_set_freq_search_step(struct OFDM *, float hz);
void ofdm_set_freq_search_fine(struct OFDM *, float hz);
/* Bench access to the four-parameter TX peak config the Hilbert clipper
 * consumes (ofdm_hilbert_clipper): whether the clipper runs at all, its
 * drive (gain1, sets PAPR), the post-BPF peak-restore gain, and the input
 * scale.  Per-mode defaults live in ofdm_mode.c; changing these changes the
 * transmitted waveform's PAPR only -- the receiver is untouched, so a
 * clipped TX interoperates with a stock one.  Enabling the clipper
 * allocates the TX BPF if the mode runs one and it is missing, since the
 * clipper requires it.  See utils/qam16_papr_sweep.c. */
void ofdm_set_tx_clip_config(struct OFDM *, bool clip_en, float gain1,
                             float gain2, float amp_scale);
void ofdm_set_sync(struct OFDM *, int);
void ofdm_set_tx_bpf(struct OFDM *, bool);
void ofdm_set_dpsk(struct OFDM *ofdm, bool val);
void ofdm_set_packets_per_burst(struct OFDM *ofdm, int packetsperburst);

void ofdm_print_info(struct OFDM *);

#ifdef __cplusplus
}
#endif

#endif
