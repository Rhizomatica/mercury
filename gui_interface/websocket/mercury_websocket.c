/* Mercury backend websocket
 *
 * Copyright (C) 2026 Rhizomatica
 * Authors: Pedro Messetti <pedromessetti.rhizomatica@gmail.com>
 *          Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The standalone build's UI transport (ui_transport.h): Mercury's own
 * websocket server, ws_server.c, which replaced Mongoose.  Mongoose is
 * GPL-2.0-only and cannot lawfully be combined with Mercury's
 * GPL-3.0-or-later, so the binaries were undistributable and Debian packaging
 * was blocked.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hermes_log.h"
#include "ui_transport.h"
#include "web_assets.h"
#include "ws_json.h"
#include "ws_server.h"

#define WS_LOG_TAG "websocket"

/* One server per process, as before: the UI port is a singleton. */
static ws_server_t *s_server = NULL;

/* Set before the server thread starts and cleared after it stops, so the
 * server thread never sees them change. */
static ui_transport_command_fn s_on_command = NULL;
static ui_transport_connect_fn s_on_connect = NULL;
static void *s_user = NULL;
static volatile bool s_running = false;

/* ---- server thread callbacks ---- */

static void on_ws_connect(ws_conn_t *c, void *user_data)
{
    (void) c;
    (void) user_data;

    if (s_on_connect != NULL)
        s_on_connect(s_user);
}

static void on_ws_message(ws_conn_t *c, const void *data, size_t len,
                          bool is_text, void *user_data)
{
    ws_command_t cmd;
    int rc;

    (void) user_data;

    if (!is_text)
        return;                         /* the UI never sends binary */

    if (s_on_command == NULL)
    {
        const char *err = "{\"error\":\"no handler\"}";

        (void) ws_conn_send(c, err, strlen(err), true);
        return;
    }

    if (len == 0 || len > UI_MAX_MESSAGE_SIZE)
    {
        const char *err = "{\"error\":\"invalid message size\"}";

        (void) ws_conn_send(c, err, strlen(err), true);
        return;
    }

    if (ws_json_parse_command((const char *) data, len, &cmd) != 0)
    {
        const char *err = "{\"error\":\"malformed command JSON\"}";

        (void) ws_conn_send(c, err, strlen(err), true);
        return;
    }

    HLOGI(WS_LOG_TAG,
          "RX command=\"%s\" value=\"%s\" value2=\"%s\" value3=\"%s\" "
          "value4=\"%s\" value5=\"%s\" value6=\"%s\" value7=\"%s\"",
          cmd.command, cmd.value, cmd.value2, cmd.value3, cmd.value4,
          cmd.value5, cmd.value6, cmd.value7);

    rc = s_on_command(&cmd, s_user);
    if (rc == 0)
    {
        const char *ack = "{\"status\":\"ok\"}";

        (void) ws_conn_send(c, ack, strlen(ack), true);
    }
    else
    {
        char err_buf[128];
        int elen = snprintf(err_buf, sizeof(err_buf),
                            "{\"status\":\"error\",\"code\":%d}", rc);

        if (elen > 0)
            (void) ws_conn_send(c, err_buf, (size_t) elen, true);
    }
}

/* ---- public API ---- */

int ui_transport_init(uint16_t port, bool tls_enabled,
                      const char *tls_cert_path, const char *tls_key_path,
                      ui_transport_command_fn on_command,
                      ui_transport_connect_fn on_connect, void *user)
{
    ws_server_opts_t opts;
    char listen_url[128];

    snprintf(listen_url, sizeof(listen_url),
             "%s://0.0.0.0:%u", tls_enabled ? "wss" : "ws", (unsigned) port);

    /* Registered before the thread starts so a client that connects
     * immediately cannot race ahead of the callback pointers. */
    s_on_command = on_command;
    s_on_connect = on_connect;
    s_user = user;

    memset(&opts, 0, sizeof(opts));
    opts.port = port;
    opts.bind_addr = NULL;                      /* 0.0.0.0, as before */
    opts.ws_path = "/websocket";
    opts.max_message_size = UI_MAX_MESSAGE_SIZE;
    opts.tls_enabled = tls_enabled;
    opts.tls_cert_path = tls_cert_path;
    opts.tls_key_path = tls_key_path;
    opts.asset_lookup = ws_asset_find;
    opts.on_connect = on_ws_connect;
    opts.on_message = on_ws_message;

    if (ws_server_start(&s_server, &opts) != 0)
    {
        s_on_command = NULL;
        s_on_connect = NULL;
        s_user = NULL;
        s_running = false;
        return -1;
    }

    s_running = true;
    HLOGI(WS_LOG_TAG, "Initialized (url=%s)", listen_url);
    return 0;
}

int ui_transport_broadcast_json(const char *json)
{
    size_t json_len;
    char *buf;
    int rc;

    if (!s_running || json == NULL || s_server == NULL)
        return -1;

    /* Sanitise a copy before queueing so the server thread only ever sends
     * well-formed UTF-8 that is also legal inside a JSON string. */
    json_len = strlen(json);
    buf = (char *) malloc(json_len + 1);
    if (buf == NULL)
        return -1;

    memcpy(buf, json, json_len + 1);
    ws_json_sanitise_utf8(buf, json_len);

    rc = ws_server_broadcast(s_server, buf, json_len, true);
    free(buf);

    return rc;
}

int ui_transport_broadcast_binary(const void *data, size_t len)
{
    if (!s_running || data == NULL || s_server == NULL)
        return -1;

    return ws_server_broadcast(s_server, data, len, false);
}

void ui_transport_shutdown(void)
{
    s_running = false;

    if (s_server != NULL)
    {
        ws_server_stop(s_server);
        s_server = NULL;
    }

    s_on_command = NULL;
    s_on_connect = NULL;
    s_user = NULL;

    HLOGI(WS_LOG_TAG, "Shut down");
}
