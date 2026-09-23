/* Optional encryption of ARQ sessions
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See arq_crypto.h for the wire format and for why the record layer sits
 * where it does.
 */

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "arq_crypto.h"
#include "hermes_log.h"

#if defined(ARQ_HAVE_CRYPTO)
#include <sodium.h>
#endif

#define LOG_COMP "crypto"

static arq_crypto_mode_t s_mode = ARQ_CRYPTO_OFF;
/* Set from the TNC thread, read on the ARQ threads. */
static atomic_bool       s_client_off = false;
static bool              s_have_station_key = false;
static uint8_t           s_station_priv[ARQ_CRYPTO_KEYLEN];
static char              s_peers_dir[512];

/* ---- small helpers ---------------------------------------------------- */

static void wipe(void *p, size_t n)
{
#if defined(ARQ_HAVE_CRYPTO)
    sodium_memzero(p, n);
#else
    volatile uint8_t *v = (volatile uint8_t *) p;
    while (n--) *v++ = 0;
#endif
}

/* Read a file that must be exactly `len` bytes. */
static int read_exact_file(const char *path, uint8_t *out, size_t len)
{
    FILE *f;
    size_t got;
    int extra;

    if (path == NULL || path[0] == '\0')
        return -1;

    f = fopen(path, "rb");
    if (f == NULL)
        return -1;

    got = fread(out, 1, len, f);
    extra = fgetc(f);
    fclose(f);

    /* Too short or too long is a malformed key, not a truncated one. */
    return (got == len && extra == EOF) ? 0 : -1;
}

/* ---- configuration ---------------------------------------------------- */

bool arq_crypto_available(void)
{
#if defined(ARQ_HAVE_CRYPTO)
    return true;
#else
    return false;
#endif
}

const char *arq_crypto_mode_name(arq_crypto_mode_t mode)
{
    switch (mode)
    {
    case ARQ_CRYPTO_OPTIONAL: return "optional";
    case ARQ_CRYPTO_REQUIRED: return "required";
    default:                  return "off";
    }
}

int arq_crypto_mode_from_name(const char *name, arq_crypto_mode_t *out)
{
    if (name == NULL || out == NULL)
        return -1;
    if (strcmp(name, "off") == 0)      { *out = ARQ_CRYPTO_OFF;      return 0; }
    if (strcmp(name, "optional") == 0) { *out = ARQ_CRYPTO_OPTIONAL; return 0; }
    if (strcmp(name, "required") == 0) { *out = ARQ_CRYPTO_REQUIRED; return 0; }
    return -1;
}

arq_crypto_mode_t arq_crypto_mode(void)
{
    return s_mode;
}

int arq_crypto_configure(arq_crypto_mode_t mode, const char *key_file,
                         const char *peers_dir)
{
    wipe(s_station_priv, sizeof(s_station_priv));
    s_have_station_key = false;
    atomic_store(&s_client_off, false);
    s_mode = mode;
    snprintf(s_peers_dir, sizeof(s_peers_dir), "%s", peers_dir ? peers_dir : "");

    if (mode == ARQ_CRYPTO_OFF)
        return 0;

    if (!arq_crypto_available())
    {
        HLOGE(LOG_COMP, "[crypto] mode = %s, but this build has no crypto "
              "backend (libsodium); refusing to run unencrypted",
              arq_crypto_mode_name(mode));
        return -1;
    }

#if defined(ARQ_HAVE_CRYPTO)
    if (noise_init() != 0)
    {
        HLOGE(LOG_COMP, "libsodium failed to initialise");
        return -1;
    }
#endif

    if (read_exact_file(key_file, s_station_priv, sizeof(s_station_priv)) != 0)
    {
        HLOGE(LOG_COMP, "cannot read the station key from \"%s\" "
              "(it must be exactly 32 bytes)", key_file ? key_file : "");
        return -1;
    }

#if !defined(_WIN32)
    {
        struct stat st;

        /* The same hygiene ssh applies to its private keys, as a warning:
         * the installer writes 0600, so anything else is worth a look. */
        if (stat(key_file, &st) == 0 && (st.st_mode & 077) != 0)
            HLOGW(LOG_COMP, "station key \"%s\" is readable by other users "
                  "(mode %03o); it should be 0600", key_file,
                  (unsigned) (st.st_mode & 0777));
    }
#endif

    s_have_station_key = true;
    HLOGI(LOG_COMP, "ARQ encryption %s (Noise_KK_25519_ChaChaPoly_SHA256), "
          "peer keys in \"%s\"", arq_crypto_mode_name(mode), s_peers_dir);
    return 0;
}

