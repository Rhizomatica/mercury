/* SHA-1 and base64, for the WebSocket opening handshake
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "ws_crypto.h"

/* ---- SHA-1 (RFC 3174) ---- */

static uint32_t rotl32(uint32_t v, unsigned n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_compress(uint32_t h[5], const uint8_t block[WS_SHA1_BLOCK_LEN])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t) block[i * 4 + 0] << 24) |
               ((uint32_t) block[i * 4 + 1] << 16) |
               ((uint32_t) block[i * 4 + 2] << 8) |
               ((uint32_t) block[i * 4 + 3]);
    for (; i < 80; i++)
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];

    for (i = 0; i < 80; i++)
    {
        uint32_t f, k;

        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }

        uint32_t t = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = t;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void ws_sha1_init(ws_sha1_t *c)
{
    c->h[0] = 0x67452301;
    c->h[1] = 0xEFCDAB89;
    c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476;
    c->h[4] = 0xC3D2E1F0;
    c->bytes = 0;
    c->used = 0;
}

void ws_sha1_update(ws_sha1_t *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *) data;

    c->bytes += len;

    /* Top up a partial block first. */
    if (c->used > 0)
    {
        size_t need = WS_SHA1_BLOCK_LEN - c->used;
        size_t take = (len < need) ? len : need;

        memcpy(c->block + c->used, p, take);
        c->used += take;
        p += take;
        len -= take;

        if (c->used < WS_SHA1_BLOCK_LEN)
            return;

        sha1_compress(c->h, c->block);
        c->used = 0;
    }

    while (len >= WS_SHA1_BLOCK_LEN)
    {
        sha1_compress(c->h, p);
        p += WS_SHA1_BLOCK_LEN;
        len -= WS_SHA1_BLOCK_LEN;
    }

    if (len > 0)
    {
        memcpy(c->block, p, len);
        c->used = len;
    }
}

void ws_sha1_final(ws_sha1_t *c, uint8_t out[WS_SHA1_DIGEST_LEN])
{
    uint64_t bits = c->bytes * 8;
    uint8_t tail[8];
    static const uint8_t pad_first = 0x80;
    static const uint8_t pad_zero = 0x00;
    int i;

    ws_sha1_update(c, &pad_first, 1);
    /* Pad with zeros until the length field is the last 8 bytes of a block. */
    while (c->used != WS_SHA1_BLOCK_LEN - 8)
        ws_sha1_update(c, &pad_zero, 1);

    for (i = 0; i < 8; i++)
        tail[i] = (uint8_t) (bits >> (56 - i * 8));
    /* Length is of the original message, so put it in place directly rather
     * than through update(), which would count these 8 bytes too. */
    memcpy(c->block + c->used, tail, 8);
    sha1_compress(c->h, c->block);
    c->used = 0;

    for (i = 0; i < 5; i++)
    {
        out[i * 4 + 0] = (uint8_t) (c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t) (c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t) (c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t) (c->h[i]);
    }
}

void ws_sha1(const void *data, size_t len, uint8_t out[WS_SHA1_DIGEST_LEN])
{
    ws_sha1_t c;

    ws_sha1_init(&c);
    ws_sha1_update(&c, data, len);
    ws_sha1_final(&c, out);
}

/* ---- base64 (RFC 4648) ---- */

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t ws_base64_encode(const void *data, size_t len, char *out, size_t out_sz)
{
    const uint8_t *p = (const uint8_t *) data;
    size_t need = 4 * ((len + 2) / 3);
    size_t o = 0;
    size_t i;

    if (out == NULL || out_sz < need + 1)
        return 0;

    for (i = 0; i + 2 < len; i += 3)
    {
        uint32_t v = ((uint32_t) p[i] << 16) | ((uint32_t) p[i + 1] << 8) | p[i + 2];

        out[o++] = b64_alphabet[(v >> 18) & 0x3F];
        out[o++] = b64_alphabet[(v >> 12) & 0x3F];
        out[o++] = b64_alphabet[(v >> 6) & 0x3F];
        out[o++] = b64_alphabet[v & 0x3F];
    }

    if (i < len)
    {
        uint32_t v = (uint32_t) p[i] << 16;
        int have_two = (i + 1 < len);

        if (have_two)
            v |= (uint32_t) p[i + 1] << 8;

        out[o++] = b64_alphabet[(v >> 18) & 0x3F];
        out[o++] = b64_alphabet[(v >> 12) & 0x3F];
        out[o++] = have_two ? b64_alphabet[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }

    out[o] = '\0';
    return o;
}
