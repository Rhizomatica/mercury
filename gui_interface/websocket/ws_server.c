/* Minimal HTTP + WebSocket server for the Mercury UI
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One thread owns every socket.  The loop is: poll, flush what is queued,
 * read what arrived, repeat.  Other threads only ever push onto the
 * broadcast queue, which this thread drains -- the same arrangement the
 * Mongoose version used, kept because it is what makes the send path
 * lock-free everywhere except one small critical section.
 *
 * Socket portability comes from common/os_interop.h: it supplies poll() on
 * Windows (over select(), because WSAPoll is unreliable), SOCK_CLOSE,
 * sock_errno() and the EAGAIN spellings.  So this file has no #ifdef for
 * Windows or macOS beyond SO_NOSIGPIPE, and WSAStartup is already done once
 * in main() and mercury_engine_init().
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "os_interop.h"

#include "hermes_log.h"
#include "ws_crypto.h"
#include "ws_frame.h"
#include "ws_server.h"
#include "ws_tls.h"

#define WS_LOG_TAG "websocket"

/* RFC 6455 section 1.3. */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define WS_POLL_INTERVAL_MS      40
#define WS_MAX_CONNS             32
#define WS_DEFAULT_MAX_MESSAGE   8192
#define WS_MAX_HTTP_HEADER       8192
#define WS_HANDSHAKE_TIMEOUT_MS  10000

/* Write-buffer policy.  The spectrum stream is 2 KB every 50 ms, so a client
 * that stops reading backs up fast.  Past the soft limit we drop frames that
 * are safe to drop -- the next spectrum frame is along in 50 ms and carries
 * the same kind of information.  Control frames (status, device lists) are
 * never silently dropped: if those cannot be delivered the client is broken,
 * so past the hard limit we close it rather than lie about its state. */
#define WS_WBUF_SOFT_MAX  (256 * 1024)
#define WS_WBUF_HARD_MAX  (2 * 1024 * 1024)

/* Cap on the broadcast queue, in case the server thread is starved. */
#define WS_QUEUE_MAX_BYTES (4 * 1024 * 1024)

struct ws_qmsg {
    struct ws_qmsg *next;
    uint8_t *data;
    size_t len;
    bool is_text;
};

struct ws_conn {
    ws_conn_t *next;
    ws_server_t *srv;
    int fd;

    ws_tls_t *tls;
    bool tls_handshaking;
    bool tls_want_write;          /* a TLS op asked for POLLOUT */

    bool is_ws;                   /* upgrade completed */
    bool close_after_write;       /* drain the write buffer, then hang up */
    bool dead;                    /* drop at the end of this iteration */

    uint64_t created_ms;

    uint8_t *rbuf; size_t rlen, rcap;
    uint8_t *wbuf; size_t wlen, wpos, wcap;

    /* Reassembly of a fragmented message. */
    uint8_t *msg; size_t msglen, msgcap;
    int msg_opcode;               /* 0 when no message is in progress */

    size_t dropped;               /* frames dropped for backpressure */
};

struct ws_server {
    ws_server_opts_t opts;
    char ws_path[128];
    size_t max_message;

    int listen_fd;
    ws_tls_ctx_t *tls_ctx;

    pthread_t tid;
    bool thread_started;
    volatile bool running;

    ws_conn_t *conns;
    size_t nconns;

    pthread_mutex_t qlock;
    struct ws_qmsg *qhead, *qtail;
    size_t qbytes;
};

/* ---- small helpers ---- */

static int buf_reserve(uint8_t **buf, size_t *cap, size_t need)
{
    size_t ncap;
    uint8_t *nbuf;

    if (need <= *cap)
        return 0;

    ncap = (*cap == 0) ? 1024 : *cap;
    while (ncap < need)
        ncap *= 2;

    nbuf = (uint8_t *) realloc(*buf, ncap);
    if (nbuf == NULL)
        return -1;

    *buf = nbuf;
    *cap = ncap;
    return 0;
}

static int str_casecmp_n(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        unsigned char ca = (unsigned char) a[i];
        unsigned char cb = (unsigned char) b[i];

        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char) (ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char) (cb - 'A' + 'a');
        if (ca != cb)
            return (int) ca - (int) cb;
        if (ca == '\0')
            return 0;
    }
    return 0;
}

/* Case-insensitive search for a header in a NUL-free header block.  Returns
 * a pointer to the value (leading spaces skipped) and its length. */
