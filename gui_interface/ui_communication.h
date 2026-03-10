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

// UDP JSON sender + receiver interface (POSIX).
// Threaded receiver, variadic JSON key/value sender.

#ifndef UDP_JSON_H
#define UDP_JSON_H

#include <stdint.h>
#include <pthread.h>

#include "../common/os_interop.h"
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include "spectrum_sender.h"

// ---- Default UDP ports for UI <-> backend communication ----
// Backend sends status TO UI on UI TX port = UI base port (UI listening)
// Backend listens for commands FROM UI on UI RX port = UI base port + 1 (UI sending)
// Spectrum / waterfall data is sent to UI_BASE_PORT + 2
#define UI_BASE_PORT 10000
#define UI_DEFAULT_IP "127.0.0.1"
// Status publish interval in microseconds (500ms)
#define UI_PUBLISH_INTERVAL_US 500000
// Spectrum publish interval in microseconds (50ms = 20 fps)
#define SPECTRUM_PUBLISH_INTERVAL_US 50000

// ---- Direction type ----
typedef enum {
    DIR_RX,
    DIR_TX
} modem_direction_t;

// ---- Status message ----
typedef struct {
    int bitrate;
    double snr;
    char user_callsign[32];
    char dest_callsign[32];
    int sync;                 // bool
    modem_direction_t dir;
    int client_tcp_connected; // bool
    long bytes_transmitted;
    long bytes_received;
} modem_status_t;

// ---- Message types ----
typedef enum {
    MSG_UNKNOWN,
    MSG_STATUS,
    MSG_CONFIG,
    MSG_SOUNDCARD_LIST,
    MSG_RADIO_LIST
} message_type_t;

// ---- Unified message ----
typedef struct {
    message_type_t type;

    // Status
    modem_status_t status;

    // Config
    char soundcard[64];   // string or index
    int broadcast_port;
    int arq_base_port;
    char aes_key[128];
    int encryption_enabled; // bool
    // Soundcard list
    struct {
        char selected[64];
        char list[512]; // JSON array string
    } soundcard_list;
    // Radio list
    struct {
        char selected[64];
        char list[512]; // JSON array string
    } radio_list;
} modem_message_t;

// ---- TX handle ----
typedef struct {
    int sock;
    struct sockaddr_in dest;
} udp_tx_t;

// ---- RX thread args ----
typedef struct {
    uint16_t listen_port;
} rx_args_t;

// ---- Publisher thread context ----
typedef struct {
    udp_tx_t tx;
    spectrum_tx_t spectrum_tx;  // spectrum/FFT sender for waterfall display
    uint16_t rx_port;
    pthread_t rx_tid;
    pthread_t pub_tid;
    pthread_t spec_tid;         // dedicated spectrum publisher thread (20 fps)
    int waterfall_enabled;      // 1 = send spectrum data to UI, 0 = disabled

    // For logging rate limiting
    modem_status_t last_sent_status;
} ui_ctx_t;

// ---- API ----
int udp_tx_init(udp_tx_t *tx, const char *ip, uint16_t port);
void udp_tx_close(udp_tx_t *tx);
int udp_tx_send_json_pairs(udp_tx_t *tx, ...);

// Helpers for specific messages
int udp_tx_send_status(udp_tx_t *tx,
                       int bitrate, double snr,
                       const char *user_callsign,
                       const char *dest_callsign,
                       int sync, modem_direction_t dir,
                       int client_tcp_connected,
                       long bytes_transmitted,
                       long bytes_received,
                       int waterfall_enabled);

int udp_tx_send_config(udp_tx_t *tx,
                       const char *soundcard,
                       int broadcast_port,
                       int arq_base_port,
                       const char *aes_key,
                       int encryption_enabled);

int udp_tx_send_soundcard_list(udp_tx_t *tx,
                               const char *selected_soundcard,
                               const char *soundcards[], int count);

int udp_tx_send_radio_list(udp_tx_t *tx,
                           const char *selected_radio,
                           const char *radios[], int count);


// RX thread (listens for commands from the UI)
void *rx_thread_main(void *arg);

// Publisher thread (periodically sends modem status to the UI)
void *ui_publisher_thread(void *arg);

// Spectrum publisher thread (sends FFT data to the UI at ~20 fps)
void *spectrum_publisher_thread(void *arg);

// High-level init/shutdown for the UI communication subsystem
// waterfall_enabled: 1 = start spectrum publisher thread (default), 0 = skip it
int ui_comm_init(ui_ctx_t *ctx, const char *ip, uint16_t tx_port, int waterfall_enabled);
void ui_comm_shutdown(ui_ctx_t *ctx);

#endif
