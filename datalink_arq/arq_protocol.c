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
#include <math.h>

#include "../common/virtual_clock.h"

/* Runtime-configurable retry counts, initialised from compile-time defaults */
_Atomic int arq_call_retry_slots       = ARQ_CALL_RETRY_SLOTS_DEFAULT;
_Atomic int arq_accept_retry_slots     = ARQ_ACCEPT_RETRY_SLOTS_DEFAULT;
_Atomic int arq_disconnect_retry_slots = ARQ_DISCONNECT_RETRY_SLOTS_DEFAULT;
_Atomic int arq_retry_stagger_ms       = ARQ_RETRY_STAGGER_MS_DEFAULT;
_Atomic int arq_disconnect_drain_timeout_s = ARQ_DISCONNECT_DRAIN_TIMEOUT_S_DEFAULT;


/* Include FreeDV mode constants */
#include "../modem/freedv/freedv_api.h"
#include "../modem/modem_mfsk.h"   /* MERCURY_MODE_MFSK */

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
 * frame_duration_s, measured on the bench (single-frame burst incl.
 * preamble): DATAC16 3740 ms, DATAC15 4400, DATAC4 5800, DATAC3 3820,
 * DATAC1 4810, DATAC17 7400, QAM16C2 3700.
 *
 * retry_interval_s: the wait before a CALL, ACCEPT or DISCONNECT sent in
 * this mode is repeated -- the frame, the peer's answer (about 4 s back on
 * DATAC16) and a margin.  The CALL interval on DATAC16 is the CALLINT
 * default; MFSK's is arq_protocol_call_interval_for_mode_s().
 *
 * DATAC13 has no row: it is no longer an ARQ mode, and a row would collide
 * with DATAC16's 14-byte payload where modes are told apart by frame size.
 * ====================================================================== */

const arq_mode_timing_t arq_mode_table[] = {
    /*  freedv_mode           frame_dur  retry_interval  payload */
    {  FREEDV_MODE_DATAC16,   3.74f,     8.0f,           14   },
    {  FREEDV_MODE_DATAC15,   4.40f,     12.0f,          30   },
    {  FREEDV_MODE_DATAC4,    5.80f,     14.0f,          54   },
    {  FREEDV_MODE_DATAC3,    3.82f,     12.0f,          126  },
    {  FREEDV_MODE_DATAC1,    4.81f,     13.0f,          510  },
    {  FREEDV_MODE_DATAC17,   7.40f,     15.0f,          1180 },
    {  FREEDV_MODE_QAM16C2,   3.70f,     12.0f,          1213 },
    /* The MFSK fringe rung: non-coherent 32-MFSK, a ~13.5 s burst of 98
     * payload bytes, about 10 dB below DATAC15.  Its row also sets the
     * longest burst, which sizes the modem's RX backlog: shorter, and the
     * backlog cap chopped MFSK bursts in half (mfsk-margin, 7feaadb). */
    {  MERCURY_MODE_MFSK,     13.50f,    18.0f,          98   },
};

const int arq_mode_table_count =
    (int)(sizeof(arq_mode_table) / sizeof(arq_mode_table[0]));

/* ======================================================================
 * Mode timing lookup
 * ====================================================================== */

/* Runtime-configurable CALL/ACCEPT retry interval in seconds (set via
 * CALLINT TCP command).  0.0 = use compiled default from arq_mode_table.
 * Only affects CALL/ACCEPT retry scheduling: the DISCONNECT uses the
 * immutable table values. */
_Atomic float arq_callint_override_s = 0.0f;

const arq_mode_timing_t *arq_protocol_mode_timing(int freedv_mode)
{
    for (int i = 0; i < arq_mode_table_count; i++)
    {
        if (arq_mode_table[i].freedv_mode == freedv_mode)
            return &arq_mode_table[i];
    }
    return NULL;
}