static const char *header_find(const char *hdrs, size_t hdrs_len,
                               const char *name, size_t *value_len)
{
    size_t name_len = strlen(name);
    const char *p = hdrs;
    const char *end = hdrs + hdrs_len;

    while (p < end)
    {
        const char *eol = (const char *) memchr(p, '\n', (size_t) (end - p));
        size_t line_len = (eol != NULL) ? (size_t) (eol - p) : (size_t) (end - p);

        if (line_len > 0 && p[line_len - 1] == '\r')
            line_len--;

        if (line_len > name_len + 1 &&
            str_casecmp_n(p, name, name_len) == 0 &&
            p[name_len] == ':')
        {
            const char *v = p + name_len + 1;
            size_t vl = line_len - name_len - 1;

            while (vl > 0 && (*v == ' ' || *v == '\t')) { v++; vl--; }
            while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;

            *value_len = vl;
            return v;
        }

        if (eol == NULL)
            break;
        p = eol + 1;
    }

    *value_len = 0;
    return NULL;
}

static bool header_has_token(const char *value, size_t len, const char *token)
{
    size_t tlen = strlen(token);
    size_t i;

    for (i = 0; i + tlen <= len; i++)
    {
        if (str_casecmp_n(value + i, token, tlen) == 0)
        {
            bool left_ok = (i == 0) || value[i - 1] == ' ' || value[i - 1] == ',';
            bool right_ok = (i + tlen == len) || value[i + tlen] == ' ' ||
                            value[i + tlen] == ',';
            if (left_ok && right_ok)
                return true;
        }
    }
    return false;
}

static const char *content_type_for(const char *path)
{
    const char *dot = strrchr(path, '.');

    if (dot == NULL)
        return "application/octet-stream";

    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".js") == 0)  return "text/javascript; charset=utf-8";
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcmp(dot, ".txt") == 0) return "text/plain; charset=utf-8";

    return "application/octet-stream";
}

/* ---- raw I/O, plain or TLS ---- */

/* >0 bytes, 0 clean EOF, WS_TLS_ERROR, WS_TLS_WANT_READ, WS_TLS_WANT_WRITE */
static int conn_recv(ws_conn_t *c, void *buf, size_t len)
{
    int n;

    if (c->tls != NULL)
        return ws_tls_read(c->tls, buf, len);

    n = (int) recv(c->fd, (char *) buf, (int) len, 0);
    if (n > 0)
        return n;
    if (n == 0)
        return 0;

    if (sock_errno() == SOCK_EAGAIN || sock_errno() == SOCK_EWOULDBLOCK ||
        sock_errno() == SOCK_EINTR)
        return WS_TLS_WANT_READ;

    return WS_TLS_ERROR;
}

static int conn_send_raw(ws_conn_t *c, const void *buf, size_t len)
{
    int n;

    if (c->tls != NULL)
        return ws_tls_write(c->tls, buf, len);

    n = (int) send(c->fd, (const char *) buf, (int) len, MSG_NOSIGNAL);
    if (n > 0)
        return n;

    if (sock_errno() == SOCK_EAGAIN || sock_errno() == SOCK_EWOULDBLOCK ||
        sock_errno() == SOCK_EINTR)
        return WS_TLS_WANT_WRITE;

    return WS_TLS_ERROR;
}

/* Push bytes into the connection's write buffer.  `droppable` marks data we
 * would rather lose than disconnect over (see the WS_WBUF_* comment). */
static int conn_queue(ws_conn_t *c, const void *data, size_t len, bool droppable)
{
    size_t pending = c->wlen - c->wpos;

    if (len == 0)
        return 0;

    if (droppable && pending + len > WS_WBUF_SOFT_MAX)
    {
        c->dropped++;
        return 0;
    }

    if (pending + len > WS_WBUF_HARD_MAX)
    {
        HLOGW(WS_LOG_TAG, "client is not reading (%zu bytes queued), dropping it",
              pending);
        c->dead = true;
        return -1;
    }

    /* Reclaim the already-sent prefix before growing. */
    if (c->wpos > 0)
    {
        memmove(c->wbuf, c->wbuf + c->wpos, pending);
        c->wlen = pending;
        c->wpos = 0;
    }

    if (buf_reserve(&c->wbuf, &c->wcap, c->wlen + len) != 0)
    {
        c->dead = true;
        return -1;
    }

    memcpy(c->wbuf + c->wlen, data, len);
    c->wlen += len;
    return 0;
}

