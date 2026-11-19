/* RFC 6455 frame codec
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ws_frame.h"

#include <string.h>

bool ws_frame_is_control(int opcode)
{
    return (opcode & 0x08) != 0;
}

int ws_frame_parse(const uint8_t *buf, size_t avail, bool require_mask,
                   ws_frame_t *f)
{
    uint64_t plen;
    size_t hlen;
    int i;

    if (f == NULL || buf == NULL)
        return WS_FRAME_PROTO_ERROR;

    if (avail < 2)
        return WS_FRAME_INCOMPLETE;

    f->fin = (buf[0] & 0x80) != 0;
    f->opcode = buf[0] & 0x0F;
    f->masked = (buf[1] & 0x80) != 0;
    f->mask = NULL;

    /* We negotiate no extensions, so a reserved bit is a protocol error
     * rather than something to skip past. */
    if ((buf[0] & 0x70) != 0)
        return WS_FRAME_PROTO_ERROR;

    plen = (uint64_t) (buf[1] & 0x7F);
    hlen = 2;

    if (plen == 126)
    {
        if (avail < 4)
            return WS_FRAME_INCOMPLETE;
        plen = ((uint64_t) buf[2] << 8) | buf[3];
        hlen = 4;

        /* A 16-bit length must actually need 16 bits; the minimal encoding is
         * mandatory, and accepting a padded one lets two implementations
         * disagree about the same bytes. */
        if (plen < 126)
            return WS_FRAME_PROTO_ERROR;
    }
    else if (plen == 127)
    {
        if (avail < 10)
            return WS_FRAME_INCOMPLETE;

        plen = 0;
        for (i = 0; i < 8; i++)
            plen = (plen << 8) | buf[2 + i];
        hlen = 10;

        /* The high bit must be clear (RFC 6455 5.2). */
        if ((plen >> 63) != 0)
            return WS_FRAME_PROTO_ERROR;
        if (plen <= 0xFFFF)
            return WS_FRAME_PROTO_ERROR;
    }

    if (require_mask && !f->masked)
        return WS_FRAME_PROTO_ERROR;

    /* Control frames are never fragmented and never longer than 125 bytes. */
    if (ws_frame_is_control(f->opcode) && (!f->fin || plen > 125))
        return WS_FRAME_PROTO_ERROR;

    /* Opcodes outside the ones RFC 6455 defines are a protocol error. */
    if (f->opcode > WS_OP_BINARY && f->opcode < WS_OP_CLOSE)
        return WS_FRAME_PROTO_ERROR;
    if (f->opcode > WS_OP_PONG)
        return WS_FRAME_PROTO_ERROR;

    if (f->masked)
    {
        if (avail < hlen + 4)
            return WS_FRAME_INCOMPLETE;
        f->mask = buf + hlen;
        hlen += 4;
    }

    f->payload_len = plen;
    f->header_len = hlen;
    f->total_len = hlen + (size_t) plen;

    return WS_FRAME_OK;
}

void ws_frame_unmask(uint8_t *payload, size_t len, const uint8_t mask[4])
{
    size_t i;

    if (payload == NULL || mask == NULL)
        return;

    for (i = 0; i < len; i++)
        payload[i] ^= mask[i & 3];
}

size_t ws_frame_header(uint8_t hdr[WS_FRAME_MAX_HEADER], int opcode,
                       size_t payload_len, bool fin)
{
    hdr[0] = (uint8_t) ((fin ? 0x80 : 0x00) | (opcode & 0x0F));

    if (payload_len < 126)
    {
        hdr[1] = (uint8_t) payload_len;
        return 2;
    }

    if (payload_len <= 0xFFFF)
    {
        hdr[1] = 126;
        hdr[2] = (uint8_t) (payload_len >> 8);
        hdr[3] = (uint8_t) payload_len;
        return 4;
    }

    {
        int i;

        hdr[1] = 127;
        for (i = 0; i < 8; i++)
            hdr[2 + i] = (uint8_t) ((uint64_t) payload_len >> (56 - i * 8));
        return 10;
    }
}

