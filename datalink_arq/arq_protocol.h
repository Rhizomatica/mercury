/* HERMES Modem — ARQ Protocol: wire format, mode timing, codec API
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARQ_PROTOCOL_H_
#define ARQ_PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* ======================================================================
 * Protocol version (informational — not carried in wire frames)
 * ====================================================================== */

#define ARQ_PROTO_VERSION  4   /* v4: framer extension field, no proto_ver field on wire */

/* ======================================================================
 * Frame header layout (v4, 8 bytes total)
 *
 * Proto_ver field removed — both sides always run the same binary.
 * ack_delay reduced to 1 byte (10ms units, max 2.55s — covers all real delays).
 * HAS_SNR bit removed — snr_raw==0 already signals "unknown".
 *
 *  Byte 0: framer byte — set/read by write_frame_header()/parse_frame_header()
 *            bits [7:5] = packet_type (3 bits: PACKET_TYPE_ARQ_CONTROL=0, ARQ_DATA=1, ARQ_CALL=2)
 *            bits [4:0] = extension field (packet-type-specific)
 *  Byte 1: subtype      — arq_subtype_t
 *  Byte 2: flags        — bit7=TURN_REQ, bit6=HAS_DATA, bits[5:0]=spare
 *  Byte 3: session_id   — random byte chosen by caller at connect time
 *  Byte 4: tx_seq       — sender's frame sequence number
 *  Byte 5: rx_ack_seq   — last sequence number received from peer
 *  Byte 6: snr_raw      — local RX SNR feedback to peer; 0=unknown
 *                         encoded as uint8_t: (int)round(snr_dB) + 128, clamped 1-255
 *  Byte 7: ack_delay    — IRS→ISS: time from data_rx to ack_tx, in 10ms units; 0=unknown
 *                         ISS computes: OTA_RTT = (ack_rx_ms - data_tx_start_ms) - ack_delay×10
 *
 * CONNECT frames (CALL/ACCEPT) use a separate compact layout — see below.
 * They are identified by PACKET_TYPE_ARQ_CALL in the framer byte.
 *
 * Payload bytes (DATA frames only) follow immediately after byte 7.
 * ====================================================================== */

#define ARQ_HDR_SUBTYPE_IDX   1
#define ARQ_HDR_FLAGS_IDX     2
#define ARQ_HDR_SESSION_IDX   3
#define ARQ_HDR_SEQ_IDX       4
#define ARQ_HDR_ACK_IDX       5
#define ARQ_HDR_SNR_IDX       6
#define ARQ_HDR_DELAY_IDX     7
#define ARQ_FRAME_HDR_SIZE    8   /* bytes 0-7 inclusive */

/* CONNECT frames (CALL/ACCEPT) compact layout — 14 bytes, DATAC13 only.
 * Uses PACKET_TYPE_ARQ_CALL in the framer byte.
 *
 *  Byte 0: framer byte (PACKET_TYPE_ARQ_CALL | BW token, set by write_frame_header)
 *  Byte 1: connect_meta  = (session_id & 0x7F) | (is_accept ? 0x80 : 0x00)
 *  Bytes 2-3:  CRC16-CCITT of DST callsign (little-endian) — for local validation
 *  Bytes 4-13: arithmetic_encode(SRC callsign only) — 10 bytes, fits callsigns up to ~14 chars
 */
#define ARQ_CONNECT_SESSION_IDX       1
#define ARQ_CONNECT_PAYLOAD_IDX       2
#define ARQ_CONNECT_SESSION_MASK      0x7F
#define ARQ_CONNECT_ACCEPT_FLAG       0x80
#define ARQ_CONTROL_FRAME_SIZE        14
#define ARQ_CONNECT_META_SIZE         2   /* framer byte + connect_meta byte */
#define ARQ_CONNECT_MAX_ENCODED       (ARQ_CONTROL_FRAME_SIZE - ARQ_CONNECT_META_SIZE)
#define ARQ_CONNECT_DST_CRC_SIZE      2   /* CRC16-CCITT of DST at bytes [2..3], little-endian */
#define ARQ_CONNECT_SRC_MAX_ENCODED   (ARQ_CONNECT_MAX_ENCODED - ARQ_CONNECT_DST_CRC_SIZE) /* 10 */

