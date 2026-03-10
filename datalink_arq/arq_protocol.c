/* HERMES Modem — ARQ Protocol: mode timing table and frame codec
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_protocol.h"

#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

/* Runtime-configurable retry counts, initialised from compile-time defaults */
_Atomic int arq_call_retry_slots       = ARQ_CALL_RETRY_SLOTS_DEFAULT;
_Atomic int arq_accept_retry_slots     = ARQ_ACCEPT_RETRY_SLOTS_DEFAULT;
_Atomic int arq_data_retry_slots       = ARQ_DATA_RETRY_SLOTS_DEFAULT;
_Atomic int arq_disconnect_retry_slots = ARQ_DISCONNECT_RETRY_SLOTS_DEFAULT;

/* Include FreeDV mode constants */
#include "../modem/freedv/freedv_api.h"

/* Framer layer (write_frame_header, PACKET_TYPE_* constants) */
#include "../modem/framer.h"

/* Arithmetic encoder for callsign compression */
extern void init_model(void);
extern int  arithmetic_encode(const char *msg, uint8_t *output);
extern int  arithmetic_decode(uint8_t *input, int max_len, char *output, int max_output);

/* CALLSIGN_MAX_SIZE from arq.h */
#include "arq.h"

/* ======================================================================
 * Mode timing table
 *
 * All durations measured empirically from bench and OTA tests.
 * ack_timeout_s is set at TX_COMPLETE (PTT-OFF) and must cover the worst
 * case where the peer piggybbacks a DATA frame immediately after its ACK:
 *
 *   ack_timeout ≥ ACK_return + inter_frame_gap + piggybacked_DATA_dur + margin
 *
 * Measured constants (bench test, dummy loads):
 *   ACK_return:      2848ms (PTT-OFF → ack_rx, mode-independent — ACK always DATAC13)
 *   inter_frame_gap: ~1035ms (IRS ACK PTT-OFF → IRS DATA PTT-ON)
 *   frame_dur DATAC4:  5800ms  (tx_start → tx_end, measured)
 *   frame_dur DATAC3:  3820ms  (measured)
 *   frame_dur DATAC1:  4810ms  (measured — table previously had wrong 6500ms)
 *
 *   DATAC13: no piggybacking (control-only) → 2848 + 1500ms margin ≈ 4.3s → 6s
 *   DATAC4:  2848 + 1035 + 5800 + 1500ms margin ≈ 11.2s → 12s
 *   DATAC3:  2848 + 1035 + 3820 + 1500ms margin ≈  9.2s →  9s
 *   DATAC1:  2848 + 1035 + 4810 + 1500ms margin ≈ 10.2s → 11s
 *
 * retry_interval_s = ack_timeout_s + ARQ_ACK_GUARD_S (1s)
 * ====================================================================== */

const arq_mode_timing_t arq_mode_table[] = {
    /*  freedv_mode           frame_dur  tx_period  ack_timeout  retry_interval  payload_bytes */
    {  FREEDV_MODE_DATAC13,   2.50f,     1.0f,      6.0f,        7.0f,           14 },
    {  FREEDV_MODE_DATAC4,    5.80f,     1.0f,      12.0f,       13.0f,          54 },
    {  FREEDV_MODE_DATAC3,    3.82f,     1.0f,       9.0f,       10.0f,          126 },
    {  FREEDV_MODE_DATAC1,    4.81f,     1.0f,      11.0f,       12.0f,          510 },
};

const int arq_mode_table_count =
    (int)(sizeof(arq_mode_table) / sizeof(arq_mode_table[0]));

/* ======================================================================
 * Mode timing lookup
 * ====================================================================== */

const arq_mode_timing_t *arq_protocol_mode_timing(int freedv_mode)
{
    for (int i = 0; i < arq_mode_table_count; i++)
    {
        if (arq_mode_table[i].freedv_mode == freedv_mode)
            return &arq_mode_table[i];
    }
    return NULL;
}

/* ======================================================================
 * Frame header codec
 * TODO Phase 2: implement encode/decode
 * ====================================================================== */

int arq_protocol_encode_hdr(uint8_t *buf, size_t buf_len, const arq_frame_hdr_t *hdr)
{
    if (!buf || !hdr || buf_len < ARQ_FRAME_HDR_SIZE)
        return -1;

    /* byte 0 (framer: CRC6 + packet_type) is set by write_frame_header() */
    buf[ARQ_HDR_SUBTYPE_IDX]  = hdr->subtype;
    buf[ARQ_HDR_FLAGS_IDX]    = hdr->flags;
    buf[ARQ_HDR_SESSION_IDX]  = hdr->session_id;
    buf[ARQ_HDR_SEQ_IDX]      = hdr->tx_seq;
    buf[ARQ_HDR_ACK_IDX]      = hdr->rx_ack_seq;
    buf[ARQ_HDR_SNR_IDX]      = hdr->snr_raw;
    buf[ARQ_HDR_DELAY_IDX]    = hdr->ack_delay_raw;
    return 0;
}

