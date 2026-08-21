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

/* Legacy radio_model values, retained for INI/CLI compatibility. */
#define RADIO_TYPE_NONE (-1)
#define RADIO_TYPE_SHM  0

#define PTT_DEVICE_PATH_MAX 1024

typedef enum {
    PTT_METHOD_NONE = 0,
    PTT_METHOD_HAMLIB,
    PTT_METHOD_SERIAL,      /* modem-control lines on a serial port */
    PTT_METHOD_HERMES_SHM,
    PTT_METHOD_CM108        /* GPIO on a CM108-class USB sound chip */
} ptt_method_t;

/* Which modem-control line(s) key the transmitter.
 *
 * BOTH is not redundancy: several interfaces (the All-In-One Cable among them)
 * expect RTS and DTR driven together, and keying only one leaves them silent. */
typedef enum {
    PTT_LINE_RTS = 0,
    PTT_LINE_DTR,
    PTT_LINE_BOTH
} ptt_line_t;

/* Configuration for the one active PTT backend.  Backend-specific fields are
 * kept here so configuration, runtime restart and UI transport all pass one
 * coherent value instead of overloading the rig model as the backend type.
 * Settings for the inactive backends are retained, so an operator can switch
 * method and back without re-entering them. */
typedef struct {
    ptt_method_t method;
    char device[PTT_DEVICE_PATH_MAX];

    /* serial */
    ptt_line_t serial_line;
    bool serial_invert_rts;  /* cable keys on the LOW state of the line */
    bool serial_invert_dtr;

    /* cm108: GPIO pin 1..4.  GPIO3 is the de-facto standard and the default. */
    int cm108_gpio;

    /* hamlib */
    int hamlib_model;
    int hamlib_serial_speed; /* 0 = use Hamlib model default */
    int hamlib_log_level;    /* 0..6 */
} ptt_config_t;

#define PTT_CM108_GPIO_DEFAULT 3

/* Initialize the selected PTT backend.
 * Returns 0 on success, -1 on failure. */
int radio_io_init(const ptt_config_t *config);

/* Shutdown direct PTT control and release resources. */
void radio_io_shutdown(void);

/* Returns true if a PTT backend is active. */
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

/* Restart the PTT subsystem with a new configuration.
 * Thread-safe — blocks key_on / key_off during the restart cycle.
 * Returns 0 on success, -1 on failure. */
int radio_io_restart(const ptt_config_t *config);

/* Copy the current runtime PTT configuration.  Shutdown or a failed backend
 * open changes the method to NONE while retaining backend-specific settings. */
void radio_io_get_config(ptt_config_t *config);

/* Return the currently selected PTT method. */
ptt_method_t radio_io_get_ptt_method(void);

/* Return a thread-local snapshot of the device path.  The pointer remains
 * valid until this function is called again on the same thread. */
const char *radio_io_get_device_path(void);

/* Legacy view of the current config: RADIO_TYPE_NONE, RADIO_TYPE_SHM, or the
 * Hamlib model ID.  Serial RTS has no legacy representation and returns NONE. */
int radio_io_get_radio_type(void);

/* Return the hamlib debug level used by the current (or last) init. */
int radio_io_get_hamlib_log_level(void);

/* Return the serial speed used by the current (or last) init (0 = hamlib default). */
int radio_io_get_serial_speed(void);

#endif /* RADIO_IO_H_ */
