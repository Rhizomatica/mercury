/* Broadcast object encryption (XChaCha20-Poly1305)
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See bcast_aead.h for the format and where this runs.
 */

#include <stdio.h>
#include <string.h>

#include "bcast_aead.h"

#if defined(ARQ_HAVE_CRYPTO) || defined(BCAST_AEAD_STANDALONE)
#define BCAST_AEAD_ENABLED 1
#include <sodium.h>
#endif

bool bcast_aead_available(void)
{
#if defined(BCAST_AEAD_ENABLED)
    return sodium_init() >= 0;
#else
    return false;
#endif
}

int bcast_aead_key_load(const char *path, uint8_t key[BCAST_AEAD_KEYLEN])
{
    FILE *f;
    size_t got;
    int extra;

    if (path == NULL || path[0] == '\0' || key == NULL)
        return -1;
    f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    got = fread(key, 1, BCAST_AEAD_KEYLEN, f);
    extra = fgetc(f);
    fclose(f);
    /* Exactly 32 bytes: too short or too long is a malformed key. */
    return (got == BCAST_AEAD_KEYLEN && extra == EOF) ? 0 : -1;
}

uint8_t bcast_aead_header(const uint8_t key[BCAST_AEAD_KEYLEN])
{
#if defined(BCAST_AEAD_ENABLED)
    uint8_t digest[crypto_hash_sha256_BYTES];

    crypto_hash_sha256(digest, key, BCAST_AEAD_KEYLEN);
    return (uint8_t) ((BCAST_AEAD_VERSION << 4) | (digest[0] & 0x0F));
#else
    (void) key;
    return 0;
#endif
}

size_t bcast_aead_seal(const uint8_t key[BCAST_AEAD_KEYLEN],
                       const uint8_t *in, size_t len,
                       uint8_t *out, size_t out_cap)
{
#if defined(BCAST_AEAD_ENABLED)
    unsigned long long clen = 0;

    if (key == NULL || in == NULL || out == NULL ||
        out_cap < len + BCAST_AEAD_OVERHEAD || sodium_init() < 0)
        return 0;

    out[0] = bcast_aead_header(key);
    randombytes_buf(out + 1, BCAST_AEAD_NONCELEN);
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(out + 1 + BCAST_AEAD_NONCELEN,
                                                   &clen, in, len,
                                                   out, 1,        /* AD = hdr */
                                                   NULL, out + 1, key) != 0)
        return 0;
    return 1 + BCAST_AEAD_NONCELEN + (size_t) clen;
#else
    (void) key; (void) in; (void) len; (void) out; (void) out_cap;
    return 0;
#endif
}

bool bcast_aead_open(const uint8_t key[BCAST_AEAD_KEYLEN],
                     const uint8_t *in, size_t len,
                     uint8_t *out, size_t out_cap, size_t *out_len)
{
#if defined(BCAST_AEAD_ENABLED)
    unsigned long long plen = 0;

    if (key == NULL || in == NULL || out == NULL || out_len == NULL ||
        len < BCAST_AEAD_OVERHEAD || out_cap < len - BCAST_AEAD_OVERHEAD ||
        sodium_init() < 0)
        return false;

    /* Only an object that names our key is worth trying. */
    if (in[0] != bcast_aead_header(key))
        return false;

    if (crypto_aead_xchacha20poly1305_ietf_decrypt(out, &plen, NULL,
                                                   in + 1 + BCAST_AEAD_NONCELEN,
                                                   len - 1 - BCAST_AEAD_NONCELEN,
                                                   in, 1, in + 1, key) != 0)
        return false;

    *out_len = (size_t) plen;
    return true;
#else
    (void) key; (void) in; (void) len; (void) out; (void) out_cap; (void) out_len;
    return false;
#endif
}
