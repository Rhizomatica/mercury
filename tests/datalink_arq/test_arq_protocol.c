/*
 * ARQ Protocol Unit Tests
 *
 * Tests for datalink_arq/arq_protocol.c — frame encode/decode,
 * builders, parsers, and utility functions.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include <stdint.h>

#include "unity.h"
#include "arq_protocol.h"
#include "arq.h"
#include "framer.h"
#include "freedv/freedv_api.h"

void setUp(void) { }
void tearDown(void) { }

/* ---- Header encode/decode ---- */

void test_encode_decode_hdr_roundtrip(void)
{
    arq_frame_hdr_t hdr_in = {
        .packet_type = PACKET_TYPE_ARQ_CONTROL,
        .frame_ext   = 0,
        .subtype     = ARQ_SUBTYPE_ACK,
        .flags       = ARQ_FLAG_HAS_DATA,
        .session_id  = 0x42,
        .tx_seq      = 5,
        .rx_ack_seq  = 3,
        .snr_raw     = 150,
        .ack_delay_raw = 10
    };

    uint8_t buf[ARQ_FRAME_HDR_SIZE + 4];
    memset(buf, 0, sizeof(buf));

    int ret = arq_protocol_encode_hdr(buf, sizeof(buf), &hdr_in);
    TEST_ASSERT_EQUAL_INT(0, ret);

    arq_frame_hdr_t hdr_out;
    memset(&hdr_out, 0, sizeof(hdr_out));
    ret = arq_protocol_decode_hdr(buf, sizeof(buf), &hdr_out);
    TEST_ASSERT_EQUAL_INT(0, ret);

    TEST_ASSERT_EQUAL_UINT8(hdr_in.subtype, hdr_out.subtype);
    TEST_ASSERT_EQUAL_UINT8(hdr_in.flags, hdr_out.flags);
    TEST_ASSERT_EQUAL_UINT8(hdr_in.session_id, hdr_out.session_id);
    TEST_ASSERT_EQUAL_UINT8(hdr_in.tx_seq, hdr_out.tx_seq);
    TEST_ASSERT_EQUAL_UINT8(hdr_in.rx_ack_seq, hdr_out.rx_ack_seq);
    TEST_ASSERT_EQUAL_UINT8(hdr_in.snr_raw, hdr_out.snr_raw);
    TEST_ASSERT_EQUAL_UINT8(hdr_in.ack_delay_raw, hdr_out.ack_delay_raw);
}

void test_encode_hdr_buffer_too_small(void)
{
    arq_frame_hdr_t hdr = {0};
    uint8_t buf[4]; /* too small */
    int ret = arq_protocol_encode_hdr(buf, sizeof(buf), &hdr);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ---- SNR encode/decode ---- */

void test_encode_decode_snr_positive(void)
{
    float in_snr = 10.0f;
    uint8_t raw = arq_protocol_encode_snr(in_snr);
    TEST_ASSERT_NOT_EQUAL(0, raw);

    float out_snr = arq_protocol_decode_snr(raw);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, in_snr, out_snr);
}

void test_encode_decode_snr_negative(void)
{
    float in_snr = -5.0f;
    uint8_t raw = arq_protocol_encode_snr(in_snr);
    TEST_ASSERT_NOT_EQUAL(0, raw);

    float out_snr = arq_protocol_decode_snr(raw);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, in_snr, out_snr);
}

void test_decode_snr_zero_is_unknown(void)
{
    float out = arq_protocol_decode_snr(0);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, out);
}

/* ---- Bandwidth token roundtrip ---- */

void test_bw_token_500hz(void)
{
    uint8_t token = arq_protocol_bw_token_from_hz(500);
    TEST_ASSERT_EQUAL_UINT8(ARQ_BW_TOKEN_500, token);
    int hz = arq_protocol_bw_hz_from_token(token);
    TEST_ASSERT_EQUAL_INT(500, hz);
}

