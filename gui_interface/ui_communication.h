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

// WebSocket-based UI communication interface (POSIX).
// Bidirectional: backend publishes status/device lists, UI sends commands.

#ifndef UI_COMMUNICATION_H
#define UI_COMMUNICATION_H

#include <stdint.h>
#include <pthread.h>

#include "websocket/mercury_websocket.h"

// ---- Default ports for UI <-> backend communication ----
// WebSocket server port = UI_DEFAULT_PORT (single bidirectional channel)
#define UI_DEFAULT_PORT 10000
// Status publish interval in microseconds (500ms)
#define UI_PUBLISH_INTERVAL_US 500000
// Spectrum publish interval in microseconds (50ms = 20 fps)
#define SPECTRUM_PUBLISH_INTERVAL_US 50000

// ---- Direction type ----
typedef enum {
    DIR_RX,
    DIR_TX
} modem_direction_t;

// ---- Status snapshot (for change-detection / rate limiting) ----
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

// ---- UI context ----
typedef struct ui_ctx ui_ctx_t;

struct ui_ctx {
    // WebSocket server (bidirectional: status TX + command RX)
    ws_ctx_t ws;
    uint16_t ws_port;           // WebSocket listen port (default=10000)

    pthread_t pub_tid;
    pthread_t spec_tid;         // dedicated spectrum publisher thread (20 fps)
    int waterfall_enabled;      // 1 = send spectrum data to UI, 0 = disabled

    // Audio subsystem info for soundcard enumeration
    int audio_system;                // AUDIO_SUBSYSTEM_* constant
    char selected_capture_dev[64];   // currently active capture (input) device
    char selected_playback_dev[64];  // currently active playback (output) device
    int rx_input_channel;            // LEFT=0, RIGHT=1, STEREO=2

    // Radio list is sent once at startup and again after set_radio_config
    volatile int radio_list_pending;      // 1 = need to (re-)send radio list to UI

    // Soundcard lists and input_channel are sent when a new UI client connects
    volatile int soundcard_list_pending;  // 1 = need to send capture/playback/input_channel to UI

    // For logging rate limiting
    modem_status_t last_sent_status;
};

// ---- API ----

// Publisher thread (periodically sends modem status to the UI)
void *ui_publisher_thread(void *arg);

// Spectrum publisher thread (sends FFT data to the UI at ~20 fps)
void *spectrum_publisher_thread(void *arg);

// High-level init/shutdown for the UI communication subsystem
// ws_port: WebSocket server port (default=10000)
// waterfall_enabled: 1 = start spectrum publisher thread (default), 0 = skip it
// audio_system: AUDIO_SUBSYSTEM_* constant for soundcard enumeration
// selected_capture: currently active capture device name (may be NULL)
// selected_playback: currently active playback device name (may be NULL)
int ui_comm_init(ui_ctx_t *ctx, uint16_t ws_port, int waterfall_enabled,
                 int audio_system, const char *selected_capture, const char *selected_playback,
                 int rx_input_channel);
void ui_comm_shutdown(ui_ctx_t *ctx);

#endif
