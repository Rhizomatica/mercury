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
#include "../common/virtual_clock.h"

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

void arq_timing_record_car_tx(arq_timing_ctx_t *ctx, int bytes)
{
    if (bytes <= 0) return;
    ctx->tx_bytes += (uint64_t)bytes;
    HLOGT(LOG_COMP, "car_tx bytes=%d tx_total=%llu", bytes, (unsigned long long)ctx->tx_bytes);
}

void arq_timing_record_car_rx(arq_timing_ctx_t *ctx, int bytes)
{
    if (bytes <= 0) return;
    ctx->data_rx_ms = time_now_ms();
    ctx->rx_bytes  += (uint64_t)bytes;
    HLOGT(LOG_COMP, "car_rx bytes=%d rx_total=%llu", bytes, (unsigned long long)ctx->rx_bytes);
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
          "frames_tx=%llu frames_rx=%llu",
          reason,
          (unsigned long long)ctx->tx_bytes,
          (unsigned long long)ctx->rx_bytes,
          (unsigned long long)ctx->frames_tx,
          (unsigned long long)ctx->frames_rx);
}