/* Compact CQ frame layout — 14 bytes, DATAC13 only.
 * Uses PACKET_TYPE_ARQ_CQ in the framer byte.
 *
 *  Byte 0: framer byte (PACKET_TYPE_ARQ_CQ | BW token)
 *  Bytes 1-13: arithmetic_encode(SRC callsign only)
 */
#define ARQ_CQ_PAYLOAD_IDX            1
#define ARQ_CQ_SRC_MAX_ENCODED        (ARQ_CONTROL_FRAME_SIZE - ARQ_CQ_PAYLOAD_IDX) /* 13 */

/* BW token values carried in the framer byte extension field for ARQ_CALL/ARQ_CQ. */
#define ARQ_BW_TOKEN_NONE             0
#define ARQ_BW_TOKEN_500              1
#define ARQ_BW_TOKEN_2300             2
#define ARQ_BW_TOKEN_2750             3

/* ======================================================================
 * Flags byte (byte 2)
 * ====================================================================== */

#define ARQ_FLAG_TURN_REQ  0x80  /* bit 7: sender requests role turn          */
#define ARQ_FLAG_HAS_DATA  0x40  /* bit 6: sender has data queued (IRS→ISS)   */
#define ARQ_FLAG_LEN_HI    0x20  /* bit 5: DATA frames only — payload_valid    *
                                  * field carries bits [7:0] of valid byte     *
                                  * count; this flag carries bit 8, allowing   *
                                  * counts up to 511 (needed for DATAC1 which  *
                                  * has 502-byte payloads).                    */

/* ======================================================================
 * Frame subtypes
 * ====================================================================== */

typedef enum
{
    ARQ_SUBTYPE_CALL          =  1,
    ARQ_SUBTYPE_ACCEPT        =  2,
    ARQ_SUBTYPE_ACK           =  3,
    ARQ_SUBTYPE_DISCONNECT    =  4,
    ARQ_SUBTYPE_DATA          =  5,
    ARQ_SUBTYPE_KEEPALIVE     =  6,
    ARQ_SUBTYPE_KEEPALIVE_ACK =  7,
    ARQ_SUBTYPE_MODE_REQ      =  8,
    ARQ_SUBTYPE_MODE_ACK      =  9,
    ARQ_SUBTYPE_TURN_REQ      = 10,
    ARQ_SUBTYPE_TURN_ACK      = 11,
    /* Subtype 12 (FLOW_HINT) removed in v3 — replaced by HAS_DATA flag */
} arq_subtype_t;

/* ======================================================================
 * Parsed frame header (in-memory representation, not wire layout)
 * ====================================================================== */

typedef struct
{
    uint8_t  packet_type;   /* PACKET_TYPE_ARQ_CONTROL or _DATA   (from framer byte) */
    uint8_t  frame_ext;     /* low 5 bits of framer byte                          */
    uint8_t  subtype;       /* arq_subtype_t                                         */
    uint8_t  flags;         /* ARQ_FLAG_* bitmask                                    */
    uint8_t  session_id;
    uint8_t  tx_seq;
    uint8_t  rx_ack_seq;
    uint8_t  snr_raw;       /* 0=unknown; decode via arq_protocol_decode_snr         */
    uint8_t  ack_delay_raw; /* 0=unknown; 10ms units; decode via _decode_ack_delay   */
} arq_frame_hdr_t;

