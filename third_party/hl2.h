/* -x hl2: a Hermes-Lite 2 on the network as Mercury's radio and sound card.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mercury is the SDR host: it streams 48 kHz I/Q with the radio over
 * openHPSDR protocol 1 (hl2_wire.h), does the upper-sideband modulation
 * itself (hl2_dsp.h), tunes the radio and keys it in the stream.  There is
 * no sound card, CAT or PTT line to configure; the device string is the
 * radio's address:
 *
 *   -i auto | ""          the first Hermes-Lite 2 that answers a broadcast
 *   -i <host>[:<port>]    that radio (port 1024 unless given)
 *
 * The frequency, RX gain, TX drive and PA switch are live settings
 * (hl2_set_*); without a frequency the radio receives but never transmits.
 */
#ifndef HL2_H_
#define HL2_H_

#include <stdbool.h>
#include <stdint.h>

#include "audioio.h"

#define HL2_DEFAULT_RX_GAIN_DB 20
#define HL2_DEFAULT_TX_DRIVE   128   /* of 255: about -3.5 dB, ~2 W on the PA */

typedef struct {
    char    addr[64];                /* "ip:port", usable as the device string */
    uint8_t mac[6];
    uint8_t gateware, gateware_minor, board_id;
    bool    busy;
} hl2_board;

typedef struct {
    const char *device;
    void (*health)(bool capture, audio_health_t st, const char *reason);
    volatile bool *stop;
} hl2_ctx;

/* The transport: discovery, streaming and keying until *stop or
 * mercury_stop.  Reports both audio paths' health through ctx->health. */
void *hl2_transport_thread(void *ctx);

/* Broadcast a discovery and collect the replies for timeout_ms. */
int hl2_discover(hl2_board *boards, int max, int timeout_ms);

void     hl2_set_frequency(uint64_t hz);      /* 0 = unset: never transmit */
uint64_t hl2_get_frequency(void);
void     hl2_set_rx_gain_db(int db);          /* -12..+48 */
int      hl2_get_rx_gain_db(void);
void     hl2_set_tx_drive(int level);         /* 0..255 */
int      hl2_get_tx_drive(void);
void     hl2_set_pa(bool on);
bool     hl2_get_pa(void);

#endif /* HL2_H_ */