int arq_crypto_set_client_off(bool off)
{
    if (off && s_mode == ARQ_CRYPTO_REQUIRED)
        return -1;
    atomic_store(&s_client_off, off);
    return 0;
}

bool arq_crypto_client_off(void)
{
    return atomic_load(&s_client_off);
}

int arq_crypto_peer_key(const char *callsign, uint8_t pub_out[ARQ_CRYPTO_KEYLEN])
{
    char call[20];
    char path[600];
    size_t i, n;

    if (callsign == NULL || pub_out == NULL || s_peers_dir[0] == '\0')
        return -1;

    /* This callsign may have come off the air unauthenticated.  Only letters,
     * digits and '-' ever reach the filesystem, so it cannot name anything
     * outside peers_dir. */
    n = strlen(callsign);
    if (n == 0 || n >= sizeof(call) - 1)
        return -1;
    for (i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char) callsign[i];

        if (!(isalnum(c) || c == '-'))
            return -1;
        call[i] = (char) toupper(c);
    }
    call[n] = '\0';

    snprintf(path, sizeof(path), "%s/%s.pub", s_peers_dir, call);
    return read_exact_file(path, pub_out, ARQ_CRYPTO_KEYLEN);
}

/* ---- negotiation ------------------------------------------------------- */

static bool can_secure_with(const char *peer)
{
    uint8_t pub[ARQ_CRYPTO_KEYLEN];
    bool ok;

    if (s_mode == ARQ_CRYPTO_OFF || !arq_crypto_available() ||
        !s_have_station_key || atomic_load(&s_client_off))
        return false;

    ok = (arq_crypto_peer_key(peer, pub) == 0);
    wipe(pub, sizeof(pub));
    return ok;
}

bool arq_crypto_should_offer(const char *callee)
{
    return can_secure_with(callee);
}

bool arq_crypto_may_call(const char *callee, const char **why)
{
    if (s_mode != ARQ_CRYPTO_REQUIRED)
        return true;

    if (!can_secure_with(callee))
    {
        if (why != NULL)
            *why = "encryption is required and there is no key for this station";
        return false;
    }
    return true;
}

arq_crypto_answer_t arq_crypto_answer_call(const char *caller, bool offered)
{
    if (offered && can_secure_with(caller))
        return ARQ_CRYPTO_ANSWER_SECURE;

    /* Required mode never lets a session go clear.  Not answering is the
     * refusal: there is no session yet to disconnect. */
    if (s_mode == ARQ_CRYPTO_REQUIRED)
        return ARQ_CRYPTO_ANSWER_REFUSE;

    return ARQ_CRYPTO_ANSWER_CLEAR;
}

/* ---- helpers ------------------------------------------------------------ */

size_t arq_crypto_record_cap(size_t frame_payload)
{
    size_t cap = frame_payload * ARQ_CRYPTO_REC_FRAMES;

    if (cap < ARQ_CRYPTO_REC_MIN)
        cap = ARQ_CRYPTO_REC_MIN;
    if (cap > ARQ_CRYPTO_REC_MAX)
        cap = ARQ_CRYPTO_REC_MAX;
    return cap;
}

size_t arq_crypto_varint_put(size_t v, uint8_t out[ARQ_CRYPTO_REC_HDR_MAX])
{
    if (v < 0x80)
    {
        out[0] = (uint8_t) v;
        return 1;
    }
    out[0] = (uint8_t) (0x80 | (v & 0x7F));
    out[1] = (uint8_t) (v >> 7);
    return 2;
}

int arq_crypto_varint_get(const uint8_t *in, size_t in_len, size_t *v_out)
{
    size_t v;

    if (in_len < 1)
        return 0;
    if ((in[0] & 0x80) == 0)
    {
        *v_out = in[0];
        return 1;
    }
    if (in_len < 2)
        return 0;
    if ((in[1] & 0x80) != 0)
        return -1;                       /* no third byte: records are small */
    v = (size_t) (in[0] & 0x7F) | ((size_t) in[1] << 7);
    if (v < 0x80)
        return -1;                       /* not the minimal encoding */
    *v_out = v;
    return 2;
}