/* ======================================================================
 * Per-mode timing table
 *
 * All times are in seconds (float) measured from the moment PTT goes ON
 * unless noted otherwise.
 *
 * frame_duration_s: empirically measured on-air TX duration.
 * tx_period_s:      expected queue-to-PTT-ON latency (scheduling jitter).
 * ack_timeout_s:    maximum time from PTT-ON until ACK must be received.
 *                   ack_timeout ≥ frame_duration + propagation + ACK return
 *                   First frame deadline: enqueue_time + tx_period_s + ack_timeout_s
 *                   Retry deadline:       tx_start_ms  + ack_timeout_s
 * retry_interval_s: = ack_timeout_s + ARQ_ACK_GUARD_S
 * payload_bytes:    usable data bytes per frame.
 * ====================================================================== */

typedef struct
{
    int   freedv_mode;          /* FREEDV_MODE_* constant                        */
    float frame_duration_s;     /* measured TX duration                          */
    float tx_period_s;          /* queue-to-PTT-ON latency                       */
    float ack_timeout_s;        /* from PTT-ON to ACK deadline                   */
    float retry_interval_s;     /* ack_timeout_s + ACK_GUARD_S                   */
    int   payload_bytes;        /* usable payload per frame                      */
} arq_mode_timing_t;

/* Timing constants shared across modules */
#define ARQ_CHANNEL_GUARD_MS          700   /* IRS response guard after frame decode.
                                            * OFDM decode fires ~200ms before sender
                                            * PTT-OFF, so effective gap at sender is
                                            * (guard - 200ms) ≈ 500ms.  Radio needs
                                            * ~340ms for TX→RX switch → 160ms margin
                                            * for preamble detection.  At 500ms the
                                            * effective gap was ~300ms, causing ~50%
                                            * ACK loss on DATAC1 (< 340ms switch).  */
#define ARQ_ISS_POST_ACK_GUARD_MS     900   /* ISS guard before resuming DATA TX
                                            * after receiving an ACK from the IRS.
                                            * Larger than ARQ_CHANNEL_GUARD_MS:
                                            * ack_rx fires ~168ms before IRS PTT-OFF,
                                            * so the effective gap at IRS is only
                                            * (guard + 100ms head) - 168ms.
                                            * At 500ms: gap=432ms, too tight for
                                            * DATAC1 re-sync after IRS ACK TX.
                                            * At 900ms: gap=832ms — 492ms of clear
                                            * air before the DATAC1 preamble. */
#define ARQ_TURN_WAIT_AFTER_ACK_MS   3500  /* IRS post-ACK wait before TURN_REQ:
                                            * ISS guard(900ms)+frame(2510ms)+margin */
#define ARQ_ACCEPT_RX_WINDOW_MS      9000  /* ACCEPTING RX window after ACCEPT TX:
                                            * ISS_guard(900)+DATAC4(5800)+margin(2300)
                                            * Old value 7000 left only ~300ms margin
                                            * and raced with TIMER_RETRY, causing
                                            * 3-4 wasted ACCEPT retries (~28s).    */
#define ARQ_ACK_GUARD_S               1     /* extra slack added to retry interval */
#define ARQ_CALL_RETRY_SLOTS_DEFAULT       4    /* CALL retries before giving up       */
#define ARQ_ACCEPT_RETRY_SLOTS_DEFAULT     4    /* ACCEPT retries before returning     */
#define ARQ_DATA_RETRY_SLOTS_DEFAULT      10    /* DATA retries before disconnect      */
#define ARQ_DISCONNECT_RETRY_SLOTS_DEFAULT 2    /* DISCONNECT frame retries            */

/* Runtime-configurable retry counts (set via RETRIES TCP command).
 * Macros below preserve existing FSM code unchanged. */
extern _Atomic int arq_call_retry_slots;
extern _Atomic int arq_accept_retry_slots;
extern _Atomic int arq_data_retry_slots;
extern _Atomic int arq_disconnect_retry_slots;

