/* Mercury UI transport (gui_interface/ui_transport.h), daemon build
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The host program carries the UI traffic: Mercury's broadcasts go to the ops
 * it registers with mercury_host_set_ui(), and its clients' commands come in
 * through mercury_host_ui_command_json().  Nothing here listens on a port.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "hermes_log.h"
#include "mercury_host.h"
#include "ui_transport.h"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static mercury_host_ui_ops s_ops;
static ui_transport_command_fn s_on_command = NULL;
static ui_transport_connect_fn s_on_connect = NULL;
static void *s_user = NULL;

void mercury_host_set_ui(const mercury_host_ui_ops *ops)
{
    pthread_mutex_lock(&s_lock);
    if (ops)
        s_ops = *ops;
    else
        memset(&s_ops, 0, sizeof(s_ops));
    pthread_mutex_unlock(&s_lock);
}

int ui_transport_init(uint16_t port, bool tls_enabled,
                      const char *tls_cert_path, const char *tls_key_path,
                      ui_transport_command_fn on_command,
                      ui_transport_connect_fn on_connect, void *user)
{
    (void) port; (void) tls_enabled; (void) tls_cert_path; (void) tls_key_path;

    pthread_mutex_lock(&s_lock);
    s_on_command = on_command;
    s_on_connect = on_connect;
    s_user = user;
    pthread_mutex_unlock(&s_lock);
    HLOGI("ui-host", "UI traffic goes through the host program");
    return 0;
}

void ui_transport_shutdown(void)
{
    pthread_mutex_lock(&s_lock);
    s_on_command = NULL;
    s_on_connect = NULL;
    s_user = NULL;
    pthread_mutex_unlock(&s_lock);
}

int ui_transport_broadcast_json(const char *json)
{
    if (!json)
        return -1;

    /* Sanitised as the websocket server does: a host may put this straight
     * into a JSON text frame. */
    size_t len = strlen(json);
    char *buf = malloc(len + 1);
    if (!buf)
        return -1;
    memcpy(buf, json, len + 1);
    ws_json_sanitise_utf8(buf, len);

    pthread_mutex_lock(&s_lock);
    if (s_ops.broadcast_json)
        s_ops.broadcast_json(buf, s_ops.user);
    pthread_mutex_unlock(&s_lock);
    free(buf);
    return 0;
}

int ui_transport_broadcast_binary(const void *data, size_t len)
{
    if (!data)
        return -1;

    pthread_mutex_lock(&s_lock);
    if (s_ops.broadcast_binary)
        s_ops.broadcast_binary(data, len, s_ops.user);
    pthread_mutex_unlock(&s_lock);
    return 0;
}

int mercury_host_ui_command_json(const char *json, size_t len)
{
    ws_command_t cmd;

    if (!json || len == 0 || len > UI_MAX_MESSAGE_SIZE ||
        ws_json_parse_command(json, len, &cmd) != 0)
        return -1;

    /* Not under s_lock: the handler broadcasts, which takes it. */
    pthread_mutex_lock(&s_lock);
    ui_transport_command_fn fn = s_on_command;
    void *user = s_user;
    pthread_mutex_unlock(&s_lock);
    return fn ? fn(&cmd, user) : -1;
}

void mercury_host_ui_client_connected(void)
{
    pthread_mutex_lock(&s_lock);
    ui_transport_connect_fn fn = s_on_connect;
    void *user = s_user;
    pthread_mutex_unlock(&s_lock);
    if (fn)
        fn(user);
}
