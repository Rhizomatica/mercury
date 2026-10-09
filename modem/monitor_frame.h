/* Mercury monitor-mode frame reporter
 *
 * Monitor mode decodes every Mercury mode in parallel and reports each
 * CRC-valid frame to the host as a "MONITOR ..." line on the control port
 * (VARA-monitor style).  These two functions live in their own translation
 * unit so the frame-parsing path can be unit-tested without pulling in the
 * whole modem/audio stack.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_MONITOR_FRAME_H
#define MERCURY_MONITOR_FRAME_H

#include <stddef.h>
#include <stdint.h>

/* Map a modem mode enum to its name ("DATAC15", "MFSK", ...).  Never NULL. */
const char *mode_name_from_enum(int mode);

/* Report one CRC-valid decoded frame to the host as a "MONITOR ..." line.
 *
 * `data` is the frame as decoded, including its trailing 2-byte CRC;
 * `nbytes_out` is the total decoded byte count (>= 2).  `mode` names the
 * decoder that produced it and `snr_est` its SNR at decode time.  The frame
 * is parsed (connect/CQ/control/data/broadcast) and dispatched through
 * tnc_send_monitor(); an unparseable frame is still reported as "FRAME".
 */
void process_monitor_frame(const uint8_t *data, size_t nbytes_out,
                           int mode, float snr_est);

#endif /* MERCURY_MONITOR_FRAME_H */