/* Try to push the write buffer out.  Returns -1 only on a fatal error. */
static int conn_flush(ws_conn_t *c)
{
    while (c->wpos < c->wlen)
    {
        int n = conn_send_raw(c, c->wbuf + c->wpos, c->wlen - c->wpos);

        if (n > 0)
        {
            c->wpos += (size_t) n;
            continue;
        }

        if (n == WS_TLS_WANT_WRITE)
        {
            c->tls_want_write = true;
            return 0;
        }
        if (n == WS_TLS_WANT_READ)
        {
            /* TLS needs to read before it can write again; the next poll
             * iteration will feed it. */
            return 0;
        }
        return -1;
    }

    c->tls_want_write = false;
    c->wpos = 0;
    c->wlen = 0;
    return 0;
}

/* ---- WebSocket framing ---- */

static int conn_send_frame(ws_conn_t *c, int opcode, const void *data,
                           size_t len, bool droppable)
{
    uint8_t hdr[WS_FRAME_MAX_HEADER];
    size_t hlen = ws_frame_header(hdr, opcode, len, true);

    /* Header and payload must not be split by a drop, so decide once. */
    if (droppable && (c->wlen - c->wpos) + hlen + len > WS_WBUF_SOFT_MAX)
    {
        c->dropped++;
        return 0;
    }

    if (conn_queue(c, hdr, hlen, false) != 0)
        return -1;
    if (len > 0 && conn_queue(c, data, len, false) != 0)
        return -1;

    return 0;
}

static void conn_close_ws(ws_conn_t *c, uint16_t code, const char *reason)
{
    uint8_t payload[125];
    size_t rlen = (reason != NULL) ? strlen(reason) : 0;

    if (rlen > sizeof(payload) - 2)
        rlen = sizeof(payload) - 2;

    payload[0] = (uint8_t) (code >> 8);
    payload[1] = (uint8_t) code;
    if (rlen > 0)
        memcpy(payload + 2, reason, rlen);

    (void) conn_send_frame(c, WS_OP_CLOSE, payload, rlen + 2, false);
    c->close_after_write = true;
}

int ws_conn_send(ws_conn_t *c, const void *data, size_t len, bool is_text)
{
    if (c == NULL || c->dead || !c->is_ws || c->close_after_write)
        return -1;

    return conn_send_frame(c, is_text ? WS_OP_TEXT : WS_OP_BINARY, data, len,
                           false);
}

/* Consume as many complete frames as the read buffer holds.
 * Returns -1 when the connection must be dropped. */
static int conn_process_frames(ws_conn_t *c)
{
    ws_server_t *s = c->srv;

    for (;;)
    {
        uint8_t *p = c->rbuf;
        ws_frame_t f;
        uint8_t *payload;
        uint64_t plen;
        bool fin;
        int opcode;
        int rc;

        rc = ws_frame_parse(p, c->rlen, true, &f);
        if (rc == WS_FRAME_INCOMPLETE)
            return 0;
        if (rc == WS_FRAME_PROTO_ERROR)
        {
            conn_close_ws(c, 1002, "protocol error");
            return 0;
        }

        fin = f.fin;
        opcode = f.opcode;
        plen = f.payload_len;

        /* Reject before waiting for the body, so a bogus 64-bit length
         * cannot make us buffer gigabytes for something that never comes. */
        if (plen > (uint64_t) s->max_message)
        {
            conn_close_ws(c, 1009, "message too big");
            return 0;
        }

        if (c->rlen < f.total_len)
            return 0;                     /* wait for the rest */

        payload = p + f.header_len;
        ws_frame_unmask(payload, (size_t) plen, f.mask);

        if (ws_frame_is_control(opcode))
        {
            if (opcode == WS_OP_CLOSE)
            {
                uint16_t code = 1000;

                if (plen >= 2)
                    code = (uint16_t) ((payload[0] << 8) | payload[1]);
                conn_close_ws(c, code, NULL);
            }
            else if (opcode == WS_OP_PING)
            {
                (void) conn_send_frame(c, WS_OP_PONG, payload, (size_t) plen,
                                       false);
            }
            /* PONG needs no action: we never send PING. */
        }
        else if (opcode == WS_OP_CONT || opcode == WS_OP_TEXT ||
                 opcode == WS_OP_BINARY)
        {
            if (opcode == WS_OP_CONT)
            {
                if (c->msg_opcode == 0)
                {
                    conn_close_ws(c, 1002, "continuation without start");
                    return 0;
                }
            }
            else
            {
                if (c->msg_opcode != 0)
                {
                    conn_close_ws(c, 1002, "interleaved message");
                    return 0;
                }
                c->msg_opcode = opcode;
                c->msglen = 0;
            }

            if (c->msglen + (size_t) plen > s->max_message)
            {
                conn_close_ws(c, 1009, "message too big");
                return 0;
            }

            if (plen > 0)
            {
                if (buf_reserve(&c->msg, &c->msgcap, c->msglen + (size_t) plen) != 0)
                    return -1;
                memcpy(c->msg + c->msglen, payload, (size_t) plen);
                c->msglen += (size_t) plen;
            }

            if (fin)
            {
                int delivered_opcode = c->msg_opcode;

                c->msg_opcode = 0;
                if (s->opts.on_message != NULL)
                    s->opts.on_message(c, c->msg, c->msglen,
                                       delivered_opcode == WS_OP_TEXT,
                                       s->opts.user_data);
                c->msglen = 0;
            }
        }
        else
        {
            conn_close_ws(c, 1002, "unknown opcode");
            return 0;
        }

        /* Drop the consumed frame. */
        memmove(c->rbuf, c->rbuf + f.total_len, c->rlen - f.total_len);
        c->rlen -= f.total_len;

        if (c->close_after_write)
            return 0;
    }
}

