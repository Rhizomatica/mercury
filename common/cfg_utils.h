/* HERMES Modem - Mercury Configuration Utilities
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Pedro Messetti <pedromessetti.rhizomatica@gmail.com>
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

#ifndef CFG_UTILS_H_
#define CFG_UTILS_H_

#include <stdbool.h>
#include <stdint.h>

/* Configuration keys (section:key) */
#define CFG_KEY_UI_ENABLED          "main:ui_enabled"
#define CFG_KEY_UI_PORT             "main:ui_port"
#define CFG_KEY_UI_PROTOCOL         "main:ui_protocol"
#define CFG_KEY_WATERFALL_ENABLED   "main:waterfall_enabled"
#define CFG_KEY_RADIO_MODEL         "main:radio_model"
#define CFG_KEY_RADIO_DEVICE        "main:radio_device"
#define CFG_KEY_INPUT_DEVICE        "main:input_device"
#define CFG_KEY_OUTPUT_DEVICE       "main:output_device"
#define CFG_KEY_CAPTURE_CHANNEL     "main:capture_channel"
#define CFG_KEY_SOUND_SYSTEM        "main:sound_system"
#define CFG_KEY_ARQ_TCP_BASE_PORT   "main:arq_tcp_base_port"
#define CFG_KEY_BROADCAST_TCP_PORT  "main:broadcast_tcp_port"
#define CFG_KEY_VERBOSE             "main:verbose"
#define CFG_KEY_FREEDV_VERBOSITY    "main:freedv_verbosity"
#define CFG_KEY_HAMLIB_LOG_LEVEL    "main:hamlib_log_level"
#define CFG_KEY_RADIO_SERIAL_SPEED  "main:radio_serial_speed"
#define CFG_KEY_NO_PROGRESS_TIMEOUT_S "arq:no_progress_timeout_s"
#define CFG_KEY_DISCONNECT_DRAIN_TIMEOUT_S "arq:disconnect_drain_timeout_s"
#define CFG_KEY_TX_GAIN_DB          "audio:tx_gain_db"
#define CFG_KEY_TNC_KEEPALIVE_S     "tnc:keepalive_s"
#define CFG_KEY_TNC_BUFFER_REPORT_MS "tnc:buffer_report_ms"

/* Holds all values read from the init configuration file */
typedef struct {
    bool     ui_enabled;
    uint16_t ui_port;
    bool     tls_enabled;           /* false = ws, true = wss */
    bool     waterfall_enabled;
    int      radio_type;            /* RADIO_TYPE_NONE by default */
    char     radio_device[1024];
    char     input_device[512];
    char     output_device[512];
    int      capture_channel;       /* LEFT / RIGHT / STEREO */
    int      sound_system;          /* AUDIO_SUBSYSTEM_* or -1 for auto */
    int      arq_tcp_base_port;
    int      broadcast_tcp_port;
    bool     verbose;
    int      freedv_verbosity;      /* 0..3 */
    int      hamlib_log_level;      /* 0..6 */
    int      radio_serial_speed;   /* 0 = use hamlib default */
    int      no_progress_timeout_s;/* ARQ: disconnect when no forward progress
                                    * (no advancing ACK) for this many seconds
                                    * after data-retry exhaustion. Default 180. */
    int      disconnect_drain_timeout_s;/* ARQ: absolute cap (s) on how long an
                                    * app DISCONNECT stays deferred while
                                    * draining the last TX bytes. Default 30. */
    float    tx_gain_db;            /* Linear-equivalent gain on the modulator
                                    * TX samples, in dB. 0.0 = no change.
                                    * Range -20.0 .. +20.0 (clamped). */
    int      tnc_keepalive_s;       /* IAMALIVE interval on the TNC control
                                    * port. Default 60, clamped 5..600.   */
    int      tnc_buffer_report_ms;  /* BUFFER report interval on the TNC
                                    * control port. Default 1000, clamped
                                    * 100..10000.                          */
} mercury_config;

/* Load configuration from an INI file into |cfg|.
 * Fields not present in the file keep their default values.
 * Returns true on success, false if the file cannot be parsed. */
bool cfg_read(mercury_config *cfg, const char *ini_path);

/* Write |cfg| to an INI file at |ini_path|.
 * Returns true on success, false if the file cannot be written. */
bool cfg_write(const mercury_config *cfg, const char *ini_path);

/* Populate |cfg| with compile/runtime defaults. Call before cfg_read(). */
void cfg_set_defaults(mercury_config *cfg);

#endif /* CFG_UTILS_H_ */
