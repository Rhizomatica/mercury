/* ARQ session encryption tests
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two sessions talk to each other over a byte stream, cut into arbitrary
 * chunks the way ARQ frames would cut it.  No radio, no ARQ: this pins the
 * record layer, the handshake sequencing, the negotiation rules and the
 * airtime overhead, all of which the protocol depends on.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unity.h"
#include "arq_crypto.h"
#include "noise_kk.h"

void setUp(void) { TEST_ASSERT_EQUAL_INT(0, noise_init()); }
void tearDown(void) {}

/* ---- a client TX queue for the pull callback -------------------------- */

typedef struct {
    uint8_t data[1 << 17];
    size_t  len, off;
} queue_t;

static size_t q_pull(uint8_t *dst, size_t max, void *ctx)
{
    queue_t *q = (queue_t *) ctx;
    size_t n = q->len - q->off;

    if (n > max)
        n = max;
    memcpy(dst, q->data + q->off, n);
    q->off += n;
    return n;
}

static void q_put(queue_t *q, const uint8_t *d, size_t n)
{
    memcpy(q->data + q->len, d, n);
    q->len += n;
}

/* ---- two endpoints ----------------------------------------------------- */

typedef struct {
    arq_xs_t x;
    queue_t  q;                     /* plaintext the client wants to send */
    uint8_t  got[1 << 17];          /* plaintext delivered to the client */
    size_t   got_len;
    int      secure_events;
    int      failed_events;
} end_t;

static uint8_t I_PRIV[32], I_PUB[32], R_PRIV[32], R_PUB[32];

static void keys(void)
{
    memset(I_PRIV, 0x41, 32);
    memset(R_PRIV, 0x52, 32);
    TEST_ASSERT_EQUAL_INT(0, noise_pubkey(I_PRIV, I_PUB));
    TEST_ASSERT_EQUAL_INT(0, noise_pubkey(R_PRIV, R_PUB));
}

static arq_xs_params_t params(bool initiator)
{
    arq_xs_params_t p;

    p.initiator = initiator;
    p.initiator_call = "PU2UIT-2";
    p.responder_call = "PU2UIT-3";
    p.bw_token = 2;
    p.session_id = 0x2a;
    p.offer_bit = true;
    p.accept_bit = true;
    return p;
}

static end_t *new_end(void)
{
    end_t *e = (end_t *) calloc(1, sizeof(end_t));

    TEST_ASSERT_NOT_NULL(e);
    return e;
}

static void start_pair(end_t *ini, end_t *res)
{
    arq_xs_params_t pi = params(true), pr = params(false);

    keys();
    TEST_ASSERT_EQUAL_INT(0, arq_xs_begin_secure_with_keys(&ini->x, &pi, I_PRIV, R_PUB));
    TEST_ASSERT_EQUAL_INT(0, arq_xs_begin_secure_with_keys(&res->x, &pr, R_PRIV, I_PUB));
}

/* Carry what `from` will send to `to`, in chunks of `chunk` bytes, as ARQ
 * frames would.  Returns the bytes that went "on air". */
static size_t carry(end_t *from, end_t *to, size_t chunk, size_t frame_payload)
{
    uint8_t buf[4096];
    uint8_t out[4096 + ARQ_CRYPTO_REC_MAX];
    size_t on_air = 0;

    for (;;)
    {
        size_t n = arq_xs_tx_read(&from->x, buf, chunk, frame_payload, q_pull, &from->q);
        arq_xs_event_t ev;
        size_t p;

        if (n == 0)
            break;
        on_air += n;
        p = arq_xs_rx_feed(&to->x, buf, n, out, sizeof(out), &ev);
        memcpy(to->got + to->got_len, out, p);
        to->got_len += p;
        if (ev == ARQ_XS_EV_SECURE) to->secure_events++;
        if (ev == ARQ_XS_EV_FAILED) to->failed_events++;
    }
    return on_air;
}

