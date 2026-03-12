/* HERMES Modem - Radio I/O abstraction
 *
 * Copyright (C) 2025 Rhizomatica
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
 *
 */

#ifndef RADIO_IO_H_
#define RADIO_IO_H_

#include <stdbool.h>

#define RADIO_TYPE_NONE (-1)
#define RADIO_TYPE_SHM  0

/* Initialize radio control.
 * radio_type: RADIO_TYPE_NONE (disabled), RADIO_TYPE_SHM, or hamlib model ID (>0).
 * device_path: hamlib device file or ip:port (ignored for SHM/NONE).
 * Returns 0 on success, -1 on failure. */
int radio_io_init(int radio_type, const char *device_path);

/* Shutdown radio control and release resources. */
void radio_io_shutdown(void);

/* Returns true if radio control is active (SHM or hamlib). */
bool radio_io_enabled(void);

/* Key transmitter on (PTT ON). */
void radio_io_key_on(void);

/* Key transmitter off (PTT OFF). */
void radio_io_key_off(void);

/* List all hamlib-supported radio models and exit. */
void radio_io_list_models(void);

/* Populate arrays with hamlib radio IDs and display names.
 * Returns number of radios written (up to max_count), or 0 if hamlib
 * is not compiled in. */
int radio_io_get_radio_list(char ids[][16], char names[][64], int max_count);

/* Restart radio subsystem with a new radio type.
 * Thread-safe — blocks key_on / key_off during the restart cycle.
 * Returns 0 on success, -1 on failure. */
int radio_io_restart(int new_radio_type, const char *device_path);

/* Return the device path used by the current (or last) init. */
const char *radio_io_get_device_path(void);

/* Return the current radio type (RADIO_TYPE_NONE, RADIO_TYPE_SHM, or hamlib model ID). */
int radio_io_get_radio_type(void);

#endif /* RADIO_IO_H_ */