size_t arq_crypto_prologue(const arq_xs_params_t *p, uint8_t *out, size_t cap)
{
    size_t n = 0;
    size_t tag_len = strlen(ARQ_CRYPTO_PROLOGUE_TAG);
    size_t ic = strlen(p->initiator_call);
    size_t rc = strlen(p->responder_call);

    if (cap < tag_len + 2 + ic + rc + 4 || ic > 255 || rc > 255)
        return 0;

    /* Length-prefixed callsigns, so "AB"+"C" and "A"+"BC" cannot collide.
     * Uppercased, because that is how they travel: the CALL frame's encoder
     * uppercases the source callsign, so a station configured as "pu2uit-2"
     * is "PU2UIT-2" to its peer -- and a case difference here would fail
     * every handshake. */
    memcpy(out + n, ARQ_CRYPTO_PROLOGUE_TAG, tag_len);  n += tag_len;
    out[n++] = (uint8_t) ic;
    for (size_t i = 0; i < ic; i++)
        out[n++] = (uint8_t) toupper((unsigned char) p->initiator_call[i]);
    out[n++] = (uint8_t) rc;
    for (size_t i = 0; i < rc; i++)
        out[n++] = (uint8_t) toupper((unsigned char) p->responder_call[i]);
    out[n++] = p->bw_token;
    out[n++] = p->session_id;
    /* The literal negotiation bits, even though a handshake only runs when
     * both are set: it keeps the transcript unambiguous if that field ever
     * grows more meanings. */
    out[n++] = p->offer_bit ? 1 : 0;
    out[n++] = p->accept_bit ? 1 : 0;
    return n;
}

void arq_crypto_fingerprint(const uint8_t pub[ARQ_CRYPTO_KEYLEN],
                            char out[ARQ_CRYPTO_FP_HEXLEN + 1])
{
#if defined(ARQ_HAVE_CRYPTO)
    uint8_t digest[crypto_hash_sha256_BYTES];
    int i;

    crypto_hash_sha256(digest, pub, ARQ_CRYPTO_KEYLEN);
    for (i = 0; i < ARQ_CRYPTO_FP_HEXLEN / 2; i++)
        snprintf(out + 2 * i, 3, "%02x", digest[i]);
#else
    (void) pub;
    out[0] = '\0';
#endif
}

/* ---- sessions ------------------------------------------------------------ */

void arq_xs_begin_clear(arq_xs_t *x)
{
    arq_xs_end(x);
    x->state = ARQ_XS_CLEAR;
}

void arq_xs_end(arq_xs_t *x)
{
    if (x == NULL)
        return;
    /* Keys, the partial inbound record and any unsent sealed bytes all go:
     * a record cut off by a disconnect never delivers its plaintext prefix. */
    wipe(x, sizeof(*x));
    x->state = ARQ_XS_IDLE;
}

static void xs_fail(arq_xs_t *x, arq_xs_event_t *ev)
{
    arq_xs_end(x);
    x->state = ARQ_XS_FAILED;
    if (ev != NULL)
        *ev = ARQ_XS_EV_FAILED;
}

int arq_xs_begin_secure_with_keys(arq_xs_t *x, const arq_xs_params_t *p,
                                  const uint8_t self_priv[ARQ_CRYPTO_KEYLEN],
                                  const uint8_t peer_pub[ARQ_CRYPTO_KEYLEN])
{
#if defined(ARQ_HAVE_CRYPTO)
    uint8_t prologue[128];
    size_t plen;
    int n;

    if (x == NULL || p == NULL || self_priv == NULL || peer_pub == NULL)
        return -1;

    arq_xs_end(x);
    x->initiator = p->initiator;
    arq_crypto_fingerprint(peer_pub, x->fingerprint);

    plen = arq_crypto_prologue(p, prologue, sizeof(prologue));
    if (plen == 0 ||
        noise_kk_init(&x->hs, p->initiator, prologue, plen, self_priv, peer_pub) != 0)
    {
        xs_fail(x, NULL);
        return -1;
    }

    if (p->initiator)
    {
        /* Handshake payloads stay empty: KK's first message offers only weak
         * forward secrecy, so nothing is sent in it. */
        n = noise_kk_write(&x->hs, NULL, 0, x->hs_out, sizeof(x->hs_out));
        if (n != ARQ_CRYPTO_HS_MSG_LEN)
        {
            xs_fail(x, NULL);
            return -1;
        }
        x->hs_out_len = (size_t) n;
    }

    x->state = ARQ_XS_HANDSHAKE;
    return 0;
#else
    (void) p; (void) self_priv; (void) peer_pub;
    if (x != NULL)
        xs_fail(x, NULL);
    return -1;
#endif
}

