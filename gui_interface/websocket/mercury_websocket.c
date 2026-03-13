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

#include "mongoose.h"
#include "mercury_websocket.h"

// ---- Internal state shared with the server thread ----
static struct mg_mgr s_mgr;            // Mongoose event manager
static ws_ctx_t     *s_ws_ctx = NULL;  // Back-pointer to the caller context

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
        const char *found = memmem(p, end - p, needle, nlen);
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
// Expected format: {"command":"<cmd>","value":"<val>","value2":"<val2>"}
// "command" is mandatory, "value" and "value2" are optional
static int parse_ws_command(const char *json, size_t len, ws_command_t *cmd)
{
    memset(cmd, 0, sizeof(*cmd));

    if (!json_find_key(json, len, "command", cmd->command, sizeof(cmd->command)))
        return -1;  // mandatory field missing

    // Optional fields
    json_find_key(json, len, "value",  cmd->value,  sizeof(cmd->value));
    json_find_key(json, len, "value2", cmd->value2, sizeof(cmd->value2));

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
    if (parse_ws_command(wm->data.ptr, wm->data.len, &cmd) != 0)
    {
        const char *err = "{\"error\":\"malformed command JSON\"}";
        mg_ws_send(c, err, strlen(err), WEBSOCKET_OP_TEXT);
        return;
    }

    printf("WS RX command=\"%s\" value=\"%s\" value2=\"%s\"\n",
           cmd.command, cmd.value, cmd.value2);

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
static void ws_event_handler(struct mg_connection *c, int ev, void *ev_data, void *fn_data)
{
    (void)fn_data;

    if (ev == MG_EV_ACCEPT)
    {
        // TLS handshake for WSS connections
        struct mg_tls_opts opts = {
            .cert    = CFG_SSL_CERT,
            .certkey = CFG_SSL_KEY,
        };
        mg_tls_init(c, &opts);
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;

        if (mg_http_match_uri(hm, "/websocket"))
        {
            // Upgrade HTTP to WebSocket
            mg_ws_upgrade(c, hm, NULL);
            printf("Mercury WS: client connected (conn %p)\n", (void *)c);
        }
        else if (s_ws_ctx && s_ws_ctx->web_root[0])
        {
            // Serve static files from web_root (e.g. test.html)
            struct mg_http_serve_opts opts = { .root_dir = s_ws_ctx->web_root };
            mg_http_serve_dir(c, ev_data, &opts);
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
        printf("Mercury WS: connection error (conn %p): %s\n",
               (void *)c, (char *)ev_data);
    }
    else if (ev == MG_EV_CLOSE)
    {
        if (c->is_websocket)
            printf("Mercury WS: client disconnected (conn %p)\n", (void *)c);
    }
}

// ---- Server thread (runs mongoose event loop) ----
static void *ws_server_thread(void *arg)
{
    ws_ctx_t *ctx = (ws_ctx_t *)arg;

    mg_mgr_init(&s_mgr);
    mg_http_listen(&s_mgr, ctx->listen_url, ws_event_handler, NULL);

    printf("Mercury WS: server listening on %s\n", ctx->listen_url);

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

    printf("Mercury WS: server thread stopped\n");
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

    snprintf(ctx->listen_url, sizeof(ctx->listen_url), "wss://0.0.0.0:%u", port);

    if (web_root)
        strncpy(ctx->web_root, web_root, sizeof(ctx->web_root) - 1);
    else
        ctx->web_root[0] = '\0';

    s_ws_ctx = ctx;

    if (pthread_create(&ctx->ws_tid, NULL, ws_server_thread, ctx) != 0)
    {
        perror("Mercury WS: pthread_create failed");
        ctx->running = false;
        return -1;
    }

    printf("Mercury WS: initialized (url=%s)\n", ctx->listen_url);
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

    printf("Mercury WS: shut down\n");
}