float arq_protocol_longest_burst_s(void)
{
    float longest_s = 0.0f;
    for (int i = 0; i < arq_mode_table_count; i++)
    {
        float burst_s = arq_mode_table[i].frame_duration_s;
        if (burst_s > longest_s)
            longest_s = burst_s;
    }
    /* Unreadable table: fall back to the slowest rung we ship (DATAC17). */
    return (longest_s > 0.0f) ? longest_s : 7.4f;
}

float arq_protocol_call_interval_s(void)
{
    float override = atomic_load(&arq_callint_override_s);
    if (override > 0.0f)
        return override;

    const arq_mode_timing_t *tm = arq_protocol_mode_timing(ARQ_CONTROL_MODE);
    return tm ? tm->retry_interval_s : 8.0f;
}

float arq_protocol_call_interval_for_mode_s(int mode)
{
    float override = atomic_load(&arq_callint_override_s);
    if (override > 0.0f)
        return override;
    if (mode == MERCURY_MODE_MFSK) {
        const arq_mode_timing_t *tm = arq_protocol_mode_timing(mode);
        return (tm ? tm->frame_duration_s : 13.5f) + 10.5f;
    }
    return arq_protocol_call_interval_s();
}

int arq_protocol_retry_rank(const char *local_call, const char *remote_call)
{
    if (!local_call || !remote_call || !local_call[0] || !remote_call[0])
        return -1;  /* missing identity — cannot decide, random fallback */
    int c = strcmp(local_call, remote_call);
    if (c == 0)
        return -1;  /* identical callsigns — misconfigured pair, random fallback */
    return (c > 0) ? 1 : 0;
}

uint64_t arq_protocol_retry_deadline_ms(float seconds, int rank)
{
    uint64_t now_ms = time_now_ms();
    uint64_t base_ms = (uint64_t)(seconds * 1000.0f + 0.5f);
    int stagger_ms = atomic_load(&arq_retry_stagger_ms);
    if (stagger_ms <= 0 || rank == 0)
        return now_ms + base_ms;

    if (rank == 1)
        return now_ms + base_ms + (uint64_t)stagger_ms;

    /* rank < 0: degenerate pair — random fallback over [0, stagger_ms). */
    uint64_t x = now_ms ^ 0x9E3779B97F4A7C15ULL;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;

    return now_ms + base_ms + (x % (uint64_t)stagger_ms);
}

/* ======================================================================
 * Frame header codec
 * ====================================================================== */

int arq_protocol_encode_hdr(uint8_t *buf, size_t buf_len, const arq_frame_hdr_t *hdr)
{
    if (!buf || !hdr || buf_len < ARQ_FRAME_HDR_SIZE)
        return -1;

    /* byte 0 (framer: extension + packet_type) is set by write_frame_header() */
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

    hdr->packet_type   = frame_header_packet_type(buf[0]);
    hdr->frame_ext     = frame_header_extension(buf[0]);
    hdr->subtype       = buf[ARQ_HDR_SUBTYPE_IDX];
    hdr->flags         = buf[ARQ_HDR_FLAGS_IDX];
    hdr->session_id    = buf[ARQ_HDR_SESSION_IDX];
    hdr->tx_seq        = buf[ARQ_HDR_SEQ_IDX];
    hdr->rx_ack_seq    = buf[ARQ_HDR_ACK_IDX];
    hdr->snr_raw       = buf[ARQ_HDR_SNR_IDX];
    hdr->ack_delay_raw = buf[ARQ_HDR_DELAY_IDX];
    return 0;
}

uint8_t arq_protocol_bw_token_from_hz(int bw_hz)
{
    switch (bw_hz)
    {
    case ARQ_BANDWIDTH_NARROW_HZ:
        return ARQ_BW_TOKEN_500;
    case ARQ_BANDWIDTH_FULL_HZ:
        return ARQ_BW_TOKEN_2300;
    case ARQ_BANDWIDTH_TACTICAL_HZ:
        return ARQ_BW_TOKEN_2750;
    default:
        return ARQ_BW_TOKEN_NONE;
    }
}

