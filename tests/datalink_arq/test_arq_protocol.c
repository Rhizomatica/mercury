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

/* Windowed ARQ: the SACK ACK frame round-trips — rcv_base in rx_ack_seq,
 * ARQ_FLAG_SACK set, bitmap byte right after the header, HAS_DATA preserved. */
void test_build_sack_roundtrip(void)
{
    for (int bm = 0; bm <= 0xFF; bm += 0x33)
    {
        uint8_t bitmap[ARQ_SACK_BITMAP_BYTES];
        for (int i = 0; i < ARQ_SACK_BITMAP_BYTES; i++)
            bitmap[i] = (uint8_t)(bm ^ (i * 0x11));
        uint8_t buf[64];
        int size = arq_protocol_build_sack(buf, sizeof(buf), 0x42, 7,
                                           ARQ_FLAG_HAS_DATA, 0, bitmap);
        TEST_ASSERT_EQUAL_INT(ARQ_FRAME_HDR_SIZE + ARQ_SACK_BITMAP_BYTES, size);

        arq_frame_hdr_t hdr;
        TEST_ASSERT_EQUAL_INT(0, arq_protocol_decode_hdr(buf, (size_t)size, &hdr));
        TEST_ASSERT_EQUAL_UINT8(ARQ_SUBTYPE_ACK, hdr.subtype);
        TEST_ASSERT_EQUAL_UINT8(7, hdr.rx_ack_seq);           /* rcv_base */
        TEST_ASSERT_TRUE(hdr.flags & ARQ_FLAG_SACK);
        TEST_ASSERT_TRUE(hdr.flags & ARQ_FLAG_HAS_DATA);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(bitmap, buf + ARQ_FRAME_HDR_SIZE,
                                      ARQ_SACK_BITMAP_BYTES);
    }
}

/* Windowed ARQ: burst_remaining (flags bits [2:0]) round-trips and coexists
 * with the LEN_* bits and HAS_DATA in the same flags byte. */
void test_data_burst_remaining_roundtrip(void)
{
    uint8_t payload[8] = {0xA5};
    for (uint8_t rem = 0; rem <= ARQ_BURST_REM_MAX; rem++)
    {
        /* Mix in HAS_DATA + a >511 length so all flag bits are exercised. */
        uint8_t flags = ARQ_FLAG_HAS_DATA | ARQ_FLAG_LEN_HI | rem;
        uint8_t buf[64];
        int size = arq_protocol_build_data(buf, sizeof(buf), 0x42, 1, 0,
                                           flags, 0, 300, payload, sizeof(payload));
        TEST_ASSERT_GREATER_THAN(0, size);

        arq_frame_hdr_t hdr;
        TEST_ASSERT_EQUAL_INT(0, arq_protocol_decode_hdr(buf, (size_t)size, &hdr));
        TEST_ASSERT_EQUAL_UINT8(rem, hdr.burst_remaining);
        TEST_ASSERT_EQUAL_UINT8(rem, arq_protocol_data_burst_remaining(buf, (size_t)size));
        /* burst_remaining bits must not corrupt HAS_DATA or the length bits. */
        TEST_ASSERT_TRUE(hdr.flags & ARQ_FLAG_HAS_DATA);
        size_t valid = (size_t)hdr.ack_delay_raw | 0x100u;
        TEST_ASSERT_EQUAL_UINT(300, valid);
    }
}

/* Block-packing: a container of N blocks round-trips through
 * build_data_blocks -> parse_data_blocks with each block's seq/len/data intact,
 * even when the frame is zero-padded to a full mode payload (as on the wire). */
void test_data_blocks_roundtrip(void)
{
    uint8_t d0[44], d1[1], d2[20];
    for (int i = 0; i < 44; i++) d0[i] = (uint8_t)(0x10 + i);
    d1[0] = 0x99;
    for (int i = 0; i < 20; i++) d2[i] = (uint8_t)(0xC0 + i);
    arq_block_t in[3] = {
        { .seq = 200, .len = 44, .data = d0 },   /* max-size block          */
        { .seq = 201, .len = 1,  .data = d1 },   /* min-size block          */
        { .seq = 202, .len = 20, .data = d2 },
    };

    uint8_t buf[1280];
    int fs = arq_protocol_build_data_blocks(buf, sizeof(buf), 0x7E, 9,
                                            ARQ_FLAG_HAS_DATA | 2, 0, /*epoch*/ 3,
                                            in, 3);
    TEST_ASSERT_EQUAL_INT(ARQ_FRAME_HDR_SIZE + (2 + 44) + (2 + 1) + (2 + 20), fs);
    /* pad to DATAC1's payload (510) as send_data_burst does; parse must ignore it */
    for (int i = fs; i < 510; i++) buf[i] = 0xEE;

    arq_frame_hdr_t hdr;
    arq_block_t out[ARQ_MAX_BLOCKS_PER_FRAME];
    int nb = arq_protocol_parse_data_blocks(buf, 510, &hdr, out,
                                            ARQ_MAX_BLOCKS_PER_FRAME);
    TEST_ASSERT_EQUAL_INT(3, nb);
    TEST_ASSERT_EQUAL_UINT8(ARQ_SUBTYPE_DATA, hdr.subtype);
    TEST_ASSERT_EQUAL_UINT8(9, hdr.rx_ack_seq);
    TEST_ASSERT_EQUAL_UINT8(2, hdr.burst_remaining);
    TEST_ASSERT_EQUAL_UINT8(3, hdr.ack_delay_raw);   /* fast-ACK keydown epoch (byte 7) */
    TEST_ASSERT_TRUE(hdr.flags & ARQ_FLAG_HAS_DATA);
    TEST_ASSERT_EQUAL_UINT8(200, out[0].seq);
    TEST_ASSERT_EQUAL_INT(44, out[0].len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(d0, out[0].data, 44);
    TEST_ASSERT_EQUAL_UINT8(201, out[1].seq);
    TEST_ASSERT_EQUAL_INT(1, out[1].len);
    TEST_ASSERT_EQUAL_UINT8(0x99, out[1].data[0]);
    TEST_ASSERT_EQUAL_UINT8(202, out[2].seq);
    TEST_ASSERT_EQUAL_INT(20, out[2].len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(d2, out[2].data, 20);
}

/* A block with len > ARQ_BLOCK_DATA_FLOOR (the largest a rung-0 floor block may
 * be) must be rejected by the builder. */
void test_data_blocks_rejects_oversize(void)
{
    uint8_t big[128] = {0};
    arq_block_t in = { .seq = 1, .len = ARQ_BLOCK_DATA_FLOOR + 1, .data = big };
    uint8_t buf[1280];
    TEST_ASSERT_EQUAL_INT(-1,
        arq_protocol_build_data_blocks(buf, sizeof(buf), 0x1, 0, 0, 0, 0, &in, 1));
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
    RUN_TEST(test_data_burst_remaining_roundtrip);
    RUN_TEST(test_build_sack_roundtrip);
    RUN_TEST(test_data_blocks_roundtrip);
    RUN_TEST(test_data_blocks_rejects_oversize);
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
