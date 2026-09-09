/* Mercury tone-pattern signalling — known tone sequences over OFDM subcarriers
 *
 * What remains of the MFSK port after the data waveform was removed: the tone
 * geometry and the known sequences built on it (preamble, postamble, ACK,
 * ACK+TURN, HAIL).  There is no modulator, no demodulator and no FEC here --
 * these sequences carry no bits of their own.  A receiver does not decode
 * them, it correlates for them (modem/mfsk/mfsk_sync.h), which is why they
 * survive several dB below where a coded frame on the same air time does.
 *
 * The MFSK data waveform that used to share this geometry was removed: it was
 * measured LOSING to DATAC16 by ~3.7 dB on MPG, so it cost a whole LDPC codec
 * and five code tables to be worse than a mode already in the ladder.  The
 * signalling half won on every axis, and is what stayed.  See
 * docs/ACK-CHANNEL.md.
 *
 * Originally a pure-C port of Mercury v1's cl_mfsk (C++) by Fadi Jerji.
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

    /* Postamble: a second known-tone sequence after the payload, enabling
     * dual-ended acquisition (sync on preamble OR postamble). Distinct tones
     * from the preamble so the two are told apart. */
    int postamble_tones[MFSK_MAX_PREAMBLE_SYMB];
    int postamble_nSymb;

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

/* Directed-HAIL callsign targeting (FNV-1a-derived tone suffix). */
void mfsk_set_hail_target(mfsk_t *m, const char *callsign, int len);
void mfsk_clear_hail_target(mfsk_t *m);

/* Known-tone sequence generators. out holds <nSymb|pattern> * Nc bins. */
void mfsk_generate_preamble(const mfsk_t *m, mfsk_cplx *out, int nSymb);
void mfsk_generate_postamble(const mfsk_t *m, mfsk_cplx *out, int nSymb);
void mfsk_generate_ack_pattern(const mfsk_t *m, mfsk_cplx *out);
void mfsk_generate_break_pattern(const mfsk_t *m, mfsk_cplx *out);
void mfsk_generate_hail_pattern(const mfsk_t *m, mfsk_cplx *out);

#endif /* MERCURY_MFSK_H */