#define ARQ_CALL_RETRY_SLOTS       atomic_load(&arq_call_retry_slots)
#define ARQ_ACCEPT_RETRY_SLOTS     atomic_load(&arq_accept_retry_slots)
#define ARQ_DATA_RETRY_SLOTS       atomic_load(&arq_data_retry_slots)
#define ARQ_DISCONNECT_RETRY_SLOTS atomic_load(&arq_disconnect_retry_slots)
#define ARQ_CONNECT_GRACE_SLOTS       2     /* extra wait slots for ACCEPT         */
#define ARQ_CONNECT_BUSY_EXT_S        2     /* busy-extension guard after CALL     */
#define ARQ_KEEPALIVE_INTERVAL_S      20    /* keepalive TX interval               */
#define ARQ_KEEPALIVE_MISS_LIMIT      5     /* missed keepalives before disconnect */
#define ARQ_TURN_REQ_RETRIES          2
#define ARQ_MODE_REQ_RETRIES          2
#define ARQ_MODE_SWITCH_HYST_COUNT    1     /* SNR provides stability gate; 1 = immediate */
#define ARQ_STARTUP_MAX_S             8     /* DATAC13-only startup window         */
#define ARQ_STARTUP_ACKS_REQUIRED     1
#define ARQ_PEER_PAYLOAD_HOLD_S       15    /* hold peer payload mode after activity */
#define ARQ_SNR_HYST_DB               1.0f
#define ARQ_SNR_MIN_DATAC4_DB        -4.0f  /* target MPP SNR (codec2 README) */
#define ARQ_SNR_MIN_DATAC3_DB        -1.0f
#define ARQ_SNR_MIN_DATAC1_DB         3.0f
#define ARQ_BACKLOG_MIN_DATAC3        56
#define ARQ_BACKLOG_MIN_DATAC1        126
#define ARQ_BACKLOG_MIN_BIDIR_UPGRADE 48    /* > DATAC4 payload capacity          */
#define ARQ_LADDER_LEVELS             3     /* 0=DATAC4, 1=DATAC3, 2=DATAC1     */
#define ARQ_LADDER_UP_SUCCESSES       4     /* clean ACKs required to step up    */
#define ARQ_RETRY_DOWNGRADE_THRESHOLD 2     /* consecutive retries to force downgrade */
#define ARQ_MODE_HOLD_AFTER_DOWNGRADE_S 15  /* hold lower mode after forced downgrade */

/* In DATA frames the ack_delay byte is repurposed to carry payload_valid:
 *   0               = full frame (all user bytes are valid data)
 *   1 .. user_bytes = only this many leading bytes are valid; rest is padding
 * This lets partial last-frames be transmitted while still filling the full
 * modem slot; the receiver uses `data_len` to distinguish valid payload bytes
 * from trailing padding. */
#define ARQ_DATA_LEN_FULL             0

/* Mode table (defined in arq_protocol.c) */
extern const arq_mode_timing_t arq_mode_table[];
extern const int                arq_mode_table_count;

/* ======================================================================
 * Frame codec API
 * ====================================================================== */

/**
 * @brief Encode a parsed header into the first ARQ_FRAME_HDR_SIZE bytes of buf.
 * @return 0 on success, -1 if buf_len < ARQ_FRAME_HDR_SIZE.
 */
int arq_protocol_encode_hdr(uint8_t *buf, size_t buf_len, const arq_frame_hdr_t *hdr);

/**
 * @brief Decode the ARQ header from the first bytes of buf.
 * @return 0 on success, -1 if buf too short.
 */
int arq_protocol_decode_hdr(const uint8_t *buf, size_t buf_len, arq_frame_hdr_t *hdr);

/**
 * @brief Map a configured bandwidth in Hz to an on-air BW token.
 * @return ARQ_BW_TOKEN_* value, or ARQ_BW_TOKEN_NONE if unsupported.
 */
uint8_t arq_protocol_bw_token_from_hz(int bw_hz);

/**
 * @brief Map an on-air BW token back to Hz.
 * @return 500/2300/2750 on success, or 0 if the token is invalid.
 */
int arq_protocol_bw_hz_from_token(uint8_t bw_token);

/**
 * @brief Encode a floating-point SNR (dB) into the snr_raw wire byte.
 * @return Encoded byte (0 if snr_db is out of range or unknown).
 */
