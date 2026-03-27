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
} mercury_config;

/* Load configuration from an INI file into |cfg|.
 * Fields not present in the file keep their default values.
 * Returns true on success, false if the file cannot be parsed. */
bool cfg_read(mercury_config *cfg, const char *ini_path);

/* Populate |cfg| with compile/runtime defaults. Call before cfg_read(). */
void cfg_set_defaults(mercury_config *cfg);

#endif /* CFG_UTILS_H_ */
