/* HERMES Modem - Spectrum Data Sender for UI Waterfall Display
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

/*
 * Sends FFT / power-spectrum data to the Mercury QT UI via UDP.
 *
 * Binary protocol (little-endian):
 *   Offset  Size   Field
 *   0       4      Magic   0x4D435259  ("MCRY")
 *   4       2      uint16  fft_size    (number of float32 values)
 *   6       2      uint16  sample_rate (Hz)
 *   8       N*4    float32[] power values in dB
 *
 * The modem already maintains an FFT buffer via modem_stats; this
 * module simply packages the latest spectrum and sends it as a
 * compact datagram.
 */

#ifndef SPECTRUM_SENDER_H
#define SPECTRUM_SENDER_H

#include <stdint.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#define SPECTRUM_MAGIC      0x4D435259  /* "MCRY" */

/* Opaque handle */
typedef struct {
    int sock;
    struct sockaddr_in dest;
} spectrum_tx_t;

/**
 * Initialise the spectrum sender socket.
 * @param stx   Pointer to sender handle.
 * @param ip    Destination IP (e.g. "127.0.0.1").
 * @param port  Destination UDP port (UI TX port + 2).
 * @return 0 on success, -1 on error.
 */
int spectrum_tx_init(spectrum_tx_t *stx, const char *ip, uint16_t port);

/**
 * Send one spectrum frame to the UI.
 * @param stx          Sender handle.
 * @param mag_spec_dB  Array of FFT magnitudes in dB.
 * @param fft_size     Number of elements in mag_spec_dB.
 * @param sample_rate  Audio sample rate in Hz.
 * @return bytes sent, or -1 on error.
 */
int spectrum_tx_send(spectrum_tx_t *stx,
                     const float *mag_spec_dB,
                     uint16_t fft_size,
                     uint16_t sample_rate);

/**
 * Close the spectrum sender socket.
 */
void spectrum_tx_close(spectrum_tx_t *stx);

#endif /* SPECTRUM_SENDER_H */