uint8_t arq_protocol_encode_snr(float snr_db);

/**
 * @brief Decode snr_raw wire byte back to float dB.
 * @return SNR in dB, or 0.0f if snr_raw == 0 (unknown).
 */
float arq_protocol_decode_snr(uint8_t snr_raw);

/**
 * @brief Encode ack_delay_ms to the 8-bit wire value (10ms units, max 2.55s).
 */
uint8_t arq_protocol_encode_ack_delay(uint32_t delay_ms);

/**
 * @brief Decode the 8-bit wire ack_delay to milliseconds.
 */
uint32_t arq_protocol_decode_ack_delay(uint8_t raw);

/**
 * @brief Look up mode timing entry for a FreeDV mode.
 * @return Pointer to timing entry, or NULL if mode is unknown.
 */
const arq_mode_timing_t *arq_protocol_mode_timing(int freedv_mode);

/**
 * @brief Compute CRC16-CCITT of an uppercase-normalised callsign.
 * Used to encode/validate the DST field in CALL/ACCEPT frames.
 */
uint16_t arq_protocol_callsign_crc16(const char *callsign);

/* ======================================================================
 * Frame builder API
 *
 * Each function fills `buf` (caller-provided) with a complete ready-to-TX
 * frame (framer byte + header/payload) and returns the total byte count,
 * or -1 if buf_len < required size or arguments are invalid.
 *
 * The framer byte (byte 0, extension field + packet_type) is written by
 * write_frame_header() inside each builder.
 *
 * For control frames, frame_size = ARQ_CONTROL_FRAME_SIZE (14 bytes).
 * Callers typically allocate INT_BUFFER_SIZE and pass ARQ_CONTROL_FRAME_SIZE.
 * ====================================================================== */

/* --- Control frames (all use PACKET_TYPE_ARQ_CONTROL) --- */

/** ACK frame. flags = ARQ_FLAG_HAS_DATA when IRS has pending TX data. */
int arq_protocol_build_ack(uint8_t *buf, size_t buf_len,
                            uint8_t session_id, uint8_t rx_ack_seq,
                            uint8_t flags, uint8_t snr_raw,
                            uint8_t ack_delay_raw);

/** DISCONNECT frame. */
int arq_protocol_build_disconnect(uint8_t *buf, size_t buf_len,
                                   uint8_t session_id, uint8_t snr_raw);

/** KEEPALIVE frame. */
int arq_protocol_build_keepalive(uint8_t *buf, size_t buf_len,
                                  uint8_t session_id, uint8_t snr_raw);

/** KEEPALIVE_ACK frame. */
int arq_protocol_build_keepalive_ack(uint8_t *buf, size_t buf_len,
                                      uint8_t session_id, uint8_t snr_raw);

/**
 * TURN_REQ frame.
 * @param buf        Output buffer (caller-provided).
 * @param buf_len    Size of buf in bytes.
 * @param session_id ARQ session identifier.
 * @param rx_ack_seq  Last seq received from current ISS (so ISS can flush pending retries).
 * @param snr_raw    Local SNR encoded for wire.
 */
int arq_protocol_build_turn_req(uint8_t *buf, size_t buf_len,
                                 uint8_t session_id, uint8_t rx_ack_seq,
                                 uint8_t snr_raw);

/** TURN_ACK frame. */
int arq_protocol_build_turn_ack(uint8_t *buf, size_t buf_len,
                                 uint8_t session_id, uint8_t snr_raw);

/**
 * MODE_REQ frame.
 * @param buf        Output buffer (caller-provided).
 * @param buf_len    Size of buf in bytes.
 * @param session_id ARQ session identifier.
 * @param snr_raw    Local SNR encoded for wire.
 * @param freedv_mode  Requested payload FreeDV mode (FREEDV_MODE_DATAC*).
 */
