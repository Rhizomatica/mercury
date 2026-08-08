/* chanutil — route a real int16 passband burst through the project's own
 * Watterson channel (common/watterson.c), so different instruments measure
 * different waveforms on the SAME validated channel.
 *
 * Every harness that rolls its own fading model produces numbers that cannot
 * be compared with any other harness's.  This exists so the directed-pattern
 * ACCEPT and the DATAC16 ACCEPT it would replace can be put on one axis.
 *
 * The signal path mirrors utils/watterson_test.c: Hilbert transform to an
 * analytic signal, Watterson (multipath + Doppler + AWGN), then the real part.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_CHANUTIL_H
#define MERCURY_CHANUTIL_H

#include <stdint.h>

/* Channel presets, named as docs/MODES.md names them. */
typedef enum {
    CHAN_AWGN = 0,      /* no fading, AWGN only                        */
    CHAN_MPP,           /* ITU "moderate": 2 paths, 1 ms, 1.0 Hz       */
    CHAN_MPG,           /* ITU "good"/calm NVIS: 2 paths, 0.5 ms, 0.1 Hz */
    CHAN_MPD,           /* ITU "disturbed": 2 paths, 2 ms, 2.0 Hz      */
    /* Calibration only: AWGN over a single STATIC path, i.e. the same
     * Hilbert -> Watterson -> Re{} pipeline as the fading presets but with no
     * fading.  Compare against a harness's own direct real-domain AWGN: any
     * difference is this pipeline's calibration error, not the channel's. */
    CHAN_AWGN_C
} chan_preset_t;

int chanutil_preset_from_name(const char *s);   /* -1 if unknown */
const char *chanutil_preset_name(int preset);

/* Persistent channel.
 *
 * The one-shot call below re-initialises the model per burst, which costs a
 * tap warm-up EVERY time (seconds of audio at low Doppler) and makes a sweep
 * of any size infeasible: 50 trials x 7 points was ~47 minutes of warm-up
 * before a single FFT.  It is also less realistic -- a real link presents ONE
 * continuous fading process, not an independent one per burst.  Open a channel
 * once, warm it once, then run every burst of the sweep through it.
 */
typedef struct chanutil chanutil_t;

chanutil_t *chanutil_open(int preset, float no_dbhz, unsigned seed);
void        chanutil_close(chanutil_t *c);

/* Fade + noise one burst through an already-warmed channel. */
int chanutil_run(chanutil_t *c, int16_t *pb, int n, float *snr3k_out);

/* Fade + noise `pb[0..n)` in place.  Convenience wrapper: opens, warms,
 * runs and closes.  Fine for a handful of bursts, far too slow for a sweep.
 *
 * `no_dbhz` is the AWGN noise density the Watterson model uses.  SNR is an
 * OUTPUT, not an input: the achieved 3 kHz-referenced SNR is written to
 * *snr3k_out (measured by the model from the actual faded signal and added
 * noise powers), because under fading the delivered SNR is not something the
 * caller can dial directly.
 *
 * `seed` selects the fading realisation, so a sweep can hold the channel fixed
 * across waveforms or vary it per trial.
 *
 * Returns 0 on success, -1 on error. */
int chanutil_fade(int16_t *pb, int n, int preset, float no_dbhz,
                  unsigned seed, float *snr3k_out);

#endif /* MERCURY_CHANUTIL_H */
