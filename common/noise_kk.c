/* Noise_KK_25519_ChaChaPoly_SHA256
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Section numbers below refer to the Noise Protocol Framework, revision 34.
 */

#include "noise_kk.h"

/* Compiled to nothing without a crypto backend; arq_crypto.c then refuses
 * [crypto] mode != off at startup instead of calling in here. */
#if defined(ARQ_HAVE_CRYPTO)

#include <string.h>

#include <sodium.h>

#define NOISE_FAILED (-1)

int noise_init(void)
{
    /* 0 = initialised now, 1 = already initialised, -1 = failure. */
    return sodium_init() < 0 ? -1 : 0;
}

int noise_pubkey(const uint8_t priv[NOISE_DHLEN], uint8_t pub_out[NOISE_DHLEN])
{
    return crypto_scalarmult_curve25519_base(pub_out, priv) == 0 ? 0 : -1;
}

/* ---- 4.3: HMAC and HKDF ----------------------------------------------- */

static void hmac(const uint8_t key[NOISE_HASHLEN],
                 const uint8_t *a, size_t a_len,
                 const uint8_t *b, size_t b_len,
                 uint8_t out[NOISE_HASHLEN])
{
    crypto_auth_hmacsha256_state st;

    crypto_auth_hmacsha256_init(&st, key, NOISE_HASHLEN);
    if (a_len > 0)
        crypto_auth_hmacsha256_update(&st, a, a_len);
    if (b_len > 0)
        crypto_auth_hmacsha256_update(&st, b, b_len);
    crypto_auth_hmacsha256_final(&st, out);
    sodium_memzero(&st, sizeof(st));
}

/* HKDF(chaining_key, input_key_material, 2) as the spec defines it. */
static void hkdf2(const uint8_t ck[NOISE_HASHLEN],
                  const uint8_t *ikm, size_t ikm_len,
                  uint8_t out1[NOISE_HASHLEN], uint8_t out2[NOISE_HASHLEN])
{
    uint8_t temp_key[NOISE_HASHLEN];
    const uint8_t one = 0x01, two = 0x02;

    hmac(ck, ikm, ikm_len, NULL, 0, temp_key);
    hmac(temp_key, &one, 1, NULL, 0, out1);
    hmac(temp_key, out1, NOISE_HASHLEN, &two, 1, out2);
    sodium_memzero(temp_key, sizeof(temp_key));
}

/* ---- 5.1: CipherState ------------------------------------------------- */

static void cipher_init_key(noise_cipher_t *c, const uint8_t key[NOISE_KEYLEN])
{
    memcpy(c->k, key, NOISE_KEYLEN);
    c->n = 0;
    c->has_key = true;
}

/* ChaChaPoly's nonce: 32 bits of zeros, then n as 64-bit little-endian. */
static void chachapoly_nonce(uint64_t n, uint8_t nonce[12])
{
    int i;

    memset(nonce, 0, 4);
    for (i = 0; i < 8; i++)
        nonce[4 + i] = (uint8_t) (n >> (8 * i));
}

int noise_cipher_encrypt(noise_cipher_t *c,
                         const uint8_t *ad, size_t ad_len,
                         const uint8_t *pt, size_t pt_len,
                         uint8_t *out)
{
    uint8_t nonce[12];
    unsigned long long clen = 0;

    if (c == NULL || out == NULL)
        return NOISE_FAILED;

    if (!c->has_key)
    {
        /* Before the first MixKey the spec passes plaintext through.  KK
         * never gets here -- its first payload follows es and ss. */
        if (pt_len > 0)
            memmove(out, pt, pt_len);
        return (int) pt_len;
    }

    /* 2^64-1 is reserved by the spec.  Fail closed: a wrapped counter would
     * reuse a nonce, which breaks ChaCha20-Poly1305 outright. */
    if (c->n == UINT64_MAX)
        return NOISE_FAILED;

    chachapoly_nonce(c->n, nonce);
    if (crypto_aead_chacha20poly1305_ietf_encrypt(out, &clen, pt, pt_len,
                                                  ad, ad_len, NULL,
                                                  nonce, c->k) != 0)
        return NOISE_FAILED;

    c->n++;
    return (int) clen;
}

int noise_cipher_decrypt(noise_cipher_t *c,
                         const uint8_t *ad, size_t ad_len,
                         const uint8_t *ct, size_t ct_len,
                         uint8_t *out)
{
    uint8_t nonce[12];
    unsigned long long plen = 0;

    if (c == NULL || out == NULL)
        return NOISE_FAILED;

    if (!c->has_key)
    {
        if (ct_len > 0)
            memmove(out, ct, ct_len);
        return (int) ct_len;
    }

    if (c->n == UINT64_MAX || ct_len < NOISE_TAGLEN)
        return NOISE_FAILED;

    chachapoly_nonce(c->n, nonce);
    if (crypto_aead_chacha20poly1305_ietf_decrypt(out, &plen, NULL, ct, ct_len,
                                                  ad, ad_len, nonce, c->k) != 0)
        return NOISE_FAILED;             /* n is NOT advanced on failure */

    c->n++;
    return (int) plen;
}

