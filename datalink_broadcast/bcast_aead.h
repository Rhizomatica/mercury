/* Broadcast object encryption (XChaCha20-Poly1305)
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Broadcast has no return path, so there is no handshake: each carousel
 * OBJECT is sealed once, before RaptorQ, with a group key -- so the 41 bytes
 * are paid once per object, not per symbol.
 *
 *     [hdr: 1 B][nonce: 24 B][ciphertext][Poly1305 tag: 16 B]
 *
 *     hdr   = (BCAST_AEAD_VERSION << 4) | key-id
 *     key-id = low 4 bits of SHA-256(key)[0], so a network rotating its key
 *              can tell an old key's objects from a new one's without any
 *              configuration
 *     nonce = 24 random bytes -- safe at random for XChaCha20
 *     AD    = hdr
 *
 * The key is 32 raw bytes from [crypto] broadcast_key_file, which the HERMES
 * installer derives from the network's NNCP area key.
 *
 * WHERE THIS RUNS.  In HERMES production the carousel objects are built by
 * hermes-broadcast's broadcast_daemon, not by Mercury, so that is where
 * sealing and opening belong for real traffic: Mercury never needs the key
 * for it.  Mercury uses the same format in its own bcast_file (the Fyne
 * panel, bcast_file_tool) so the two stay wire-compatible.  This file is
 * deliberately self-contained -- libsodium and libc only -- so it can be
 * vendored into hermes-broadcast verbatim; define BCAST_AEAD_STANDALONE
 * there to enable it without Mercury's build.
 *
 * WHAT IT ADDS, given that the payload is usually an NNCP area packet that is
 * already encrypted: it hides the bundle metadata (sender node id, area id,
 * sizes), authenticates the whole object, and lets a receiver outside the
 * network discard an object cheaply instead of handing it to nncp-toss.
 *
 * RECEIVING.  A receiver tries the AEAD only when the header names its own
 * key; anything else, or anything that fails to open, is treated as a clear
 * object.  That is safe because a false pass is 2^-128, and it keeps clear
 * and encrypted objects interoperable on one carousel.  A station that
 * expects only encrypted objects should log the clear ones rather than let
 * them pass silently.
 */

#ifndef BCAST_AEAD_H_
#define BCAST_AEAD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BCAST_AEAD_VERSION   1
#define BCAST_AEAD_KEYLEN    32
#define BCAST_AEAD_NONCELEN  24
#define BCAST_AEAD_TAGLEN    16
#define BCAST_AEAD_OVERHEAD  (1 + BCAST_AEAD_NONCELEN + BCAST_AEAD_TAGLEN)  /* 41 */

/* True when built with a crypto backend. */
bool bcast_aead_available(void);

/* Read a 32-byte raw key file.  0 on success. */
int bcast_aead_key_load(const char *path, uint8_t key[BCAST_AEAD_KEYLEN]);

/* The header byte objects sealed with this key carry. */
uint8_t bcast_aead_header(const uint8_t key[BCAST_AEAD_KEYLEN]);

/* Seal an object.  `out` must hold len + BCAST_AEAD_OVERHEAD.  Returns the
 * sealed length, or 0 on failure. */
size_t bcast_aead_seal(const uint8_t key[BCAST_AEAD_KEYLEN],
                       const uint8_t *in, size_t len,
                       uint8_t *out, size_t out_cap);

/* Try to open an object.  Returns true and the plaintext (len - 41 bytes,
 * into `out`) when it was sealed with this key; false when it was not --
 * a clear object, another key's, or damaged -- in which case the caller
 * treats `in` as a clear object.  `out` must hold len bytes. */
bool bcast_aead_open(const uint8_t key[BCAST_AEAD_KEYLEN],
                     const uint8_t *in, size_t len,
                     uint8_t *out, size_t out_cap, size_t *out_len);

#endif /* BCAST_AEAD_H_ */
