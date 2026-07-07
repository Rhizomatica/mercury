/* HERMES Modem — ARQ→TNC notification seam
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "arq_tnc.h"
#include <stddef.h>

/* Registered once at startup by the TNC layer (interfaces_init).  All
 * invokers below NULL-guard both the table and the individual field, so
 * the ARQ layer is safe to call them before registration (mirrors how the
 * FSM NULL-guards g_cbs in arq_fsm.c). */
static const arq_tnc_callbacks_t *g_tnc;

void arq_set_tnc_callbacks(const arq_tnc_callbacks_t *cbs)
{
    g_tnc = cbs;
}

void arq_tnc_send_pending(void)
{
    if (g_tnc && g_tnc->send_pending) g_tnc->send_pending();
}

void arq_tnc_send_cancelpending(void)
{
    if (g_tnc && g_tnc->send_cancelpending) g_tnc->send_cancelpending();
}

void arq_tnc_send_connected(void)
{
    if (g_tnc && g_tnc->send_connected) g_tnc->send_connected();
}

void arq_tnc_send_cqframe(const char *source_call, int bw_hz)
{
    if (g_tnc && g_tnc->send_cqframe) g_tnc->send_cqframe(source_call, bw_hz);
}

void arq_tnc_send_disconnected(void)
{
    if (g_tnc && g_tnc->send_disconnected) g_tnc->send_disconnected();
}

void arq_tnc_send_buffer(uint32_t bytes)
{
    if (g_tnc && g_tnc->send_buffer) g_tnc->send_buffer(bytes);
}
