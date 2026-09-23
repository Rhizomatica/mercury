/* Optional encryption of ARQ sessions
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Off by default: amateur rules forbid encryption in many places.  Stations
 * under a licence that allows it opt in with [crypto] mode = optional or
 * required.  Callsigns and ARQ/PHY headers always travel in the clear, for
 * legal identification; only the application byte stream is encrypted.
 *
 * WIRE FORMAT -- designed so that crypto OFF costs nothing at all:
 *
 *  Negotiation rides in bit 0x04 of the CALL/ACCEPT framer byte's extension
 *  field, which the 2-bit bandwidth token leaves unused.  No extra bytes, no
 *  extra turnaround.  With crypto off the bit is clear and the frames are
 *  byte-identical to Mercury 1.9.x.  An older receiver reads the whole field
 *  as the bandwidth token, so a CALL with the bit set is unparseable to it and
 *  is dropped: offering crypto to a pre-crypto Mercury fails closed.
 *
 *    CALL   bit set  = the initiator offers Noise_KK (mode != off, holds the
 *                      callee's key, and the client has not said ENCRYPT OFF)
 *    ACCEPT bit set  = the responder accepts (offered, mode != off, holds the
 *                      caller's key, and its client has not said ENCRYPT OFF)
 *
 *  With both bits set, the first bytes of each direction's ARQ stream are the
 *  Noise_KK handshake messages (48 B each; the payloads are always empty), and
 *  after that the stream is a sequence of records:
 *
 *    [len: 1-2 B varint][ciphertext: len B][Poly1305 tag: 16 B]
 *
 *  The varint header is the AEAD's associated data.  The nonce is an implicit
 *  64-bit counter per direction and never goes on air.
 *
 * WHERE THE RECORD LAYER SITS -- and why it must stay there:
 *
 *  Records are sealed and opened on the in-order application byte stream,
 *  ABOVE ARQ segmentation, at the tx_read / deliver_rx_data callbacks in
 *  arq.c.  The implicit nonce counter is only sound because every byte there
 *  is handed over exactly once and in order: ARQ retransmits and re-frames
 *  (the restage path) BELOW this layer, and deliver_rx_checked() drops
 *  duplicates before delivery.  Move record processing down into per-frame
 *  handling, where retransmits exist, and the two sides' counters drift apart
 *  -- or worse, a nonce repeats.
 *
 * RECORD SIZE is chosen for airtime, not convenience.  A record cannot be
 * released to the client until its tag verifies, so a large record at a slow
 * mode holds data back, while a small one wastes the 17-18 B of overhead.  A
 * record therefore spans about ARQ_CRYPTO_REC_FRAMES frames of the current
 * mode, clamped to [ARQ_CRYPTO_REC_MIN, ARQ_CRYPTO_REC_MAX]: about 0.4% overhead
 * on the fast modes and a delivery gap of roughly 30-50 s of airtime at the
 * slowest.  Records are sealed lazily, at the moment ARQ pulls bytes to send,
 * from everything the client has queued -- so a burst of small writes costs one
 * record, not one per write.
 */

#ifndef ARQ_CRYPTO_H_
#define ARQ_CRYPTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Always included, whether or not this build has a crypto backend:
 * noise_kk.h only declares types, and arq_xs_t must have ONE layout in every
 * translation unit.  A member that existed only under ARQ_HAVE_CRYPTO would
 * make arq.c and arq_crypto.c disagree about the struct's size if any file
 * were built without the define -- silent memory corruption. */
#include "noise_kk.h"

/* Bit in the CALL/ACCEPT framer extension field (bits 0-1 are the BW token). */
#define ARQ_CONNECT_EXT_BW_MASK   0x03
#define ARQ_CONNECT_EXT_CRYPTO    0x04

#define ARQ_CRYPTO_PROLOGUE_TAG   "hermes-mercury-arq-v1"

#define ARQ_CRYPTO_KEYLEN         32
#define ARQ_CRYPTO_TAGLEN         16
#define ARQ_CRYPTO_HS_MSG_LEN     48    /* 32 B ephemeral + 16 B tag */