/* ---- HTTP ---- */

static void http_reply(ws_conn_t *c, int status, const char *reason,
                       const char *extra_headers,
                       const char *content_type,
                       const void *body, size_t body_len,
                       bool send_body)
{
    char head[512];
    int n;

    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "%s"
                 "\r\n",
                 status, reason, content_type, body_len,
                 (extra_headers != NULL) ? extra_headers : "");

    if (n > 0 && (size_t) n < sizeof(head))
        (void) conn_queue(c, head, (size_t) n, false);

    if (send_body && body_len > 0)
        (void) conn_queue(c, body, body_len, false);

    c->close_after_write = true;
}

/* Handle one request.  Returns -1 to drop the connection. */
static int conn_handle_request(ws_conn_t *c, const char *req, size_t req_len)
{
    ws_server_t *s = c->srv;
    const char *line_end;
    const char *method, *target, *hdrs;
    size_t method_len, target_len, hdrs_len;
    const char *sp1, *sp2;
    char path[256];
    size_t path_len;
    const char *q;
    size_t vlen;
    const char *v;
    bool is_head;

    line_end = (const char *) memchr(req, '\n', req_len);
    if (line_end == NULL)
        return -1;

    hdrs = line_end + 1;
    hdrs_len = req_len - (size_t) (hdrs - req);

    method = req;
    sp1 = (const char *) memchr(req, ' ', (size_t) (line_end - req));
    if (sp1 == NULL)
    {
        http_reply(c, 400, "Bad Request", NULL, "text/plain", "bad request", 11, true);
        return 0;
    }
    method_len = (size_t) (sp1 - method);

    target = sp1 + 1;
    sp2 = (const char *) memchr(target, ' ', (size_t) (line_end - target));
    if (sp2 == NULL)
    {
        http_reply(c, 400, "Bad Request", NULL, "text/plain", "bad request", 11, true);
        return 0;
    }
    target_len = (size_t) (sp2 - target);

    is_head = (method_len == 4 && str_casecmp_n(method, "HEAD", 4) == 0);
    if (!is_head && !(method_len == 3 && str_casecmp_n(method, "GET", 3) == 0))
    {
        http_reply(c, 405, "Method Not Allowed", "Allow: GET, HEAD\r\n",
                   "text/plain", "method not allowed", 18, true);
        return 0;
    }

    /* Strip any query string; none of the pages use one. */
    q = (const char *) memchr(target, '?', target_len);
    if (q != NULL)
        target_len = (size_t) (q - target);

    if (target_len == 0 || target_len >= sizeof(path))
    {
        http_reply(c, 400, "Bad Request", NULL, "text/plain", "bad target", 10, true);
        return 0;
    }
    memcpy(path, target, target_len);
    path[target_len] = '\0';
    path_len = target_len;

    /* The WebSocket endpoint. */
    if (strcmp(path, s->ws_path) == 0)
    {
        const char *key;
        size_t key_len;
        char accept_src[256];
        uint8_t digest[WS_SHA1_DIGEST_LEN];
        char accept[64];
        char resp[256];
        int n;

        v = header_find(hdrs, hdrs_len, "Upgrade", &vlen);
        if (v == NULL || !header_has_token(v, vlen, "websocket"))
        {
            http_reply(c, 426, "Upgrade Required",
                       "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n",
                       "text/plain", "websocket endpoint", 18, true);
            return 0;
        }

        v = header_find(hdrs, hdrs_len, "Sec-WebSocket-Version", &vlen);
        if (v == NULL || vlen != 2 || memcmp(v, "13", 2) != 0)
        {
            http_reply(c, 426, "Upgrade Required",
                       "Sec-WebSocket-Version: 13\r\n",
                       "text/plain", "unsupported version", 19, true);
            return 0;
        }

        key = header_find(hdrs, hdrs_len, "Sec-WebSocket-Key", &key_len);
        if (key == NULL || key_len == 0 ||
            key_len + strlen(WS_GUID) >= sizeof(accept_src))
        {
            http_reply(c, 400, "Bad Request", NULL, "text/plain",
                       "missing key", 11, true);
            return 0;
        }

        memcpy(accept_src, key, key_len);
        memcpy(accept_src + key_len, WS_GUID, strlen(WS_GUID));
        ws_sha1(accept_src, key_len + strlen(WS_GUID), digest);
        if (ws_base64_encode(digest, sizeof(digest), accept, sizeof(accept)) == 0)
            return -1;

        n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n"
                     "\r\n",
                     accept);
        if (n <= 0 || (size_t) n >= sizeof(resp))
            return -1;

        if (conn_queue(c, resp, (size_t) n, false) != 0)
            return -1;

        c->is_ws = true;
        HLOGI(WS_LOG_TAG, "client connected (fd %d)", c->fd);

        if (s->opts.on_connect != NULL)
            s->opts.on_connect(c, s->opts.user_data);

        return 0;
    }

    /* Static pages. */
    if (s->opts.asset_lookup == NULL)
    {
        http_reply(c, 404, "Not Found", NULL, "text/plain", "not found", 9, true);
        return 0;
    }

    if (strcmp(path, "/") == 0)
    {
        memcpy(path, "/index.html", sizeof("/index.html"));
        path_len = sizeof("/index.html") - 1;
    }

    /* The assets are a fixed in-binary table, not a directory, so there is no
     * traversal to speak of -- but refuse the syntax anyway so nobody has to
     * re-derive that when the lookup changes. */
    if (strstr(path, "..") != NULL)
    {
        http_reply(c, 400, "Bad Request", NULL, "text/plain", "bad path", 8, true);
        return 0;
    }

    {
        size_t asset_len = 0;
        const void *asset = s->opts.asset_lookup(path, &asset_len);

        (void) path_len;

        if (asset == NULL)
        {
            http_reply(c, 404, "Not Found", NULL, "text/plain", "not found", 9, true);
            return 0;
        }

        http_reply(c, 200, "OK", NULL, content_type_for(path),
                   asset, asset_len, !is_head);
    }

    return 0;
}