int arq_xs_begin_secure(arq_xs_t *x, const arq_xs_params_t *p,
                        const uint8_t peer_pub[ARQ_CRYPTO_KEYLEN])
{
    if (!s_have_station_key)
    {
        if (x != NULL)
            xs_fail(x, NULL);
        return -1;
    }
    return arq_xs_begin_secure_with_keys(x, p, s_station_priv, peer_pub);
}

size_t arq_xs_tx_pending(const arq_xs_t *x, size_t plaintext_waiting)
{
    size_t n;

    if (x == NULL)
        return 0;

    switch (x->state)
    {
    case ARQ_XS_CLEAR:
        return plaintext_waiting;
    case ARQ_XS_HANDSHAKE:
        /* Only our own handshake message: client bytes wait for SECURE. */
        return x->hs_out_len - x->hs_out_off;
    case ARQ_XS_SECURE:
        n = (x->hs_out_len - x->hs_out_off) + (x->sealed_len - x->sealed_off);
        if (plaintext_waiting > 0)
            n += plaintext_waiting + ARQ_CRYPTO_REC_OVERHEAD;
        return n;
    default:
        return 0;
    }
}

size_t arq_xs_tx_read(arq_xs_t *x, uint8_t *buf, size_t len, size_t frame_payload,
                      arq_xs_pull_fn pull, void *ctx)
{
    size_t n = 0;

    if (x == NULL || buf == NULL || len == 0)
        return 0;

    if (x->state == ARQ_XS_CLEAR)
        return pull != NULL ? pull(buf, len, ctx) : 0;

    /* Our handshake message goes first, ahead of anything else. */
    while (n < len && x->hs_out_off < x->hs_out_len)
        buf[n++] = x->hs_out[x->hs_out_off++];

    if (x->state != ARQ_XS_SECURE)
        return n;

#if defined(ARQ_HAVE_CRYPTO)
    while (n < len)
    {
        uint8_t pt[ARQ_CRYPTO_REC_MAX];
        size_t cap, got, hdr;
        int ct;

        if (x->sealed_off < x->sealed_len)
        {
            size_t take = x->sealed_len - x->sealed_off;

            if (take > len - n)
                take = len - n;
            memcpy(buf + n, x->sealed + x->sealed_off, take);
            x->sealed_off += take;
            n += take;
            continue;
        }

        /* Seal as late as possible and as much as is queued: this is the
         * moment ARQ is about to transmit, so whatever the client has written
         * so far goes into one record. */
        if (pull == NULL)
            break;
        cap = arq_crypto_record_cap(frame_payload);
        got = pull(pt, cap, ctx);
        if (got == 0)
            break;

        hdr = arq_crypto_varint_put(got, x->sealed);
        ct = noise_cipher_encrypt(&x->tx, x->sealed, hdr, pt, got, x->sealed + hdr);
        wipe(pt, got);
        if (ct < 0)
        {
            /* Only reachable at counter exhaustion.  Fail closed. */
            xs_fail(x, NULL);
            break;
        }
        x->sealed_len = hdr + (size_t) ct;
        x->sealed_off = 0;
    }
#else
    (void) frame_payload; (void) ctx;
#endif

    return n;
}

