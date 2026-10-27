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
 * Plain session frame header (8 bytes): the DISCONNECT's.  A session's other
 * frames are the carousel's (carousel.h), CALL/ACCEPT have their own layout
 * (below).
 *
 *  Byte 0: framer byte — set/read by write_frame_header()/parse_frame_header()
 *            bits [7:5] = packet_type (PACKET_TYPE_ARQ_CONTROL)
 *            bits [4:0] = extension field (0)
 *  Byte 1: subtype      — arq_subtype_t
 *  Byte 2: flags        — 0
 *  Byte 3: session_id   — the session's id, chosen by the caller
 *  Byte 4: tx_seq       — 0
 *  Byte 5: rx_ack_seq   — 0
 *  Byte 6: snr_raw      — local RX SNR; 0=unknown, else round(dB) + 128
 *  Byte 7: ack_delay    — 0
 * ====================================================================== */

#define ARQ_HDR_SUBTYPE_IDX   1
#define ARQ_HDR_FLAGS_IDX     2
#define ARQ_HDR_SESSION_IDX   3
#define ARQ_HDR_SEQ_IDX       4
#define ARQ_HDR_ACK_IDX       5
#define ARQ_HDR_SNR_IDX       6
#define ARQ_HDR_DELAY_IDX     7
#define ARQ_FRAME_HDR_SIZE    8   /* bytes 0-7 inclusive */

/* CONNECT frames (CALL/ACCEPT) compact layout — 14 bytes, DATAC16 only.
 * Uses PACKET_TYPE_ARQ_CALL2 in the framer byte.
 *
 *  Byte 0: framer byte (PACKET_TYPE_ARQ_CALL2 | BW token, minor revision,
 *          TX-MFSK-only bit, set by write_frame_header)
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

/* Compact CQ frame layout — 14 bytes, DATAC16 only.
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
 * Frame subtypes.  1.9.x sent others (DATA, ACK, KEEPALIVE, MODE and TURN
 * requests); this version reads only the DISCONNECT.
 * ====================================================================== */

typedef enum
{
    ARQ_SUBTYPE_DISCONNECT    =  4,
} arq_subtype_t;

/* ======================================================================
 * Parsed frame header (in-memory representation, not wire layout)
 * ====================================================================== */

typedef struct
{
    uint8_t  packet_type;   /* PACKET_TYPE_ARQ_CONTROL            (from framer byte) */
    uint8_t  frame_ext;     /* low 5 bits of framer byte                          */
    uint8_t  subtype;       /* arq_subtype_t                                         */
    uint8_t  flags;
    uint8_t  session_id;
    uint8_t  tx_seq;
    uint8_t  rx_ack_seq;
    uint8_t  snr_raw;       /* 0=unknown; decode via arq_protocol_decode_snr         */
    uint8_t  ack_delay_raw;
} arq_frame_hdr_t;

/* ======================================================================
 * Per-mode timing table (arq_protocol.c)
 * ====================================================================== */

/* Upper bound on frames per modem TX action. */
#define ARQ_BURST_MAX 5

typedef struct
{
    int   freedv_mode;          /* FREEDV_MODE_* constant                        */
    float frame_duration_s;     /* measured TX duration                          */
    float retry_interval_s;     /* wait before repeating a CALL/ACCEPT/DISCONNECT */
    int   payload_bytes;        /* frame payload                                 */
} arq_mode_timing_t;

/* The one FreeDV mode used for the ARQ control frames (CALL/ACCEPT/
 * DISCONNECT/CQ, and the carousel's control frames).  Swapping the control mode is a
 * one-line change here; everything else references this define.  The mode
 * must carry exactly ARQ_CONTROL_FRAME_SIZE payload bytes and must be the
 * only mode with that frame size in arq_mode_table. */
#define ARQ_CONTROL_MODE  FREEDV_MODE_DATAC16

/* Timing constants shared across modules.  The two guards are measured, not
 * tuned, and are no longer configuration: they bundle the radio's TX->RX
 * switch with the audio backend's buffering, which nobody can tune blind. */
