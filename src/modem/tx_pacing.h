/* TX burst pacing: how long to sleep before the next waterfall step.
 *
 * Copyright (C) 2026 Rhizomatica
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
 * ---------------------------------------------------------------------------
 *
 * The TX thread holds PTT for the length of the burst while publishing the
 * waterfall every TX_SPECTRUM_STEP_MS.  The obvious loop -- usleep(step) once
 * per slice -- is wrong, because usleep may return late but never early, so the
 * per-slice overshoot ACCUMULATES over the ~75-90 slices in a burst.
 *
 * That is invisible on Linux (~7 ms over a 4.41 s burst) and fatal on Windows,
 * whose default scheduler tick is ~15.6 ms: the same burst then keys for 5.5 s,
 * i.e. 1.1 s of dead carrier after the audio has finished.  The peer's IRS
 * answers 700 ms after it decodes, so its ACK lands squarely inside that tail,
 * is never heard, and the link retransmits to ack_timeout forever.
 *
 * The fix is to re-derive each sleep from elapsed time against an absolute
 * deadline, which bounds the total error at one tick no matter how coarse the
 * tick is.  Kept as a pure function so the property can be tested without a
 * sound card, a radio, or a real clock.
 */

#ifndef TX_PACING_H
#define TX_PACING_H

#include <stdint.h>

/* Advance one waterfall slice.
 *
 *   waited_us    signal-time position already scheduled (0 on the first call)
 *   elapsed_us   real time since the burst started
 *   duration_us  total burst length in signal time
 *   step_us      waterfall publish interval
 *
 * Writes the next scheduled position to *next_waited_us and returns how long to
 * sleep -- 0 when already behind the deadline, so a slow host catches up by
 * skipping sleep rather than by extending the keyed window.
 */
static inline uint64_t tx_pace_sleep_us(uint64_t waited_us, uint64_t elapsed_us,
                                        uint64_t duration_us, uint64_t step_us,
                                        uint64_t *next_waited_us)
{
    uint64_t target_us = waited_us + step_us;
    if (target_us > duration_us)
        target_us = duration_us;

    *next_waited_us = target_us;

    return (target_us > elapsed_us) ? (target_us - elapsed_us) : 0;
}

#endif /* TX_PACING_H */