#define ARQ_CRYPTO_REC_MIN        256
#define ARQ_CRYPTO_REC_MAX        4096
#define ARQ_CRYPTO_REC_FRAMES     8
#define ARQ_CRYPTO_REC_HDR_MAX    2
#define ARQ_CRYPTO_REC_OVERHEAD   (ARQ_CRYPTO_REC_HDR_MAX + ARQ_CRYPTO_TAGLEN)

#define ARQ_CRYPTO_FP_HEXLEN      16    /* first 8 bytes of SHA-256, in hex */

typedef enum {
    ARQ_CRYPTO_OFF      = 0,
    ARQ_CRYPTO_OPTIONAL = 1,
    ARQ_CRYPTO_REQUIRED = 2,
} arq_crypto_mode_t;

/* ---- process-wide configuration -------------------------------------- */

/* True when this build carries a crypto backend (libsodium). */
bool arq_crypto_available(void);

/* Load the station key and remember where peer keys live.  With mode != off
 * this fails -- and Mercury should refuse to start -- when the build has no
 * backend or the station key cannot be read: an operator who asked for
 * encryption must not silently get cleartext.  0 on success. */
int arq_crypto_configure(arq_crypto_mode_t mode, const char *key_file,
                         const char *peers_dir);

arq_crypto_mode_t arq_crypto_mode(void);
const char *arq_crypto_mode_name(arq_crypto_mode_t mode);
int arq_crypto_mode_from_name(const char *name, arq_crypto_mode_t *out);

/* Per-client override (TNC "ENCRYPT OFF"): for clients that already encrypt
 * end to end, such as NNCP, to save the handshake and the record overhead.
 * Refused -- returns -1 -- in required mode.  Cleared when the client
 * reconnects. */
int  arq_crypto_set_client_off(bool off);
bool arq_crypto_client_off(void);

/* Look up peers/<CALLSIGN>.pub.  The callsign comes from the air, unverified,
 * so only [A-Z0-9-] is ever turned into a path.  0 when found. */
int arq_crypto_peer_key(const char *callsign, uint8_t pub_out[ARQ_CRYPTO_KEYLEN]);

/* ---- negotiation ------------------------------------------------------ */

/* Initiator: should our CALL carry the offer bit? */
bool arq_crypto_should_offer(const char *callee);

/* Initiator, before calling: false when required mode cannot be satisfied
 * (no key for the callee, or the client asked for ENCRYPT OFF). */
bool arq_crypto_may_call(const char *callee, const char **why);

typedef enum {
    ARQ_CRYPTO_ANSWER_CLEAR = 0,     /* ACCEPT without the bit */
    ARQ_CRYPTO_ANSWER_SECURE,        /* ACCEPT with the bit */
    ARQ_CRYPTO_ANSWER_REFUSE,        /* do not answer at all (required mode) */
} arq_crypto_answer_t;

/* Responder: what to do with a CALL that did or did not offer crypto. */
arq_crypto_answer_t arq_crypto_answer_call(const char *caller, bool offered);

/* ---- one session ------------------------------------------------------ */

typedef enum {
    ARQ_XS_IDLE = 0,       /* no session */
    ARQ_XS_CLEAR,          /* session without encryption: bytes pass through */
    ARQ_XS_HANDSHAKE,      /* waiting for the peer's handshake message */
    ARQ_XS_SECURE,         /* transport keys in place */
    ARQ_XS_FAILED,         /* authentication failed: drop everything */
} arq_xs_state_t;

typedef enum {
    ARQ_XS_EV_NONE = 0,
    ARQ_XS_EV_SECURE,      /* handshake completed: report CONNECTED + ENCRYPTED */
    ARQ_XS_EV_FAILED,      /* disconnect now, deliver nothing more */
} arq_xs_event_t;

/* The parameters both sides bind into the handshake. */
typedef struct {
    bool        initiator;
    const char *initiator_call;
    const char *responder_call;
    uint8_t     bw_token;
    uint8_t     session_id;
    bool        offer_bit;
    bool        accept_bit;
} arq_xs_params_t;