#define ARQ_CHANNEL_GUARD_MS              700   /* IRS response guard after frame decode.
                                                * OFDM decode fires ~200ms before sender
                                                * PTT-OFF, so effective gap at sender is
                                                * (guard - 200ms) ~= 500ms.  Radio needs
                                                * ~340ms for TX->RX switch -> 160ms margin
                                                * for preamble detection.  At 500ms the
                                                * effective gap was ~300ms, causing ~50%
                                                * ACK loss on DATAC1 (< 340ms switch).  */

#define ARQ_ISS_POST_ACK_GUARD_MS         900   /* ISS guard before resuming DATA TX
                                                * after receiving an ACK from the IRS.
                                                * Larger than ARQ_CHANNEL_GUARD_MS:
                                                * ack_rx fires ~168ms before IRS PTT-OFF,
                                                * so the effective gap at IRS is only
                                                * (guard + 100ms head) - 168ms.
                                                * At 500ms: gap=432ms, too tight for
                                                * DATAC1 re-sync after IRS ACK TX.
                                                * At 900ms: gap=832ms -- 492ms of clear
                                                * air before the DATAC1 preamble. */
#define ARQ_ACCEPT_RX_WINDOW_MS      9000  /* ACCEPTING RX window after ACCEPT TX:
                                            * ISS_guard(900)+DATAC15(4400)+margin(3700)
                                            * Old value 7000 left only ~300ms margin
                                            * and raced with TIMER_RETRY, causing
                                            * 3-4 wasted ACCEPT retries (~28s).    */
/* Grace period after entering CALLING/ACCEPTING (i.e. after PENDING is sent)
 * during which LISTEN OFF is deferred.  Scanning hosts (BPQ32) need a moment to
 * process PENDING and cancel their own dwell timers; without this grace, a
 * LISTEN OFF that crosses PENDING on the wire cancels the handshake — the host
 * moves to the next channel and the reply goes out on the wrong frequency.
 * 2 s is conservative: ~0.5 s of TCP+host processing with headroom. */
#define ARQ_LISTEN_OFF_GRACE_MS       2000
#define ARQ_CALL_RETRY_SLOTS_DEFAULT       4    /* CALL retries before giving up       */
/* CALLs on the fast control mode before the caller escalates to the MFSK
 * floor; after that it alternates MFSK and the control mode (#235: under
 * fading an MFSK CALL is no surer than a DATAC16 one at -9..-11 dB, and all-
 * MFSK lost connects DATAC16 made; MFSK is what connects at -13 dB).  A good
 * link connects exactly as quickly as before. */
#define ARQ_CALL_FAST_SLOTS_DEFAULT        2
/* Runtime value: RETRIES N above the default raises it to N/2, so a long
 * retry count buys more fast CALLs (what a scanning BBS hears while it dwells
 * on the channel), not only more slow MFSK steps (#442). */
extern _Atomic int arq_call_fast_slots;
#define ARQ_CALL_FAST_SLOTS                atomic_load(&arq_call_fast_slots)
/* Derive the fast CALL count from a RETRIES value (<= 0: the default). */
void arq_protocol_set_call_fast_slots(int retries);
/* After an ACCEPT on MFSK the callee listens this long for the caller's first
 * round: the caller decodes the ACCEPT ~4 s after it ends, keys one ISS guard
 * later, and a floor round is a 13.5 s frame the callee decodes ~4 s after. */
#define ARQ_ACCEPT_RX_WINDOW_FLOOR_MS 26000
#define ARQ_ACCEPT_RETRY_SLOTS_DEFAULT     4    /* ACCEPT retries before returning     */
#define ARQ_DISCONNECT_RETRY_SLOTS_DEFAULT 2    /* DISCONNECT frame retries            */

/* Runtime-configurable retry counts (set via RETRIES TCP command). */
extern _Atomic int arq_call_retry_slots;
extern _Atomic int arq_accept_retry_slots;
extern _Atomic int arq_disconnect_retry_slots;

/* Station capability: transmit MFSK only (FSK-only radios — QDX, AFSK III).
 * When set, this station's CALL/ACCEPT/control/payload all transmit on the
 * MFSK floor rung, and its peers keep its transmit direction there.  Set via
 * arq:mfsk_tx_only (arq_set_tx_mfsk_only). */
