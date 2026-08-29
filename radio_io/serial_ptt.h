/* HERMES Modem - Serial modem-control-line PTT backend
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SERIAL_PTT_H_
#define SERIAL_PTT_H_

#include <stdbool.h>

#include "radio_io.h"

/* Open the serial device and drive the configured modem-control line(s) to
 * their idle (un-keyed) state, honouring inversion.  Unconfigured lines are
 * left untouched.  Returns 0 on success, -1 on error. */
int serial_ptt_open(const ptt_config_t *config);

/* Assert or release PTT on the configured line(s).  0 on success, -1 on error. */
int serial_ptt_set(bool on);

/* Release PTT and close the device.  Safe to call when not open. */
void serial_ptt_close(void);

/* Read the live state of the modem-control lines through the OPEN handle.
 * Reading them by opening the port again does not work -- opening a serial
 * port re-asserts RTS/DTR, so a second handle always reports them high.
 * Returns 0 on success, -1 if not open or the ioctl fails. */
int serial_ptt_line_state(bool *rts_high, bool *dtr_high);

#endif /* SERIAL_PTT_H_ */
