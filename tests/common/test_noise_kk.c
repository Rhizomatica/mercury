/* Noise_KK_25519_ChaChaPoly_SHA256 tests
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * First the published test vector, byte for byte: if this passes, our KK is
 * the Noise spec's KK, not something that merely round-trips with itself.
 * Then the properties Mercury leans on: fail closed on tampering, a wrong key,
 * a different prologue, and counter exhaustion.
 */

#include <string.h>

#include "unity.h"
#include "noise_kk.h"
#include "noise_kk_vectors.h"

void setUp(void) { TEST_ASSERT_EQUAL_INT(0, noise_init()); }
void tearDown(void) {}

/* ---- the published vector -------------------------------------------- */

static void vector_handshake(noise_kk_t *ini, noise_kk_t *res)
{
    TEST_ASSERT_EQUAL_INT(0, noise_kk_init(ini, true, V_INIT_PROLOGUE,
                                           sizeof(V_INIT_PROLOGUE),
                                           V_INIT_STATIC, V_INIT_REMOTE_STATIC));
    TEST_ASSERT_EQUAL_INT(0, noise_kk_init(res, false, V_RESP_PROLOGUE,
                                           sizeof(V_RESP_PROLOGUE),
                                           V_RESP_STATIC, V_RESP_REMOTE_STATIC));
    noise_kk_set_ephemeral_for_test(ini, V_INIT_EPHEMERAL);
    noise_kk_set_ephemeral_for_test(res, V_RESP_EPHEMERAL);
}

/* Both handshake messages, the handshake hash, and all four transport
 * messages must match the published vector exactly. */