size_t arq_xs_rx_feed(arq_xs_t *x, const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_cap, arq_xs_event_t *ev)
{
    size_t produced = 0;
    arq_xs_event_t local_ev = ARQ_XS_EV_NONE;

    if (ev == NULL)
        ev = &local_ev;
    *ev = ARQ_XS_EV_NONE;

    if (x == NULL || in == NULL || out == NULL)
        return 0;

    if (x->state == ARQ_XS_CLEAR)
    {
        size_t take = in_len < out_cap ? in_len : out_cap;

        memcpy(out, in, take);
        return take;
    }

#if defined(ARQ_HAVE_CRYPTO)
    if (x->state == ARQ_XS_HANDSHAKE)
    {
        size_t take = ARQ_CRYPTO_HS_MSG_LEN - x->hs_in_len;
        uint8_t no_payload[1];

        if (take > in_len)
            take = in_len;
        memcpy(x->hs_in + x->hs_in_len, in, take);
        x->hs_in_len += take;
        in += take;
        in_len -= take;

        if (x->hs_in_len < ARQ_CRYPTO_HS_MSG_LEN)
            return 0;

        /* A zero-capacity payload buffer is the "payloads stay empty" rule
         * enforced: a peer message carrying one fails to read. */
        if (noise_kk_read(&x->hs, x->hs_in, ARQ_CRYPTO_HS_MSG_LEN,
                          no_payload, 0) != 0)
        {
            HLOGW(LOG_COMP, "handshake failed: wrong key, tampered or replayed");
            xs_fail(x, ev);
            return 0;
        }

        if (!x->initiator)
        {
            int n = noise_kk_write(&x->hs, NULL, 0, x->hs_out, sizeof(x->hs_out));

            if (n != ARQ_CRYPTO_HS_MSG_LEN)
            {
                xs_fail(x, ev);
                return 0;
            }
            x->hs_out_len = (size_t) n;
            x->hs_out_off = 0;

            /* The initiator cannot send records before it has our reply, so
             * anything after its handshake message is a protocol violation. */
            if (in_len > 0)
            {
                HLOGW(LOG_COMP, "data after the handshake message, before the reply");
                xs_fail(x, ev);
                return 0;
            }
        }

        if (noise_kk_split(&x->hs, &x->tx, &x->rx) != 0)
        {
            xs_fail(x, ev);
            return 0;
        }
        noise_kk_wipe(&x->hs);
        x->state = ARQ_XS_SECURE;
        *ev = ARQ_XS_EV_SECURE;
    }

    while (x->state == ARQ_XS_SECURE && in_len > 0)
    {
        size_t v = 0;
        int hdr;

        if (x->rneed == 0)
        {
            /* Collect the varint header a byte at a time. */
            x->rbuf[x->rlen++] = *in++;
            in_len--;

            hdr = arq_crypto_varint_get(x->rbuf, x->rlen, &v);
            if (hdr == 0)
                continue;
            /* Refuse an impossible length before buffering a single body
             * byte for it: it can only be damage or an attack. */
            if (hdr < 0 || v == 0 || v > ARQ_CRYPTO_REC_MAX)
            {
                HLOGW(LOG_COMP, "invalid record length; ending the session");
                xs_fail(x, ev);
                return produced;
            }
            x->rneed = (size_t) hdr + v + ARQ_CRYPTO_TAGLEN;
            continue;
        }

        {
            size_t take = x->rneed - x->rlen;

            if (take > in_len)
                take = in_len;
            memcpy(x->rbuf + x->rlen, in, take);
            x->rlen += take;
            in += take;
            in_len -= take;
        }

        if (x->rlen == x->rneed)
        {
            int pt;

            hdr = arq_crypto_varint_get(x->rbuf, x->rlen, &v);
            if (hdr <= 0 || produced + v > out_cap)
            {
                xs_fail(x, ev);
                return produced;
            }

            pt = noise_cipher_decrypt(&x->rx, x->rbuf, (size_t) hdr,
                                      x->rbuf + hdr, v + ARQ_CRYPTO_TAGLEN,
                                      out + produced);
            if (pt < 0)
            {
                /* Fail closed.  Records already opened in this call passed
                 * their own tags and stay delivered; this one never is. */
                HLOGW(LOG_COMP, "record failed authentication; ending the session");
                xs_fail(x, ev);
                return produced;
            }
            produced += (size_t) pt;
            wipe(x->rbuf, x->rlen);
            x->rlen = 0;
            x->rneed = 0;
        }
    }
#else
    (void) out_cap;
#endif

    return produced;
}
