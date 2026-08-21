/* HERMES Modem - Cross-platform serial RTS PTT backend
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SERIAL_RTS_H_
#define SERIAL_RTS_H_

#include <stdbool.h>

/* Open |device_path| for manual modem-line control.  Hardware flow control is
 * disabled and RTS is forced inactive before success is reported. */
int serial_rts_open(const char *device_path);

/* Assert or clear RTS.  Returns 0 on success, -1 on failure. */
int serial_rts_set(bool on);

/* Force RTS inactive and close the port.  Safe to call when not open. */
void serial_rts_close(void);

#endif /* SERIAL_RTS_H_ */
