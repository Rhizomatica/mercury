/* HERMES Modem — UI status snapshot
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One typed description of "what the UI shows", filled by a single gatherer in
 * ui_communication.c and rendered two ways:
 *
 *   - the embedded Fyne UI reads the struct directly across the CGo bridge;
 *   - remote clients (HERMES web UI, mercury-qt) get ui_status_to_json().
 *
 * The point of the struct existing is that both paths read the SAME gathered
 * snapshot, so a field can never mean one thing locally and another remotely.
 * Adding a field means: add it here, set it in the gatherer, and — if remote
 * clients need it — render it in ui_status_to_json(), whose exact output is
 * pinned by tests/gui_interface/test_ui_status.c.
 */
#ifndef UI_STATUS_H
#define UI_STATUS_H

#include <stdbool.h>
#include <stddef.h>

#include "arq.h"   /* CALLSIGN_MAX_SIZE */

typedef struct {
    int    bitrate_bps;
    double snr_db;
    char   user_callsign[CALLSIGN_MAX_SIZE];
    char   dest_callsign[CALLSIGN_MAX_SIZE];
    bool   sync;                 /* ARQ session established               */
    bool   transmitting;         /* PTT asserted (direction tx vs rx)     */
    bool   client_tcp_connected; /* a host is attached to the TNC ports   */
    long   bytes_transmitted;
    long   bytes_received;
    float  tx_gain_db;
    float  tx_peak_dbfs;
    bool   waterfall_enabled;
} ui_status_t;

/* Render the snapshot as the status JSON remote clients already expect.
 * Returns the number of bytes written (excluding the NUL), or -1 if the
 * buffer is too small.  Pure function: no locks, no globals, unit-testable. */
int ui_status_to_json(const ui_status_t *st, char *buf, size_t buflen);

#endif /* UI_STATUS_H */
