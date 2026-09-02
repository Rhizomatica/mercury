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
#include <stdatomic.h>
#include <pthread.h>

#include "websocket/mercury_websocket.h"
#include "cfg_utils.h"
#include "ui_status.h"

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
    bool tls_enabled;           // false = plain WS (default), true = WSS

    pthread_t pub_tid;
    pthread_t spec_tid;         // dedicated spectrum publisher thread (20 fps)
    _Atomic bool spec_run;      // drives the spectrum publisher loop (0 = stop)
    int waterfall_enabled;      // 1 = send spectrum data to UI, 0 = disabled

    // Audio subsystem info for soundcard enumeration.  Written by the command
    // handler (websocket server thread or embedded UI goroutine) and read by
    // the publisher thread, so it is atomic.
    _Atomic int audio_system;        // AUDIO_SUBSYSTEM_* constant
    char selected_capture_dev[UI_DEV_ID_MAX];   // currently active capture (input) device
    char selected_playback_dev[UI_DEV_ID_MAX];  // currently active playback (output) device
    int rx_input_channel;            // LEFT=0, RIGHT=1, STEREO=2

    // Radio list is sent once at startup and again after a PTT config change
    volatile int radio_list_pending;      // 1 = need to (re-)send radio list to UI

    // Soundcard lists and input_channel are sent when a new UI client connects
    volatile int soundcard_list_pending;  // 1 = need to send capture/playback/input_channel to UI

    // Persistent configuration (written back to INI on UI changes)
    pthread_mutex_t cfg_mutex;
    mercury_config cfg;
    char cfg_path[1024];

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
// tls_enabled: false = plain WS (default), true = WSS (requires cert/key on disk)
// waterfall_enabled: 1 = start spectrum publisher thread (default), 0 = skip it
// audio_system: AUDIO_SUBSYSTEM_* constant for soundcard enumeration
// selected_capture: currently active capture device name (may be NULL)
// selected_playback: currently active playback device name (may be NULL)
int ui_comm_init(ui_ctx_t *ctx, uint16_t ws_port, bool tls_enabled,
                 int waterfall_enabled, int audio_system,
                 const char *selected_capture, const char *selected_playback,
                 int rx_input_channel,
                 const mercury_config *initial_cfg, const char *cfg_path);
void ui_comm_shutdown(ui_ctx_t *ctx);

/* ---- Local (in-process) UI access -----------------------------------------
 * The embedded Fyne UI drives the engine through these instead of opening a
 * websocket to itself.  Remote clients keep using the websocket; both end up
 * in the same gatherer and the same command handler. */

/* Handle one UI command.  Shared by the websocket callback and the CGo bridge
 * so local and remote behave identically. */
int  ui_comm_handle_command(ui_ctx_t *ctx, const ws_command_t *cmd);

/* Same, addressed to the running engine (no ctx to hand around from Go).
 * Returns -1 if no UI context is running. */
int  ui_comm_command(const char *command, const char *value,
                     const char *value2, const char *value3,
                     const char *value4, const char *value5,
                     const char *value6, const char *value7);

/* Enable / disable the waterfall/spectrum FFT pipeline at runtime.  Saves the
 * choice to mercury.ini so it survives restarts. */
void ui_comm_set_waterfall(bool enabled);

/* Read the TNC TCP ports the engine actually listens on (arq_tcp_base_port
 * and broadcast_tcp_port from the config). */
void ui_comm_get_tcp_ports(int *arq_base_port, int *broadcast_port);

/* Copy the most recent status snapshot.  False until the first publish. */
bool ui_comm_get_status(ui_status_t *out);

/* Enumerate audio devices of one kind.  Returns how many were written and, if
 * `selected` is non-NULL, the id currently in use. */
/* Append the device id to any display name shared by more than one device, so
 * a label-driven UI can tell two identical-looking sound cards apart.  Names
 * that do not collide are left alone. */
void ui_devices_disambiguate(ui_device_t *devs, int count);

int  ui_comm_get_audio_devices(ui_device_kind_t kind, ui_device_t *out, int max,
                               char *selected, size_t sel_len);

/* Enumerate Hamlib models ("None" first), plus the retained model selection,
 * PTT device, Hamlib serial speed and active PTT method. */
int  ui_comm_get_radio_list(ui_device_t *out, int max, char *selected, size_t sel_len,
                            char *device_path, size_t dev_len, int *serial_speed,
                            char *ptt_method, size_t method_len,
                            char *ptt_line, size_t line_len,
                            char *ptt_invert, size_t invert_len,
                            int *cm108_gpio);

/* Current RX input channel: 0 = left, 1 = right, 2 = stereo. */
int  ui_comm_get_input_channel(void);

/* Current audio subsystem name ("alsa", "pulse", ...).  Returns a static
 * string; never NULL.  Empty string when no UI context is running. */
const char *ui_comm_get_audio_system(void);

/* Pre-load the radio model list from the main thread.  The embedded UI calls
 * this during mercury_init() so hamlib backend loading happens on a regular
 * POSIX thread, not a CGo goroutine. */
void ui_comm_preload_radio_list(void);

#endif
