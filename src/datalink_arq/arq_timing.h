/* HERMES Modem — ARQ Timing: instrumentation context and record API
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARQ_TIMING_H_
#define ARQ_TIMING_H_

#include <stdint.h>

/* ======================================================================
 * Timing context: a session's cumulative counters (the UI's bytes
 * transmitted / received, and the [TMG] disconnect totals).
 * ====================================================================== */

typedef struct
{
    uint64_t tx_bytes;          /* bytes the carousel took from the application */
    uint64_t rx_bytes;          /* bytes it delivered to the application        */
    uint64_t frames_tx;         /* carousel frames keyed                        */
    uint64_t frames_rx;         /* frames decoded from the peer                 */
    uint64_t data_rx_ms;        /* when the last bytes were delivered           */
} arq_timing_ctx_t;

/* ======================================================================
 * API
 * ====================================================================== */

/** @brief Zero-initialise a timing context at session start. */
void arq_timing_init(arq_timing_ctx_t *ctx);

/** @brief Bytes the carousel took from the application to send, or delivered
 *  to it; logs [TMG] car_tx / car_rx. */
void arq_timing_record_car_tx(arq_timing_ctx_t *ctx, int bytes);
void arq_timing_record_car_rx(arq_timing_ctx_t *ctx, int bytes);

/** @brief Record session connected; logs [TMG] connect. */
void arq_timing_record_connect(arq_timing_ctx_t *ctx, int mode);

/** @brief Record session disconnected; logs [TMG] disconnect and session totals. */
void arq_timing_record_disconnect(arq_timing_ctx_t *ctx, const char *reason);

#endif /* ARQ_TIMING_H_ */
