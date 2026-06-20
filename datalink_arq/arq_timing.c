/* HERMES Modem — ARQ Timing: instrumentation and telemetry
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_timing.h"

#include <string.h>

#include "../common/hermes_log.h"
#include "arq_protocol.h"

#define LOG_COMP "arq-timing"

/* FreeDV mode name helper (brief form) */
static const char *mode_name(int mode)
{
    switch (mode)
    {
    case 25: return "QAM16C2";
    case 24: return "DATAC17";
    case 23: return "DATAC16";
    case 22: return "DATAC15";
    case 19: return "DATAC13";
    case 18: return "DATAC4";
    case 12: return "DATAC3";
    case 10: return "DATAC1";
    default: return "?";
    }
}

void arq_timing_init(arq_timing_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void arq_timing_record_tx_queue(arq_timing_ctx_t *ctx, int seq, int mode,
                                int backlog_bytes, int payload_bytes)
{
    ctx->tx_queue_ms = hermes_uptime_ms();
    if (payload_bytes > 0)
        ctx->tx_bytes += (uint64_t)payload_bytes;
    HLOGT(LOG_COMP, "tx_queue seq=%d mode=%s backlog=%d bytes=%d tx_total=%llu",
          seq, mode_name(mode), backlog_bytes, payload_bytes,
          (unsigned long long)ctx->tx_bytes);
}

void arq_timing_record_tx_start(arq_timing_ctx_t *ctx, int seq, int mode,
                                int backlog_bytes)
{
    ctx->tx_start_ms = hermes_uptime_ms();
    ctx->frames_tx++;
    HLOGT(LOG_COMP, "tx_start seq=%d mode=%s backlog=%d",
          seq, mode_name(mode), backlog_bytes);
}

void arq_timing_record_tx_end(arq_timing_ctx_t *ctx, int seq)
{
    ctx->tx_end_ms = hermes_uptime_ms();
    uint32_t dur = (uint32_t)(ctx->tx_end_ms - ctx->tx_start_ms);
    HLOGT(LOG_COMP, "tx_end seq=%d dur=%ums", seq, dur);
}

void arq_timing_record_ack_rx(arq_timing_ctx_t *ctx, int seq,
                               uint8_t ack_delay_raw, int peer_snr_x10)
{
    ctx->ack_rx_ms       = hermes_uptime_ms();
    ctx->last_snr_peer_x10 = peer_snr_x10;

    uint32_t peer_delay = arq_protocol_decode_ack_delay(ack_delay_raw);
    ctx->ack_delay_ms = peer_delay;

    if (ctx->tx_start_ms > 0)
    {
        uint32_t elapsed = (uint32_t)(ctx->ack_rx_ms - ctx->tx_start_ms);
        ctx->rtt_ms = (elapsed > peer_delay) ? (elapsed - peer_delay) : elapsed;
    }

    HLOGT(LOG_COMP, "ack_rx seq=%d rtt=%ums peer_delay=%ums snr_peer=%.1f",
          seq, ctx->rtt_ms, peer_delay, (float)peer_snr_x10 / 10.0f);
}

void arq_timing_record_data_rx(arq_timing_ctx_t *ctx, int seq,
                                int bytes, int snr_x10)
{
    ctx->data_rx_ms         = hermes_uptime_ms();
    ctx->last_snr_local_x10 = snr_x10;
    ctx->rx_bytes          += (uint64_t)bytes;
    ctx->frames_rx++;

    /* Channel turnaround: time from our last PTT-OFF to this decode.
     * Subtract the mode's frame duration to estimate the clear-air gap —
     * the per-station measurement that informs guard-interval choices
     * (guards bundle radio switch time AND audio-backend buffering, so
     * they must never be tuned blind). */
    uint32_t turnaround = 0;
    if (ctx->tx_end_ms > 0 && ctx->data_rx_ms > ctx->tx_end_ms &&
        ctx->data_rx_ms - ctx->tx_end_ms < 60000)
        turnaround = (uint32_t)(ctx->data_rx_ms - ctx->tx_end_ms);

    HLOGT(LOG_COMP, "data_rx seq=%d bytes=%d snr_local=%.1f rx_total=%llu turnaround=%ums",
          seq, bytes, (float)snr_x10 / 10.0f,
          (unsigned long long)ctx->rx_bytes, turnaround);
}

void arq_timing_record_ack_tx(arq_timing_ctx_t *ctx, int seq)
{
    ctx->ack_tx_start_ms = hermes_uptime_ms();
    uint32_t delay = 0;
    if (ctx->data_rx_ms > 0)
        delay = (uint32_t)(ctx->ack_tx_start_ms - ctx->data_rx_ms);
    HLOGT(LOG_COMP, "ack_tx seq=%d delay_from_rx=%ums", seq, delay);
}

void arq_timing_record_retry(arq_timing_ctx_t *ctx, int seq,
                              int attempt, const char *reason)
{
    ctx->retry_count++;
    ctx->retries_total++;
    HLOGT(LOG_COMP, "retry seq=%d attempt=%d reason=%s", seq, attempt, reason);
}

void arq_timing_record_turn(arq_timing_ctx_t *ctx, bool to_iss,
                             const char *reason)
{
    (void)ctx;
    HLOGT(LOG_COMP, "turn dir=%s reason=%s", to_iss ? "→ISS" : "→IRS", reason);
}

void arq_timing_record_connect(arq_timing_ctx_t *ctx, int mode)
{
    arq_timing_init(ctx);  /* reset per-session counters */
    HLOGT(LOG_COMP, "connect mode=%s", mode_name(mode));
}

void arq_timing_record_disconnect(arq_timing_ctx_t *ctx, const char *reason)
{
    HLOGT(LOG_COMP,
          "disconnect reason=%s tx_bytes=%llu rx_bytes=%llu "
          "frames_tx=%llu frames_rx=%llu retries=%llu",
          reason,
          (unsigned long long)ctx->tx_bytes,
          (unsigned long long)ctx->rx_bytes,
          (unsigned long long)ctx->frames_tx,
          (unsigned long long)ctx->frames_rx,
          (unsigned long long)ctx->retries_total);
}
