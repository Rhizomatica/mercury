/* HERMES Modem
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_history.h"

char *ui_history_frame_build(const char *snap, size_t count, size_t snap_len)
{
    /* Worst case: the wrapper (~31 bytes) plus every line, with the snapshot's
     * trailing newlines replaced by commas.  snap_len already counts those
     * newlines, so snap_len + 64 is comfortably enough. */
    char *buf = malloc(snap_len + 64);
    if (!buf)
        return NULL;

    size_t off = (size_t)snprintf(buf, snap_len + 64,
                                  "{\"type\":\"history\",\"messages\":[");
    const char *p = snap;
    for (size_t i = 0; i < count; i++)
    {
        const char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
        if (i > 0)
            buf[off++] = ',';
        memcpy(buf + off, p, linelen);
        off += linelen;
        p = nl ? nl + 1 : p + linelen;
    }
    off += (size_t)snprintf(buf + off, snap_len + 64 - off, "]}");
    return buf;
}
