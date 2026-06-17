/* HERMES Modem
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */


#ifndef MODEM_H
#define MODEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "freedv_api.h"

#define TAIL_TIME_US 100000 // 100 ms (in microseconds) tail time after sending data

typedef struct generic_modem {
    struct freedv *freedv;
    int mode;
    size_t payload_bytes_per_modem_frame;
    void *future_extension; // Placeholder for future extensions
} generic_modem_t;

int init_modem(generic_modem_t *g_modem, int mode, int frames_per_burst, int test_mode, int freedv_verbosity);

int run_tests_tx(generic_modem_t *g_modem);

int run_tests_rx(generic_modem_t *g_modem);

int shutdown_modem(generic_modem_t *g_modem);

// always send the frame size in bytes_in
int send_modulated_data(generic_modem_t *g_modem, uint8_t *bytes_in, int frames_per_burst);

int receive_modulated_data(generic_modem_t *g_modem, uint8_t *bytes_out, size_t *nbytes_out);

// Threads
// tx to the modem the data received from the tcp socket
void *tx_thread(void *g_modem);
// rx from the modem and send to the tcp socket
void *rx_thread(void *g_modem);

// Spectrum data for UI waterfall display
// Copies the latest rx spectrum (MODEM_STATS_NSPEC floats) into out_dB.
// Returns the sample rate on success, 0 if no spectrum is available yet.
int modem_get_rx_spectrum(float *out_dB, int max_bins);

/* Enable/disable the RX spectrum FFT (skipped when no UI consumes it). */
void modem_set_spectrum_enabled(bool enabled);

// TX audio gain (linear multiplier on modulator output samples).
// Default 1.0f = no change. Hot-tunable from any thread; the modulator
// reads it once per burst.  Applied with int32 saturation, so any value
// is safe — values that would overflow simply clip cleanly.
void  modem_set_tx_gain(float linear);
float modem_get_tx_gain(void);

// Most recent TX burst peak amplitude in dBFS (0 dBFS = INT32 full-scale,
// i.e. the clip ceiling).  Measured post-gain but pre-saturation, once per
// burst, so a return value above 0 dBFS means the burst clipped by that many
// dB.  Returns -120.0f when no TX has occurred yet or the last burst was
// silent.
float modem_get_tx_peak_dbfs(void);

#endif // MODEM_H
