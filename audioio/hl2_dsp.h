/* Upper-sideband modem audio <-> 48 kHz I/Q for the -x hl2 transport.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RX: I/Q at 48 kHz -> low-pass and decimate by 6 -> complex band-pass on
 *     the positive frequencies (the upper sideband) -> real part -> AGC ->
 *     8 kHz int32 for the capture ring.
 * TX: 8 kHz int32 from the playback ring -> the same complex band-pass (the
 *     analytic signal: upper sideband only) -> interpolate by 6 -> I/Q at
 *     48 kHz, int16.
 *
 * Passband 150-2950 Hz above the dial frequency, covering every Mercury
 * waveform up to BW2750.  Opposite-sideband and image rejection > 60 dB.
 *
 * The AGC is what a receiver does before its audio output: the modem
 * converts the ring to int16, and an HF signal at the ADC sits 60-100 dB
 * below full scale.  It holds the 20 ms block RMS at HL2_DSP_AGC_TARGET with
 * a ~200 ms time constant, drops at once when a peak would clip, and ramps
 * across each block so a gain step makes no click.
 *
 * Pinned by tests/audioio/test_hl2_dsp.c.
 */
#ifndef HL2_DSP_H_
#define HL2_DSP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HL2_DSP_L          6                  /* 48 kHz / 8 kHz             */
#define HL2_DSP_NA         132                /* 48 kHz low-pass (22 x 6)   */
#define HL2_DSP_NB         255                /* 8 kHz complex band-pass    */
#define HL2_DSP_BLOCK      160                /* AGC block, 20 ms at 8 kHz  */
#define HL2_DSP_AGC_TARGET 0.1f               /* block RMS, -20 dBFS        */

typedef struct {
    float xi[2 * HL2_DSP_NA], xq[2 * HL2_DSP_NA];   /* doubled delay lines */
    int   pa, phase;
    float zi[2 * HL2_DSP_NB], zq[2 * HL2_DSP_NB];
    int   pb;
    float block[HL2_DSP_BLOCK];
    int   nblock;
    bool  block_muted;                              /* the block saw TX */
    float gain;
} hl2_rx;

typedef struct {
    float x[2 * HL2_DSP_NB];
    int   pb;
    float zi[2 * (HL2_DSP_NA / HL2_DSP_L)], zq[2 * (HL2_DSP_NA / HL2_DSP_L)];
    int   pa;
} hl2_tx;

void hl2_rx_init(hl2_rx *rx);

/* n I/Q pairs (interleaved, +-1.0) in; returns the number of 8 kHz samples
 * written to out (room for n / 6 + HL2_DSP_BLOCK).  mute (transmitting)
 * feeds silence through the filters and leaves the AGC where it was. */
size_t hl2_rx_process(hl2_rx *rx, const float *iq, size_t n, bool mute, int32_t *out);

void hl2_tx_init(hl2_tx *tx);

/* n 8 kHz samples in; writes 6 * n I/Q pairs (interleaved int16).  The
 * envelope follows the audio's amplitude (a full-scale tone is a full-scale
 * carrier) and is limited to full scale without changing its phase. */
void hl2_tx_process(hl2_tx *tx, const int32_t *in, size_t n, int16_t *iq);

/* The filters' frequency responses, for the tests: the 48 kHz low-pass at
 * hz, and the band-pass at hz (negative = the lower sideband).  Linear. */
double hl2_dsp_lowpass_gain(double hz);
double hl2_dsp_bandpass_gain(double hz);

#endif /* HL2_DSP_H_ */