int arq_protocol_bw_hz_from_token(uint8_t bw_token)
{
    switch (bw_token)
    {
    case ARQ_BW_TOKEN_500:
        return ARQ_BANDWIDTH_NARROW_HZ;
    case ARQ_BW_TOKEN_2300:
        return ARQ_BANDWIDTH_FULL_HZ;
    case ARQ_BW_TOKEN_2750:
        return ARQ_BANDWIDTH_TACTICAL_HZ;
    default:
        return 0;
    }
}

/* ======================================================================
 * SNR codec
 *
 * Encoding: snr_raw = clamp(round(snr_db) + 128, 1, 255); 0 = unknown.
 * Decoding: snr_db  = (float)(snr_raw - 128).
 * ====================================================================== */

uint8_t arq_protocol_encode_snr(float snr_db)
{
    /* floorf: plain (int) truncates toward zero, so negative SNRs rounded
     * asymmetrically (-1.0 dB encoded as 0 dB) — a +1 dB flattery right at
     * the fade cliff where mode decisions are most sensitive. */
    int v = (int)floorf(snr_db + 0.5f) + 128;
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
 * DISCONNECT: the one plain session frame -- an 8-byte header on the
 * control mode.  A session's other frames are the carousel's.
 * ====================================================================== */

int arq_protocol_build_disconnect(uint8_t *buf, size_t buf_len,
                                   uint8_t session_id, uint8_t snr_raw)
{
    if (!buf || buf_len < ARQ_FRAME_HDR_SIZE)
        return -1;

    memset(buf, 0, ARQ_FRAME_HDR_SIZE);
    buf[ARQ_HDR_SUBTYPE_IDX] = ARQ_SUBTYPE_DISCONNECT;
    buf[ARQ_HDR_SESSION_IDX] = session_id;
    buf[ARQ_HDR_SNR_IDX]     = snr_raw;
    write_frame_header(buf, PACKET_TYPE_ARQ_CONTROL, 0);
    return ARQ_FRAME_HDR_SIZE;
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

/* CALL payload layout (frame bytes 2-13, i.e. 12 payload bytes; the ACCEPT's
 * is in arq_protocol.h):
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
    /* Refuse rather than truncate.  A truncated arithmetic code is not a
     * shortened callsign -- it decodes to a DIFFERENT string at the far end,
     * so the peer either answers a call from a station that does not exist or
     * silently drops one that does.  Failing here makes the operator's
     * over-long callsign visible instead of turning it into a wrong one. */
    if ((size_t)enc_len > src_cap)
        return -1;
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

static int encode_callsign_only_payload(const char *src, uint8_t *out, size_t out_cap)
{
    uint8_t tmp[4096];
    int enc_len;

    if (!src || !out || out_cap == 0)
        return -1;

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

    /* Same reasoning as encode_callsign_payload: a truncated arithmetic code
     * decodes to a different callsign, so refuse it. */
    if ((size_t)enc_len > out_cap)
        return -1;
    memcpy(out, tmp, (size_t)enc_len);
    return enc_len;
}

static int decode_callsign_only_payload(const uint8_t *in, size_t in_len, char *src_out)
{
    if (!in || !src_out || in_len == 0)
        return -1;

    init_model();
    if (arithmetic_decode((uint8_t *)in, (int)in_len, src_out, CALLSIGN_MAX_SIZE) < 0)
        return -1;
    src_out[CALLSIGN_MAX_SIZE - 1] = 0;
    return 0;
}

static uint8_t connect_ext(uint8_t bw_token)
{
    return (uint8_t)(bw_token | (ARQ_CONNECT_VERSION << ARQ_CONNECT_EXT_VERSION_SHIFT) |
                     ARQ_CONNECT_EXT_CAROUSEL);
}

static void put_crc16(uint8_t *p, uint16_t crc) { p[0] = (uint8_t)crc; p[1] = (uint8_t)(crc >> 8); }
static uint16_t get_crc16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

bool arq_protocol_connect_version_ok(const uint8_t *buf, size_t buf_len)
{
    if (!buf || buf_len < ARQ_CONTROL_FRAME_SIZE ||
        frame_header_packet_type(buf[0]) != PACKET_TYPE_ARQ_CALL)
        return false;
    uint8_t ext = frame_header_extension(buf[0]);
    return (ext & ARQ_CONNECT_EXT_CAROUSEL) &&
           ((ext & ARQ_CONNECT_EXT_VERSION_MASK) >> ARQ_CONNECT_EXT_VERSION_SHIFT) == ARQ_CONNECT_VERSION;
}

static int connect_bw_hz(const uint8_t *buf)
{
    return arq_protocol_bw_hz_from_token(frame_header_extension(buf[0]) & ARQ_CONNECT_EXT_BW_MASK);
}

int arq_protocol_build_call(uint8_t *buf, size_t buf_len,
                            uint8_t session_id,
                            const char *caller, const char *called,
                            int bw_hz)
{
    uint8_t encoded[ARQ_CONNECT_MAX_ENCODED];
    if (!buf || !caller || !called || buf_len < ARQ_CONTROL_FRAME_SIZE)
        return -1;
    uint8_t bw_token = arq_protocol_bw_token_from_hz(bw_hz);
    if (bw_token == ARQ_BW_TOKEN_NONE)
        return -1;
    /* [CRC16 of called][caller's callsign, arithmetic-coded] */
    int enc_len = encode_callsign_payload(caller, called, encoded, sizeof(encoded));
    if (enc_len <= 0)
        return -1;
    memset(buf, 0, ARQ_CONTROL_FRAME_SIZE);
    buf[ARQ_CONNECT_SESSION_IDX] = (uint8_t)(session_id & ARQ_CONNECT_SESSION_MASK);
    memcpy(buf + ARQ_CONNECT_PAYLOAD_IDX, encoded, (size_t)enc_len);
    write_frame_header(buf, PACKET_TYPE_ARQ_CALL, connect_ext(bw_token));
    return ARQ_CONTROL_FRAME_SIZE;
}

int arq_protocol_build_accept(uint8_t *buf, size_t buf_len,
                              uint8_t session_id,
                              const char *called, const char *caller,
                              int bw_hz, int start_level, uint16_t nonce)
{
    if (!buf || !called || !caller || buf_len < ARQ_CONTROL_FRAME_SIZE ||
        start_level < 0 || start_level > ARQ_ACCEPT_LEVEL_CTL_DEAF)
        return -1;
    uint8_t bw_token = arq_protocol_bw_token_from_hz(bw_hz);
    if (bw_token == ARQ_BW_TOKEN_NONE)
        return -1;
    memset(buf, 0, ARQ_CONTROL_FRAME_SIZE);
    buf[ARQ_CONNECT_SESSION_IDX] = (uint8_t)((session_id & ARQ_CONNECT_SESSION_MASK) |
                                             ARQ_CONNECT_ACCEPT_FLAG);
    put_crc16(buf + ARQ_CONNECT_PAYLOAD_IDX, arq_protocol_callsign_crc16(caller));
    put_crc16(buf + ARQ_ACCEPT_CALLED_CRC_IDX, arq_protocol_callsign_crc16(called));
    put_crc16(buf + ARQ_ACCEPT_NONCE_IDX, nonce);
    buf[ARQ_ACCEPT_LEVEL_IDX] = (uint8_t)start_level;
    write_frame_header(buf, PACKET_TYPE_ARQ_CALL, connect_ext(bw_token));
    return ARQ_CONTROL_FRAME_SIZE;
}

bool arq_protocol_connect_dst_matches(const uint8_t *buf, const char *callsign)
{
    return get_crc16(buf + ARQ_CONNECT_PAYLOAD_IDX) == arq_protocol_callsign_crc16(callsign);
}

int arq_protocol_parse_call(const uint8_t *buf, size_t buf_len,
                            uint8_t *session_id_out, char *caller_out,
                            int *bw_hz_out)
{
    char unused[CALLSIGN_MAX_SIZE];
    if (!session_id_out || !caller_out || !bw_hz_out ||
        !arq_protocol_connect_version_ok(buf, buf_len) ||
        (buf[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_ACCEPT_FLAG))
        return -1;
    *bw_hz_out = connect_bw_hz(buf);
    if (*bw_hz_out == 0)
        return -1;
    *session_id_out = buf[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_SESSION_MASK;
    return decode_callsign_payload(buf + ARQ_CONNECT_PAYLOAD_IDX, ARQ_CONNECT_MAX_ENCODED,
                                   caller_out, unused);
}

int arq_protocol_parse_accept(const uint8_t *buf, size_t buf_len, arq_accept_t *out)
{
    if (!out || !arq_protocol_connect_version_ok(buf, buf_len) ||
        !(buf[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_ACCEPT_FLAG))
        return -1;
    /* The rung is 3 bits, the rest of byte 8 and bytes 9-13 are zero: a
     * frame that passed its CRC with any of them set is not an ACCEPT. */
    if (buf[ARQ_ACCEPT_LEVEL_IDX] > ARQ_ACCEPT_LEVEL_CTL_DEAF)
        return -1;
    for (int i = ARQ_ACCEPT_LEVEL_IDX + 1; i < ARQ_CONTROL_FRAME_SIZE; i++)
        if (buf[i])
            return -1;
    memset(out, 0, sizeof(*out));
    out->bw_hz = connect_bw_hz(buf);
    if (out->bw_hz == 0)
        return -1;
    out->session_id  = buf[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_SESSION_MASK;
    out->called_crc  = get_crc16(buf + ARQ_ACCEPT_CALLED_CRC_IDX);
    out->nonce       = get_crc16(buf + ARQ_ACCEPT_NONCE_IDX);
    out->start_level = buf[ARQ_ACCEPT_LEVEL_IDX];
    return 0;
}

int arq_protocol_build_cq(uint8_t *buf, size_t buf_len,
                           const char *src, int bw_hz)
{
    int enc_len;
    uint8_t bw_token;

    if (!buf || !src || buf_len < ARQ_CONTROL_FRAME_SIZE)
        return -1;

    bw_token = arq_protocol_bw_token_from_hz(bw_hz);
    if (bw_token == ARQ_BW_TOKEN_NONE)
        return -1;

    memset(buf, 0, ARQ_CONTROL_FRAME_SIZE);
    enc_len = encode_callsign_only_payload(src,
                                           buf + ARQ_CQ_PAYLOAD_IDX,
                                           ARQ_CQ_SRC_MAX_ENCODED);
    if (enc_len <= 0)
        return -1;

    write_frame_header(buf, PACKET_TYPE_ARQ_CQ, bw_token);
    return ARQ_CONTROL_FRAME_SIZE;
}

int arq_protocol_parse_cq(const uint8_t *buf, size_t buf_len,
                           char *src_out, int *bw_hz_out)
{
    if (!buf || !src_out || !bw_hz_out || buf_len < ARQ_CONTROL_FRAME_SIZE)
        return -1;

    *bw_hz_out = arq_protocol_bw_hz_from_token(frame_header_extension(buf[0]));
    if (*bw_hz_out == 0)
        return -1;

    return decode_callsign_only_payload(buf + ARQ_CQ_PAYLOAD_IDX,
                                        ARQ_CQ_SRC_MAX_ENCODED,
                                        src_out);
}
