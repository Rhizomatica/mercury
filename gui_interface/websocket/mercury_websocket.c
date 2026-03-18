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

// Bidirectional WebSocket server for Mercury C backend <-> MercuryQT UI.
// Based on https://mongoose.ws/tutorials/websocket-server/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include <errno.h>

#include "mongoose.h"
#include "mercury_websocket.h"
#include "../../common/hermes_log.h"

extern const void *portable_memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen);

#if MG_TLS != MG_TLS_NONE
#define MERCURY_WS_USE_TLS 1
#define MERCURY_WS_SCHEME "wss"
#else
#define MERCURY_WS_USE_TLS 0
#define MERCURY_WS_SCHEME "ws"
#endif

#define WS_LOG_TAG "websocket"

// ---- Internal state shared with the server thread ----
static struct mg_mgr s_mgr;            // Mongoose event manager
static ws_ctx_t     *s_ws_ctx = NULL;  // Back-pointer to the caller context
#if MERCURY_WS_USE_TLS
static struct mg_str s_tls_cert = {0};
static struct mg_str s_tls_key = {0};

static void ws_free_tls_material(void)
{
    if (s_tls_cert.buf) {
        mg_free((void *)s_tls_cert.buf);
        s_tls_cert.buf = NULL;
        s_tls_cert.len = 0;
    }

    if (s_tls_key.buf) {
        mg_free((void *)s_tls_key.buf);
        s_tls_key.buf = NULL;
        s_tls_key.len = 0;
    }
}

static int ws_load_tls_material(void)
{
    s_tls_cert = mg_file_read(&mg_fs_posix, CFG_SSL_CERT);
    if (s_tls_cert.buf == NULL) {
        HLOGE(WS_LOG_TAG, "Failed to read TLS certificate: %s", CFG_SSL_CERT);
        return -1;
    }

    s_tls_key = mg_file_read(&mg_fs_posix, CFG_SSL_KEY);
    if (s_tls_key.buf == NULL) {
        HLOGE(WS_LOG_TAG, "Failed to read TLS private key: %s", CFG_SSL_KEY);
        ws_free_tls_material();
        return -1;
    }

    return 0;
}
#endif

// ---- Minimal JSON helpers ----

// Find the value string for a given key inside a flat JSON object.
// Writes the value (unquoted for strings, raw for numbers/bools) into
// `out` (max `out_sz` bytes). Returns 1 if found, 0 otherwise.
static int json_find_key(const char *json, size_t json_len,
                         const char *key, char *out, size_t out_sz)
{
    if (!json || !key || !out || out_sz == 0)
        return 0;

    // Build the search needle: "key":
    char needle[128];
    int nlen = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nlen <= 0 || (size_t)nlen >= sizeof(needle))
        return 0;

    const char *p = json;
    const char *end = json + json_len;
    while (p < end)
    {
        const char *found = (const char *) portable_memmem(p, (size_t) (end - p), needle, (size_t) nlen);
        if (!found)
            return 0;

        // Skip past the key and optional whitespace + colon
        const char *vp = found + nlen;
        while (vp < end && (*vp == ' ' || *vp == '\t' || *vp == ':'))
            vp++;

        if (vp >= end)
            return 0;

        if (*vp == '"')
        {
            // String value
            vp++;
            const char *ve = vp;
            while (ve < end && *ve != '"')
                ve++;
            size_t vlen = ve - vp;
            if (vlen >= out_sz) vlen = out_sz - 1;
            memcpy(out, vp, vlen);
            out[vlen] = '\0';
            return 1;
        }
        else
        {
            // Bare value (number, true, false, null)
            const char *ve = vp;
            while (ve < end && *ve != ',' && *ve != '}' && *ve != ' ' && *ve != '\n')
                ve++;
            size_t vlen = ve - vp;
            if (vlen >= out_sz) vlen = out_sz - 1;
            memcpy(out, vp, vlen);
            out[vlen] = '\0';
            return 1;
        }
    }
    return 0;
}

