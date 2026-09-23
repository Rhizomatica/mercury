/* Broadcast object encryption tests
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The format is shared with hermes-broadcast's broadcast_daemon, so these pin
 * the wire layout as well as the behaviour: 41 bytes per object, a header a
 * receiver can match on, and "not ours" -- never garbage -- for anything that
 * does not open.
 */

#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "bcast_aead.h"

static uint8_t K1[BCAST_AEAD_KEYLEN], K2[BCAST_AEAD_KEYLEN];

void setUp(void)
{
    TEST_ASSERT_TRUE(bcast_aead_available());
    memset(K1, 0x11, sizeof(K1));
    memset(K2, 0x22, sizeof(K2));
}
void tearDown(void) {}

static void test_round_trip_costs_exactly_41_bytes(void)
{
    static const uint8_t obj[] = "an NNCP bundle would go here";
    uint8_t sealed[256], plain[256];
    size_t n, pl = 0;

    n = bcast_aead_seal(K1, obj, sizeof(obj), sealed, sizeof(sealed));
    TEST_ASSERT_EQUAL_UINT(sizeof(obj) + 41, n);
    TEST_ASSERT_EQUAL_UINT(41, BCAST_AEAD_OVERHEAD);

    TEST_ASSERT_TRUE(bcast_aead_open(K1, sealed, n, plain, sizeof(plain), &pl));
    TEST_ASSERT_EQUAL_UINT(sizeof(obj), pl);
    TEST_ASSERT_EQUAL_MEMORY(obj, plain, pl);
}

/* The header carries the format version in its high nibble. */
static void test_header_carries_the_version(void)
{
    TEST_ASSERT_EQUAL_HEX8(BCAST_AEAD_VERSION, bcast_aead_header(K1) >> 4);
}

/* A random nonce per object: the same object sealed twice must differ, or a
 * receiver could tell repeats apart from new content. */
static void test_each_seal_uses_a_fresh_nonce(void)
{
    static const uint8_t obj[] = "same object";
    uint8_t a[128], b[128];
    size_t na = bcast_aead_seal(K1, obj, sizeof(obj), a, sizeof(a));
    size_t nb = bcast_aead_seal(K1, obj, sizeof(obj), b, sizeof(b));

    TEST_ASSERT_EQUAL_UINT(na, nb);
    TEST_ASSERT_TRUE(memcmp(a, b, na) != 0);
}

/* Another key's object is "not ours", not an error and not garbage. */
static void test_another_keys_object_is_not_ours(void)
{
    static const uint8_t obj[] = "for network one";
    uint8_t sealed[128], plain[128];
    size_t n = bcast_aead_seal(K1, obj, sizeof(obj), sealed, sizeof(sealed));
    size_t pl = 0;

    TEST_ASSERT_FALSE(bcast_aead_open(K2, sealed, n, plain, sizeof(plain), &pl));
}

/* Any altered bit -- header, nonce, body or tag -- fails to open. */
static void test_any_altered_bit_fails_to_open(void)
{
    static const uint8_t obj[] = "authenticated whole";
    uint8_t sealed[128], plain[128];
    size_t n = bcast_aead_seal(K1, obj, sizeof(obj), sealed, sizeof(sealed));
    size_t i, pl;

    for (i = 0; i < n * 8; i += 13)
    {
        uint8_t copy[128];

        memcpy(copy, sealed, n);
        copy[i / 8] ^= (uint8_t) (1u << (i % 8));
        TEST_ASSERT_FALSE_MESSAGE(bcast_aead_open(K1, copy, n, plain, sizeof(plain), &pl),
                                  "an altered bit still opened");
    }
}

/* A clear object -- even one whose first byte happens to equal our header --
 * is "not ours", so the caller handles it as clear. */
static void test_a_clear_object_is_not_ours(void)
{
    uint8_t clear[100], plain[100];
    size_t pl;

    memset(clear, 0x5a, sizeof(clear));
    TEST_ASSERT_FALSE(bcast_aead_open(K1, clear, sizeof(clear), plain, sizeof(plain), &pl));

    clear[0] = bcast_aead_header(K1);
    TEST_ASSERT_FALSE(bcast_aead_open(K1, clear, sizeof(clear), plain, sizeof(plain), &pl));

    /* Too short to be sealed at all. */
    TEST_ASSERT_FALSE(bcast_aead_open(K1, clear, 40, plain, sizeof(plain), &pl));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_costs_exactly_41_bytes);
    RUN_TEST(test_header_carries_the_version);
    RUN_TEST(test_each_seal_uses_a_fresh_nonce);
    RUN_TEST(test_another_keys_object_is_not_ours);
    RUN_TEST(test_any_altered_bit_fails_to_open);
    RUN_TEST(test_a_clear_object_is_not_ours);
    return UNITY_END();
}
