/* HERMES Modem — ARQ Timing: instrumentation context and record API
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARQ_TIMING_H_
#define ARQ_TIMING_H_

#include <stdbool.h>
#include <stdint.h>

/* ======================================================================
 * Timing context
 *
 * Timestamps are uint64_t monotonic milliseconds (from hermes_log startup).
 * Counters are unsigned and accumulate over the session lifetime.
 * SNR values are stored as integer * 10 to avoid float in the critical path.
 * ====================================================================== */

typedef struct
{
    /* Per-frame timestamps (reset at each new TX sequence) */
    uint64_t tx_queue_ms;       /* when frame was submitted to action queue    */
    uint64_t tx_start_ms;       /* when PTT went ON (frame on air)             */
    uint64_t tx_end_ms;         /* when PTT went OFF                           */
    uint64_t ack_rx_ms;         /* when ACK for this seq was decoded           */
    uint64_t data_rx_ms;        /* when last data frame was decoded (IRS side) */
    uint64_t ack_tx_start_ms;   /* when ACK TX started (IRS side)              */

    /* Derived measurements */
    uint32_t rtt_ms;            /* OTA RTT for last ACKed frame                */
    uint32_t ack_delay_ms;      /* peer-reported delay between data-rx/ack-tx  */

    /* Per-frame retry state */
    uint32_t retry_count;       /* retries for current tx_seq                  */

    /* SNR (integer * 10, e.g. -23 = -2.3 dB) */
    int      last_snr_local_x10;
    int      last_snr_peer_x10;

    /* Session cumulative counters */
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t retries_total;
    uint64_t frames_tx;
    uint64_t frames_rx;
} arq_timing_ctx_t;

/* ======================================================================
 * API
 * ====================================================================== */

/** @brief Zero-initialise a timing context at session start. */
void arq_timing_init(arq_timing_ctx_t *ctx);

/** @brief Record frame queued for TX; logs [TMG] tx_queue.
 *  @param ctx            Timing context.
 *  @param seq            TX sequence number.
 *  @param mode           FreeDV mode used for TX.
 *  @param backlog_bytes  Application bytes remaining in TX queue.
 *  @param payload_bytes  Actual application bytes in this frame (0 for retransmits). */
void arq_timing_record_tx_queue(arq_timing_ctx_t *ctx, int seq, int mode,
                                int backlog_bytes, int payload_bytes);

/** @brief Record PTT ON; logs [TMG] tx_start. */
void arq_timing_record_tx_start(arq_timing_ctx_t *ctx, int seq, int mode,
                                int backlog_bytes);

/** @brief Record PTT OFF; logs [TMG] tx_end with duration. */
void arq_timing_record_tx_end(arq_timing_ctx_t *ctx, int seq);

/**
 * @brief Record ACK received; computes and logs OTA RTT.
 * @param ctx            Timing context.
 * @param seq            TX sequence number being ACKed.
 * @param ack_delay_raw  Wire-encoded ack_delay (10ms units, 0=unknown).
 * @param peer_snr_x10   Peer-reported SNR * 10.
 */
void arq_timing_record_ack_rx(arq_timing_ctx_t *ctx, int seq,
                               uint8_t ack_delay_raw, int peer_snr_x10);

/** @brief Record data frame decoded (IRS side); logs [TMG] data_rx. */
void arq_timing_record_data_rx(arq_timing_ctx_t *ctx, int seq,
                                int bytes, int snr_x10);

/** @brief Carousel session: bytes the carousel took from the application to
 *  send, or delivered to it; logs [TMG] car_tx / car_rx.  The stop-and-wait
 *  records above never run in a carousel session, so without these the
 *  session's byte counters (and the UI's) stayed at 0. */
void arq_timing_record_car_tx(arq_timing_ctx_t *ctx, int bytes);
void arq_timing_record_car_rx(arq_timing_ctx_t *ctx, int bytes);

/** @brief Record ACK TX started (IRS side); logs [TMG] ack_tx. */
void arq_timing_record_ack_tx(arq_timing_ctx_t *ctx, int seq);

/**
 * @brief Record a retry event; logs [TMG] retry.
 * @param ctx      Timing context.
 * @param seq      TX sequence number.
 * @param attempt  1-based retry number.
 * @param reason   Short reason string (e.g. "timeout", "nack").
 */
void arq_timing_record_retry(arq_timing_ctx_t *ctx, int seq,
                              int attempt, const char *reason);

/**
 * @brief Record turn direction change; logs [TMG] turn.
 * @param ctx     Timing context.
 * @param to_iss  true = becoming ISS, false = becoming IRS.
 * @param reason  "piggyback", "turn_req", "startup" etc.
 */
void arq_timing_record_turn(arq_timing_ctx_t *ctx, bool to_iss,
                             const char *reason);

/** @brief Record session connected; logs [TMG] connect. */
void arq_timing_record_connect(arq_timing_ctx_t *ctx, int mode);

/** @brief Record session disconnected; logs [TMG] disconnect and session totals. */
void arq_timing_record_disconnect(arq_timing_ctx_t *ctx, const char *reason);

#endif /* ARQ_TIMING_H_ */