extern _Atomic int arq_tx_mfsk_only;
#define ARQ_TX_MFSK_ONLY atomic_load(&arq_tx_mfsk_only)

/* Runtime-configurable CALL/ACCEPT retry interval in seconds (set via CALLINT
 * TCP command).  0.0 = use compiled default (7.0s).  Minimum enforced: 4.0s.
 * Only affects CALL/ACCEPT retry scheduling during connection setup; the
 * DISCONNECT uses the immutable table values. */
#define ARQ_CALLINT_MIN_S      4.0f
#define ARQ_CALLINT_DEFAULT_S  0.0f   /* 0 = use table default */
extern _Atomic float arq_callint_override_s;

/* Deterministic retry stagger (ms).  The two peers of a session compare the
 * same pair of exchanged callsigns, so exactly one of them defers by this
 * fixed offset on every retry — no random draw, no tail.  0 disables the
 * stagger (an explicit off-switch); tunable via arq:retry_stagger_ms. */
#define ARQ_RETRY_STAGGER_MS_DEFAULT 2000
#define ARQ_RETRY_STAGGER_MS_MAX     5000
extern _Atomic int arq_retry_stagger_ms;

/* How long a session hears nothing before its control moves to the MFSK floor
 * (the carousel's SILENT_MS), in seconds.  The receiver's own wait for the
 * floor (S_FALLBACK_MS) is 60 % of it.  Default 150 (so 90), range 110..600.
 * Shorter reacts faster to QRM but moved multipath links to a floor they did
 * not need in the sim (#442); see carousel.c.
 *
 * The minimum keeps each timer at least one exchange long, which is what
 * specs/rungs/FloorSilence.tla counts them in (specs/rungs/README.md): the
 * longest gap between two keydowns of M's that S decodes, on a working link,
 * is ~61 s (BW500 at the floor: S's 27.6 s round, M's 27.6 s MFSK control,
 * the guards and MFSK's 4.5 s decode), so S_FALLBACK_MS >= 61 s and
 * SILENT_MS >= 102 s; 110 leaves a margin.  Shorter, S fell back between the
 * exchanges of a link that was answering. */
#define ARQ_SILENCE_FALLBACK_S_DEFAULT 150
#define ARQ_SILENCE_FALLBACK_S_MIN     110
#define ARQ_SILENCE_FALLBACK_S_MAX     600
extern _Atomic int arq_silence_fallback_s;
void arq_protocol_set_silence_fallback_s(int seconds);

/* Deterministic per-node retry rank from the exchanged callsign pair.
 * strcmp() is antisymmetric, so the two peers always disagree: one returns 0
 * (send first) and the other 1 (defer).  Returns -1 when the pair is
 * degenerate (identical or missing callsigns) — a misconfiguration that falls
 * back to a random draw. */
int arq_protocol_retry_rank(const char *local_call, const char *remote_call);

uint64_t arq_protocol_retry_deadline_ms(float seconds, int rank);

#define ARQ_CALL_RETRY_SLOTS       atomic_load(&arq_call_retry_slots)
#define ARQ_ACCEPT_RETRY_SLOTS     atomic_load(&arq_accept_retry_slots)
#define ARQ_DISCONNECT_RETRY_SLOTS atomic_load(&arq_disconnect_retry_slots)

/* The silence between two frames of one keydown (the carousel's poll and its
 * first burst).  The peer is already receiving, so this is not a turnaround:
 * it only has to let the peer's control decoder report the first frame and
 * its payload decoder drop any sync taken off it before the next preamble
 * arrives.  Measured, not tuned: see the one-keydown bench. */
#define ARQ_ACK_DATA_GAP_MS           300

