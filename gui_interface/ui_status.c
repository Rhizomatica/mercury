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
    char frequency[32];
    char age[32];
    if (st->radio_frequency_valid)
    {
        snprintf(frequency, sizeof(frequency), "%llu",
                 (unsigned long long)st->radio_frequency_hz);
        snprintf(age, sizeof(age), "%llu",
                 (unsigned long long)st->radio_frequency_age_ms);
    }
    else
    {
        snprintf(frequency, sizeof(frequency), "null");
        snprintf(age, sizeof(age), "null");
    }

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
        "\"waterfall\":%s,"
        "\"audio_ok\":%s,"
        "\"audio_error\":\"%s\","
        "\"arq_tx_mode\":\"%s\","
        "\"arq_rx_mode\":\"%s\","
        "\"radio_frequency_hz\":%s,"
        "\"radio_frequency_age_ms\":%s,"
        /* Appended, not inserted: the field order above is the wire format
         * that remote clients parse. */
        "\"peer_snr\":%.1f,"
        "\"peer_snr_valid\":%s}",
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
        st->waterfall_enabled ? "true" : "false",
        st->audio_ok ? "true" : "false",
        st->audio_error,
        st->arq_tx_mode,
        st->arq_rx_mode,
        frequency,
        age,
        st->peer_snr_db,
        st->peer_snr_valid ? "true" : "false");

    if (n < 0 || (size_t)n >= buflen)
        return -1;
    return n;
}

int ui_device_list_to_json(const char *type_name, const ui_device_t *devs, int count,
                           const char *selected, char *buf, size_t buflen)
{
    if (!type_name || !buf || buflen == 0 || (count > 0 && !devs))
        return -1;

    int pos = snprintf(buf, buflen, "{\"type\":\"%s\",\"selected\":\"%s\",\"list\":[",
                       type_name, selected ? selected : "");
    if (pos < 0 || (size_t)pos >= buflen)
        return -1;

    for (int i = 0; i < count; i++)
    {
        int n = snprintf(buf + pos, buflen - pos, "%s{\"name\":\"%s\",\"id\":\"%s\"}",
                         i ? "," : "", devs[i].name, devs[i].id);
        if (n < 0 || (size_t)(pos + n) >= buflen)
            return -1;   /* truncated JSON is worse than none — see the status path */
        pos += n;
    }

    int n = snprintf(buf + pos, buflen - pos, "]}");
    if (n < 0 || (size_t)(pos + n) >= buflen)
        return -1;
    return pos + n;
}
