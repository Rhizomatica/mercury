/* SHA-1 and base64, for the WebSocket opening handshake
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RFC 6455 computes Sec-WebSocket-Accept as base64(SHA1(key + GUID)), so a
 * WebSocket server needs both.  Written from RFC 3174 (SHA-1) and RFC 4648
 * (base64); no third-party code.  SHA-1 is used here only as the handshake
 * checksum the protocol mandates -- it is not a security primitive in this
 * program, and nothing else in Mercury should reach for it.
 */

#ifndef WS_CRYPTO_H_
#define WS_CRYPTO_H_

#include <stddef.h>
#include <stdint.h>

#define WS_SHA1_DIGEST_LEN 20
#define WS_SHA1_BLOCK_LEN  64

typedef struct {
    uint32_t h[5];
    uint64_t bytes;                     /* total message length so far */
    uint8_t  block[WS_SHA1_BLOCK_LEN];
    size_t   used;                      /* bytes buffered in block */
} ws_sha1_t;

void ws_sha1_init(ws_sha1_t *c);
void ws_sha1_update(ws_sha1_t *c, const void *data, size_t len);
void ws_sha1_final(ws_sha1_t *c, uint8_t out[WS_SHA1_DIGEST_LEN]);

/* One-shot convenience wrapper. */
void ws_sha1(const void *data, size_t len, uint8_t out[WS_SHA1_DIGEST_LEN]);

/* Base64-encode `len` bytes into `out` as a NUL-terminated string.
 * Needs 4 * ((len + 2) / 3) + 1 bytes.  Returns the number of characters
 * written excluding the NUL, or 0 if `out_sz` is too small. */
size_t ws_base64_encode(const void *data, size_t len, char *out, size_t out_sz);

#endif /* WS_CRYPTO_H_ */