static void handshake(end_t *ini, end_t *res)
{
    TEST_ASSERT_EQUAL_UINT(48, carry(ini, res, 7, 30));    /* msg1 */
    TEST_ASSERT_EQUAL_INT(1, res->secure_events);
    TEST_ASSERT_EQUAL_UINT(48, carry(res, ini, 11, 30));   /* msg2 */
    TEST_ASSERT_EQUAL_INT(1, ini->secure_events);
    TEST_ASSERT_EQUAL_INT(ARQ_XS_SECURE, ini->x.state);
    TEST_ASSERT_EQUAL_INT(ARQ_XS_SECURE, res->x.state);
}

static void fill(uint8_t *p, size_t n, unsigned seed)
{
    size_t i;

    for (i = 0; i < n; i++)
    {
        seed = seed * 1103515245u + 12345u;
        p[i] = (uint8_t) (seed >> 16);
    }
}

/* ---- the handshake ----------------------------------------------------- */

/* Nothing but the 48-byte handshake message leaves before the session is
 * secure -- not even if the client has already queued data. */
static void test_no_client_byte_leaves_before_the_handshake_completes(void)
{
    end_t *ini = new_end(), *res = new_end();
    uint8_t buf[512];

    start_pair(ini, res);
    q_put(&ini->q, (const uint8_t *) "secret", 6);

    TEST_ASSERT_EQUAL_UINT(48, arq_xs_tx_pending(&ini->x, 6));
    TEST_ASSERT_EQUAL_UINT(48, arq_xs_tx_read(&ini->x, buf, sizeof(buf), 510, q_pull, &ini->q));
    TEST_ASSERT_EQUAL_UINT(0, arq_xs_tx_read(&ini->x, buf, sizeof(buf), 510, q_pull, &ini->q));
    TEST_ASSERT_EQUAL_UINT(0, ini->q.off);            /* untouched */
    TEST_ASSERT_NULL(memmem(buf, 48, "secret", 6));
    free(ini); free(res);
}

static void test_both_sides_agree_and_carry_data_both_ways(void)
{
    end_t *ini = new_end(), *res = new_end();
    static uint8_t a[9000], b[5000];

    start_pair(ini, res);
    handshake(ini, res);

    fill(a, sizeof(a), 1);
    fill(b, sizeof(b), 2);
    q_put(&ini->q, a, sizeof(a));
    q_put(&res->q, b, sizeof(b));

    carry(ini, res, 97, 126);
    carry(res, ini, 13, 30);

    TEST_ASSERT_EQUAL_UINT(sizeof(a), res->got_len);
    TEST_ASSERT_EQUAL_MEMORY(a, res->got, sizeof(a));
    TEST_ASSERT_EQUAL_UINT(sizeof(b), ini->got_len);
    TEST_ASSERT_EQUAL_MEMORY(b, ini->got, sizeof(b));
    TEST_ASSERT_EQUAL_STRING(res->x.fingerprint[0] ? res->x.fingerprint : "x",
                             res->x.fingerprint);
    TEST_ASSERT_EQUAL_UINT(ARQ_CRYPTO_FP_HEXLEN, strlen(ini->x.fingerprint));
    free(ini); free(res);
}

/* ARQ cuts the stream wherever a frame ends.  Every cut, down to one byte at
 * a time, must reassemble to the same plaintext. */
static void test_any_chunking_of_the_stream_reassembles(void)
{
    static const size_t chunks[] = { 1, 2, 3, 17, 18, 19, 255, 1205 };
    size_t c;

    for (c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++)
    {
        end_t *ini = new_end(), *res = new_end();
        static uint8_t a[3000];

        start_pair(ini, res);
        handshake(ini, res);
        fill(a, sizeof(a), (unsigned) c + 7);
        q_put(&ini->q, a, sizeof(a));
        carry(ini, res, chunks[c], 54);
        TEST_ASSERT_EQUAL_UINT(sizeof(a), res->got_len);
        TEST_ASSERT_EQUAL_MEMORY(a, res->got, sizeof(a));
        free(ini); free(res);
    }
}

