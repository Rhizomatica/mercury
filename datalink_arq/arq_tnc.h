/* HERMES Modem — ARQ→TNC notification seam
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Decouples the ARQ datalink from the TCP TNC layer above it.  The TNC
 * layer registers a table of notification callbacks via
 * arq_set_tnc_callbacks(); the ARQ layer emits notifications through the
 * arq_tnc_send_*() invokers, which are NULL-safe before registration.
 */
#ifndef ARQ_TNC_H
#define ARQ_TNC_H

#include <stdint.h>

typedef struct
{
    void (*send_pending)(void);
    void (*send_cancelpending)(void);
    void (*send_connected)(void);
    void (*send_cqframe)(const char *source_call, int bw_hz);
    void (*send_disconnected)(void);
    void (*send_buffer)(uint32_t bytes);
    void (*send_registered)(const char *callsign);
} arq_tnc_callbacks_t;

/* Register the TNC notification callbacks (call once, after arq_init()).
 * Passing NULL clears the registration. The pointed-to struct must outlive
 * all subsequent arq_tnc_send_* calls (register a static/const table). */
void arq_set_tnc_callbacks(const arq_tnc_callbacks_t *cbs);

/* NULL-safe notification invokers. No-op if no table is registered or the
 * specific callback is NULL. */
void arq_tnc_send_pending(void);
void arq_tnc_send_cancelpending(void);
void arq_tnc_send_connected(void);
void arq_tnc_send_cqframe(const char *source_call, int bw_hz);
void arq_tnc_send_disconnected(void);
void arq_tnc_send_buffer(uint32_t bytes);
void arq_tnc_send_registered(const char *callsign);

#endif /* ARQ_TNC_H */
