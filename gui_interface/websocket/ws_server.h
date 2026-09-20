/* Minimal HTTP + WebSocket server for the Mercury UI
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Serves the handful of UI pages compiled into the binary and speaks RFC 6455
 * to the browser and to the Fyne client.  It exists because the library that
 * used to do this, Mongoose, is GPL-2.0-only, which cannot be combined with
 * Mercury's GPL-3.0-or-later -- so the binaries were undistributable and
 * Debian packaging was blocked.
 *
 * The scope is deliberately the traffic Mercury actually generates: one
 * WebSocket endpoint, four static pages, a handful of concurrent clients on a
 * LAN.  It is not a general-purpose web server, and it is not hardened for
 * the open internet -- see ws_server_opts_t.bind_addr.
 *
 * All sockets live on one thread, created by ws_server_start().  Every
 * callback below runs on that thread.  ws_server_broadcast() is the only
 * entry point other threads may call.
 */

#ifndef WS_SERVER_H_
#define WS_SERVER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ws_server ws_server_t;
typedef struct ws_conn ws_conn_t;

/* Look up a static asset by request path ("/index.html").  Return the bytes
 * and set *size, or NULL when there is no such asset. */
typedef const void *(*ws_asset_lookup_fn)(const char *path, size_t *size);

typedef struct {
    uint16_t port;
    const char *bind_addr;        /* NULL or "" means 0.0.0.0 */
    const char *ws_path;          /* endpoint that upgrades, e.g. "/websocket" */
    size_t max_message_size;      /* inbound cap; 0 means the built-in default */

    bool tls_enabled;
    const char *tls_cert_path;
    const char *tls_key_path;

    ws_asset_lookup_fn asset_lookup;   /* NULL disables static serving */

    /* Callbacks, all on the server thread.  Any may be NULL. */
    void (*on_connect)(ws_conn_t *c, void *user_data);
    void (*on_message)(ws_conn_t *c, const void *data, size_t len,
                       bool is_text, void *user_data);
    void (*on_close)(ws_conn_t *c, void *user_data);
    void *user_data;
} ws_server_opts_t;

/* Start the listener and its thread.  Returns 0 on success.  On failure
 * nothing is left running and *out is untouched. */
int ws_server_start(ws_server_t **out, const ws_server_opts_t *opts);

/* Stop the thread, drop every connection and free the server. */
void ws_server_stop(ws_server_t *s);

/* Queue a message for every connected WebSocket client.  Thread-safe; this
 * is the only call other threads may make.  Copies `data`.  Returns 0 when
 * queued, -1 on error. */
int ws_server_broadcast(ws_server_t *s, const void *data, size_t len,
                        bool is_text);

/* Send on one connection.  Server thread only -- use it from a callback. */
int ws_conn_send(ws_conn_t *c, const void *data, size_t len, bool is_text);

/* Number of connections that have completed the WebSocket handshake. */
size_t ws_server_client_count(ws_server_t *s);

#endif /* WS_SERVER_H_ */