static bool ci_eq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char) (x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char) (y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return true;
}

/* "a.b.c.d", each part 1-3 digits and <= 255. */
static bool is_ipv4_literal(const char *h, size_t n)
{
    int parts = 0, digits = 0, val = 0;

    for (size_t i = 0; i <= n; i++)
    {
        if (i == n || h[i] == '.')
        {
            if (digits == 0 || val > 255)
                return false;
            parts++;
            digits = val = 0;
        }
        else if (h[i] >= '0' && h[i] <= '9' && digits < 3)
        {
            val = val * 10 + (h[i] - '0');
            digits++;
        }
        else
            return false;
    }
    return parts == 4;
}

/* "[...]" holding only hex digits, ':' and '.' (an embedded IPv4), with at
 * least one ':'.  Not a full IPv6 parser: enough that a bracketed name such as
 * "[evil.example]" is not taken for an IP literal. */
static bool is_bracketed_ipv6(const char *h, size_t n)
{
    bool colon = false;

    if (n < 4 || h[0] != '[' || h[n - 1] != ']')
        return false;
    for (size_t i = 1; i + 1 < n; i++)
    {
        char c = h[i];
        if (c == ':')
            colon = true;
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F') || c == '.'))
            return false;
    }
    return colon;
}

static bool has_suffix(const char *h, size_t n, const char *sfx)
{
    size_t k = strlen(sfx);
    return n > k && ci_eq(h + n - k, sfx, k);
}

/* Hostnames an attacker's DNS cannot point at us. */
static bool host_is_rebind_safe(const char *h, size_t n)
{
    if (n == 0)
        return false;
    if (h[0] == '[')
        return is_bracketed_ipv6(h, n);                 /* IPv6 literal */
    if (is_ipv4_literal(h, n))                          /* IPv4 literal */
        return true;
    if (n == 9 && ci_eq(h, "localhost", 9))
        return true;
    if (memchr(h, '.', n) == NULL)                      /* single label */
        return true;
    /* Names that only resolve on the local network: mDNS, the RFC 8375 and
     * ICANN-reserved private zones, and the common undelegated .lan. */
    return has_suffix(h, n, ".local") || has_suffix(h, n, ".home.arpa") ||
           has_suffix(h, n, ".internal") || has_suffix(h, n, ".lan");
}

/* Is origin one of the comma- or space-separated entries of list? */
static bool origin_listed(const char *origin, size_t origin_len, const char *list)
{
    const char *p = list;

    while (p != NULL && *p != '\0')
    {
        size_t n;

        while (*p == ',' || *p == ' ')
            p++;
        n = strcspn(p, ", ");
        if (n != 0 && n == origin_len && ci_eq(p, origin, n))
            return true;
        p += n;
    }
    return false;
}

bool ws_origin_allowed(const char *origin, size_t origin_len,
                       const char *host, size_t host_len, bool tls,
                       const char *allowed_origins)
{
    const char *sep, *auth;
    size_t auth_len, hn;

    if (origin == NULL)
        return true;
    if (origin_listed(origin, origin_len, allowed_origins))
        return true;
    if (host == NULL || host_len == 0)
        return false;

    /* "scheme://authority"; "null" and anything else is refused. */
    sep = NULL;
    for (size_t i = 0; i + 3 <= origin_len; i++)
        if (memcmp(origin + i, "://", 3) == 0) { sep = origin + i; break; }
    if (sep == NULL)
        return false;
    auth = sep + 3;
    auth_len = origin_len - (size_t) (auth - origin);
    if (auth_len != host_len || !ci_eq(auth, host, host_len))
        return false;
    if (tls)
        return true;

    /* The host part of Host: up to the port, or the whole "[v6]". */
    if (host[0] == '[')
    {
        const char *e = memchr(host, ']', host_len);
        hn = e ? (size_t) (e - host) + 1 : host_len;
    }
    else
    {
        const char *c = memchr(host, ':', host_len);
        hn = c ? (size_t) (c - host) : host_len;
    }
    return host_is_rebind_safe(host, hn);
}
