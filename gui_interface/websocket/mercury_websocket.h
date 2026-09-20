/* Mercury backend websocket
 *
 * Copyright (C) 2026 Rhizomatica
 * Authors: Pedro Messetti <pedromessetti.rhizomatica@gmail.com>
 *          Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bidirectional WebSocket server for the Mercury C backend <-> UI link,
 * used by both the browser pages and the Fyne client.  The transport lives
 * in ws_server.c; this is the API the rest of Mercury sees.
 */

#ifndef MERCURY_WEBSOCKET_H_
#define MERCURY_WEBSOCKET_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ws_json.h"     /* ws_command_t */

/* ---- SSL certificate and key paths (wss:// only) ---- */
#define CFG_SSL_CERT "/etc/ssl/certs/hermes.radio.crt"
#define CFG_SSL_KEY  "/etc/ssl/private/hermes.radio.key"

/* ---- WebSocket server defaults ---- */
#define WS_MAX_MESSAGE_SIZE   8192

/* Callback invoked on the websocket thread when the UI sends a command.
 * The implementation should be thread-safe.  Return 0 on success. */
typedef int (*ws_command_callback_t)(const ws_command_t *cmd, void *user_data);

/* Callback invoked on the websocket thread when a new client connects. */
typedef void (*ws_connect_callback_t)(void *user_data);

/* ---- WebSocket server context ---- */
typedef struct {
    volatile bool running;             /* true between ws_init and ws_shutdown */

    /* Runtime TLS mode: false = plain WS (default), true = WSS.
     * WSS needs a build with OpenSSL; without one ws_init() fails and says so
     * rather than silently falling back to plaintext. */
    bool tls_enabled;

    /* Callback for incoming UI commands */
    ws_command_callback_t cmd_callback;
    void *cmd_callback_data;           /* opaque pointer passed to cmd_callback */

    /* Callback invoked when a new WebSocket client connects */
    ws_connect_callback_t connect_callback;
    void *connect_callback_data;       /* opaque pointer passed to the callback */

    /* Server listen URL, for logging (e.g. "ws://0.0.0.0:10000") */
    char listen_url[128];
} ws_ctx_t;

/* ---- Public API ---- */

/**
 * Initialise and start the WebSocket server thread.
 *
 * @param ctx            WebSocket context (caller-allocated).
 * @param port           WebSocket listen port (e.g. 10000). Listens on 0.0.0.0.
 * @param cmd_callback   Called when a command is received from the UI.
 *                       May be NULL if no command handling is needed yet.
 * @param cb_data        Opaque pointer forwarded to cmd_callback.
 * @param tls_enabled    false = plain WS (default); true = WSS using the certs
 *                       at CFG_SSL_CERT / CFG_SSL_KEY.
 * @return 0 on success, -1 on error.
 */
int ws_init(ws_ctx_t *ctx,
            uint16_t port,
            ws_command_callback_t cmd_callback,
            void *cb_data,
            ws_connect_callback_t connect_callback,
            void *connect_cb_data,
            bool tls_enabled);

/**
 * Queue a JSON text message for all connected WebSocket clients.
 * Thread-safe - may be called from any thread.
 *
 * @return 0 when queued, -1 on error.
 */
int ws_broadcast_json(ws_ctx_t *ctx, const char *json);

/**
 * Queue raw binary data for all connected WebSocket clients.
 * Used for the spectrum / waterfall frames.  Thread-safe.
 *
 * @return 0 when queued, -1 on error.
 */
int ws_broadcast_binary(ws_ctx_t *ctx, const void *data, size_t len);

/**
 * Gracefully shut down the WebSocket server, joining its thread.
 */
void ws_shutdown(ws_ctx_t *ctx);

#endif /* MERCURY_WEBSOCKET_H_ */
