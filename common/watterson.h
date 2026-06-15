/* Watterson HF Ionospheric Channel Model (ITU-R F.1487)
 *
 * Copyright (C) 2025-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This implements the Watterson narrowband HF channel model as specified in
 * ITU-R Recommendation F.1487. The model represents the HF skywave channel as
 * a tapped delay line where each tap's gain is a complex Gaussian random
 * process with a Gaussian-shaped Doppler spectrum.
 *
 * Channel impulse response:
 *   h(τ,t) = Σ g_i(t) · δ(τ - τ_i)
 *
 * Each tap gain g_i(t) is a complex Gaussian process whose power spectrum is:
 *   S_i(f) = (1/(√(2π)·σ_i)) · exp(-f²/(2σ_i²))
 *
 * where σ_i is the Doppler spread of the i-th path.
 *
 * The Doppler shaping is implemented using a 2nd-order Butterworth IIR
 * low-pass filter applied to complex white Gaussian noise.
 */

#ifndef watterson_h
#define watterson_h

#include "modem/freedv/comp.h"

/* Maximum number of paths supported */
#define WATTERSON_MAX_PATHS 4

typedef struct watterson_path
{
    /* Path configuration */
    float delay_ms;      /* Path delay in milliseconds */
    float doppler_hz;    /* Doppler spread (σ) in Hz, typically 0.1-2.0 */
    float freq_hz;       /* Frequency offset / Doppler shift in Hz */
    float gain;          /* Linear gain factor (0.0 to 1.0) */

    /* IIR filter state for Doppler shaping (2nd-order Butterworth LPF)
     * Separate filters are used for the I and Q components */
    float x_i[3];        /* Input history, I component */
    float y_i[3];        /* Output history, I component */
    float x_q[3];        /* Input history, Q component */
    float y_q[3];        /* Output history, Q component */

    /* IIR filter coefficients (a0 is normalised to 1.0) */
    float b0, b1, b2;    /* Numerator */
    float a1, a2;        /* Denominator (a0 = 1) */

    /* Frequency offset state */
    float phase;          /* Current phase accumulator for rotation */
    float phase_inc;      /* Phase increment per sample */

    /* Delay line (circular buffer) */
    COMP *delay_buf;      /* Delay line buffer */
    int   delay_samples;  /* Delay length in samples */
    int   delay_idx;      /* Current write position */
} watterson_path_t;

/* Watterson channel simulation object */
typedef struct watterson
{
    int                sample_rate;    /* Sample rate in Hz (e.g. 8000) */
    int                num_paths;      /* Number of active paths */
    watterson_path_t   paths[WATTERSON_MAX_PATHS];

    /* AWGN noise parameters (optional) */
    int    awgn_en;     /* Enable AWGN (0 or 1) */
    float  noise_var;   /* Noise variance per sample */
} watterson_t;

/* Initialise the Watterson channel simulator.
 *
 * Parameters:
 *   w            - Pointer to watterson_t struct to initialise
 *   sample_rate  - Sample rate in Hz
 *
 * Returns 0 on success, -1 on error.
 */
int watterson_init(watterson_t *w, int sample_rate);

/* Release resources allocated by the Watterson simulator. */
void watterson_dispose(watterson_t *w);

/* Add a propagation path to the model.
 *
 * Parameters:
 *   w            - Watterson state
 *   delay_ms     - Path delay in milliseconds
 *   doppler_hz   - Doppler spread (σ) in Hz (e.g. 0.1 for slow, 1.0 for moderate, 2.0 for fast)
 *   freq_hz      - Frequency offset (Doppler shift) in Hz
 *   gain         - Linear gain factor (0.0 to 1.0)
 *
 * Returns the path index (0-based) on success, -1 if max paths reached.
 */
int watterson_add_path(watterson_t *w, float delay_ms, float doppler_hz,
                       float freq_hz, float gain);

/* Set the AWGN noise density.
 *
 * Parameters:
 *   w            - Watterson state
 *   nodb         - Noise density in dB/Hz (e.g. -100)
 *
 * Set nodb <= -160 (or any very negative value) to disable AWGN.
 */
void watterson_set_noise(watterson_t *w, float nodb);

/* Process a block of complex samples through the Watterson model.
 * Samples are processed in-place: input and output share the same buffer.
 *
 * Parameters:
 *   w            - Watterson state
 *   samples      - Buffer of complex samples (modified in-place)
 *   n            - Number of samples to process
 */
void watterson_process(watterson_t *w, COMP *samples, int n);

#endif