/* ---- failing closed ------------------------------------------------------ */

static void test_wrong_key_fails_the_handshake(void)
{
    end_t *ini = new_end(), *res = new_end();
    arq_xs_params_t pi = params(true), pr = params(false);
    uint8_t other[32], other_pub[32];

    keys();
    memset(other, 0x77, 32);
    noise_pubkey(other, other_pub);
    arq_xs_begin_secure_with_keys(&ini->x, &pi, I_PRIV, R_PUB);
    /* The responder expects a different caller key. */
    arq_xs_begin_secure_with_keys(&res->x, &pr, R_PRIV, other_pub);

    carry(ini, res, 48, 30);
    TEST_ASSERT_EQUAL_INT(1, res->failed_events);
    TEST_ASSERT_EQUAL_INT(ARQ_XS_FAILED, res->x.state);
    TEST_ASSERT_EQUAL_UINT(0, arq_xs_tx_pending(&res->x, 100));
    free(ini); free(res);
}

/* A mismatch in anything bound into the prologue -- here the session id --
 * fails the handshake. */
static void test_prologue_mismatch_fails_the_handshake(void)
{
    end_t *ini = new_end(), *res = new_end();
    arq_xs_params_t pi = params(true), pr = params(false);

    keys();
    pr.session_id = 0x2b;
    arq_xs_begin_secure_with_keys(&ini->x, &pi, I_PRIV, R_PUB);
    arq_xs_begin_secure_with_keys(&res->x, &pr, R_PRIV, I_PUB);
    carry(ini, res, 48, 30);
    TEST_ASSERT_EQUAL_INT(1, res->failed_events);
    free(ini); free(res);
}

/* One flipped bit anywhere in a record ends the session, and that record's
 * plaintext is never delivered -- not even its prefix. */
static void test_tampered_record_is_never_delivered(void)
{
    end_t *ini = new_end(), *res = new_end();
    uint8_t buf[512], out[512 + ARQ_CRYPTO_REC_MAX];
    arq_xs_event_t ev;
    size_t n, p;

    start_pair(ini, res);
    handshake(ini, res);

    q_put(&ini->q, (const uint8_t *) "launch codes follow", 19);
    n = arq_xs_tx_read(&ini->x, buf, sizeof(buf), 510, q_pull, &ini->q);
    TEST_ASSERT_EQUAL_UINT(1 + 19 + 16, n);

    buf[5] ^= 0x01;
    p = arq_xs_rx_feed(&res->x, buf, n, out, sizeof(out), &ev);
    TEST_ASSERT_EQUAL_UINT(0, p);
    TEST_ASSERT_EQUAL_INT(ARQ_XS_EV_FAILED, ev);
    TEST_ASSERT_EQUAL_INT(ARQ_XS_FAILED, res->x.state);
    free(ini); free(res);
}

/* A length above the record limit is refused on the header alone, before a
 * body byte is buffered for it. */
static void test_oversized_length_is_refused_on_the_header(void)
{
    end_t *ini = new_end(), *res = new_end();
    uint8_t hdr[2], out[64];
    arq_xs_event_t ev;

    start_pair(ini, res);
    handshake(ini, res);

    arq_crypto_varint_put(ARQ_CRYPTO_REC_MAX + 1, hdr);
    arq_xs_rx_feed(&res->x, hdr, 2, out, sizeof(out), &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_XS_EV_FAILED, ev);
    free(ini); free(res);
}

/* The initiator cannot send records before it has the reply, so bytes glued
 * after its handshake message are a protocol violation. */
static void test_data_before_the_reply_is_refused(void)
{
    end_t *ini = new_end(), *res = new_end();
    uint8_t buf[64], out[64 + ARQ_CRYPTO_REC_MAX];
    arq_xs_event_t ev;

    start_pair(ini, res);
    arq_xs_tx_read(&ini->x, buf, 48, 30, q_pull, &ini->q);
    buf[48] = 0x05;
    arq_xs_rx_feed(&res->x, buf, 49, out, sizeof(out), &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_XS_EV_FAILED, ev);
    free(ini); free(res);
}

