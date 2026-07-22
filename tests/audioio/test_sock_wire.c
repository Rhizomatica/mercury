/*
 * Wire-codec tests for the -x sock lockstep bench transport (sock_wire.h).
 *
 * Pins the frame layout byte-exact against the shared contract
 * (skywave sock_frames.py: little-endian, HDR_SIM = <QQH>, HDR_STA = <QBH>,
 * u32 length prefix counting the bytes after itself), so a codec change on
 * either side fails here instead of corrupting a bench run.  Expected byte
 * arrays below are hand-derived from the Python struct formats.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unity.h"
#include "sock_wire.h"

#include <stdint.h>
#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

/* ---- scalar helpers: little-endian on any host ---- */

void test_u16_roundtrip_and_layout(void)
{
    uint8_t b[2];
    sock_wire_wr_u16(b, 0x1234);
    TEST_ASSERT_EQUAL_HEX8(0x34, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x12, b[1]);
    TEST_ASSERT_EQUAL_HEX16(0x1234, sock_wire_rd_u16(b));
}

void test_u32_roundtrip_and_layout(void)
{
    uint8_t b[4];
    sock_wire_wr_u32(b, 0xdeadbeefu);
    const uint8_t expect[4] = { 0xef, 0xbe, 0xad, 0xde };
    TEST_ASSERT_EQUAL_MEMORY(expect, b, 4);
    TEST_ASSERT_EQUAL_HEX32(0xdeadbeefu, sock_wire_rd_u32(b));
}

void test_u64_roundtrip_and_layout(void)
{
    uint8_t b[8];
    sock_wire_wr_u64(b, 0x0102030405060708ULL);
    const uint8_t expect[8] = { 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01 };
    TEST_ASSERT_EQUAL_MEMORY(expect, b, 8);
    TEST_ASSERT_EQUAL_HEX64(0x0102030405060708ULL, sock_wire_rd_u64(b));
}

/* ---- sample conversion: same convention as the FIFO path ---- */

void test_sample_conversion(void)
{
    TEST_ASSERT_EQUAL_INT32(0x7fff0000, sock_wire_i16_to_ring(0x7fff));
    TEST_ASSERT_EQUAL_INT32((int32_t)0x80000000, sock_wire_i16_to_ring(0x8000));
    TEST_ASSERT_EQUAL_INT32(0, sock_wire_i16_to_ring(0));
    TEST_ASSERT_EQUAL_INT16(0x7fff, sock_wire_ring_to_i16(0x7fff0000));
    TEST_ASSERT_EQUAL_INT16(-32768, sock_wire_ring_to_i16((int32_t)0x80000000));
    /* roundtrip is exact for every i16 */
    for (int32_t v = -32768; v <= 32767; v++)
        TEST_ASSERT_EQUAL_INT16((int16_t)v,
            sock_wire_ring_to_i16(sock_wire_i16_to_ring((uint16_t)(int16_t)v)));
}

/* ---- sim frame parse: pinned against Python
 *   struct.pack("<I", 18 + 2n) + struct.pack("<QQH", seq, vnow_ms, n) ---- */

void test_parse_sim_pinned_bytes(void)
{
    /* seq = 7, virtual_now_ms = 128000, n = 2 -> len = 22 */
    const uint8_t hdr[SOCK_WIRE_HDR_SIM_BYTES] = {
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* seq = 7        */
        0x00, 0xf4, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,   /* vnow = 0x1f400 */
        0x02, 0x00,                                        /* n = 2          */
    };
    uint64_t seq = 0, vnow = 0;
    uint16_t n = 0;
    TEST_ASSERT_EQUAL_INT(0, sock_wire_parse_sim(hdr, 22, &seq, &vnow, &n));
    TEST_ASSERT_EQUAL_UINT64(7, seq);
    TEST_ASSERT_EQUAL_UINT64(128000, vnow);
    TEST_ASSERT_EQUAL_UINT16(2, n);
}

void test_parse_sim_rejects_inconsistent_len(void)
{
    uint8_t hdr[SOCK_WIRE_HDR_SIM_BYTES] = {0};
    sock_wire_wr_u16(hdr + 16, 4);            /* n = 4 -> len must be 26 */
    uint64_t seq, vnow;
    uint16_t n;
    TEST_ASSERT_EQUAL_INT(-1, sock_wire_parse_sim(hdr, 25, &seq, &vnow, &n));
    TEST_ASSERT_EQUAL_INT(-1, sock_wire_parse_sim(hdr, 27, &seq, &vnow, &n));
    TEST_ASSERT_EQUAL_INT(0,  sock_wire_parse_sim(hdr, 26, &seq, &vnow, &n));
}

