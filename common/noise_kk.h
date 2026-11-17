/* Noise_KK_25519_ChaChaPoly_SHA256
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A direct transcription of the Noise Protocol Framework, revision 34
 * (noiseprotocol.org), for exactly one handshake pattern and one suite.
 * Nothing here is new cryptography: every primitive is a libsodium call
 * (X25519, ChaCha20-Poly1305 IETF, SHA-256, HMAC-SHA256), and HKDF is the
 * spec's own definition over HMAC.  Checked against the published Noise test
 * vectors for this protocol name (tests/common/test_noise_kk.c).
 *
 * Why this suite: the design brief asked for BLAKE2s, but libsodium has no
 * BLAKE2s.  SHA-256 and HMAC-SHA256 are libsodium primitives, so with SHA256
 * nothing has to be hand-built.  The hash only affects handshake internals;
 * message sizes are identical.
 *
 * KK: both sides already hold each other's static public key.  Two messages,
 * mutual authentication, forward secrecy:
 *
 *     -> s
 *     <- s
 *     ...
 *     -> e, es, ss        48 B with an empty payload (32 B e + 16 B tag)
 *     <- e, ee, se        48 B with an empty payload
 */

#ifndef NOISE_KK_H_
#define NOISE_KK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOISE_KK_PROTOCOL_NAME "Noise_KK_25519_ChaChaPoly_SHA256"

#define NOISE_DHLEN    32
#define NOISE_HASHLEN  32
#define NOISE_KEYLEN   32
#define NOISE_TAGLEN   16

/* A handshake message with an empty payload: the ephemeral key plus the tag
 * that EncryptAndHash produces once a key is in place. */
#define NOISE_KK_MSG_LEN (NOISE_DHLEN + NOISE_TAGLEN)

/* CipherState (spec section 5.1). */
typedef struct {
    uint8_t  k[NOISE_KEYLEN];
    uint64_t n;
    bool     has_key;
} noise_cipher_t;

/* HandshakeState for KK (spec sections 5.2-5.3). */
typedef struct {
    bool     initiator;
    uint8_t  s_priv[NOISE_DHLEN];
    uint8_t  s_pub[NOISE_DHLEN];
    uint8_t  rs[NOISE_DHLEN];           /* remote static, known in advance */
    uint8_t  e_priv[NOISE_DHLEN];
    uint8_t  e_pub[NOISE_DHLEN];
    uint8_t  re[NOISE_DHLEN];           /* remote ephemeral, read in-band */
    uint8_t  h[NOISE_HASHLEN];
    uint8_t  ck[NOISE_HASHLEN];
    noise_cipher_t cs;
    int      next_msg;                  /* 0 = msg1 next, 1 = msg2 next, 2 = done */
    bool     have_fixed_e;              /* test vectors only */
} noise_kk_t;

/* One-time library initialisation; safe to call repeatedly.  0 on success. */
int noise_init(void);

/* Derive an X25519 public key from a private key. */
int noise_pubkey(const uint8_t priv[NOISE_DHLEN], uint8_t pub_out[NOISE_DHLEN]);

/* Start a handshake.  `rs` is the peer's static public key.  0 on success. */
int noise_kk_init(noise_kk_t *hs, bool initiator,
                  const uint8_t *prologue, size_t prologue_len,
                  const uint8_t s_priv[NOISE_DHLEN],
                  const uint8_t rs[NOISE_DHLEN]);

/* Pin the ephemeral key.  ONLY for the published test vectors: a fixed
 * ephemeral in production destroys forward secrecy. */
void noise_kk_set_ephemeral_for_test(noise_kk_t *hs,
                                     const uint8_t e_priv[NOISE_DHLEN]);

/* Write this side's next handshake message.  Returns its length, or -1 when
 * it is not this side's turn or a DH fails. */
int noise_kk_write(noise_kk_t *hs, const uint8_t *payload, size_t payload_len,
                   uint8_t *out, size_t out_cap);

/* Read the peer's next handshake message.  Returns the payload length, or -1
 * on any failure -- which must end the session. */
int noise_kk_read(noise_kk_t *hs, const uint8_t *msg, size_t msg_len,
                  uint8_t *payload_out, size_t payload_cap);

bool noise_kk_done(const noise_kk_t *hs);

/* After both messages: derive the transport keys.  `send` is the key this
 * side encrypts with, `recv` the one it decrypts with.  The handshake state
 * still holds secrets afterwards; call noise_kk_wipe() once finished with it. */
int noise_kk_split(noise_kk_t *hs, noise_cipher_t *send, noise_cipher_t *recv);

/* The final handshake hash (channel binding / test vectors).  Valid once the
 * handshake is done and before split. */
const uint8_t *noise_kk_handshake_hash(const noise_kk_t *hs);

void noise_kk_wipe(noise_kk_t *hs);

/* Transport encryption.  `out` must hold pt_len + NOISE_TAGLEN.  Returns the
 * ciphertext length, or -1 -- including when the 64-bit counter is exhausted:
 * it fails closed rather than wrapping, because a repeated nonce under the
 * same key breaks ChaCha20-Poly1305 outright. */
int noise_cipher_encrypt(noise_cipher_t *c,
                         const uint8_t *ad, size_t ad_len,
                         const uint8_t *pt, size_t pt_len,
                         uint8_t *out);

/* Returns the plaintext length, or -1 on an authentication failure.  The
 * counter only advances on success. */
int noise_cipher_decrypt(noise_cipher_t *c,
                         const uint8_t *ad, size_t ad_len,
                         const uint8_t *ct, size_t ct_len,
                         uint8_t *out);

void noise_cipher_wipe(noise_cipher_t *c);

#endif /* NOISE_KK_H_ */
