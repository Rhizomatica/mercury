/* OFDM framing for the Mercury MFSK mode — pure-C port of v1 cl_ofdm subset
 *
 * The MFSK-relevant slice of Mercury v1's cl_ofdm (Fadi Jerji, C++): a
 * self-contained radix-2 FFT/IFFT plus subcarrier zero-padding and cyclic-
 * prefix (guard interval) framing. No FFT library dependency. Carries the
 * mfsk.c tone bins to/from OFDM time-domain symbols.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original C++ implementation)
 * Copyright (C) 2026 Rhizomatica (C port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_MFSK_OFDM_H
#define MERCURY_MFSK_OFDM_H

#include <complex.h>

typedef struct
{
    int Nfft;         /* FFT size (power of 2)              */
    int Nc;           /* active subcarriers                 */
    int Ngi;          /* guard interval (cyclic prefix) len */
    int start_shift;  /* subcarrier map start shift         */
} ofdm_frame_t;

/* Nofdm = time samples per OFDM symbol (Nfft + guard interval). */
static inline int ofdm_frame_nofdm(const ofdm_frame_t *o) { return o->Nfft + o->Ngi; }

/* gi is the guard fraction (Ngi = Nfft*gi), e.g. 0.25. */
void ofdm_frame_init(ofdm_frame_t *o, int Nfft, int Nc, double gi, int start_shift);

/* radix-2 FFT/IFFT, in place, n a power of two. Convention (matches v1):
 * ofdm_fft is normalized by 1/n; ofdm_ifft is unnormalized -> fft(ifft(x))=x. */
void ofdm_fft(const ofdm_frame_t *o, const double complex *in, double complex *out);
void ofdm_ifft(const ofdm_frame_t *o, const double complex *in, double complex *out);

/* Subcarrier map: Nc data bins <-> Nfft spectrum (DC-centred, v1 layout). */
void ofdm_zero_padder(const ofdm_frame_t *o, const double complex *in, double complex *out);
void ofdm_zero_depadder(const ofdm_frame_t *o, const double complex *in, double complex *out);

/* Cyclic prefix: gi_adder in[Nfft] -> out[Nfft+Ngi]; gi_remover reverses. */
void ofdm_gi_adder(const ofdm_frame_t *o, const double complex *in, double complex *out);
void ofdm_gi_remover(const ofdm_frame_t *o, const double complex *in, double complex *out);

#endif /* MERCURY_MFSK_OFDM_H */