int arq_protocol_build_mode_req(uint8_t *buf, size_t buf_len,
                                 uint8_t session_id, uint8_t snr_raw,
                                 int freedv_mode);

/** MODE_ACK frame — accept peer's mode request. */
int arq_protocol_build_mode_ack(uint8_t *buf, size_t buf_len,
                                 uint8_t session_id, uint8_t snr_raw,
                                 int freedv_mode);

/* --- Data frame (PACKET_TYPE_ARQ_DATA) --- */

/**
 * DATA frame — 8-byte header + payload bytes.
 * @param buf          Output buffer (caller-provided).
 * @param buf_len      Size of buf in bytes.
 * @param session_id   ARQ session identifier.
 * @param tx_seq       TX sequence number.
 * @param rx_ack_seq   Last seq received from peer (piggybacked ACK).
 * @param flags        ARQ_FLAG_TURN_REQ | ARQ_FLAG_HAS_DATA (bitmask).
 * @param snr_raw      Local SNR encoded for wire.
 * @param payload_valid Number of valid bytes in the payload slot.
 * @param payload      Payload bytes (must be <= buf_len - ARQ_FRAME_HDR_SIZE).
 * @param payload_len  Number of payload bytes.
 */
int arq_protocol_build_data(uint8_t *buf, size_t buf_len,
                             uint8_t session_id, uint8_t tx_seq,
                             uint8_t rx_ack_seq, uint8_t flags,
                             uint8_t snr_raw, uint8_t payload_valid,
                             const uint8_t *payload, size_t payload_len);

/* --- CALL/ACCEPT compact frames (PACKET_TYPE_ARQ_CALL) --- */

/**
 * Build a CALL frame.
 * @param buf          Output buffer (caller-provided).
 * @param buf_len      Size of buf in bytes.
 * @param session_id   ARQ session identifier.
 * @param src  Local callsign.
 * @param dst  Remote callsign.
 * @return Total frame bytes (ARQ_CONTROL_FRAME_SIZE = 14) on success, -1 on error.
 */
int arq_protocol_build_call(uint8_t *buf, size_t buf_len,
                              uint8_t session_id,
                              const char *src, const char *dst,
                              int bw_hz);

/**
 * Build an ACCEPT frame.
 * @param buf          Output buffer (caller-provided).
 * @param buf_len      Size of buf in bytes.
 * @param session_id   ARQ session identifier.
 * @param src  Local callsign.
 * @param dst  Remote callsign.
 */
int arq_protocol_build_accept(uint8_t *buf, size_t buf_len,
                                uint8_t session_id,
                                const char *src, const char *dst,
                                int bw_hz);

/**
 * Parse a CALL frame; extract callsigns.
 * @param buf            Input frame buffer.
 * @param buf_len        Size of buf in bytes.
 * @param session_id_out  Receives the session_id byte.
 * @param src_out         Buffer for local (transmitting) callsign, CALLSIGN_MAX_SIZE bytes.
 * @param dst_out         Buffer for remote callsign, CALLSIGN_MAX_SIZE bytes.
 * @return 0 on success, -1 on parse error.
 */
int arq_protocol_parse_call(const uint8_t *buf, size_t buf_len,
                              uint8_t *session_id_out,
                              char *src_out, char *dst_out,
                              int *bw_hz_out);

/**
 * Parse an ACCEPT frame; same layout as CALL.
 */
int arq_protocol_parse_accept(const uint8_t *buf, size_t buf_len,
                                uint8_t *session_id_out,
                                char *src_out, char *dst_out,
                                int *bw_hz_out);

/**
 * Build a compact DATAC13 CQ frame carrying source callsign and BW token.
 */
int arq_protocol_build_cq(uint8_t *buf, size_t buf_len,
                           const char *src, int bw_hz);

/**
 * Parse a compact DATAC13 CQ frame.
 */
int arq_protocol_parse_cq(const uint8_t *buf, size_t buf_len,
                           char *src_out, int *bw_hz_out);

#endif /* ARQ_PROTOCOL_H_ */
