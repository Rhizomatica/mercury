/* OFDM framing for the Mercury MFSK mode — pure-C port of v1 cl_ofdm subset.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original C++ implementation)
 * Copyright (C) 2026 Rhizomatica (C port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mfsk_ofdm.h"

#include <math.h>

void ofdm_frame_init(ofdm_frame_t *o, int Nfft, int Nc, double gi, int start_shift)
{
    o->Nfft = Nfft;
    o->Nc = Nc;
    o->Ngi = (int)(Nfft * gi);
    o->start_shift = start_shift;
}

/* Recursive radix-2 FFT (decimation-in-time), in place. Direct port of v1's
 * cl_ofdm::_fft (Wickerhauser). n must be a power of two. */
static void rfft(double complex *v, int n, int inverse)
{
    if (n <= 1) return;
    double complex tmp[1024];        /* Nfft <= 1024 for our modes */
    double complex *ve = tmp, *vo = tmp + n / 2;
    for (int k = 0; k < n / 2; k++) { ve[k] = v[2*k]; vo[k] = v[2*k+1]; }
    rfft(ve, n / 2, inverse);
    rfft(vo, n / 2, inverse);
    double s = inverse ? +1.0 : -1.0;
    for (int m = 0; m < n / 2; m++)
    {
        double complex w = cos(2*M_PI*m/(double)n) + s * sin(2*M_PI*m/(double)n) * I;
        double complex z = w * vo[m];
        v[m]       = ve[m] + z;
        v[m + n/2] = ve[m] - z;
    }
}

void ofdm_fft(const ofdm_frame_t *o, const double complex *in, double complex *out)
{
    for (int i = 0; i < o->Nfft; i++) out[i] = in[i];
    rfft(out, o->Nfft, 0);
    for (int i = 0; i < o->Nfft; i++) out[i] /= (double)o->Nfft;   /* normalized */
}

void ofdm_ifft(const ofdm_frame_t *o, const double complex *in, double complex *out)
{
    for (int i = 0; i < o->Nfft; i++) out[i] = in[i];
    rfft(out, o->Nfft, 1);                                          /* unnormalized */
}

void ofdm_zero_padder(const ofdm_frame_t *o, const double complex *in, double complex *out)
{
    int Nfft = o->Nfft, Nc = o->Nc, ss = o->start_shift;
    for (int j = 0; j < Nc/2; j++) out[j + Nfft - Nc/2] = in[j];
    for (int j = 0; j < ss; j++) out[j] = 0;
    for (int j = Nc/2 + ss; j < Nfft - Nc/2; j++) out[j] = 0;
    for (int j = Nc/2; j < Nc; j++) out[j - Nc/2 + ss] = in[j];
}

void ofdm_zero_depadder(const ofdm_frame_t *o, const double complex *in, double complex *out)
{
    int Nfft = o->Nfft, Nc = o->Nc, ss = o->start_shift;
    for (int j = 0; j < Nc/2; j++) out[j] = in[j + Nfft - Nc/2];
    for (int j = Nc/2; j < Nc; j++) out[j] = in[j - Nc/2 + ss];
}

void ofdm_gi_adder(const ofdm_frame_t *o, const double complex *in, double complex *out)
{
    int Nfft = o->Nfft, Ngi = o->Ngi;
    for (int j = 0; j < Nfft; j++) out[j + Ngi] = in[j];
    for (int j = 0; j < Ngi; j++) out[j] = in[j + Nfft - Ngi];
}

void ofdm_gi_remover(const ofdm_frame_t *o, const double complex *in, double complex *out)
{
    for (int j = 0; j < o->Nfft; j++) out[j] = in[j + o->Ngi];
}