void test_bw_token_2300hz(void)
{
    uint8_t token = arq_protocol_bw_token_from_hz(2300);
    TEST_ASSERT_EQUAL_UINT8(ARQ_BW_TOKEN_2300, token);
    int hz = arq_protocol_bw_hz_from_token(token);
    TEST_ASSERT_EQUAL_INT(2300, hz);
}

void test_bw_token_invalid(void)
{
    int hz = arq_protocol_bw_hz_from_token(255);
    TEST_ASSERT_EQUAL_INT(0, hz);
}

/* ---- ACK delay encode/decode ---- */

void test_ack_delay_roundtrip(void)
{
    uint8_t raw = arq_protocol_encode_ack_delay(500);
    uint32_t decoded = arq_protocol_decode_ack_delay(raw);
    /* 10ms resolution: 500ms -> raw=50, decoded=500ms */
    TEST_ASSERT_EQUAL_UINT32(500, decoded);
}

void test_ack_delay_zero(void)
{
    uint8_t raw = arq_protocol_encode_ack_delay(0);
    TEST_ASSERT_EQUAL_UINT8(0, raw);
    uint32_t decoded = arq_protocol_decode_ack_delay(0);
    TEST_ASSERT_EQUAL_UINT32(0, decoded);
}

/* ---- Build ACK frame ---- */

void test_build_ack(void)
{
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    int size = arq_protocol_build_ack(buf, sizeof(buf),
        0x42, /* session_id */
        3,    /* rx_ack_seq */
        ARQ_FLAG_HAS_DATA,
        150,  /* snr_raw */
        10    /* ack_delay_raw */
    );
    TEST_ASSERT_GREATER_THAN(0, size);

    /* Decode the header and check fields */
    arq_frame_hdr_t hdr;
    int ret = arq_protocol_decode_hdr(buf, (size_t)size, &hdr);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT8(ARQ_SUBTYPE_ACK, hdr.subtype);
    TEST_ASSERT_EQUAL_UINT8(0x42, hdr.session_id);
}

/* ---- Build DISCONNECT frame ---- */

void test_build_disconnect(void)
{
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    int size = arq_protocol_build_disconnect(buf, sizeof(buf), 0x42, 128);
    TEST_ASSERT_GREATER_THAN(0, size);

    arq_frame_hdr_t hdr;
    int ret = arq_protocol_decode_hdr(buf, (size_t)size, &hdr);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT8(ARQ_SUBTYPE_DISCONNECT, hdr.subtype);
    TEST_ASSERT_EQUAL_UINT8(0x42, hdr.session_id);
}

/* ---- Mode timing lookup ---- */

void test_mode_timing_datac4(void)
{
    const arq_mode_timing_t *t = arq_protocol_mode_timing(FREEDV_MODE_DATAC4);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC4, t->freedv_mode);
    TEST_ASSERT_EQUAL(54, t->payload_bytes);
}

void test_mode_timing_invalid(void)
{
    const arq_mode_timing_t *t = arq_protocol_mode_timing(9999);
    TEST_ASSERT_NULL(t);
}

int main(void)
{
    UNITY_BEGIN();
    /* Header */
    RUN_TEST(test_encode_decode_hdr_roundtrip);
    RUN_TEST(test_encode_hdr_buffer_too_small);
    /* SNR */
    RUN_TEST(test_encode_decode_snr_positive);
    RUN_TEST(test_encode_decode_snr_negative);
    RUN_TEST(test_decode_snr_zero_is_unknown);
    /* Bandwidth */
    RUN_TEST(test_bw_token_500hz);
    RUN_TEST(test_bw_token_2300hz);
    RUN_TEST(test_bw_token_invalid);
    /* ACK delay */
    RUN_TEST(test_ack_delay_roundtrip);
    RUN_TEST(test_ack_delay_zero);
    /* Frame builders */
    RUN_TEST(test_build_ack);
    RUN_TEST(test_build_disconnect);
    /* Mode timing */
    RUN_TEST(test_mode_timing_datac4);
    RUN_TEST(test_mode_timing_invalid);
    return UNITY_END();
}