/* Returns -1 to drop the connection. */
static int conn_process_http(ws_conn_t *c)
{
    const char *start;
    size_t hdr_len;

    /* Headers end at the first blank line. */
    if (c->rlen < 4)
        return 0;

    start = (const char *) c->rbuf;
    {
        const char *found = NULL;
        size_t i;

        for (i = 0; i + 3 < c->rlen; i++)
        {
            if (c->rbuf[i] == '\r' && c->rbuf[i + 1] == '\n' &&
                c->rbuf[i + 2] == '\r' && c->rbuf[i + 3] == '\n')
            {
                found = start + i;
                break;
            }
        }

        if (found == NULL)
        {
            if (c->rlen > WS_MAX_HTTP_HEADER)
            {
                http_reply(c, 431, "Request Header Fields Too Large", NULL,
                           "text/plain", "headers too large", 17, true);
                return 0;
            }
            return 0;
        }

        hdr_len = (size_t) (found - start) + 4;

        /* Refuse an over-long header block even when it arrived complete.
         * Checking only the incomplete case made the limit depend on how the
         * request was split across reads: the same oversized request was
         * refused when it trickled in and served when it came in one piece. */
        if (hdr_len > WS_MAX_HTTP_HEADER)
        {
            http_reply(c, 431, "Request Header Fields Too Large", NULL,
                       "text/plain", "headers too large", 17, true);
            return 0;
        }
    }

    if (conn_handle_request(c, start, hdr_len) != 0)
        return -1;

    memmove(c->rbuf, c->rbuf + hdr_len, c->rlen - hdr_len);
    c->rlen -= hdr_len;

    return 0;
}

/* ---- connection lifecycle ---- */

static void conn_free(ws_conn_t *c)
{
    if (c == NULL)
        return;

    if (c->tls != NULL)
        ws_tls_free(c->tls);
    if (c->fd >= 0)
        SOCK_CLOSE(c->fd);

    free(c->rbuf);
    free(c->wbuf);
    free(c->msg);
    free(c);
}

