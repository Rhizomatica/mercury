/* Optional TLS transport for the Mercury WebSocket server
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A thin seam over OpenSSL so the server's I/O path does not care whether a
 * connection is plain or encrypted.  Built with WS_HAVE_OPENSSL=1 where
 * OpenSSL is present (Linux); everywhere else these calls compile to stubs
 * and ws_tls_available() returns false, so wss:// is refused at startup
 * instead of failing per connection.
 *
 * OpenSSL is Apache-2.0, which is compatible with Mercury's GPL-3.0-or-later.
 * That compatibility is the whole point of this file: the WebSocket server it
 * serves replaced a GPL-2.0-only library that was not.
 */

#ifndef WS_TLS_H_
#define WS_TLS_H_

#include <stdbool.h>
#include <stddef.h>

typedef struct ws_tls_ctx ws_tls_ctx_t;   /* server-wide: certificate + key */
typedef struct ws_tls ws_tls_t;           /* per connection */

/* Return values shared by the handshake and the I/O calls.  WANT_READ and
 * WANT_WRITE are the whole reason for this seam: OpenSSL may need to write
 * during a read (and vice versa) to service a renegotiation, so the caller
 * has to drive poll(2) from what the TLS layer asks for, not from what the
 * application wants to do next. */
#define WS_TLS_ERROR       (-1)
#define WS_TLS_WANT_READ   (-2)
#define WS_TLS_WANT_WRITE  (-3)

/* True when this build can actually serve wss://. */
bool ws_tls_available(void);

/* Load a PEM certificate and private key.  Returns NULL on failure. */
ws_tls_ctx_t *ws_tls_ctx_new(const char *cert_path, const char *key_path);
void ws_tls_ctx_free(ws_tls_ctx_t *ctx);

/* Wrap an accepted, non-blocking socket.  Does not start the handshake. */
ws_tls_t *ws_tls_new(ws_tls_ctx_t *ctx, int fd);
void ws_tls_free(ws_tls_t *t);

/* Drive the server-side handshake.  1 when complete, else a WS_TLS_ code. */
int ws_tls_handshake(ws_tls_t *t);

/* Byte count on success, 0 on clean shutdown, else a WS_TLS_ code. */
int ws_tls_read(ws_tls_t *t, void *buf, size_t len);
int ws_tls_write(ws_tls_t *t, const void *buf, size_t len);

/* Last error text for logging; never NULL. */
const char *ws_tls_last_error(void);

#endif /* WS_TLS_H_ */