void noise_cipher_wipe(noise_cipher_t *c)
{
    if (c != NULL)
        sodium_memzero(c, sizeof(*c));
}

/* ---- 5.2: SymmetricState --------------------------------------------- */

static void mix_hash(noise_kk_t *hs, const uint8_t *data, size_t len)
{
    crypto_hash_sha256_state st;

    crypto_hash_sha256_init(&st);
    crypto_hash_sha256_update(&st, hs->h, NOISE_HASHLEN);
    if (len > 0)
        crypto_hash_sha256_update(&st, data, len);
    crypto_hash_sha256_final(&st, hs->h);
}

static void mix_key(noise_kk_t *hs, const uint8_t *ikm, size_t ikm_len)
{
    uint8_t temp_k[NOISE_HASHLEN];

    hkdf2(hs->ck, ikm, ikm_len, hs->ck, temp_k);
    cipher_init_key(&hs->cs, temp_k);   /* HASHLEN == 32: no truncation */
    sodium_memzero(temp_k, sizeof(temp_k));
}

static int encrypt_and_hash(noise_kk_t *hs, const uint8_t *pt, size_t pt_len,
                            uint8_t *out)
{
    int n = noise_cipher_encrypt(&hs->cs, hs->h, NOISE_HASHLEN, pt, pt_len, out);

    if (n < 0)
        return NOISE_FAILED;
    mix_hash(hs, out, (size_t) n);
    return n;
}

static int decrypt_and_hash(noise_kk_t *hs, const uint8_t *ct, size_t ct_len,
                            uint8_t *out)
{
    int n = noise_cipher_decrypt(&hs->cs, hs->h, NOISE_HASHLEN, ct, ct_len, out);

    if (n < 0)
        return NOISE_FAILED;
    mix_hash(hs, ct, ct_len);
    return n;
}

/* ---- 5.3: HandshakeState --------------------------------------------- */

static int dh(const uint8_t priv[NOISE_DHLEN], const uint8_t pub[NOISE_DHLEN],
              uint8_t out[NOISE_DHLEN])
{
    /* libsodium refuses an all-zero result, i.e. a low-order peer point.
     * The spec permits either behaviour; refusing is the safer one. */
    return crypto_scalarmult_curve25519(out, priv, pub) == 0 ? 0 : NOISE_FAILED;
}

static int mix_dh(noise_kk_t *hs, const uint8_t priv[NOISE_DHLEN],
                  const uint8_t pub[NOISE_DHLEN])
{
    uint8_t shared[NOISE_DHLEN];
    int rc = dh(priv, pub, shared);

    if (rc == 0)
        mix_key(hs, shared, sizeof(shared));
    sodium_memzero(shared, sizeof(shared));
    return rc;
}

static int make_ephemeral(noise_kk_t *hs)
{
    if (!hs->have_fixed_e)
        randombytes_buf(hs->e_priv, sizeof(hs->e_priv));
    return noise_pubkey(hs->e_priv, hs->e_pub);
}

int noise_kk_init(noise_kk_t *hs, bool initiator,
                  const uint8_t *prologue, size_t prologue_len,
                  const uint8_t s_priv[NOISE_DHLEN],
                  const uint8_t rs[NOISE_DHLEN])
{
    size_t name_len = strlen(NOISE_KK_PROTOCOL_NAME);

    if (hs == NULL || s_priv == NULL || rs == NULL)
        return NOISE_FAILED;

    memset(hs, 0, sizeof(*hs));
    hs->initiator = initiator;
    memcpy(hs->s_priv, s_priv, NOISE_DHLEN);
    memcpy(hs->rs, rs, NOISE_DHLEN);
    if (noise_pubkey(hs->s_priv, hs->s_pub) != 0)
        return NOISE_FAILED;

    /* InitializeSymmetric: a name no longer than HASHLEN is used directly,
     * zero-padded.  This one is exactly 32 bytes. */
    if (name_len <= NOISE_HASHLEN)
        memcpy(hs->h, NOISE_KK_PROTOCOL_NAME, name_len);
    else
        crypto_hash_sha256(hs->h, (const uint8_t *) NOISE_KK_PROTOCOL_NAME,
                           name_len);
    memcpy(hs->ck, hs->h, NOISE_HASHLEN);
    hs->cs.has_key = false;

    mix_hash(hs, prologue, prologue_len);

    /* Pre-messages "-> s" then "<- s": the initiator's static first. */
    if (initiator)
    {
        mix_hash(hs, hs->s_pub, NOISE_DHLEN);
        mix_hash(hs, hs->rs, NOISE_DHLEN);
    }
    else
    {
        mix_hash(hs, hs->rs, NOISE_DHLEN);
        mix_hash(hs, hs->s_pub, NOISE_DHLEN);
    }

    hs->next_msg = 0;
    return 0;
}

void noise_kk_set_ephemeral_for_test(noise_kk_t *hs,
                                     const uint8_t e_priv[NOISE_DHLEN])
{
    memcpy(hs->e_priv, e_priv, NOISE_DHLEN);
    hs->have_fixed_e = true;
}

