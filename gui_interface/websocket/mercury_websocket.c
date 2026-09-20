/* Mercury backend websocket
 *
 * Copyright (C) 2026 Rhizomatica
 * Authors: Pedro Messetti <pedromessetti.rhizomatica@gmail.com>
 *          Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adapter between Mercury's UI layer and ws_server.c.  It keeps the four-call
 * API that ui_communication.c has always used; what changed underneath is the
 * transport, which used to be Mongoose.  Mongoose is GPL-2.0-only and cannot
 * lawfully be combined with Mercury's GPL-3.0-or-later, so the binaries were
 * undistributable and Debian packaging was blocked.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hermes_log.h"
#include "mercury_websocket.h"
#include "web_assets.h"
#include "ws_json.h"
#include "ws_server.h"

#define WS_LOG_TAG "websocket"

/* One server per process, as before: the UI port is a singleton. */
static ws_server_t *s_server = NULL;
static ws_ctx_t    *s_ws_ctx = NULL;

/* ---- server thread callbacks ---- */

static void on_ws_connect(ws_conn_t *c, void *user_data)
{
    (void) c;
    (void) user_data;

    if (s_ws_ctx != NULL && s_ws_ctx->connect_callback != NULL)
        s_ws_ctx->connect_callback(s_ws_ctx->connect_callback_data);
}

static void on_ws_message(ws_conn_t *c, const void *data, size_t len,
                          bool is_text, void *user_data)
{
    ws_command_t cmd;
    int rc;

    (void) user_data;

    if (!is_text)
        return;                         /* the UI never sends binary */

    if (s_ws_ctx == NULL || s_ws_ctx->cmd_callback == NULL)
    {
        const char *err = "{\"error\":\"no handler\"}";

        (void) ws_conn_send(c, err, strlen(err), true);
        return;
    }

    if (len == 0 || len > WS_MAX_MESSAGE_SIZE)
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

    rc = s_ws_ctx->cmd_callback(&cmd, s_ws_ctx->cmd_callback_data);
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

int ws_init(ws_ctx_t *ctx,
            uint16_t port,
            ws_command_callback_t cmd_callback,
            void *cb_data,
            ws_connect_callback_t connect_callback,
            void *connect_cb_data,
            bool tls_enabled,
            const char *tls_cert_path,
            const char *tls_key_path)
{
    ws_server_opts_t opts;

    if (ctx == NULL)
        return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->tls_enabled = tls_enabled;
    ctx->cmd_callback = cmd_callback;
    ctx->cmd_callback_data = cb_data;
    ctx->connect_callback = connect_callback;
    ctx->connect_callback_data = connect_cb_data;

    snprintf(ctx->listen_url, sizeof(ctx->listen_url),
             "%s://0.0.0.0:%u", tls_enabled ? "wss" : "ws", (unsigned) port);

    /* Registered before the thread starts so a client that connects
     * immediately cannot race ahead of the callback pointers. */
    s_ws_ctx = ctx;

    memset(&opts, 0, sizeof(opts));
    opts.port = port;
    opts.bind_addr = NULL;                      /* 0.0.0.0, as before */
    opts.ws_path = "/websocket";
    opts.max_message_size = WS_MAX_MESSAGE_SIZE;
    opts.tls_enabled = tls_enabled;
    opts.tls_cert_path = tls_cert_path;
    opts.tls_key_path = tls_key_path;
    opts.asset_lookup = ws_asset_find;
    opts.on_connect = on_ws_connect;
    opts.on_message = on_ws_message;

    if (ws_server_start(&s_server, &opts) != 0)
    {
        s_ws_ctx = NULL;
        ctx->running = false;
        return -1;
    }

    ctx->running = true;
    HLOGI(WS_LOG_TAG, "Initialized (url=%s)", ctx->listen_url);
    return 0;
}

int ws_broadcast_json(ws_ctx_t *ctx, const char *json)
{
    size_t json_len;
    char *buf;
    int rc;

    if (ctx == NULL || !ctx->running || json == NULL || s_server == NULL)
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

int ws_broadcast_binary(ws_ctx_t *ctx, const void *data, size_t len)
{
    if (ctx == NULL || !ctx->running || data == NULL || s_server == NULL)
        return -1;

    return ws_server_broadcast(s_server, data, len, false);
}

void ws_shutdown(ws_ctx_t *ctx)
{
    if (ctx == NULL)
        return;

    ctx->running = false;

    if (s_server != NULL)
    {
        ws_server_stop(s_server);
        s_server = NULL;
    }

    s_ws_ctx = NULL;

    HLOGI(WS_LOG_TAG, "Shut down");
}