/* ---- overhead -------------------------------------------------------------- */

/* What encryption costs on air for a bulk transfer at each rung.  These are
 * the numbers the record-size policy was chosen for; if one moves, the
 * trade-off in arq_crypto.h needs re-examining. */
static void test_bulk_overhead_per_mode(void)
{
    static const struct { size_t frame; double max_pct; } modes[] = {
        {   30, 7.1 },   /* DATAC15 */
        {   54, 4.8 },   /* DATAC4  */
        {  126, 1.9 },   /* DATAC3: 18 B per 1008 B record, plus the last partial one */
        {  510, 0.5 },   /* DATAC1  */
        { 1180, 0.5 },   /* DATAC17 */
        { 1213, 0.5 },   /* QAM16C2 */
    };
    size_t m;

    for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++)
    {
        end_t *ini = new_end(), *res = new_end();
        static uint8_t a[64 * 1024];
        size_t wire;
        double pct;
        char msg[96];

        start_pair(ini, res);
        handshake(ini, res);
        fill(a, sizeof(a), (unsigned) m);
        q_put(&ini->q, a, sizeof(a));
        wire = carry(ini, res, modes[m].frame, modes[m].frame);
        TEST_ASSERT_EQUAL_UINT(sizeof(a), res->got_len);

        pct = 100.0 * (double) (wire - sizeof(a)) / (double) sizeof(a);
        snprintf(msg, sizeof(msg), "frame %zu B: %.2f%% overhead", modes[m].frame, pct);
        TEST_ASSERT_TRUE_MESSAGE(pct <= modes[m].max_pct, msg);
        printf("    %s\n", msg);
        free(ini); free(res);
    }
}

/* Lazy sealing: many small client writes queued before ARQ pulls cost one
 * record, not one each. */
static void test_small_writes_coalesce_into_one_record(void)
{
    end_t *ini = new_end(), *res = new_end();
    uint8_t buf[4096];
    size_t i, n;

    start_pair(ini, res);
    handshake(ini, res);
    for (i = 0; i < 20; i++)
        q_put(&ini->q, (const uint8_t *) "0123456789", 10);

    n = arq_xs_tx_read(&ini->x, buf, sizeof(buf), 510, q_pull, &ini->q);
    TEST_ASSERT_EQUAL_UINT(2 + 200 + 16, n);   /* one record */
    free(ini); free(res);
}

/* ---- helpers ------------------------------------------------------------ */

static void test_varint_is_minimal_and_bounded(void)
{
    uint8_t b[2];
    size_t v, len;

    for (v = 1; v <= 16383; v++)
    {
        size_t got = 0;

        len = arq_crypto_varint_put(v, b);
        TEST_ASSERT_EQUAL_INT((int) len, arq_crypto_varint_get(b, len, &got));
        TEST_ASSERT_EQUAL_UINT(v, got);
        TEST_ASSERT_EQUAL_UINT(v < 128 ? 1 : 2, len);
    }

    b[0] = 0x85; b[1] = 0x00;                  /* 5 encoded in two bytes */
    TEST_ASSERT_EQUAL_INT(-1, arq_crypto_varint_get(b, 2, &v));
    b[0] = 0x80; b[1] = 0x81;                  /* would need a third byte */
    TEST_ASSERT_EQUAL_INT(-1, arq_crypto_varint_get(b, 2, &v));
    b[0] = 0x80;
    TEST_ASSERT_EQUAL_INT(0, arq_crypto_varint_get(b, 1, &v));   /* need more */
}

static void test_record_size_follows_the_mode(void)
{
    TEST_ASSERT_EQUAL_UINT(ARQ_CRYPTO_REC_MIN, arq_crypto_record_cap(0));
    TEST_ASSERT_EQUAL_UINT(256,  arq_crypto_record_cap(30));
    TEST_ASSERT_EQUAL_UINT(432,  arq_crypto_record_cap(54));
    TEST_ASSERT_EQUAL_UINT(4080, arq_crypto_record_cap(510));
    TEST_ASSERT_EQUAL_UINT(ARQ_CRYPTO_REC_MAX, arq_crypto_record_cap(1213));
}