/* Listen before talk.  "Busy" is a decoder holding sync on an incoming burst
 * (arq_update_link_metrics()); HOLD_MS is how stale a sync observation may be
 * and still count as busy.  The modem updates it once per RX chunk, so this
 * only covers scheduling jitter, not a whole frame: sync keeps being
 * re-observed for as long as the burst lasts.
 *
 * A deferred transmission (the DISCONNECT) waits in DEFER_MS steps, at most
 * DEFER_MAX of them: false sync is real (a payload decoder can latch onto a
 * control burst), and a decoder stuck in sync must not block a transmission
 * forever, so after that we key anyway and take the collision. */
#define ARQ_CHANNEL_SYNC_HOLD_MS      250
#define ARQ_CHANNEL_DEFER_MS          500
#define ARQ_CHANNEL_DEFER_MAX         20   /* ~10 s, then key regardless */
/* A CALL held for a frame (arq_fsm.c, call_must_wait) waits this long past
 * the frame's end, so that the frame can decode first: an ACCEPT does, and a
 * CALL keyed just before it went out from a caller already connected.  The
 * floor's decode allowance (carousel.c D_MFSK_MS 4500; ~3.7 s on air) and one
 * deferral step: a decode at the allowance's very end otherwise ties with the
 * CALL (specs/connect/ConnectCarrier.tla, DW). */
#define ARQ_CALL_DECODE_WAIT_MS       (4500 + ARQ_CHANNEL_DEFER_MS)
/* Before a DISCONNECT: how long past the longer reply guard to leave for a
 * frame our last transmission invited to start and be heard, before keying
 * over the gap. */
#define ARQ_REPLY_WINDOW_MARGIN_MS    1000

/* Decode thresholds (dB) of the rungs, which set the carousel's start rung
 * (carousel.c, car_start_level) and MFSK's place below DATAC15. */
#define ARQ_SNR_HYST_DB               5.0f
#define ARQ_SNR_MIN_DATAC15_DB       -7.0f  /* its cliff (sim, bench): below
                                             * it the carousel's MFSK rung wins */
#define ARQ_SNR_MIN_DATAC4_DB        -6.0f  /* bench (docs/MODES.md):
                                             * DATAC15/DATAC4 goodput crossover
                                             * ~-7.5 dB MPP / -8.5 dB AWGN     */
#define ARQ_SNR_MIN_DATAC3_DB        -1.0f
#define ARQ_SNR_MIN_DATAC1_DB         3.0f
#define ARQ_SNR_MIN_DATAC17_DB        7.0f  /* bench (docs/MODES.md): goodput
                                             * crossover vs DATAC1 ~ +6 dB MPP;
                                             * 89/100 delivery at +8          */
#define ARQ_SNR_MIN_QAM16C2_DB       13.0f  /* bench: goodput crossover vs
                                             * DATAC17 ~ +11 dB MPP, 84/100 at
                                             * +13.7; upstream's published
                                             * operating point is +15        */

/* Absolute cap on how long an APP_DISCONNECT may stay deferred while the FSM
 * tries to drain the last app bytes.  Once the application has asked to
 * disconnect, the deferral must always resolve into a clean air-side
 * DISCONNECT handshake within this window — otherwise a stuck session keeps
 * keying the rig forever (the K7EK "Mercury kept hanging on" report).  On a
 * healthy link the drain completes in seconds, so this only bites when the
 * link is failing. */
#define ARQ_DISCONNECT_DRAIN_TIMEOUT_S_DEFAULT 45
extern _Atomic int arq_disconnect_drain_timeout_s;
#define ARQ_DISCONNECT_DRAIN_TIMEOUT_S atomic_load(&arq_disconnect_drain_timeout_s)

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
 * @brief Look up mode timing entry for a FreeDV mode.
 * @return Pointer to timing entry, or NULL if mode is unknown.
 */
const arq_mode_timing_t *arq_protocol_mode_timing(int freedv_mode);

/**
 * @brief Longest single-burst airtime across the whole mode table, in seconds.
 *
 * The RX side must be able to hold a burst of ANY mode the peer might send,
 * not just the one currently selected — the peer can change rung between our
 * bursts, and the first frame of the new rung arrives before we know about it.
 * Buffers sized from the active mode are therefore always one step behind.
 */
float arq_protocol_longest_burst_s(void);

