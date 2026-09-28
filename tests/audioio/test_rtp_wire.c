/*
 * Wire-codec tests for the -x rtp transport (rtp_wire.h).
 *
 * Pins the RTP header layout (RFC 3550), the PT 125 S16BE payload that
 * ka9q-radio's payload-type table defines, the empty end-of-transmission
 * packet and the ka9q status TLV encoding, so a change on either side of
 * the radio <-> modem link fails here.  Expected bytes are hand-derived.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unity.h"
#include "rtp_wire.h"

#include <stdint.h>
#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

void test_build_pinned_bytes(void)
{
    uint8_t pkt[RTP_WIRE_MAX_BYTES];
    const int32_t ring[2] = { (int32_t)0x12340000, (int32_t)0x80000000u };
    size_t n = rtp_wire_build(pkt, true, 0xBEEF, 0x01020304u, 0xCAFEBABEu, ring, 2, 3);

    const uint8_t expect[] = {
        0x80, 0x80 | 125,             /* V=2; marker, PT 125 */
        0xBE, 0xEF,                   /* sequence */
        0x01, 0x02, 0x03, 0x04,       /* timestamp */
        0xCA, 0xFE, 0xBA, 0xBE,       /* SSRC */
        0x12, 0x34, 0x80, 0x00,       /* S16BE samples */
        0x00, 0x00,                   /* silence pad */
    };
    TEST_ASSERT_EQUAL_size_t(sizeof(expect), n);
    TEST_ASSERT_EQUAL_MEMORY(expect, pkt, n);
}

void test_end_packet_is_header_only(void)
{
    uint8_t pkt[RTP_WIRE_MAX_BYTES];
    size_t n = rtp_wire_build(pkt, false, 7, 160, 1, NULL, 0, 0);
    TEST_ASSERT_EQUAL_size_t(RTP_WIRE_HDR_BYTES, n);
    TEST_ASSERT_EQUAL_HEX8(125, pkt[1]);         /* no marker */

    rtp_wire_hdr h = { 0 };
    TEST_ASSERT_EQUAL_INT(0, rtp_wire_parse(pkt, n, &h));
    TEST_ASSERT_EQUAL_size_t(0, h.payload_len);
}

void test_parse_roundtrip(void)
{
    uint8_t pkt[RTP_WIRE_MAX_BYTES];
    int32_t ring[RTP_WIRE_FRAME];
    for (int i = 0; i < RTP_WIRE_FRAME; i++)
        ring[i] = (int32_t)((uint32_t)(int16_t)(i * 200 - 16000) << 16);
    size_t n = rtp_wire_build(pkt, false, 65535, 0xFFFFFF60u, 42, ring, RTP_WIRE_FRAME, RTP_WIRE_FRAME);

    rtp_wire_hdr h = { 0 };
    TEST_ASSERT_EQUAL_INT(0, rtp_wire_parse(pkt, n, &h));
    TEST_ASSERT_FALSE(h.marker);
    TEST_ASSERT_EQUAL_UINT8(125, h.pt);
    TEST_ASSERT_EQUAL_UINT16(65535, h.seq);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFF60u, h.ts);
    TEST_ASSERT_EQUAL_UINT32(42, h.ssrc);
    TEST_ASSERT_EQUAL_size_t(2 * RTP_WIRE_FRAME, h.payload_len);
    for (int i = 0; i < RTP_WIRE_FRAME; i++)
        TEST_ASSERT_EQUAL_INT32(ring[i], rtp_wire_s16be_to_ring(h.payload + 2 * i));
}

void test_parse_skips_csrc_extension_padding(void)
{
    /* CC=1, X=1, P=1: header, 1 CSRC, 4+4 bytes extension, 2 payload
     * bytes, 3 bytes padding (last byte = 3). */
    const uint8_t pkt[] = {
        0x80 | 0x20 | 0x10 | 0x01, 125, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3,
        0xAA, 0xAA, 0xAA, 0xAA,             /* CSRC */
        0xBE, 0xDE, 0x00, 0x01,             /* extension header, 1 word */
        0x11, 0x22, 0x33, 0x44,
        0x7F, 0xFF,                         /* payload */
        0x00, 0x00, 0x03,                   /* padding */
    };
    rtp_wire_hdr h = { 0 };
    TEST_ASSERT_EQUAL_INT(0, rtp_wire_parse(pkt, sizeof(pkt), &h));
    TEST_ASSERT_EQUAL_size_t(2, h.payload_len);
    TEST_ASSERT_EQUAL_HEX8(0x7F, h.payload[0]);
    TEST_ASSERT_EQUAL_INT32((int32_t)0x7FFF0000, rtp_wire_s16be_to_ring(h.payload));
}

void test_parse_rejects_malformed(void)
{
    uint8_t pkt[RTP_WIRE_MAX_BYTES];
    rtp_wire_hdr h = { 0 };
    size_t n = rtp_wire_build(pkt, false, 1, 2, 3, NULL, 0, 4);

    TEST_ASSERT_EQUAL_INT(-1, rtp_wire_parse(pkt, 11, &h));       /* short */
    pkt[0] = 0x40;                                                /* version 1 */
    TEST_ASSERT_EQUAL_INT(-1, rtp_wire_parse(pkt, n, &h));
    pkt[0] = 0x80 | 0x0f;                                         /* 15 CSRCs */
    TEST_ASSERT_EQUAL_INT(-1, rtp_wire_parse(pkt, n, &h));
    pkt[0] = 0x80 | 0x20;                                         /* padding 0 */
    pkt[n - 1] = 0;
    TEST_ASSERT_EQUAL_INT(-1, rtp_wire_parse(pkt, n, &h));
}

void test_status_tlvs(void)
{
    uint8_t pkt[128];
    size_t n = rtp_wire_build_status(pkt, 0x00ABCDEFu, 0, 0x0100000000ULL, "tx");
    const uint8_t expect[] = {
        0,                                  /* STATUS */
        3, 5, 0x01, 0, 0, 0, 0,             /* GPS_TIME, leading zeros stripped */
        4, 2, 't', 'x',                     /* DESCRIPTION */
        8, 0,                               /* RTP_TIMESNAP 0 -> length 0 */
        18, 3, 0xAB, 0xCD, 0xEF,            /* OUTPUT_SSRC */
        20, 2, 0x1F, 0x40,                  /* OUTPUT_SAMPRATE 8000 */
        49, 1, 1,                           /* OUTPUT_CHANNELS */
        105, 1, 125,                        /* RTP_PT */
        107, 1, 2,                          /* OUTPUT_ENCODING S16BE */
        0,                                  /* EOL */
    };
    TEST_ASSERT_EQUAL_size_t(sizeof(expect), n);
    TEST_ASSERT_EQUAL_MEMORY(expect, pkt, n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_pinned_bytes);
    RUN_TEST(test_end_packet_is_header_only);
    RUN_TEST(test_parse_roundtrip);
    RUN_TEST(test_parse_skips_csrc_extension_padding);
    RUN_TEST(test_parse_rejects_malformed);
    RUN_TEST(test_status_tlvs);
    UNITY_END();
    return 0;
}