// ---- Parse incoming JSON command from UI ----
// Expected format: {"command":"<cmd>","value":"<val>","value2":"<val2>","value3":"<val3>"}
// "command" is mandatory, "value", "value2" and "value3" are optional
static int parse_ws_command(const char *json, size_t len, ws_command_t *cmd)
{
    memset(cmd, 0, sizeof(*cmd));

    if (!json_find_key(json, len, "command", cmd->command, sizeof(cmd->command)))
        return -1;  // mandatory field missing

    // Optional fields
    json_find_key(json, len, "value",  cmd->value,  sizeof(cmd->value));
    json_find_key(json, len, "value2", cmd->value2, sizeof(cmd->value2));
    json_find_key(json, len, "value3", cmd->value3, sizeof(cmd->value3));

    return 0;
}

// ---- Handle incoming WebSocket message from UI ----
static void ws_handle_message(struct mg_connection *c, struct mg_ws_message *wm)
{
    if (!s_ws_ctx || !s_ws_ctx->cmd_callback)
    {
        // No callback registered - acknowledge but ignore
        mg_ws_send(c, "{\"error\":\"no handler\"}", 21, WEBSOCKET_OP_TEXT);
        return;
    }

    if (wm->data.len == 0 || wm->data.len > WS_MAX_MESSAGE_SIZE)
    {
        const char *err = "{\"error\":\"invalid message size\"}";
        mg_ws_send(c, err, strlen(err), WEBSOCKET_OP_TEXT);
        return;
    }

    ws_command_t cmd;
    if (parse_ws_command(wm->data.buf, wm->data.len, &cmd) != 0)
    {
        const char *err = "{\"error\":\"malformed command JSON\"}";
        mg_ws_send(c, err, strlen(err), WEBSOCKET_OP_TEXT);
        return;
    }

    HLOGI(WS_LOG_TAG, "RX command=\"%s\" value=\"%s\" value2=\"%s\" value3=\"%s\"",
           cmd.command, cmd.value, cmd.value2, cmd.value3);

    int rc = s_ws_ctx->cmd_callback(&cmd, s_ws_ctx->cmd_callback_data);
    if (rc == 0)
    {
        const char *ack = "{\"status\":\"ok\"}";
        mg_ws_send(c, ack, strlen(ack), WEBSOCKET_OP_TEXT);
    }
    else
    {
        char err_buf[128];
        int elen = snprintf(err_buf, sizeof(err_buf),
                            "{\"status\":\"error\",\"code\":%d}", rc);
        mg_ws_send(c, err_buf, elen, WEBSOCKET_OP_TEXT);
    }
}

// ---- Mongoose event handler ----
static void ws_event_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_ACCEPT)
    {
#if MERCURY_WS_USE_TLS
        // TLS handshake for WSS connections
        struct mg_tls_opts opts = {
            .cert = s_tls_cert,
            .key = s_tls_key,
        };
        mg_tls_init(c, &opts);
#endif
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;

        if (mg_match(hm->uri, mg_str("/websocket"), NULL))
        {
            // Upgrade HTTP to WebSocket
            mg_ws_upgrade(c, hm, NULL);
            HLOGI(WS_LOG_TAG, "Client connected (conn %p)", (void *)c);
            if (s_ws_ctx && s_ws_ctx->connect_callback)
                s_ws_ctx->connect_callback(s_ws_ctx->connect_callback_data);
        }
        else if (s_ws_ctx && s_ws_ctx->web_root[0])
        {
            // Serve static files from web_root (e.g. test.html)
            struct mg_http_serve_opts opts = { .root_dir = s_ws_ctx->web_root };
            mg_http_serve_dir(c, hm, &opts);
        }
        else
        {
            mg_http_reply(c, 404, "", "Not Found\n");
        }
    }
    else if (ev == MG_EV_WS_MSG)
    {
        // Incoming WebSocket message from UI - bidirectional RX path
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
        ws_handle_message(c, wm);
    }
    else if (ev == MG_EV_ERROR)
    {
        HLOGE(WS_LOG_TAG, "Connection error (conn %p): %s",
               (void *)c, (char *)ev_data);
    }
    else if (ev == MG_EV_CLOSE)
    {
        if (c->is_websocket)
            HLOGI(WS_LOG_TAG, "Client disconnected (conn %p)", (void *)c);
    }
}

