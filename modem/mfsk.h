/* Mercury MFSK — non-coherent M-FSK over OFDM subcarriers
 *
 * Pure-C port of Mercury v1's cl_mfsk (C++), originally by Fadi Jerji.
 * Weak-signal modulation: each symbol places one tone per stream in an OFDM
 * subcarrier band; RX does non-coherent energy detection -> soft LLRs, so it
 * works below the coherent OFDM acquisition threshold. Feeds an LDPC decoder.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original C++ implementation)
 * Copyright (C) 2026 Rhizomatica (C port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_MFSK_H
#define MERCURY_MFSK_H

#include <stdbool.h>

#define MFSK_MAX_STREAMS       4
#define MFSK_MAX_PREAMBLE_SYMB 8
#define MFSK_MAX_ACK_TONES     48   /* max for M=4 NB (48-symbol Sidelnikov) */
#define MFSK_HAIL_SUFFIX_LEN   4

/* Frequency-domain sample (one OFDM subcarrier). Plain struct rather than
 * C99 <complex.h> so the codec interops cleanly with any FFT back-end. */
typedef struct { double re, im; } mfsk_cplx;

typedef struct
{
    int M;              /* tones per stream (e.g. 4/8/16/32)      */
    int nBits;          /* log2(M) = bits per stream per symbol   */
    int Nc;             /* total OFDM subcarriers (e.g. 50)       */
    int nStreams;       /* parallel MFSK streams (1..4)           */
    int tone_hop_step;  /* tone hop for frequency diversity       */

    int stream_offsets[MFSK_MAX_STREAMS];   /* first bin per stream */

    int preamble_tones[MFSK_MAX_PREAMBLE_SYMB];
    int preamble_nSymb;

    int ack_tones[MFSK_MAX_ACK_TONES];
    int break_tones[MFSK_MAX_ACK_TONES];
    int hail_tones[MFSK_MAX_ACK_TONES];
    int ack_pattern_len;
    int ack_pattern_nsymb;
    int ack_match_threshold;
    int break_match_threshold;
    int hail_match_threshold;

    int  hail_suffix[MFSK_HAIL_SUFFIX_LEN];
    bool hail_directed;
    int  hail_detect_tones[MFSK_MAX_ACK_TONES + MFSK_HAIL_SUFFIX_LEN];
    int  hail_detect_nsymb;
    int  hail_detect_threshold;
} mfsk_t;

/* Lifecycle. mfsk_init zeroes then configures for (M, Nc, nStreams). */
void mfsk_init(mfsk_t *m, int M, int Nc, int nStreams);
void mfsk_deinit(mfsk_t *m);

/* Effective bits consumed/produced per symbol period. */
static inline int mfsk_bits_per_symbol(const mfsk_t *m)
{
    return m->nBits * m->nStreams;
}

/* Directed-HAIL callsign targeting (FNV-1a-derived tone suffix). */
void mfsk_set_hail_target(mfsk_t *m, const char *callsign, int len);
void mfsk_clear_hail_target(mfsk_t *m);

/* Known-tone sequence generators. out holds <nSymb|pattern> * Nc bins. */
void mfsk_generate_preamble(const mfsk_t *m, mfsk_cplx *out, int nSymb);
void mfsk_generate_ack_pattern(const mfsk_t *m, mfsk_cplx *out);
void mfsk_generate_break_pattern(const mfsk_t *m, mfsk_cplx *out);
void mfsk_generate_hail_pattern(const mfsk_t *m, mfsk_cplx *out);

/* TX: bits -> one-hot subcarrier vectors (mfsk_bits_per_symbol bits/symbol).
 *     symbols_out holds (total_bits / bits_per_symbol) * Nc bins. */
void mfsk_mod(const mfsk_t *m, const int *bits_in, int total_bits,
              mfsk_cplx *symbols_out);

/* RX: non-coherent energy detection -> soft LLRs (clamped to +/-5). */
void mfsk_demod(const mfsk_t *m, const mfsk_cplx *fft_in, int total_bits,
                float *llr_out);

#endif /* MERCURY_MFSK_H */