void test_parse_sim_max_samples(void)
{
    uint8_t hdr[SOCK_WIRE_HDR_SIM_BYTES] = {0};
    sock_wire_wr_u16(hdr + 16, 0xffff);
    uint64_t seq, vnow;
    uint16_t n;
    /* len = 18 + 65535*2 = 131088: must not overflow the u16 * 2 math */
    TEST_ASSERT_EQUAL_INT(0, sock_wire_parse_sim(hdr, 131088u, &seq, &vnow, &n));
    TEST_ASSERT_EQUAL_UINT16(0xffff, n);
}

/* ---- station frame build: pinned against Python
 *   struct.pack("<I", 11 + 2n) + struct.pack("<QBH", seq, ptt, n) + payload ---- */

void test_build_station_pinned_bytes(void)
{
    /* seq = 7, ptt = 1, n = 3, samples 0x11220000, -0x10000 (= -1 as i16), pad */
    const int32_t ring[2] = { 0x11220000, (int32_t)0xffff0000 };
    uint8_t frame[4 + SOCK_WIRE_HDR_STA_BYTES + 3 * 2];
    size_t len = sock_wire_build_station(frame, 7, SOCK_WIRE_PTT_ON, 3, ring, 2);

    const uint8_t expect[] = {
        0x11, 0x00, 0x00, 0x00,                            /* len = 17       */
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,    /* seq = 7        */
        0x01,                                              /* ptt = 1        */
        0x03, 0x00,                                        /* n = 3          */
        0x22, 0x11,                                        /* 0x1122         */
        0xff, 0xff,                                        /* -1             */
        0x00, 0x00,                                        /* silence pad    */
    };
    TEST_ASSERT_EQUAL_size_t(sizeof(expect), len);
    TEST_ASSERT_EQUAL_MEMORY(expect, frame, sizeof(expect));
}

void test_build_station_empty_playback_is_all_silence(void)
{
    uint8_t frame[4 + SOCK_WIRE_HDR_STA_BYTES + 4 * 2];
    size_t len = sock_wire_build_station(frame, 0, SOCK_WIRE_PTT_OFF, 4, NULL, 0);
    TEST_ASSERT_EQUAL_size_t(sizeof(frame), len);
    for (size_t i = 4 + SOCK_WIRE_HDR_STA_BYTES; i < len; i++)
        TEST_ASSERT_EQUAL_HEX8(0x00, frame[i]);
    TEST_ASSERT_EQUAL_HEX8(SOCK_WIRE_PTT_OFF, frame[12]);
}

void test_build_station_zero_samples(void)
{
    uint8_t frame[4 + SOCK_WIRE_HDR_STA_BYTES];
    size_t len = sock_wire_build_station(frame, 1, SOCK_WIRE_PTT_UNKNOWN, 0, NULL, 0);
    TEST_ASSERT_EQUAL_size_t(4 + SOCK_WIRE_HDR_STA_BYTES, len);
    TEST_ASSERT_EQUAL_UINT32(SOCK_WIRE_HDR_STA_BYTES, sock_wire_rd_u32(frame));
    TEST_ASSERT_EQUAL_HEX8(SOCK_WIRE_PTT_UNKNOWN, frame[12]);
}

/* ---- header size constants match the struct formats ---- */

void test_header_sizes(void)
{
    TEST_ASSERT_EQUAL_INT(18, SOCK_WIRE_HDR_SIM_BYTES);   /* <QQH> */
    TEST_ASSERT_EQUAL_INT(11, SOCK_WIRE_HDR_STA_BYTES);   /* <QBH> */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_u16_roundtrip_and_layout);
    RUN_TEST(test_u32_roundtrip_and_layout);
    RUN_TEST(test_u64_roundtrip_and_layout);
    RUN_TEST(test_sample_conversion);
    RUN_TEST(test_parse_sim_pinned_bytes);
    RUN_TEST(test_parse_sim_rejects_inconsistent_len);
    RUN_TEST(test_parse_sim_max_samples);
    RUN_TEST(test_build_station_pinned_bytes);
    RUN_TEST(test_build_station_empty_playback_is_all_silence);
    RUN_TEST(test_build_station_zero_samples);
    RUN_TEST(test_header_sizes);
    UNITY_END();
    return 0;
}
