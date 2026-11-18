/* mpsk -- few-carrier differential QPSK rungs for the 0..+6 dB stretch.
 *
 * Each carrier runs differential QPSK at 100 baud with a raised-cosine
 * (one-symbol) or root-raised-cosine pulse.  Marker symbols ("no change")
 * are spread through the frame, and every symbol carries a known
 * scrambling phase.  There is no preamble: the receiver finds a frame by
 * correlating the differential products with the scrambling sequence (the
 * markers directly, the data through their fourth power, which removes the
 * QPSK modulation), estimates the frequency offset from the same sum, and
 * demodulates differentially, so nothing depends on a channel estimate --
 * what keeps it working under fading, where coherent OFDM needs ~5 dB more.
 * The code is LDPC (PEG repeat-accumulate, utils/ldpc_ira_peg.py) with a
 * CRC-16.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MPSK_H_
#define MPSK_H_

#include <stdint.h>

#define MPSK_FS        8000
#define MPSK_SPS       80            /* 100 baud */
#define MPSK_DEC       5             /* the receiver runs at 1600 Hz ...     */
#define MPSK_RSPS      (MPSK_SPS / MPSK_DEC)   /* ... 16 samples a symbol    */
#define MPSK_MAXC      10
#define MPSK_LEAD      8             /* lead-in symbols, 80 ms              */

typedef struct {
    const char *name;
    int nc;                          /* carriers                            */
    int fc[MPSK_MAXC];               /* their centres, Hz                   */
    int nsym;                        /* symbols a carrier: reference + diffs */
    int nmark;                       /* markers a carrier                   */
    int rrc_span;                    /* 0: one-symbol RC; else RRC +/- span  */
    const char *code;                /* LDPC code (modem/freedv/ldpc_codes.c) */
} mpsk_level_t;

extern const mpsk_level_t mpsk_levels[];
extern const int mpsk_nlevels;

typedef struct mpsk mpsk_t;

mpsk_t *mpsk_open(int level);
void    mpsk_close(mpsk_t *m);
const mpsk_level_t *mpsk_level(const mpsk_t *m);
int     mpsk_frame_bytes(const mpsk_t *m);     /* payload + 2 CRC bytes      */
int     mpsk_tx_samples(const mpsk_t *m);      /* one frame on the air       */

/* One frame (mpsk_frame_bytes(): the payload and room for its CRC, which is
 * written here) into out[mpsk_tx_samples()], scaled to |peak|. */
int  mpsk_modulate(mpsk_t *m, uint8_t *frame, int16_t *out, double peak);

/* Find the strongest frame in in[n] and decode it.  1 with frame[] filled
 * and its CRC good; 0 otherwise.  *found: a detection was made; *snr_db: a
 * raw estimate from the markers (may be NULL). */
int  mpsk_demod(mpsk_t *m, const int16_t *in, int n, uint8_t *frame, int *found, float *snr_db);

int mpsk_debug_codeword(mpsk_t *m, uint8_t *frame, uint8_t *cwout);
const float *mpsk_debug_llr(const mpsk_t *m);
int mpsk_debug_lock(const mpsk_t *m, int off, double *chi);
void mpsk_debug_errmap(const mpsk_t *m, const uint8_t *cw, char *out, int cap);

#endif