/**
 * Return the CALL/ACCEPT retry interval in seconds, applying any
 * CALLINT override.  Falls back to the DATAC16 table default (8.0s).
 */
float arq_protocol_call_interval_s(void);
/* The wait after a CALL on this carrier.  After an MFSK CALL: the callee
 * decodes it ~4 s after it ends, answers one guard later with a 13.5 s MFSK
 * ACCEPT, which this end decodes ~4 s after -- about 24 s, where the table's
 * DATAC16-shaped interval would key the next CALL over the answer. */
float arq_protocol_call_interval_for_mode_s(int mode);

/* The MFSK floor of a session at this bandwidth (Hz): the 500 Hz geometry
 * (MERCURY_MODE_MFSK_N32, 32 tones 15.6 Hz apart) for BW500, whose stations
 * may sit behind a 500 Hz filter that no 1 kHz MFSK passes; the 1 kHz floor
 * (MERCURY_MODE_MFSK) otherwise.  Per bit the two reach equally far on AWGN
 * (the narrow one's 3 dB is twice the airtime), the 1 kHz one further on
 * flutter, in half the keydown. */
int arq_floor_mode(int bw_hz);

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
 * or -1 if buf_len < required size or arguments are invalid.  Control frames
 * are padded to ARQ_CONTROL_FRAME_SIZE (14 bytes) on the air.
 * ====================================================================== */

/** DISCONNECT frame (PACKET_TYPE_ARQ_CONTROL). */
int arq_protocol_build_disconnect(uint8_t *buf, size_t buf_len,
                                   uint8_t session_id, uint8_t snr_raw);

/* --- CALL/ACCEPT of Mercury 2.0 (PACKET_TYPE_ARQ_CALL2) ---
 *
 * 14 bytes (a DATAC16 frame), padded to the frame on MFSK.  Byte 0 is the
 * framer: type CALL2, and in its extension the bandwidth token (2 bits), the
 * 2.x minor revision (2 bits, 1) and the TX-MFSK-only bit.  Every 1.9.x
 * release drops the type,
 * and a CALL of theirs (PACKET_TYPE_ARQ_CALL) addressed to this station is
 * logged, not answered: the versions do not connect (docs/CAROUSEL-ARQ.md,
 * "Wire formats").
 *
 * The 1.9.x frames, type CALL, carried in their extension the bandwidth
 * token, a 2-bit protocol version and a carousel bit:
 * Version 1 was 1.9.18.  Version 2 moved MFSK's tones from 1531..2500 Hz to
 * 1015.6..1984.4 Hz, centred on 1500 Hz like the codec2 modes; the frames are
 * the same, but a pair of versions 1 and 2 would connect on DATAC16 and then
 * never hear each other at the floor, so the version keeps them apart.
 * Version 3 (after 1.9.19) gave a BW500 session its own floor, the 500 Hz
 * MFSK (arq_floor_mode), and was the last value the two bits held.
 * Mercury 2.0 took another packet type (CALL2, above).
 *
 * Minor 1 added the TX-MFSK-only bit, a station capability: "I can only
 * transmit MFSK" (arq:mfsk_tx_only; an FSK-only radio).  1.9.20 sent minor
 * 0 in 3 bits and reads any minor, so it reads a minor-1 frame as before,
 * the bit as part of the minor; a station of minor 1 or later reads 1.9.20's
 * frames as before too.  Only a station that sets the bit needs a peer that
 * reads it, so it ignores frames of minor 0 (arq_protocol_connect_minor).
 *
 *   CALL    1: 0 | session id   2-3: CRC16 of the called callsign
 *           4-13: the caller's callsign, arithmetic-coded
 *   ACCEPT  1: 1 | session id   2-3: CRC16 of the caller's callsign
 *           4-5: CRC16 of the called callsign   6-7: the callee's nonce
 *           8: the caller's start rung (3 bits), the callee hears the caller
 *              below the control mode (bit 3), 4 bits zero
 *           9: the SNR the callee read the CALL at, dB + 128 (0: unknown)
 *           10-13: zero */