int arq_protocol_decode_hdr(const uint8_t *buf, size_t buf_len, arq_frame_hdr_t *hdr)
{
    if (!buf || !hdr || buf_len < ARQ_FRAME_HDR_SIZE)
        return -1;

    hdr->packet_type   = (buf[0] >> PACKET_TYPE_SHIFT) & PACKET_TYPE_MASK;
    hdr->subtype       = buf[ARQ_HDR_SUBTYPE_IDX];
    hdr->flags         = buf[ARQ_HDR_FLAGS_IDX];
    hdr->session_id    = buf[ARQ_HDR_SESSION_IDX];
    hdr->tx_seq        = buf[ARQ_HDR_SEQ_IDX];
    hdr->rx_ack_seq    = buf[ARQ_HDR_ACK_IDX];
    hdr->snr_raw       = buf[ARQ_HDR_SNR_IDX];
    hdr->ack_delay_raw = buf[ARQ_HDR_DELAY_IDX];
    return 0;
}

/* ======================================================================
 * SNR codec
 *
 * Encoding: snr_raw = clamp(round(snr_db) + 128, 1, 255); 0 = unknown.
 * Decoding: snr_db  = (float)(snr_raw - 128).
 * ====================================================================== */

uint8_t arq_protocol_encode_snr(float snr_db)
{
    int v = (int)(snr_db + 0.5f) + 128;
    if (v < 1)   v = 1;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

float arq_protocol_decode_snr(uint8_t snr_raw)
{
    if (snr_raw == 0)
        return 0.0f;  /* unknown */
    return (float)((int)snr_raw - 128);
}

/* ======================================================================
 * ACK delay codec
 *
 * Wire value is in 10ms units (uint16_t big-endian).
 * 0 = unknown.
 * ====================================================================== */

uint8_t arq_protocol_encode_ack_delay(uint32_t delay_ms)
{
    uint32_t units = delay_ms / 10;
    if (units == 0 && delay_ms > 0)
        units = 1;  /* round up from sub-10ms */
    if (units > 0xff)
        units = 0xff;
    return (uint8_t)units;
}

uint32_t arq_protocol_decode_ack_delay(uint8_t raw)
{
    return (uint32_t)raw * 10;
}

/* ======================================================================
 * Internal helpers
 * ====================================================================== */

/* Write a standard 8-byte control frame header; return frame size or -1. */
static int build_ctrl(uint8_t *buf, size_t buf_len,
                      uint8_t subtype, uint8_t session_id,
                      uint8_t tx_seq, uint8_t rx_ack_seq,
                      uint8_t flags, uint8_t snr_raw, uint8_t ack_delay_raw,
                      const uint8_t *payload, size_t payload_len)
{
    size_t total = ARQ_FRAME_HDR_SIZE + payload_len;
    if (buf_len < total)
        return -1;

    memset(buf, 0, total);
    /* byte 0 set by write_frame_header below */
    buf[ARQ_HDR_SUBTYPE_IDX]  = subtype;
    buf[ARQ_HDR_FLAGS_IDX]    = flags;
    buf[ARQ_HDR_SESSION_IDX]  = session_id;
    buf[ARQ_HDR_SEQ_IDX]      = tx_seq;
    buf[ARQ_HDR_ACK_IDX]      = rx_ack_seq;
    buf[ARQ_HDR_SNR_IDX]      = snr_raw;
    buf[ARQ_HDR_DELAY_IDX]    = ack_delay_raw;
    if (payload && payload_len > 0)
        memcpy(buf + ARQ_FRAME_HDR_SIZE, payload, payload_len);
    write_frame_header(buf, PACKET_TYPE_ARQ_CONTROL, total);
    return (int)total;
}

/* ======================================================================
 * Frame builder implementations
 * ====================================================================== */

int arq_protocol_build_ack(uint8_t *buf, size_t buf_len,
                            uint8_t session_id, uint8_t rx_ack_seq,
                            uint8_t flags, uint8_t snr_raw,
                            uint8_t ack_delay_raw)
{
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_ACK, session_id,
                      0, rx_ack_seq, flags, snr_raw, ack_delay_raw,
                      NULL, 0);
}

int arq_protocol_build_disconnect(uint8_t *buf, size_t buf_len,
                                   uint8_t session_id, uint8_t snr_raw)
{
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_DISCONNECT, session_id,
                      0, 0, 0, snr_raw, 0,
                      NULL, 0);
}