static void server_drop_conn(ws_server_t *s, ws_conn_t *c)
{
    ws_conn_t **pp = &s->conns;

    while (*pp != NULL && *pp != c)
        pp = &(*pp)->next;

    if (*pp == c)
    {
        *pp = c->next;
        s->nconns--;
    }

    if (c->is_ws)
    {
        if (c->dropped > 0)
            HLOGW(WS_LOG_TAG, "client disconnected (fd %d, %zu frames dropped)",
                  c->fd, c->dropped);
        else
            HLOGI(WS_LOG_TAG, "client disconnected (fd %d)", c->fd);

        if (s->opts.on_close != NULL)
            s->opts.on_close(c, s->opts.user_data);
    }

    conn_free(c);
}

static void server_accept(ws_server_t *s)
{
    for (;;)
    {
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        ws_conn_t *c;
        int fd = (int) accept(s->listen_fd, (struct sockaddr *) &addr, &alen);

        if (fd < 0)
            return;                     /* EAGAIN, or nothing left to accept */

        if (s->nconns >= WS_MAX_CONNS)
        {
            HLOGW(WS_LOG_TAG, "refusing connection: %d already open", WS_MAX_CONNS);
            SOCK_CLOSE(fd);
            continue;
        }

        if (sock_set_nonblocking(fd) != 0)
        {
            SOCK_CLOSE(fd);
            continue;
        }

        {
            int one = 1;

            /* Status frames are small and latency matters more than packing. */
            (void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                              (const char *) &one, sizeof(one));
#if defined(SO_NOSIGPIPE)
            (void) setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                              (const char *) &one, sizeof(one));
#endif
        }

        c = (ws_conn_t *) calloc(1, sizeof(*c));
        if (c == NULL)
        {
            SOCK_CLOSE(fd);
            continue;
        }

        c->srv = s;
        c->fd = fd;
        c->created_ms = hermes_uptime_ms();

        if (s->tls_ctx != NULL)
        {
            c->tls = ws_tls_new(s->tls_ctx, fd);
            if (c->tls == NULL)
            {
                HLOGE(WS_LOG_TAG, "cannot start TLS: %s", ws_tls_last_error());
                conn_free(c);
                continue;
            }
            c->tls_handshaking = true;
        }

        c->next = s->conns;
        s->conns = c;
        s->nconns++;
    }
}

/* Read and act on whatever this connection has to say.
 * Returns -1 when it should be dropped. */
static int conn_service_read(ws_conn_t *c)
{
    for (;;)
    {
        uint8_t tmp[8192];
        int n;

        if (c->close_after_write)
            return 0;

        n = conn_recv(c, tmp, sizeof(tmp));

        if (n == WS_TLS_WANT_READ)
            return 0;
        if (n == WS_TLS_WANT_WRITE)
        {
            c->tls_want_write = true;
            return 0;
        }
        if (n == 0)
            return -1;                  /* peer closed */
        if (n < 0)
            return -1;

        if (buf_reserve(&c->rbuf, &c->rcap, c->rlen + (size_t) n) != 0)
            return -1;
        memcpy(c->rbuf + c->rlen, tmp, (size_t) n);
        c->rlen += (size_t) n;

        if (!c->is_ws)
        {
            if (conn_process_http(c) != 0)
                return -1;
        }

        if (c->is_ws)
        {
            if (conn_process_frames(c) != 0)
                return -1;
        }

        /* A short read means the socket is drained for now. */
        if ((size_t) n < sizeof(tmp))
            return 0;
    }
}

static void server_drain_queue(ws_server_t *s)
{
    struct ws_qmsg *head;
    ws_conn_t *c;

    pthread_mutex_lock(&s->qlock);
    head = s->qhead;
    s->qhead = NULL;
    s->qtail = NULL;
    s->qbytes = 0;
    pthread_mutex_unlock(&s->qlock);

    while (head != NULL)
    {
        struct ws_qmsg *m = head;

        head = head->next;

        for (c = s->conns; c != NULL; c = c->next)
        {
            if (!c->is_ws || c->dead || c->close_after_write)
                continue;

            /* Binary frames are the spectrum stream: droppable under
             * backpressure.  Text frames are state the UI needs. */
            (void) conn_send_frame(c, m->is_text ? WS_OP_TEXT : WS_OP_BINARY,
                                   m->data, m->len, !m->is_text);
        }

        free(m->data);
        free(m);
    }
}

static void server_free_queue(ws_server_t *s)
{
    struct ws_qmsg *head;

    pthread_mutex_lock(&s->qlock);
    head = s->qhead;
    s->qhead = NULL;
    s->qtail = NULL;
    s->qbytes = 0;
    pthread_mutex_unlock(&s->qlock);

    while (head != NULL)
    {
        struct ws_qmsg *m = head;

        head = head->next;
        free(m->data);
        free(m);
    }
}

