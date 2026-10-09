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
#include "mercury_prng.h"

/* Maximum number of paths supported */
#define WATTERSON_MAX_PATHS 4

/* Doppler spectrum of the tap process (watterson_set_spectrum, before the
 * paths are added).  GAUSSIAN is the model of ITU-R F.1487 / CCIR 520 and the
 * default; LEGACY is the 2nd-order Butterworth this file shipped with up to
 * v1.9.17, kept so the numbers in docs/MODES.md reproduce from the same
 * commands. */
#define WATTERSON_SPECTRUM_GAUSSIAN 0
#define WATTERSON_SPECTRUM_LEGACY   1

typedef struct watterson_path
{
    /* Path configuration */
    float delay_ms;      /* Path delay in milliseconds */
    float doppler_hz;    /* Doppler spread in Hz.  WATTERSON_SPECTRUM_GAUSSIAN
                          * (the default): the ITU-R F.1487 / CCIR 520 "Doppler
                          * spread", i.e. twice the standard deviation of the
                          * Gaussian Doppler power spectrum (0.1 Hz CCIR Good ..
                          * 30 Hz ITU-R high-latitude disturbed).
                          * WATTERSON_SPECTRUM_LEGACY: the 3 dB cutoff of the
                          * 2nd-order Butterworth tap filter, which measures as
                          * the spectrum's rms width (sigma), so the legacy
                          * "1 Hz" fades about twice as fast as CCIR Poor. */
    int   spectrum;      /* WATTERSON_SPECTRUM_* this path was built with */

    /* Gaussian spectrum: the tap is produced at a low rate fs/interp_ratio by
     * a Gaussian FIR driven by complex white Gaussian noise (impulse response
     * exp(-4 pi^2 sigma^2 t^2), so |H(f)|^2 is Gaussian with std sigma), then
     * linearly interpolated to the sample rate.  Linear is enough: the process
     * has no energy above ~3 sigma and is sampled at 64 x the spread. */
    float *fir;          /* FIR taps, normalised so E[|tap|^2] = 1           */
    int    fir_len;
    COMP  *fir_hist;     /* circular history of the driving noise            */
    int    fir_idx;
    int    interp_ratio; /* sample-rate samples per low-rate tap sample      */
    int    interp_phase; /* 0 .. interp_ratio-1                              */
    COMP   tap_prev, tap_next;
    float freq_hz;       /* Frequency offset / Doppler shift in Hz */
    float gain;          /* Linear gain factor (0.0 to 1.0) */
    float tap_norm;      /* normalises the Doppler-shaped tap to unit average
                          * power (E[|tap|^2]=1).  The narrow Butterworth LPF
                          * otherwise passes only a tiny fraction of the driving
                          * white-noise power, attenuating the signal ~20-30 dB. */

    /* IIR filter state for Doppler shaping (2nd-order Butterworth LPF)
     * Separate filters are used for the I and Q components */
    /* double, not float: at a high sample rate with a slow Doppler the
     * cutoff ratio fc/fs becomes tiny and the Butterworth poles sit within
     * ~1e-4 of the unit circle. float carries only ~6e-8 of relative
     * resolution there, which is enough to push a pole OUTSIDE the circle:
     * the filter then diverges instead of fading. Measured before the change,
     * 2 paths at 1 Hz: 8 kHz gave a sane SNR3k of -8.37 dB but 48 kHz gave
     * +65.76 dB, i.e. the "signal" was the filter blowing up. */
    double x_i[3];       /* Input history, I component */
    double y_i[3];       /* Output history, I component */
    double x_q[3];       /* Input history, Q component */
    double y_q[3];       /* Output history, Q component */

    /* IIR filter coefficients (a0 is normalised to 1.0) */
    double b0, b1, b2;   /* Numerator   (double: see the history arrays) */
    double a1, a2;       /* Denominator (a0 = 1) */

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

    /* Normalises the summed multi-path output to unit average power gain
     * (E[|h|^2]=1, i.e. sum of path powers = 1), so the faded signal carries
     * the same average power as the unfaded input — matching ch.c's hf_gain
     * convention so SNR3k = -No - 14.82 holds. */
    float  chan_norm;

    /* Power accumulators for SNR measurement.  Reset at init and on
     * watterson_reset_meas(); accumulated across watterson_process() calls.
     * The faded signal power is summed BEFORE the AWGN is added, and the
     * actual added-noise power is summed, so the true post-channel SNR is
     * sig_pwr_acc / noise_pwr_acc (independent of the configured noise_var). */
    double sig_pwr_acc;    /* Sum |faded signal|^2 (pre-noise)            */
    double noise_pwr_acc;  /* Sum |noise added|^2                         */
    long   meas_nsamp;     /* samples accumulated into the sums           */

    /* The fading and AWGN draws come from HERE, not rand().
     *
     * Per-instance so two channels in one process cannot perturb each other's
     * realisation, and private so nothing else in the program can either.  It
     * also makes a seed mean the same channel on every platform, which libc's
     * rand() does not: glibc, musl, macOS and MinGW each implement a different
     * generator, so a fading result pinned on one host could not be replayed
     * on another. */
    mercury_prng_t rng;
    unsigned int   seed;   /* the seed rng was last seeded with */
    int            spectrum;  /* WATTERSON_SPECTRUM_* applied to paths added next */
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

/* Select the Doppler spectrum for paths added after this call (default
 * WATTERSON_SPECTRUM_GAUSSIAN).  Returns 0, or -1 for an unknown value. */
int watterson_set_spectrum(watterson_t *w, int spectrum);

/* Named channel presets.  The standard ones (CCIR 520-2 and ITU-R F.1487)
 * use the Gaussian spectrum with doppler_hz as the standard's Doppler spread;
 * the legacy ones reproduce the Butterworth-shaped presets this tool shipped
 * with (watterson_test --good/--moderate/--poor/--flutter, chanutil
 * mpg/mpp/mpd), whose numbers are the ones in docs/MODES.md. */
typedef struct watterson_preset
{
    const char *name;        /* command-line / MERCURY_CH_FADING name */
    const char *title;       /* one line for --help and logs */
    int         spectrum;    /* WATTERSON_SPECTRUM_* */
    int         npaths;
    struct { float delay_ms, doppler_hz, gain; } path[WATTERSON_MAX_PATHS];
} watterson_preset_t;

const watterson_preset_t *watterson_presets(int *count);
const watterson_preset_t *watterson_preset_find(const char *name);   /* NULL if unknown */
/* Reset the path list and apply the preset (the AWGN setting is kept). */
int watterson_apply_preset(watterson_t *w, const watterson_preset_t *preset);

/* Pin the fading realisation to a known seed.
 *
 * Use this to replay one specific channel: bisecting a failure, or A/B-ing two
 * builds over EXACTLY the same fade (which is the only way to compare them at
 * a sample size a laptop can afford). */
void watterson_seed(watterson_t *w, unsigned int seed);

/* Seed from MERCURY_WATTERSON_SEED if set, otherwise from the clock and pid.
 *
 * Called automatically by watterson_init(), so a channel is never unseeded.
 * That used to be possible, and it was silent: the model drew from rand(), and
 * no entry point called srand(), so libc behaved as if seeded with 1 and EVERY
 * run produced the identical fade.  Repeat runs looked like independent
 * agreement while being one sample reported N times.
 *
 * Returns the seed used.  Print it -- an unpinned result is only worth having
 * if the run can be replayed later. */
unsigned int watterson_seed_auto(watterson_t *w);

/* The seed this channel is currently running on.
 *
 * Use this to REPORT the seed; calling watterson_seed_auto() again to find out
 * what init chose would re-seed, discarding the draws watterson_add_path() has
 * already made and yielding a different channel from the one the seed names. */
unsigned int watterson_get_seed(const watterson_t *w);

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

/* Reset the SNR power accumulators (sig_pwr_acc / noise_pwr_acc / meas_nsamp).
 * Called automatically by watterson_init(). */
void watterson_reset_meas(watterson_t *w);

/* Measured post-channel SNR in a 3 kHz bandwidth (dB), from the accumulated
 * faded-signal and added-noise powers.  Returns a large value if no AWGN was
 * added.  Call after processing the stream. */
float watterson_measured_snr3k(const watterson_t *w);

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