int arq_protocol_build_keepalive(uint8_t *buf, size_t buf_len,
                                  uint8_t session_id, uint8_t snr_raw)
{
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_KEEPALIVE, session_id,
                      0, 0, 0, snr_raw, 0,
                      NULL, 0);
}

int arq_protocol_build_keepalive_ack(uint8_t *buf, size_t buf_len,
                                      uint8_t session_id, uint8_t snr_raw)
{
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_KEEPALIVE_ACK, session_id,
                      0, 0, 0, snr_raw, 0,
                      NULL, 0);
}

int arq_protocol_build_turn_req(uint8_t *buf, size_t buf_len,
                                 uint8_t session_id, uint8_t rx_ack_seq,
                                 uint8_t snr_raw)
{
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_TURN_REQ, session_id,
                      0, rx_ack_seq, 0, snr_raw, 0,
                      NULL, 0);
}

int arq_protocol_build_turn_ack(uint8_t *buf, size_t buf_len,
                                 uint8_t session_id, uint8_t snr_raw)
{
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_TURN_ACK, session_id,
                      0, 0, 0, snr_raw, 0,
                      NULL, 0);
}

int arq_protocol_build_mode_req(uint8_t *buf, size_t buf_len,
                                 uint8_t session_id, uint8_t snr_raw,
                                 int freedv_mode)
{
    uint8_t payload[1] = { (uint8_t)freedv_mode };
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_MODE_REQ, session_id,
                      0, 0, 0, snr_raw, 0,
                      payload, 1);
}

int arq_protocol_build_mode_ack(uint8_t *buf, size_t buf_len,
                                 uint8_t session_id, uint8_t snr_raw,
                                 int freedv_mode)
{
    uint8_t payload[1] = { (uint8_t)freedv_mode };
    return build_ctrl(buf, buf_len,
                      ARQ_SUBTYPE_MODE_ACK, session_id,
                      0, 0, 0, snr_raw, 0,
                      payload, 1);
}

int arq_protocol_build_data(uint8_t *buf, size_t buf_len,
                             uint8_t session_id, uint8_t tx_seq,
                             uint8_t rx_ack_seq, uint8_t flags,
                             uint8_t snr_raw, uint8_t payload_valid,
                             const uint8_t *payload, size_t payload_len)
{
    size_t total = ARQ_FRAME_HDR_SIZE + payload_len;
    if (buf_len < total || !payload || payload_len == 0)
        return -1;

    memset(buf, 0, ARQ_FRAME_HDR_SIZE);
    buf[ARQ_HDR_SUBTYPE_IDX]  = ARQ_SUBTYPE_DATA;
    buf[ARQ_HDR_FLAGS_IDX]    = flags;
    buf[ARQ_HDR_SESSION_IDX]  = session_id;
    buf[ARQ_HDR_SEQ_IDX]      = tx_seq;
    buf[ARQ_HDR_ACK_IDX]      = rx_ack_seq;
    buf[ARQ_HDR_SNR_IDX]      = snr_raw;
    buf[ARQ_HDR_DELAY_IDX]    = payload_valid;   /* 0=full, else=valid bytes */
    memcpy(buf + ARQ_FRAME_HDR_SIZE, payload, payload_len);
    write_frame_header(buf, PACKET_TYPE_ARQ_DATA, total);
    return (int)total;
}

/* ======================================================================
 * CALL/ACCEPT frame builders and parsers
 * ====================================================================== */

/* CRC16-CCITT of uppercase-normalised callsign — for DST field validation. */
uint16_t arq_protocol_callsign_crc16(const char *callsign)
{
    char upper[CALLSIGN_MAX_SIZE];
    int n = 0;
    for (; callsign[n] && n < (int)sizeof(upper) - 1; n++)
    {
        char c = callsign[n];
        if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
        upper[n] = c;
    }
    upper[n] = 0;
    return freedv_gen_crc16((unsigned char *)upper, n);
}

/* CALL/ACCEPT payload layout (frame bytes 2-13, i.e. 12 payload bytes):
 *   Frame bytes 2-3  (payload [0..1]):  CRC16-CCITT of DST callsign, LE  (ARQ_CONNECT_DST_CRC_SIZE)
 *   Frame bytes 4-13 (payload [2..11]): arithmetic_encode(SRC only)       (ARQ_CONNECT_SRC_MAX_ENCODED = 10)
 *
 * 10 bytes encodes up to ~14 characters (38-symbol alphabet, ~5.25 bits/sym).
 * Callsigns longer than 14 characters will fail to encode. */