static void *ws_server_thread(void *arg)
{
    ws_server_t *s = (ws_server_t *) arg;

    while (s->running)
    {
        struct pollfd pfds[WS_MAX_CONNS + 1];
        ws_conn_t *conn_of[WS_MAX_CONNS + 1];
        nfds_t nfds = 0;
        ws_conn_t *c, *next;
        uint64_t now;
        int rc;
        nfds_t i;

        pfds[0].fd = s->listen_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        conn_of[0] = NULL;
        nfds = 1;

        for (c = s->conns; c != NULL && nfds < WS_MAX_CONNS + 1; c = c->next)
        {
            pfds[nfds].fd = c->fd;
            pfds[nfds].events = POLLIN;
            if (c->wlen > c->wpos || c->tls_want_write)
                pfds[nfds].events |= POLLOUT;
            pfds[nfds].revents = 0;
            conn_of[nfds] = c;
            nfds++;
        }

        rc = poll(pfds, nfds, WS_POLL_INTERVAL_MS);
        if (rc < 0 && sock_errno() != SOCK_EINTR)
        {
            HLOGE(WS_LOG_TAG, "poll failed (%d)", sock_errno());
            hermes_usleep(100000);
        }

        if (!s->running)
            break;

        if (rc > 0 && (pfds[0].revents & POLLIN) != 0)
            server_accept(s);

        for (i = 1; i < nfds; i++)
        {
            c = conn_of[i];
            if (c == NULL || c->dead)
                continue;

            if ((pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            {
                /* POLLHUP with data still buffered is a half-close; let the
                 * read below decide, but a hard error is terminal. */
                if ((pfds[i].revents & (POLLERR | POLLNVAL)) != 0)
                {
                    c->dead = true;
                    continue;
                }
            }

            if (c->tls_handshaking)
            {
                int hs = ws_tls_handshake(c->tls);

                if (hs == 1)
                {
                    c->tls_handshaking = false;
                    c->tls_want_write = false;
                }
                else if (hs == WS_TLS_WANT_WRITE)
                {
                    c->tls_want_write = true;
                    continue;
                }
                else if (hs == WS_TLS_WANT_READ)
                {
                    c->tls_want_write = false;
                    continue;
                }
                else
                {
                    HLOGW(WS_LOG_TAG, "TLS handshake failed: %s",
                          ws_tls_last_error());
                    c->dead = true;
                    continue;
                }
            }

            if (conn_flush(c) != 0)
            {
                c->dead = true;
                continue;
            }

            if ((pfds[i].revents & (POLLIN | POLLHUP)) != 0)
            {
                if (conn_service_read(c) != 0)
                    c->dead = true;
            }
        }

        server_drain_queue(s);

        /* Push whatever the drain just queued, and retire finished sockets. */
        now = hermes_uptime_ms();
        for (c = s->conns; c != NULL; c = next)
        {
            next = c->next;

            if (!c->dead && !c->tls_handshaking && conn_flush(c) != 0)
                c->dead = true;

            if (!c->dead && c->close_after_write && c->wpos >= c->wlen)
                c->dead = true;

            /* A connection that never finishes its handshake is either a
             * port scan or a broken client; do not let those accumulate. */
            if (!c->dead && !c->is_ws &&
                now - c->created_ms > WS_HANDSHAKE_TIMEOUT_MS)
                c->dead = true;

            if (c->dead)
                server_drop_conn(s, c);
        }
    }

    /* Shutdown: drop everything immediately rather than draining.  A browser
     * tab left open holds its socket forever, and waiting for it stalls past
     * main()'s alarm(10) watchdog -- the process then dies with SIGALRM. */
    while (s->conns != NULL)
        server_drop_conn(s, s->conns);

    server_free_queue(s);

    HLOGI(WS_LOG_TAG, "server thread stopped");
    return NULL;
}

/* ---- public API ---- */

static int open_listener(const char *bind_addr, uint16_t port)
{
    struct sockaddr_in addr;
    int fd;
    int one = 1;

    fd = (int) socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    (void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one,
                      sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (bind_addr == NULL || bind_addr[0] == '\0')
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1)
    {
        HLOGE(WS_LOG_TAG, "bad bind address \"%s\"", bind_addr);
        SOCK_CLOSE(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0)
    {
        SOCK_CLOSE(fd);
        return -1;
    }

    if (listen(fd, 8) != 0)
    {
        SOCK_CLOSE(fd);
        return -1;
    }

    if (sock_set_nonblocking(fd) != 0)
    {
        SOCK_CLOSE(fd);
        return -1;
    }

    return fd;
}

int ws_server_start(ws_server_t **out, const ws_server_opts_t *opts)
{
    ws_server_t *s;

    if (out == NULL || opts == NULL)
        return -1;

    s = (ws_server_t *) calloc(1, sizeof(*s));
    if (s == NULL)
        return -1;

    s->opts = *opts;
    s->listen_fd = -1;
    s->max_message = (opts->max_message_size > 0) ? opts->max_message_size
                                                  : WS_DEFAULT_MAX_MESSAGE;
    snprintf(s->ws_path, sizeof(s->ws_path), "%s",
             (opts->ws_path != NULL) ? opts->ws_path : "/websocket");

    pthread_mutex_init(&s->qlock, NULL);

    if (opts->tls_enabled)
    {
        if (!ws_tls_available())
        {
            HLOGE(WS_LOG_TAG,
                  "wss:// requested but this build has no TLS support; "
                  "rebuild with OpenSSL or set ui_protocol=ws");
            pthread_mutex_destroy(&s->qlock);
            free(s);
            return -1;
        }

        s->tls_ctx = ws_tls_ctx_new(opts->tls_cert_path, opts->tls_key_path);
        if (s->tls_ctx == NULL)
        {
            HLOGE(WS_LOG_TAG, "cannot enable TLS: %s", ws_tls_last_error());
            pthread_mutex_destroy(&s->qlock);
            free(s);
            return -1;
        }
    }

    s->listen_fd = open_listener(opts->bind_addr, opts->port);
    if (s->listen_fd < 0)
    {
        HLOGE(WS_LOG_TAG, "cannot listen on port %u", (unsigned) opts->port);
        ws_tls_ctx_free(s->tls_ctx);
        pthread_mutex_destroy(&s->qlock);
        free(s);
        return -1;
    }

    s->running = true;
    if (pthread_create(&s->tid, NULL, ws_server_thread, s) != 0)
    {
        HLOGE(WS_LOG_TAG, "cannot start server thread");
        s->running = false;
        SOCK_CLOSE(s->listen_fd);
        ws_tls_ctx_free(s->tls_ctx);
        pthread_mutex_destroy(&s->qlock);
        free(s);
        return -1;
    }
    s->thread_started = true;

    HLOGI(WS_LOG_TAG, "listening on %s://%s:%u%s",
          opts->tls_enabled ? "wss" : "ws",
          (opts->bind_addr && opts->bind_addr[0]) ? opts->bind_addr : "0.0.0.0",
          (unsigned) opts->port, s->ws_path);

    *out = s;
    return 0;
}

void ws_server_stop(ws_server_t *s)
{
    if (s == NULL)
        return;

    s->running = false;

    if (s->thread_started)
        pthread_join(s->tid, NULL);

    if (s->listen_fd >= 0)
        SOCK_CLOSE(s->listen_fd);

    ws_tls_ctx_free(s->tls_ctx);
    pthread_mutex_destroy(&s->qlock);
    free(s);
}

int ws_server_broadcast(ws_server_t *s, const void *data, size_t len,
                        bool is_text)
{
    struct ws_qmsg *m;

    if (s == NULL || data == NULL || len == 0 || !s->running)
        return -1;

    m = (struct ws_qmsg *) malloc(sizeof(*m));
    if (m == NULL)
        return -1;

    m->data = (uint8_t *) malloc(len);
    if (m->data == NULL)
    {
        free(m);
        return -1;
    }

    memcpy(m->data, data, len);
    m->len = len;
    m->is_text = is_text;
    m->next = NULL;

    pthread_mutex_lock(&s->qlock);

    if (s->qbytes + len > WS_QUEUE_MAX_BYTES)
    {
        /* The server thread is not keeping up.  Dropping here beats growing
         * without bound; the next status frame carries the same state. */
        pthread_mutex_unlock(&s->qlock);
        free(m->data);
        free(m);
        return -1;
    }

    if (s->qtail != NULL)
        s->qtail->next = m;
    else
        s->qhead = m;
    s->qtail = m;
    s->qbytes += len;

    pthread_mutex_unlock(&s->qlock);
    return 0;
}

size_t ws_server_client_count(ws_server_t *s)
{
    size_t n = 0;
    ws_conn_t *c;

    if (s == NULL)
        return 0;

    for (c = s->conns; c != NULL; c = c->next)
        if (c->is_ws)
            n++;

    return n;
}
