/* HERMES Modem — UI status rendering
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

#include "ui_status.h"

int ui_status_to_json(const ui_status_t *st, char *buf, size_t buflen)
{
    if (!st || !buf || buflen == 0)
        return -1;

    /* Field order and formatting are the established wire format — remote
     * clients parse this. Keep it byte-for-byte stable; test_ui_status.c
     * fails if it drifts. */
    int n = snprintf(buf, buflen,
        "{\"type\":\"status\","
        "\"bitrate\":%d,"
        "\"snr\":%.1f,"
        "\"user_callsign\":\"%s\","
        "\"dest_callsign\":\"%s\","
        "\"sync\":%s,"
        "\"direction\":\"%s\","
        "\"client_tcp_connected\":%s,"
        "\"bytes_transmitted\":%ld,"
        "\"bytes_received\":%ld,"
        "\"tx_gain_db\":%.1f,"
        "\"tx_peak_dbfs\":%.1f,"
        "\"waterfall\":%s}",
        st->bitrate_bps,
        st->snr_db,
        st->user_callsign,
        st->dest_callsign,
        st->sync ? "true" : "false",
        st->transmitting ? "tx" : "rx",
        st->client_tcp_connected ? "true" : "false",
        st->bytes_transmitted,
        st->bytes_received,
        (double)st->tx_gain_db,
        (double)st->tx_peak_dbfs,
        st->waterfall_enabled ? "true" : "false");

    if (n < 0 || (size_t)n >= buflen)
        return -1;
    return n;
}