static int encode_callsign_payload(const char *src, const char *dst,
                                   uint8_t *out, size_t out_cap)
{
    uint8_t tmp[4096];
    int enc_len;

    if (out_cap < ARQ_CONNECT_DST_CRC_SIZE)
        return -1;

    /* Bytes [0..1]: CRC16 of DST, little-endian */
    uint16_t crc = arq_protocol_callsign_crc16(dst);
    out[0] = (uint8_t)(crc & 0xFF);
    out[1] = (uint8_t)(crc >> 8);

    /* Bytes [2..]: arithmetic_encode(uppercase SRC) */
    char src_upper[CALLSIGN_MAX_SIZE];
    int n = 0;
    for (; src[n] && n < (int)sizeof(src_upper) - 1; n++)
    {
        char c = src[n];
        if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
        src_upper[n] = c;
    }
    src_upper[n] = 0;

    init_model();
    enc_len = arithmetic_encode(src_upper, tmp);
    if (enc_len <= 0)
        return -1;

    size_t src_cap = out_cap - ARQ_CONNECT_DST_CRC_SIZE;
    if ((size_t)enc_len > src_cap)
        enc_len = (int)src_cap;
    memcpy(out + ARQ_CONNECT_DST_CRC_SIZE, tmp, (size_t)enc_len);
    return (int)(ARQ_CONNECT_DST_CRC_SIZE + (size_t)enc_len);
}

/* Decode: bytes [2..] are arithmetic-encoded SRC only.
 * DST is not in the frame; caller validates via CRC at bytes [0..1]. */
static int decode_callsign_payload(const uint8_t *in, size_t in_len,
                                   char *src_out, char *dst_out)
{
    if (in_len < ARQ_CONNECT_DST_CRC_SIZE)
        return -1;

    dst_out[0] = 0;  /* DST not transmitted as string */

    init_model();
    if (arithmetic_decode((uint8_t *)(in + ARQ_CONNECT_DST_CRC_SIZE),
                          (int)(in_len - ARQ_CONNECT_DST_CRC_SIZE),
                          src_out, CALLSIGN_MAX_SIZE) < 0)
        return -1;
    src_out[CALLSIGN_MAX_SIZE - 1] = 0;
    return 0;
}

static int build_call_accept(uint8_t *buf, size_t buf_len, bool is_accept,
                              uint8_t session_id,
                              const char *src, const char *dst)
{
    uint8_t encoded[ARQ_CONNECT_MAX_ENCODED];
    int enc_len;

    if (!buf || !src || !dst || buf_len < ARQ_CONTROL_FRAME_SIZE)
        return -1;

    enc_len = encode_callsign_payload(src, dst, encoded, sizeof(encoded));
    if (enc_len <= 0)
        return -1;

    memset(buf, 0, ARQ_CONTROL_FRAME_SIZE);
    buf[ARQ_CONNECT_SESSION_IDX] =
        (uint8_t)((session_id & ARQ_CONNECT_SESSION_MASK) |
                  (is_accept ? ARQ_CONNECT_ACCEPT_FLAG : 0));
    memcpy(buf + ARQ_CONNECT_PAYLOAD_IDX, encoded, (size_t)enc_len);
    write_frame_header(buf, PACKET_TYPE_ARQ_CALL, ARQ_CONTROL_FRAME_SIZE);
    return ARQ_CONTROL_FRAME_SIZE;
}

int arq_protocol_build_call(uint8_t *buf, size_t buf_len,
                             uint8_t session_id,
                             const char *src, const char *dst)
{
    return build_call_accept(buf, buf_len, false, session_id, src, dst);
}

int arq_protocol_build_accept(uint8_t *buf, size_t buf_len,
                               uint8_t session_id,
                               const char *src, const char *dst)
{
    return build_call_accept(buf, buf_len, true, session_id, src, dst);
}

static int parse_call_accept(const uint8_t *buf, size_t buf_len,
                              uint8_t *session_id_out,
                              char *src_out, char *dst_out)
{
    if (!buf || buf_len < ARQ_CONTROL_FRAME_SIZE ||
        !session_id_out || !src_out || !dst_out)
        return -1;

    *session_id_out = buf[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_SESSION_MASK;
    return decode_callsign_payload(buf + ARQ_CONNECT_PAYLOAD_IDX,
                                   ARQ_CONNECT_MAX_ENCODED,
                                   src_out, dst_out);
}

int arq_protocol_parse_call(const uint8_t *buf, size_t buf_len,
                             uint8_t *session_id_out,
                             char *src_out, char *dst_out)
{
    return parse_call_accept(buf, buf_len, session_id_out, src_out, dst_out);
}

int arq_protocol_parse_accept(const uint8_t *buf, size_t buf_len,
                               uint8_t *session_id_out,
                               char *src_out, char *dst_out)
{
    return parse_call_accept(buf, buf_len, session_id_out, src_out, dst_out);
}