#define ARQ_CONNECT_MINOR             1
#define ARQ_CONNECT_EXT_BW_MASK       0x03
#define ARQ_CONNECT_EXT_MINOR_SHIFT   2
#define ARQ_CONNECT_EXT_MINOR_MASK    0x0C
#define ARQ_CONNECT_EXT_TXMFSK        0x10  /* the sender transmits only MFSK */
#define ARQ_ACCEPT_CALLED_CRC_IDX     4
#define ARQ_ACCEPT_NONCE_IDX          6
#define ARQ_ACCEPT_LEVEL_IDX          8
#define ARQ_ACCEPT_LEVEL_MASK         0x07
#define ARQ_ACCEPT_CTL_DEAF_BIT       0x08  /* the callee hears the caller below
                                               the control mode */
#define ARQ_ACCEPT_SNR_IDX            9

/** Build a CALL.  tx_mfsk: this station can only transmit MFSK.  @return
 *  ARQ_CONTROL_FRAME_SIZE (14), or -1. */
int arq_protocol_build_call(uint8_t *buf, size_t buf_len,
                            uint8_t session_id,
                            const char *caller, const char *called,
                            int bw_hz, bool tx_mfsk);

/** Build an ACCEPT.  called: the callsign the CALL was addressed to (the
 *  callee's primary or a secondary); start_level 0..6; ctl_deaf: the callee
 *  hears the caller below the control mode; call_snr_db: what it read the
 *  CALL at (NAN: unknown); tx_mfsk: this station can only transmit MFSK.
 *  @return ARQ_CONTROL_FRAME_SIZE (14), or -1. */
int arq_protocol_build_accept(uint8_t *buf, size_t buf_len,
                              uint8_t session_id,
                              const char *called, const char *caller,
                              int bw_hz, int start_level, bool ctl_deaf,
                              float call_snr_db, uint16_t nonce,
                              bool tx_mfsk);

/** Is buf a CALL or ACCEPT of this protocol (2.0, any minor revision)? */
bool arq_protocol_connect_version_ok(const uint8_t *buf, size_t buf_len);

/** The 2.x minor revision of a CALL or ACCEPT (version_ok already checked). */
int arq_protocol_connect_minor(const uint8_t *buf);

/** Is a CALL or ACCEPT addressed to callsign (bytes 2-3, all 16 bits)?
 *  Requires at least ARQ_CONNECT_PAYLOAD_IDX + 2 bytes in @p buf. */
bool arq_protocol_connect_dst_matches(const uint8_t *buf, size_t buf_len,
                                      const char *callsign);

/** Parse a CALL: session id, the caller's callsign, the bandwidth, and the
 *  caller's TX-MFSK-only capability.  @return 0, or -1 (wrong version, bad
 *  bandwidth token, undecodable call). */
int arq_protocol_parse_call(const uint8_t *buf, size_t buf_len,
                            uint8_t *session_id_out, char *caller_out,
                            int *bw_hz_out, bool *tx_mfsk_out);

typedef struct {
    uint8_t  session_id;
    int      bw_hz;
    uint16_t called_crc;     /* CRC16 of the callsign the caller called */
    uint16_t nonce;          /* the callee's session nonce */
    int      start_level;    /* 0..6 */
    bool     ctl_deaf;       /* the callee hears the caller below the control mode */
    float    call_snr_db;    /* what the callee read the CALL at; NAN: unknown */
    bool     tx_mfsk;        /* the callee can only transmit MFSK */
} arq_accept_t;

/** Parse an ACCEPT.  @return 0, or -1 (wrong version, bad bandwidth token,
 *  a reserved bit set, a start rung off the ladder). */
int arq_protocol_parse_accept(const uint8_t *buf, size_t buf_len, arq_accept_t *out);

/**
 * Build a compact DATAC16 CQ frame carrying source callsign and BW token.
 */
int arq_protocol_build_cq(uint8_t *buf, size_t buf_len,
                           const char *src, int bw_hz);

/**
 * Parse a compact DATAC16 CQ frame.
 */
int arq_protocol_parse_cq(const uint8_t *buf, size_t buf_len,
                           char *src_out, int *bw_hz_out);

#endif /* ARQ_PROTOCOL_H_ */
