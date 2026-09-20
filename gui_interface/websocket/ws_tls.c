/* Optional TLS transport for the Mercury WebSocket server
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ws_tls.h"

static char s_last_error[256] = "";

static void set_error(const char *msg)
{
    snprintf(s_last_error, sizeof(s_last_error), "%s", msg ? msg : "unknown");
}

const char *ws_tls_last_error(void)
{
    return s_last_error[0] ? s_last_error : "none";
}

#if defined(WS_HAVE_OPENSSL)

#include <openssl/ssl.h>
#include <openssl/err.h>

struct ws_tls_ctx { SSL_CTX *ctx; };
struct ws_tls     { SSL *ssl; };

bool ws_tls_available(void) { return true; }

static void set_ssl_error(const char *what)
{
    unsigned long e = ERR_get_error();
    char buf[160];

    if (e == 0)
        snprintf(s_last_error, sizeof(s_last_error), "%s", what);
    else
    {
        ERR_error_string_n(e, buf, sizeof(buf));
        snprintf(s_last_error, sizeof(s_last_error), "%s: %s", what, buf);
    }
}

ws_tls_ctx_t *ws_tls_ctx_new(const char *cert_path, const char *key_path)
{
    ws_tls_ctx_t *c;

    if (cert_path == NULL || key_path == NULL)
    {
        set_error("no certificate or key path");
        return NULL;
    }

    c = (ws_tls_ctx_t *) calloc(1, sizeof(*c));
    if (c == NULL)
    {
        set_error("out of memory");
        return NULL;
    }

    c->ctx = SSL_CTX_new(TLS_server_method());
    if (c->ctx == NULL)
    {
        set_ssl_error("SSL_CTX_new");
        free(c);
        return NULL;
    }

    /* TLS 1.2 is the floor: everything below it is broken, and every browser
     * and Go client that talks to this server does 1.2 or 1.3. */
    SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);

    /* Retrying a write with the same logical data but a moved buffer is
     * exactly what our per-connection write buffer does when it compacts, so
     * say so; without this OpenSSL treats a moved buffer as a fatal misuse. */
    SSL_CTX_set_mode(c->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                             SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if (SSL_CTX_use_certificate_chain_file(c->ctx, cert_path) != 1)
    {
        set_ssl_error("cannot read certificate");
        ws_tls_ctx_free(c);
        return NULL;
    }

    if (SSL_CTX_use_PrivateKey_file(c->ctx, key_path, SSL_FILETYPE_PEM) != 1)
    {
        set_ssl_error("cannot read private key");
        ws_tls_ctx_free(c);
        return NULL;
    }

    if (SSL_CTX_check_private_key(c->ctx) != 1)
    {
        set_ssl_error("private key does not match certificate");
        ws_tls_ctx_free(c);
        return NULL;
    }

    return c;
}

void ws_tls_ctx_free(ws_tls_ctx_t *ctx)
{
    if (ctx == NULL)
        return;
    if (ctx->ctx != NULL)
        SSL_CTX_free(ctx->ctx);
    free(ctx);
}

ws_tls_t *ws_tls_new(ws_tls_ctx_t *ctx, int fd)
{
    ws_tls_t *t;

    if (ctx == NULL)
        return NULL;

    t = (ws_tls_t *) calloc(1, sizeof(*t));
    if (t == NULL)
        return NULL;

    t->ssl = SSL_new(ctx->ctx);
    if (t->ssl == NULL)
    {
        set_ssl_error("SSL_new");
        free(t);
        return NULL;
    }

    if (SSL_set_fd(t->ssl, fd) != 1)
    {
        set_ssl_error("SSL_set_fd");
        SSL_free(t->ssl);
        free(t);
        return NULL;
    }

    SSL_set_accept_state(t->ssl);
    return t;
}

void ws_tls_free(ws_tls_t *t)
{
    if (t == NULL)
        return;
    if (t->ssl != NULL)
    {
        /* One non-blocking attempt at a close_notify, then go.  Waiting for
         * the peer's reply would stall shutdown, which is what the old
         * mongoose drain did before it tripped main()'s alarm(10). */
        SSL_shutdown(t->ssl);
        SSL_free(t->ssl);
    }
    free(t);
}

/* Map an OpenSSL return into our shared codes. */
static int classify(ws_tls_t *t, int rc, const char *what)
{
    int err = SSL_get_error(t->ssl, rc);

    switch (err)
    {
    case SSL_ERROR_WANT_READ:
        return WS_TLS_WANT_READ;
    case SSL_ERROR_WANT_WRITE:
        return WS_TLS_WANT_WRITE;
    case SSL_ERROR_ZERO_RETURN:
        return 0;                       /* clean TLS shutdown */
    default:
        set_ssl_error(what);
        return WS_TLS_ERROR;
    }
}

int ws_tls_handshake(ws_tls_t *t)
{
    int rc;

    if (t == NULL || t->ssl == NULL)
        return WS_TLS_ERROR;

    ERR_clear_error();
    rc = SSL_accept(t->ssl);
    if (rc == 1)
        return 1;

    rc = classify(t, rc, "TLS handshake");
    /* A peer that hangs up mid-handshake reads as a clean zero here; for the
     * handshake that is still a failure, not a completed connection. */
    return (rc == 0) ? WS_TLS_ERROR : rc;
}

int ws_tls_read(ws_tls_t *t, void *buf, size_t len)
{
    int rc;

    if (t == NULL || t->ssl == NULL || len == 0)
        return WS_TLS_ERROR;

    ERR_clear_error();
    rc = SSL_read(t->ssl, buf, (int) (len > INT_MAX ? INT_MAX : len));
    if (rc > 0)
        return rc;

    return classify(t, rc, "TLS read");
}

int ws_tls_write(ws_tls_t *t, const void *buf, size_t len)
{
    int rc;

    if (t == NULL || t->ssl == NULL || len == 0)
        return WS_TLS_ERROR;

    ERR_clear_error();
    rc = SSL_write(t->ssl, buf, (int) (len > INT_MAX ? INT_MAX : len));
    if (rc > 0)
        return rc;

    return classify(t, rc, "TLS write");
}

#else /* !WS_HAVE_OPENSSL */

bool ws_tls_available(void) { return false; }

ws_tls_ctx_t *ws_tls_ctx_new(const char *cert_path, const char *key_path)
{
    (void) cert_path;
    (void) key_path;
    set_error("this build has no TLS support");
    return NULL;
}

void ws_tls_ctx_free(ws_tls_ctx_t *ctx) { (void) ctx; }

ws_tls_t *ws_tls_new(ws_tls_ctx_t *ctx, int fd)
{
    (void) ctx;
    (void) fd;
    return NULL;
}

void ws_tls_free(ws_tls_t *t) { (void) t; }
int ws_tls_handshake(ws_tls_t *t) { (void) t; return WS_TLS_ERROR; }

int ws_tls_read(ws_tls_t *t, void *buf, size_t len)
{
    (void) t; (void) buf; (void) len;
    return WS_TLS_ERROR;
}

int ws_tls_write(ws_tls_t *t, const void *buf, size_t len)
{
    (void) t; (void) buf; (void) len;
    return WS_TLS_ERROR;
}

#endif /* WS_HAVE_OPENSSL */
