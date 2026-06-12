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
#include <stdatomic.h>

#include "unity.h"
#include "arq_protocol.h"
#include "arq.h"
#include "framer.h"
#include "freedv/freedv_api.h"

void setUp(void)
{
    /* Reset CALLINT override before each test so tests are isolated */
    atomic_store(&arq_callint_override_s, 0.0f);
}

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

/* ---- DATA frame valid-length encoding (3 flag bits + low byte) ---- */

/* Encode payload_valid > 511 the way send_data_frame() does, then verify
 * the wire bytes reconstruct the exact count the way arq.c does.  1205 is
 * QAM16C2's user payload (1213 - 8 header); 0x4B5 exercises LEN_B10. */
void test_data_valid_length_over_511(void)
{
    static const uint16_t lens[] = { 256, 511, 512, 1023, 1172, 1205, 2047 };
    uint8_t payload[8] = {0xA5};

    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
    {
        uint16_t len = lens[i];
        uint8_t flags = 0;
        if (len & 0x100) flags |= ARQ_FLAG_LEN_HI;
        if (len & 0x200) flags |= ARQ_FLAG_LEN_B9;
        if (len & 0x400) flags |= ARQ_FLAG_LEN_B10;

        uint8_t buf[64];
        int size = arq_protocol_build_data(buf, sizeof(buf), 0x42, 1, 0,
                                           flags, 0, len,
                                           payload, sizeof(payload));
        TEST_ASSERT_GREATER_THAN(0, size);

        arq_frame_hdr_t hdr;
        TEST_ASSERT_EQUAL_INT(0, arq_protocol_decode_hdr(buf, (size_t)size, &hdr));

        size_t valid = (size_t)hdr.ack_delay_raw;
        if (hdr.flags & ARQ_FLAG_LEN_HI)  valid |= 0x100u;
        if (hdr.flags & ARQ_FLAG_LEN_B9)  valid |= 0x200u;
        if (hdr.flags & ARQ_FLAG_LEN_B10) valid |= 0x400u;
        TEST_ASSERT_EQUAL_UINT(len, valid);
    }
}

/* ---- Mode timing lookup ---- */

void test_mode_timing_datac4(void)
{
    const arq_mode_timing_t *t = arq_protocol_mode_timing(FREEDV_MODE_DATAC4);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC4, t->freedv_mode);
    TEST_ASSERT_EQUAL(54, t->payload_bytes);
}

void test_mode_timing_datac15(void)
{
    const arq_mode_timing_t *t = arq_protocol_mode_timing(FREEDV_MODE_DATAC15);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC15, t->freedv_mode);
    TEST_ASSERT_EQUAL(30, t->payload_bytes);
}

void test_mode_timing_datac16(void)
{
    const arq_mode_timing_t *t = arq_protocol_mode_timing(FREEDV_MODE_DATAC16);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC16, t->freedv_mode);
    /* Control frames are fixed 14 bytes — the control mode must match. */
    TEST_ASSERT_EQUAL(ARQ_CONTROL_FRAME_SIZE, t->payload_bytes);
}

void test_mode_timing_datac13_removed(void)
{
    /* DATAC13 was replaced by DATAC16 as the control mode and intentionally
     * has no timing row (its 14-byte payload would collide with DATAC16 in
     * the DATA-frame mode-inference loop). */
    const arq_mode_timing_t *t = arq_protocol_mode_timing(FREEDV_MODE_DATAC13);
    TEST_ASSERT_NULL(t);
}

void test_mode_timing_fast_modes(void)
{
    const arq_mode_timing_t *t17 = arq_protocol_mode_timing(FREEDV_MODE_DATAC17);
    TEST_ASSERT_NOT_NULL(t17);
    TEST_ASSERT_EQUAL(1180, t17->payload_bytes);

    const arq_mode_timing_t *tq = arq_protocol_mode_timing(FREEDV_MODE_QAM16C2);
    TEST_ASSERT_NOT_NULL(tq);
    TEST_ASSERT_EQUAL(1213, tq->payload_bytes);

    /* Frame sizes must stay pairwise unique across the whole table — the
     * RX path infers the peer's TX mode from frame size alone. */
    for (int i = 0; i < arq_mode_table_count; i++)
        for (int j = i + 1; j < arq_mode_table_count; j++)
            TEST_ASSERT_NOT_EQUAL(arq_mode_table[i].payload_bytes,
                                  arq_mode_table[j].payload_bytes);
}

void test_mode_timing_invalid(void)
{
    const arq_mode_timing_t *t = arq_protocol_mode_timing(9999);
    TEST_ASSERT_NULL(t);
}

/* ---- CALLINT / call interval override ---- */

void test_call_interval_default(void)
{
    /* Override is 0 (default) → should return DATAC16 table value (8.0s) */
    float interval = arq_protocol_call_interval_s();
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 8.0f, interval);
}

void test_call_interval_override(void)
{
    /* Set override to 5.0s → should return 5.0s */
    atomic_store(&arq_callint_override_s, 5.0f);
    float interval = arq_protocol_call_interval_s();
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, interval);
}

void test_call_interval_reset(void)
{
    /* Set override, then reset to 0 → should return table default */
    atomic_store(&arq_callint_override_s, 5.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, arq_protocol_call_interval_s());

    atomic_store(&arq_callint_override_s, ARQ_CALLINT_DEFAULT_S);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 8.0f, arq_protocol_call_interval_s());
}

void test_mode_timing_datac16_unaffected_by_override(void)
{
    /* Even with CALLINT override active, arq_protocol_mode_timing() must
     * return the immutable table entry — the override only takes effect
     * through arq_protocol_call_interval_s(). */
    atomic_store(&arq_callint_override_s, 5.0f);

    const arq_mode_timing_t *t = arq_protocol_mode_timing(FREEDV_MODE_DATAC16);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 8.0f, t->retry_interval_s);
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
    RUN_TEST(test_data_valid_length_over_511);
    /* Mode timing */
    RUN_TEST(test_mode_timing_datac4);
    RUN_TEST(test_mode_timing_datac15);
    RUN_TEST(test_mode_timing_datac16);
    RUN_TEST(test_mode_timing_datac13_removed);
    RUN_TEST(test_mode_timing_fast_modes);
    RUN_TEST(test_mode_timing_invalid);
    /* CALLINT / call interval override */
    RUN_TEST(test_call_interval_default);
    RUN_TEST(test_call_interval_override);
    RUN_TEST(test_call_interval_reset);
    RUN_TEST(test_mode_timing_datac16_unaffected_by_override);
    return UNITY_END();
}
