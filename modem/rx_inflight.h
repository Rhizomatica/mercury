/* The frame a decoder is locked onto: when it began, pass by pass of the RX loop.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RX_INFLIGHT_H_
#define RX_INFLIGHT_H_

#include <stdbool.h>
#include <stdint.h>

/* How long a lock outlives its decoder's last report.  A worker that stops
 * (rebound, flushed, stalled) says nothing more, and its lock must not stay
 * "in flight" for good.  The same bound as ARQ_ARRIVAL_STALE_MS. */
#define RX_INFLIGHT_STALE_MS 750

/* When the frame a decoder is locked onto began, after one pass of the RX loop;
 * 0 when there is none.
 *
 * The loop passes every 20 ms, and an MFSK decoder reports every 40 ms (80 ms
 * at 500 Hz).  A pass with no report is no news: read as "no lock", it reset
 * the start on every other pass, and every LISTEN OFF logged "0.0 s into" a
 * frame (774 of 774 lines, K7EK and KG9NI, 19-20 Nov 2026). */
static inline uint64_t rx_inflight_next(uint64_t since, uint64_t heard, bool reported,
                                        bool sync, bool decoded, uint64_t now)
{
    if (!reported)
        return since && now - heard >= RX_INFLIGHT_STALE_MS ? 0 : since;
    if (sync && !decoded)
        return since ? since : now;
    return 0;
}

#endif