bool noise_kk_done(const noise_kk_t *hs)
{
    return hs != NULL && hs->next_msg == 2;
}

static void fail(noise_kk_t *hs)
{
    /* Any failure ends the handshake for good. */
    hs->next_msg = -1;
}

int noise_kk_write(noise_kk_t *hs, const uint8_t *payload, size_t payload_len,
                   uint8_t *out, size_t out_cap)
{
    int n;

    if (hs == NULL || out == NULL)
        return NOISE_FAILED;

    /* msg1 is the initiator's to write, msg2 the responder's. */
    if (!((hs->next_msg == 0 && hs->initiator) ||
          (hs->next_msg == 1 && !hs->initiator)))
        return NOISE_FAILED;

    if (out_cap < NOISE_DHLEN + payload_len + NOISE_TAGLEN)
        return NOISE_FAILED;

    /* "e" */
    if (make_ephemeral(hs) != 0)
    {
        fail(hs);
        return NOISE_FAILED;
    }
    memcpy(out, hs->e_pub, NOISE_DHLEN);
    mix_hash(hs, hs->e_pub, NOISE_DHLEN);

    if (hs->next_msg == 0)
    {
        /* -> e, es, ss   (initiator: es = DH(e, rs)) */
        if (mix_dh(hs, hs->e_priv, hs->rs) != 0 ||
            mix_dh(hs, hs->s_priv, hs->rs) != 0)
        {
            fail(hs);
            return NOISE_FAILED;
        }
    }
    else
    {
        /* <- e, ee, se   (responder: se = DH(e, rs)) */
        if (mix_dh(hs, hs->e_priv, hs->re) != 0 ||
            mix_dh(hs, hs->e_priv, hs->rs) != 0)
        {
            fail(hs);
            return NOISE_FAILED;
        }
    }

    n = encrypt_and_hash(hs, payload, payload_len, out + NOISE_DHLEN);
    if (n < 0)
    {
        fail(hs);
        return NOISE_FAILED;
    }

    hs->next_msg++;
    return NOISE_DHLEN + n;
}

int noise_kk_read(noise_kk_t *hs, const uint8_t *msg, size_t msg_len,
                  uint8_t *payload_out, size_t payload_cap)
{
    int n;

    if (hs == NULL || msg == NULL || payload_out == NULL)
        return NOISE_FAILED;

    if (!((hs->next_msg == 0 && !hs->initiator) ||
          (hs->next_msg == 1 && hs->initiator)))
        return NOISE_FAILED;

    /* Both KK messages carry an ephemeral and a tag at least. */
    if (msg_len < NOISE_KK_MSG_LEN ||
        payload_cap < msg_len - NOISE_KK_MSG_LEN)
    {
        fail(hs);
        return NOISE_FAILED;
    }

    /* "e" */
    memcpy(hs->re, msg, NOISE_DHLEN);
    mix_hash(hs, hs->re, NOISE_DHLEN);

    if (hs->next_msg == 0)
    {
        /* -> e, es, ss   (responder: es = DH(s, re)) */
        if (mix_dh(hs, hs->s_priv, hs->re) != 0 ||
            mix_dh(hs, hs->s_priv, hs->rs) != 0)
        {
            fail(hs);
            return NOISE_FAILED;
        }
    }
    else
    {
        /* <- e, ee, se   (initiator: se = DH(s, re)) */
        if (mix_dh(hs, hs->e_priv, hs->re) != 0 ||
            mix_dh(hs, hs->s_priv, hs->re) != 0)
        {
            fail(hs);
            return NOISE_FAILED;
        }
    }

    n = decrypt_and_hash(hs, msg + NOISE_DHLEN, msg_len - NOISE_DHLEN,
                         payload_out);
    if (n < 0)
    {
        fail(hs);
        return NOISE_FAILED;
    }

    hs->next_msg++;
    return n;
}

const uint8_t *noise_kk_handshake_hash(const noise_kk_t *hs)
{
    return hs != NULL ? hs->h : NULL;
}

int noise_kk_split(noise_kk_t *hs, noise_cipher_t *send, noise_cipher_t *recv)
{
    uint8_t k1[NOISE_HASHLEN], k2[NOISE_HASHLEN];

    if (hs == NULL || send == NULL || recv == NULL || !noise_kk_done(hs))
        return NOISE_FAILED;

    hkdf2(hs->ck, NULL, 0, k1, k2);

    /* The initiator sends with the first key, the responder with the second. */
    if (hs->initiator)
    {
        cipher_init_key(send, k1);
        cipher_init_key(recv, k2);
    }
    else
    {
        cipher_init_key(send, k2);
        cipher_init_key(recv, k1);
    }

    sodium_memzero(k1, sizeof(k1));
    sodium_memzero(k2, sizeof(k2));
    return 0;
}

void noise_kk_wipe(noise_kk_t *hs)
{
    if (hs != NULL)
        sodium_memzero(hs, sizeof(*hs));
}

#endif /* ARQ_HAVE_CRYPTO */