/* Swapping the callsigns, or moving a byte between them, must not produce the
 * same prologue. */
static void test_prologue_is_unambiguous(void)
{
    arq_xs_params_t a = params(true), b = params(true);
    uint8_t pa[128], pb[128];
    size_t la, lb;

    a.initiator_call = "AB"; a.responder_call = "C";
    b.initiator_call = "A";  b.responder_call = "BC";
    la = arq_crypto_prologue(&a, pa, sizeof(pa));
    lb = arq_crypto_prologue(&b, pb, sizeof(pb));
    TEST_ASSERT_TRUE(la != lb || memcmp(pa, pb, la) != 0);

    b = a;
    b.accept_bit = false;
    lb = arq_crypto_prologue(&b, pb, sizeof(pb));
    TEST_ASSERT_TRUE(memcmp(pa, pb, la) != 0);

    /* Case must NOT matter: the air uppercases callsigns, so a station
     * configured in lowercase has to bind the same prologue as its peer. */
    a = params(true);
    b = params(true);
    b.initiator_call = "pu2uit-2";
    la = arq_crypto_prologue(&a, pa, sizeof(pa));
    lb = arq_crypto_prologue(&b, pb, sizeof(pb));
    TEST_ASSERT_EQUAL_UINT(la, lb);
    TEST_ASSERT_EQUAL_MEMORY(pa, pb, la);
}

/* ---- negotiation --------------------------------------------------------- */

static char s_dir[256];

static void write_file(const char *path, const uint8_t *d, size_t n, int mode)
{
    FILE *f = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(f);
    fwrite(d, 1, n, f);
    fclose(f);
    chmod(path, (mode_t) mode);
}

static void make_keys_dir(void)
{
    char path[400];

    keys();
    snprintf(s_dir, sizeof(s_dir), "/tmp/mercury-crypto-test-%d", (int) getpid());
    mkdir(s_dir, 0700);
    snprintf(path, sizeof(path), "%s/peers", s_dir);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/station.key", s_dir);
    write_file(path, I_PRIV, 32, 0600);
    snprintf(path, sizeof(path), "%s/peers/PU2UIT-3.pub", s_dir);
    write_file(path, R_PUB, 32, 0644);
}

static void configure(arq_crypto_mode_t mode)
{
    char key[400], peers[400];

    snprintf(key, sizeof(key), "%s/station.key", s_dir);
    snprintf(peers, sizeof(peers), "%s/peers", s_dir);
    TEST_ASSERT_EQUAL_INT(0, arq_crypto_configure(mode, key, peers));
}