static void test_matches_the_published_vector(void)
{
    noise_kk_t ini, res;
    noise_cipher_t i_tx, i_rx, r_tx, r_rx;
    uint8_t buf[256], pt[256];
    int n;

    vector_handshake(&ini, &res);

    /* -> e, es, ss */
    n = noise_kk_write(&ini, V_MSG0_PAYLOAD, sizeof(V_MSG0_PAYLOAD), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(sizeof(V_MSG0_CIPHERTEXT), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(V_MSG0_CIPHERTEXT, buf, n);
    n = noise_kk_read(&res, buf, (size_t) n, pt, sizeof(pt));
    TEST_ASSERT_EQUAL_INT(sizeof(V_MSG0_PAYLOAD), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(V_MSG0_PAYLOAD, pt, n);

    /* <- e, ee, se */
    n = noise_kk_write(&res, V_MSG1_PAYLOAD, sizeof(V_MSG1_PAYLOAD), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(sizeof(V_MSG1_CIPHERTEXT), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(V_MSG1_CIPHERTEXT, buf, n);
    n = noise_kk_read(&ini, buf, (size_t) n, pt, sizeof(pt));
    TEST_ASSERT_EQUAL_INT(sizeof(V_MSG1_PAYLOAD), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(V_MSG1_PAYLOAD, pt, n);

    TEST_ASSERT_TRUE(noise_kk_done(&ini));
    TEST_ASSERT_TRUE(noise_kk_done(&res));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(V_HANDSHAKE_HASH, noise_kk_handshake_hash(&ini), 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(V_HANDSHAKE_HASH, noise_kk_handshake_hash(&res), 32);

    TEST_ASSERT_EQUAL_INT(0, noise_kk_split(&ini, &i_tx, &i_rx));
    TEST_ASSERT_EQUAL_INT(0, noise_kk_split(&res, &r_tx, &r_rx));

    /* Transport messages alternate, initiator first, with empty AD. */
    {
        const uint8_t *pl[] = { V_MSG2_PAYLOAD, V_MSG3_PAYLOAD,
                                V_MSG4_PAYLOAD, V_MSG5_PAYLOAD };
        const size_t   pll[] = { sizeof(V_MSG2_PAYLOAD), sizeof(V_MSG3_PAYLOAD),
                                 sizeof(V_MSG4_PAYLOAD), sizeof(V_MSG5_PAYLOAD) };
        const uint8_t *ct[] = { V_MSG2_CIPHERTEXT, V_MSG3_CIPHERTEXT,
                                V_MSG4_CIPHERTEXT, V_MSG5_CIPHERTEXT };
        const size_t   ctl[] = { sizeof(V_MSG2_CIPHERTEXT), sizeof(V_MSG3_CIPHERTEXT),
                                 sizeof(V_MSG4_CIPHERTEXT), sizeof(V_MSG5_CIPHERTEXT) };
        int i;

        for (i = 0; i < 4; i++)
        {
            noise_cipher_t *tx = (i % 2 == 0) ? &i_tx : &r_tx;
            noise_cipher_t *rx = (i % 2 == 0) ? &r_rx : &i_rx;

            n = noise_cipher_encrypt(tx, NULL, 0, pl[i], pll[i], buf);
            TEST_ASSERT_EQUAL_INT((int) ctl[i], n);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(ct[i], buf, n);

            n = noise_cipher_decrypt(rx, NULL, 0, buf, (size_t) n, pt);
            TEST_ASSERT_EQUAL_INT((int) pll[i], n);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(pl[i], pt, n);
        }
    }
}

/* ---- Mercury's use of it --------------------------------------------- */

static void keypair(uint8_t priv[32], uint8_t pub[32], uint8_t seed)
{
    memset(priv, seed, 32);
    TEST_ASSERT_EQUAL_INT(0, noise_pubkey(priv, pub));
}

static const uint8_t PROLOGUE_A[] = "hermes-mercury-arq-v1|A";
static const uint8_t PROLOGUE_B[] = "hermes-mercury-arq-v1|B";

/* With an empty payload each message is exactly 48 B -- the airtime sizing
 * the whole design assumes. */
static void test_empty_payload_messages_are_48_bytes(void)
{
    uint8_t is[32], ip[32], rs[32], rp[32], m1[64], m2[64], pt[16];
    noise_kk_t ini, res;

    keypair(is, ip, 0x11);
    keypair(rs, rp, 0x22);
    TEST_ASSERT_EQUAL_INT(0, noise_kk_init(&ini, true, PROLOGUE_A, sizeof(PROLOGUE_A), is, rp));
    TEST_ASSERT_EQUAL_INT(0, noise_kk_init(&res, false, PROLOGUE_A, sizeof(PROLOGUE_A), rs, ip));

    TEST_ASSERT_EQUAL_INT(NOISE_KK_MSG_LEN, noise_kk_write(&ini, NULL, 0, m1, sizeof(m1)));
    TEST_ASSERT_EQUAL_INT(0, noise_kk_read(&res, m1, NOISE_KK_MSG_LEN, pt, sizeof(pt)));
    TEST_ASSERT_EQUAL_INT(NOISE_KK_MSG_LEN, noise_kk_write(&res, NULL, 0, m2, sizeof(m2)));
    TEST_ASSERT_EQUAL_INT(0, noise_kk_read(&ini, m2, NOISE_KK_MSG_LEN, pt, sizeof(pt)));
    TEST_ASSERT_EQUAL_INT(48, NOISE_KK_MSG_LEN);
}

/* Any altered bit of the first message must end the handshake. */
static void test_tampered_first_message_is_refused(void)
{
    uint8_t is[32], ip[32], rs[32], rp[32], m1[64], pt[16];
    int bit;

    keypair(is, ip, 0x11);
    keypair(rs, rp, 0x22);

    for (bit = 0; bit < NOISE_KK_MSG_LEN * 8; bit += 37)
    {
        noise_kk_t ini, res;

        noise_kk_init(&ini, true, PROLOGUE_A, sizeof(PROLOGUE_A), is, rp);
        noise_kk_init(&res, false, PROLOGUE_A, sizeof(PROLOGUE_A), rs, ip);
        noise_kk_write(&ini, NULL, 0, m1, sizeof(m1));
        m1[bit / 8] ^= (uint8_t) (1u << (bit % 8));
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, noise_kk_read(&res, m1, NOISE_KK_MSG_LEN, pt, sizeof(pt)),
                                      "a flipped bit was accepted");
    }
}

/* The caller-callsign key lookup is safe because a station claiming someone
 * else's callsign does not hold that station's private key: ss fails. */
static void test_impostor_without_the_claimed_key_is_refused(void)
{
    uint8_t is[32], ip[32], rs[32], rp[32], xs[32], xp[32], m1[64], pt[16];
    noise_kk_t imp, res;

    keypair(is, ip, 0x11);             /* the real caller */
    keypair(rs, rp, 0x22);             /* responder */
    keypair(xs, xp, 0x33);             /* an impostor using its own key */

    /* The responder believes the caller is `ip`, as the CALL callsign said. */
    noise_kk_init(&res, false, PROLOGUE_A, sizeof(PROLOGUE_A), rs, ip);
    noise_kk_init(&imp, true, PROLOGUE_A, sizeof(PROLOGUE_A), xs, rp);
    noise_kk_write(&imp, NULL, 0, m1, sizeof(m1));
    TEST_ASSERT_EQUAL_INT(-1, noise_kk_read(&res, m1, NOISE_KK_MSG_LEN, pt, sizeof(pt)));
}

/* The prologue binds callsigns, bandwidth, the negotiated bits and the session
 * id.  A first message replayed into a session with a different prologue --
 * the same caller, but a new session id -- must fail immediately. */
static void test_replayed_first_message_fails_under_a_new_prologue(void)
{
    uint8_t is[32], ip[32], rs[32], rp[32], m1[64], pt[16];
    noise_kk_t ini, res;

    keypair(is, ip, 0x11);
    keypair(rs, rp, 0x22);

    noise_kk_init(&ini, true, PROLOGUE_A, sizeof(PROLOGUE_A), is, rp);
    noise_kk_write(&ini, NULL, 0, m1, sizeof(m1));

    noise_kk_init(&res, false, PROLOGUE_B, sizeof(PROLOGUE_B), rs, ip);
    TEST_ASSERT_EQUAL_INT(-1, noise_kk_read(&res, m1, NOISE_KK_MSG_LEN, pt, sizeof(pt)));
}

/* Messages must come in order, from the right side. */
static void test_out_of_turn_messages_are_refused(void)
{
    uint8_t is[32], ip[32], rs[32], rp[32], m[64], pt[16];
    noise_kk_t ini, res;

    keypair(is, ip, 0x11);
    keypair(rs, rp, 0x22);
    noise_kk_init(&ini, true, PROLOGUE_A, sizeof(PROLOGUE_A), is, rp);
    noise_kk_init(&res, false, PROLOGUE_A, sizeof(PROLOGUE_A), rs, ip);

    TEST_ASSERT_EQUAL_INT(-1, noise_kk_write(&res, NULL, 0, m, sizeof(m)));   /* responder first */
    TEST_ASSERT_EQUAL_INT(-1, noise_kk_read(&ini, m, NOISE_KK_MSG_LEN, pt, sizeof(pt)));
}

/* A failed decrypt must not advance the counter, and an exhausted counter must
 * refuse rather than wrap into a reused nonce. */
static void test_counter_fails_closed(void)
{
    noise_cipher_t c;
    uint8_t out[64], junk[32] = { 0 };

    memset(&c, 0, sizeof(c));
    memset(c.k, 0x5a, sizeof(c.k));
    c.has_key = true;

    c.n = 7;
    TEST_ASSERT_EQUAL_INT(-1, noise_cipher_decrypt(&c, NULL, 0, junk, sizeof(junk), out));
    TEST_ASSERT_EQUAL_UINT64(7, c.n);

    c.n = UINT64_MAX;
    TEST_ASSERT_EQUAL_INT(-1, noise_cipher_encrypt(&c, NULL, 0, junk, 4, out));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, c.n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_matches_the_published_vector);
    RUN_TEST(test_empty_payload_messages_are_48_bytes);
    RUN_TEST(test_tampered_first_message_is_refused);
    RUN_TEST(test_impostor_without_the_claimed_key_is_refused);
    RUN_TEST(test_replayed_first_message_fails_under_a_new_prologue);
    RUN_TEST(test_out_of_turn_messages_are_refused);
    RUN_TEST(test_counter_fails_closed);
    return UNITY_END();
}
