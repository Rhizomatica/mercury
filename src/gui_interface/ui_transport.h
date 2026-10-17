/* Mercury UI transport
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * How ui_communication.c reaches the remote UI clients: status and spectrum
 * out, commands in.  One interface, one implementation per build
 * (MERCURY_HOST in the Makefile):
 *
 *   standalone  websocket/mercury_websocket.c, Mercury's own websocket server;
 *   daemon      host/ui_host.c, handing both directions to the program that
 *               links Mercury in (hermes-radio-daemon).
 *
 * The embedded Fyne UI does not go through here: it calls ui_communication
 * directly, through the bridge.
 */

#ifndef UI_TRANSPORT_H_
#define UI_TRANSPORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "websocket/ws_json.h"     /* ws_command_t */

/* Largest command message a client may send. */
#define UI_MAX_MESSAGE_SIZE   8192

/* Called when a client sends a command; may be called from the transport's
 * own thread, so it must be thread-safe.  Returns 0 on success. */
typedef int (*ui_transport_command_fn)(const ws_command_t *cmd, void *user);

/* Called when a new client connects, so it can be sent the state a client
 * gets once (device lists, radio list, history). */
typedef void (*ui_transport_connect_fn)(void *user);

/* Start the transport.  port and the TLS settings are the standalone
 * server's; a transport that does not listen ignores them.  Both callbacks
 * are registered before any client can be seen.  Returns 0 on success. */
int ui_transport_init(uint16_t port, bool tls_enabled,
                      const char *tls_cert_path, const char *tls_key_path,
                      ui_transport_command_fn on_command,
                      ui_transport_connect_fn on_connect, void *user);

/* Send to every connected client.  Thread-safe.  Return 0 when queued. */
int ui_transport_broadcast_json(const char *json);
int ui_transport_broadcast_binary(const void *data, size_t len);

/* Stop the transport; no callback runs after it returns. */
void ui_transport_shutdown(void);

#endif /* UI_TRANSPORT_H_ */
