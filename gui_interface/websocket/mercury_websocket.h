/* Mercury backend websocket
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Pedro Messetti <pedromessetti.rhizomatica@gmail.com>
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
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

// Bidirectional WebSocket server for Mercury C backend <-> MercuryQT UI
// communication. Uses mongoose for the WebSocket transport layer, enabling WSS
// when a TLS backend is compiled in.
// Based on the sbitx_websocket implementation (unidirectional) but extended
// to support receiving commands from the UI (bidirectional).

#ifndef MERCURY_WEBSOCKET_H_
#define MERCURY_WEBSOCKET_H_

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

// ---- SSL certificate and key paths ----
#define CFG_SSL_CERT "/etc/ssl/certs/hermes.radio.crt"
#define CFG_SSL_KEY  "/etc/ssl/private/hermes.radio.key"

// ---- WebSocket server defaults ----
#define WS_MAX_MESSAGE_SIZE   8192
#define WS_POLL_INTERVAL_MS   40

// ---- Incoming command from UI (parsed from JSON) ----
typedef struct {
    char command[64];   // e.g. "set_capture_dev", "set_input_channel", "set_radio_config"
    char value[256];    // primary value (e.g. device id, channel name)
    char value2[256];   // optional second value (e.g. device path for radio config)
    char value3[256];   // optional third value (e.g. input channel for audio config)
} ws_command_t;

// Callback invoked on the websocket thread when the UI sends a command.
// The implementation should be thread-safe. Return 0 on success.
typedef int (*ws_command_callback_t)(const ws_command_t *cmd, void *user_data);

// Callback invoked on the websocket thread when a new client connects.
typedef void (*ws_connect_callback_t)(void *user_data);

// ---- WebSocket server context ----
typedef struct {
    pthread_t ws_tid;                  // websocket server thread
    volatile bool running;             // thread run flag (set false to stop)

    // Runtime TLS mode: false = plain WS (default), true = WSS (requires certs)
    bool tls_enabled;

    // Callback for incoming UI commands
    ws_command_callback_t cmd_callback;
    void *cmd_callback_data;           // opaque pointer passed to cmd_callback

    // Callback invoked when a new WebSocket client connects
    ws_connect_callback_t connect_callback;
    void *connect_callback_data;       // opaque pointer passed to connect_callback

    // Server listen URL (e.g. "wss://0.0.0.0:10000" or "ws://0.0.0.0:10000")
    char listen_url[128];

    // Web root for serving static files (e.g. test.html)
    char web_root[256];
} ws_ctx_t;

// ---- Public API ----

/**
 * Initialise and start the WebSocket server thread.
 *
 * @param ctx            WebSocket context (caller-allocated, zeroed before call).
 * @param port           WebSocket listen port (e.g. 10000). Listens on 0.0.0.0.
 * @param web_root       Path to directory with static files to serve (e.g. test.html).
 *                       Pass NULL to disable static file serving.
 * @param cmd_callback   Function called when a command is received from the UI.
 *                       May be NULL if no command handling is needed yet.
 * @param cb_data        Opaque pointer forwarded to cmd_callback.
 * @param tls_enabled    false = plain WS (default); true = WSS using mongoose
 *                       built-in TLS with certs at CFG_SSL_CERT / CFG_SSL_KEY.
 * @return 0 on success, -1 on error.
 */
int ws_init(ws_ctx_t *ctx,
            uint16_t port,
            const char *web_root,
            ws_command_callback_t cmd_callback,
            void *cb_data,
            bool tls_enabled);

/**
 * Send a JSON text message to all connected WebSocket clients.
 * Thread-safe - may be called from any thread.
 *
 * @param ctx   WebSocket context.
 * @param json  Null-terminated JSON string.
 * @return number of clients the message was sent to, or -1 on error.
 */
int ws_broadcast_json(ws_ctx_t *ctx, const char *json);

/**
 * Send raw binary data to all connected WebSocket clients.
 * Useful for spectrum / waterfall binary frames.
 *
 * @param ctx   WebSocket context.
 * @param data  Binary payload.
 * @param len   Payload length in bytes.
 * @return number of clients the message was sent to, or -1 on error.
 */
int ws_broadcast_binary(ws_ctx_t *ctx, const void *data, size_t len);

/**
 * Gracefully shut down the WebSocket server.
 * Joins the server thread and frees mongoose resources.
 *
 * @param ctx  WebSocket context previously initialised with ws_init().
 */
void ws_shutdown(ws_ctx_t *ctx);

#endif // MERCURY_WEBSOCKET_H_