typedef struct {
    arq_xs_state_t state;
    bool           initiator;
    char           fingerprint[ARQ_CRYPTO_FP_HEXLEN + 1];

    noise_kk_t     hs;
    noise_cipher_t tx;
    noise_cipher_t rx;

    /* This side's handshake message, sent ahead of any client data. */
    uint8_t        hs_out[ARQ_CRYPTO_HS_MSG_LEN];
    size_t         hs_out_len;
    size_t         hs_out_off;

    /* The peer's handshake message, as it arrives. */
    uint8_t        hs_in[ARQ_CRYPTO_HS_MSG_LEN];
    size_t         hs_in_len;

    /* The current sealed record, not yet all handed to ARQ. */
    uint8_t        sealed[ARQ_CRYPTO_REC_OVERHEAD + ARQ_CRYPTO_REC_MAX];
    size_t         sealed_len;
    size_t         sealed_off;

    /* The inbound record being reassembled. */
    uint8_t        rbuf[ARQ_CRYPTO_REC_OVERHEAD + ARQ_CRYPTO_REC_MAX];
    size_t         rlen;
    size_t         rneed;      /* total bytes of the current record, 0 = unknown */
} arq_xs_t;

/* A session without encryption. */
void arq_xs_begin_clear(arq_xs_t *x);

/* A session with encryption.  Uses the station key; the initiator's
 * handshake message is ready to send on return.  0 on success. */
int arq_xs_begin_secure(arq_xs_t *x, const arq_xs_params_t *p,
                        const uint8_t peer_pub[ARQ_CRYPTO_KEYLEN]);

/* Same, with explicit keys -- for tests. */
int arq_xs_begin_secure_with_keys(arq_xs_t *x, const arq_xs_params_t *p,
                                  const uint8_t self_priv[ARQ_CRYPTO_KEYLEN],
                                  const uint8_t peer_pub[ARQ_CRYPTO_KEYLEN]);

/* Drop the session: wipes keys, and any partial inbound record goes with it
 * -- its plaintext prefix is never delivered. */
void arq_xs_end(arq_xs_t *x);

/* Callback the session uses to take plaintext from the client queue when it
 * seals a record.  Returns the bytes copied, 0 when the queue is empty. */
typedef size_t (*arq_xs_pull_fn)(uint8_t *dst, size_t max, void *ctx);

/* Bytes this session could hand ARQ right now, given how much plaintext is
 * waiting in the client queue.  Never counts client bytes before the session
 * is SECURE: nothing leaves in the clear during the handshake. */
size_t arq_xs_tx_pending(const arq_xs_t *x, size_t plaintext_waiting);

/* Fill up to `len` stream bytes for ARQ.  `frame_payload` is the current
 * mode's frame size, which sets the record size.  Returns the bytes written. */
size_t arq_xs_tx_read(arq_xs_t *x, uint8_t *buf, size_t len, size_t frame_payload,
                      arq_xs_pull_fn pull, void *ctx);

/* Consume stream bytes from ARQ.  Plaintext goes to `out`, which must hold
 * in_len + ARQ_CRYPTO_REC_MAX bytes.  Returns the plaintext length and sets
 * *ev when the handshake completes or authentication fails. */
size_t arq_xs_rx_feed(arq_xs_t *x, const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_cap, arq_xs_event_t *ev);

/* ---- helpers, exposed for tests ---------------------------------------- */

size_t arq_crypto_record_cap(size_t frame_payload);
size_t arq_crypto_varint_put(size_t v, uint8_t out[ARQ_CRYPTO_REC_HDR_MAX]);
/* Returns the header length, 0 if more bytes are needed, -1 if invalid. */
int    arq_crypto_varint_get(const uint8_t *in, size_t in_len, size_t *v_out);
size_t arq_crypto_prologue(const arq_xs_params_t *p, uint8_t *out, size_t cap);
void   arq_crypto_fingerprint(const uint8_t pub[ARQ_CRYPTO_KEYLEN],
                              char out[ARQ_CRYPTO_FP_HEXLEN + 1]);

#endif /* ARQ_CRYPTO_H_ */