// ---- Server thread (runs mongoose event loop) ----
static void *ws_server_thread(void *arg)
{
    ws_ctx_t *ctx = (ws_ctx_t *)arg;

    mg_mgr_init(&s_mgr);
    mg_log_set(MG_LL_NONE);  /* suppress mongoose internal debug output */
    if (mg_http_listen(&s_mgr, ctx->listen_url, ws_event_handler, NULL) == NULL)
    {
        HLOGE(WS_LOG_TAG, "Failed to listen on %s", ctx->listen_url);
        ctx->running = false;
        mg_mgr_free(&s_mgr);
        return NULL;
    }

    HLOGI(WS_LOG_TAG, "Server listening on %s", ctx->listen_url);

    while (ctx->running)
    {
        mg_mgr_poll(&s_mgr, WS_POLL_INTERVAL_MS);
    }

    // Drain remaining connections
    for (struct mg_connection *c = s_mgr.conns; c != NULL; c = c->next)
    {
        c->is_closing = 1;
    }
    mg_mgr_poll(&s_mgr, WS_POLL_INTERVAL_MS);
    mg_mgr_free(&s_mgr);

    HLOGI(WS_LOG_TAG, "Server thread stopped");
    return NULL;
}

// ---- Public API ----

int ws_init(ws_ctx_t *ctx,
            uint16_t port,
            const char *web_root,
            ws_command_callback_t cmd_callback,
            void *cb_data)
{
    if (!ctx)
        return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->running = true;
    ctx->cmd_callback = cmd_callback;
    ctx->cmd_callback_data = cb_data;

    snprintf(ctx->listen_url, sizeof(ctx->listen_url), MERCURY_WS_SCHEME "://0.0.0.0:%u", port);

    if (web_root)
        strncpy(ctx->web_root, web_root, sizeof(ctx->web_root) - 1);
    else
        ctx->web_root[0] = '\0';

#if MERCURY_WS_USE_TLS
    if (ws_load_tls_material() != 0) {
        ctx->running = false;
        return -1;
    }
#endif

    s_ws_ctx = ctx;

    if (pthread_create(&ctx->ws_tid, NULL, ws_server_thread, ctx) != 0)
    {
        HLOGE(WS_LOG_TAG, "pthread_create failed: %s", strerror(errno));
        ctx->running = false;
#if MERCURY_WS_USE_TLS
        ws_free_tls_material();
#endif
        return -1;
    }

    HLOGI(WS_LOG_TAG, "Initialized (url=%s)", ctx->listen_url);
    return 0;
}

int ws_broadcast_json(ws_ctx_t *ctx, const char *json)
{
    if (!ctx || !ctx->running || !json)
        return -1;

    int count = 0;
    for (struct mg_connection *c = s_mgr.conns; c != NULL; c = c->next)
    {
        if (c->is_accepted && c->is_websocket && !c->is_draining)
        {
            mg_ws_send(c, json, strlen(json), WEBSOCKET_OP_TEXT);
            count++;
        }
    }
    return count;
}

int ws_broadcast_binary(ws_ctx_t *ctx, const void *data, size_t len)
{
    if (!ctx || !ctx->running || !data)
        return -1;

    int count = 0;
    for (struct mg_connection *c = s_mgr.conns; c != NULL; c = c->next)
    {
        if (c->is_accepted && c->is_websocket && !c->is_draining)
        {
            mg_ws_send(c, data, len, WEBSOCKET_OP_BINARY);
            count++;
        }
    }
    return count;
}

void ws_shutdown(ws_ctx_t *ctx)
{
    if (!ctx)
        return;

    ctx->running = false;
    pthread_join(ctx->ws_tid, NULL);
    s_ws_ctx = NULL;
#if MERCURY_WS_USE_TLS
    ws_free_tls_material();
#endif

    HLOGI(WS_LOG_TAG, "Shut down");
}
