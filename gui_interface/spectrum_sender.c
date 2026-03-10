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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../common/os_interop.h"
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

#include "spectrum_sender.h"
#include "../common/hermes_log.h"

#define SPECTRUM_LOG_TAG "spectrum-tx"

/* ---------- init ---------- */

int spectrum_tx_init(spectrum_tx_t *stx, const char *ip, uint16_t port)
{
    stx->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (stx->sock < 0) {
        HLOGE(SPECTRUM_LOG_TAG, "socket(): %s", strerror(errno));
        return -1;
    }

    memset(&stx->dest, 0, sizeof(stx->dest));
    stx->dest.sin_family = AF_INET;
    stx->dest.sin_port   = htons(port);

    if (inet_pton(AF_INET, ip, &stx->dest.sin_addr) <= 0) {
        HLOGE(SPECTRUM_LOG_TAG, "inet_pton(%s): %s", ip, strerror(errno));
        SOCK_CLOSE(stx->sock);
        stx->sock = -1;
        return -1;
    }

    return 0;
}

/* ---------- send ---------- */

int spectrum_tx_send(spectrum_tx_t *stx,
                     const float *mag_spec_dB,
                     uint16_t fft_size,
                     uint16_t sample_rate)
{
    /* Header: magic(4) + fft_size(2) + sample_rate(2) = 8 bytes */
    const size_t hdr_sz  = 8;
    const size_t data_sz = (size_t)fft_size * sizeof(float);
    const size_t pkt_sz  = hdr_sz + data_sz;

    /* Allocate exact-size buffer on the heap — avoids stack overflow for large
     * FFT sizes (e.g. MODEM_STATS_NSPEC=512 → 2056 bytes) and removes the
     * old 1400-byte MTU cap that was silently dropping all spectrum packets. */
    uint8_t *buf = malloc(pkt_sz);
    if (!buf) {
        HLOGE(SPECTRUM_LOG_TAG, "malloc(%zu) failed: %s", pkt_sz, strerror(errno));
        return -1;
    }

    /* Little-endian header */
    uint32_t magic = (uint32_t)SPECTRUM_MAGIC;
    /* magic (4 bytes, little-endian) */
    buf[0] = (uint8_t)(magic & 0xFF);
    buf[1] = (uint8_t)((magic >> 8) & 0xFF);
    buf[2] = (uint8_t)((magic >> 16) & 0xFF);
    buf[3] = (uint8_t)((magic >> 24) & 0xFF);
    /* fft_size (2 bytes, little-endian) */
    buf[4] = (uint8_t)(fft_size & 0xFF);
    buf[5] = (uint8_t)((fft_size >> 8) & 0xFF);
    /* sample_rate (2 bytes, little-endian) */
    buf[6] = (uint8_t)(sample_rate & 0xFF);
    buf[7] = (uint8_t)((sample_rate >> 8) & 0xFF);

    /* Payload: float32 array (platform native == little-endian on x86/ARM) */
    memcpy(buf + hdr_sz, mag_spec_dB, data_sz);

    ssize_t sent = sendto(stx->sock, buf, pkt_sz, 0,
                          (struct sockaddr *)&stx->dest,
                          sizeof(stx->dest));
    free(buf);

    if (sent < 0) {
        HLOGE(SPECTRUM_LOG_TAG, "sendto(): %s", strerror(errno));
        return -1;
    }

    return (int)sent;
}

/* ---------- close ---------- */

void spectrum_tx_close(spectrum_tx_t *stx)
{
    if (stx->sock >= 0) {
        SOCK_CLOSE(stx->sock);
        stx->sock = -1;
    }
}