static void test_negotiation_follows_the_mode(void)
{
    make_keys_dir();

    configure(ARQ_CRYPTO_OFF);
    TEST_ASSERT_FALSE(arq_crypto_should_offer("PU2UIT-3"));
    TEST_ASSERT_EQUAL_INT(ARQ_CRYPTO_ANSWER_CLEAR, arq_crypto_answer_call("PU2UIT-3", true));

    configure(ARQ_CRYPTO_OPTIONAL);
    TEST_ASSERT_TRUE(arq_crypto_should_offer("PU2UIT-3"));
    TEST_ASSERT_TRUE(arq_crypto_should_offer("pu2uit-3"));        /* case-folded */
    TEST_ASSERT_FALSE(arq_crypto_should_offer("PU2UIT-9"));       /* no key */
    TEST_ASSERT_EQUAL_INT(ARQ_CRYPTO_ANSWER_SECURE, arq_crypto_answer_call("PU2UIT-3", true));
    TEST_ASSERT_EQUAL_INT(ARQ_CRYPTO_ANSWER_CLEAR,  arq_crypto_answer_call("PU2UIT-3", false));
    TEST_ASSERT_EQUAL_INT(ARQ_CRYPTO_ANSWER_CLEAR,  arq_crypto_answer_call("PU2UIT-9", true));
    TEST_ASSERT_TRUE(arq_crypto_may_call("PU2UIT-9", NULL));

    /* ENCRYPT OFF for clients that encrypt end to end. */
    TEST_ASSERT_EQUAL_INT(0, arq_crypto_set_client_off(true));
    TEST_ASSERT_FALSE(arq_crypto_should_offer("PU2UIT-3"));
    TEST_ASSERT_EQUAL_INT(ARQ_CRYPTO_ANSWER_CLEAR, arq_crypto_answer_call("PU2UIT-3", true));

    configure(ARQ_CRYPTO_REQUIRED);
    TEST_ASSERT_EQUAL_INT(-1, arq_crypto_set_client_off(true));  /* refused */
    TEST_ASSERT_TRUE(arq_crypto_should_offer("PU2UIT-3"));
    TEST_ASSERT_EQUAL_INT(ARQ_CRYPTO_ANSWER_SECURE, arq_crypto_answer_call("PU2UIT-3", true));
    TEST_ASSERT_EQUAL_INT(ARQ_CRYPTO_ANSWER_REFUSE, arq_crypto_answer_call("PU2UIT-3", false));
    TEST_ASSERT_EQUAL_INT(ARQ_CRYPTO_ANSWER_REFUSE, arq_crypto_answer_call("PU2UIT-9", true));
    TEST_ASSERT_FALSE(arq_crypto_may_call("PU2UIT-9", NULL));
    TEST_ASSERT_TRUE(arq_crypto_may_call("PU2UIT-3", NULL));
}

/* A callsign off the air must never become a path outside peers_dir. */
static void test_callsign_cannot_escape_the_peers_dir(void)
{
    uint8_t pub[32];

    make_keys_dir();
    configure(ARQ_CRYPTO_OPTIONAL);
    TEST_ASSERT_EQUAL_INT(0,  arq_crypto_peer_key("PU2UIT-3", pub));
    TEST_ASSERT_EQUAL_INT(-1, arq_crypto_peer_key("../station", pub));
    TEST_ASSERT_EQUAL_INT(-1, arq_crypto_peer_key("..", pub));
    TEST_ASSERT_EQUAL_INT(-1, arq_crypto_peer_key("A/B", pub));
    TEST_ASSERT_EQUAL_INT(-1, arq_crypto_peer_key("", pub));
}

static void test_missing_station_key_refuses_to_start(void)
{
    make_keys_dir();
    TEST_ASSERT_EQUAL_INT(-1, arq_crypto_configure(ARQ_CRYPTO_OPTIONAL,
                                                   "/nonexistent/station.key", s_dir));
    TEST_ASSERT_EQUAL_INT(0, arq_crypto_configure(ARQ_CRYPTO_OFF,
                                                  "/nonexistent/station.key", s_dir));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_client_byte_leaves_before_the_handshake_completes);
    RUN_TEST(test_both_sides_agree_and_carry_data_both_ways);
    RUN_TEST(test_any_chunking_of_the_stream_reassembles);
    RUN_TEST(test_wrong_key_fails_the_handshake);
    RUN_TEST(test_prologue_mismatch_fails_the_handshake);
    RUN_TEST(test_tampered_record_is_never_delivered);
    RUN_TEST(test_oversized_length_is_refused_on_the_header);
    RUN_TEST(test_data_before_the_reply_is_refused);
    RUN_TEST(test_bulk_overhead_per_mode);
    RUN_TEST(test_small_writes_coalesce_into_one_record);
    RUN_TEST(test_varint_is_minimal_and_bounded);
    RUN_TEST(test_record_size_follows_the_mode);
    RUN_TEST(test_prologue_is_unambiguous);
    RUN_TEST(test_negotiation_follows_the_mode);
    RUN_TEST(test_callsign_cannot_escape_the_peers_dir);
    RUN_TEST(test_missing_station_key_refuses_to_start);
    return UNITY_END();
}
