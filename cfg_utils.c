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

#include <iniparser/iniparser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg_utils.h"
#include "audioio/audioio.h"
#include "radio_io.h"
#include "tcp_interfaces.h"
#include "gui_interface/ui_communication.h"

void cfg_set_defaults(mercury_config *cfg)
{
    cfg->ui_enabled         = false;
    cfg->ui_port            = UI_DEFAULT_PORT;       /* 10000  */
    cfg->tls_enabled        = false;                 /* ws     */
    cfg->waterfall_enabled  = true;
    cfg->radio_type         = RADIO_TYPE_NONE;       /* -1     */
    cfg->radio_device[0]    = '\0';
    cfg->input_device[0]    = '\0';
    cfg->output_device[0]   = '\0';
    cfg->capture_channel    = LEFT;
#ifdef _WIN32
    cfg->sound_system       = AUDIO_SUBSYSTEM_DSOUND;
#else
    cfg->sound_system       = AUDIO_SUBSYSTEM_ALSA;
#endif
    cfg->arq_tcp_base_port  = DEFAULT_ARQ_PORT;       /* 8300   */
    cfg->broadcast_tcp_port = DEFAULT_BROADCAST_PORT; /* 8100  */
}

/* Map a sound-system name to the AUDIO_SUBSYSTEM_* constant.
 * Returns -1 (auto) for unrecognised strings. */
static int parse_sound_system(const char *s)
{
    if (!s) return -1;
    if (!strcmp(s, "alsa"))      return AUDIO_SUBSYSTEM_ALSA;
    if (!strcmp(s, "pulse"))     return AUDIO_SUBSYSTEM_PULSE;
    if (!strcmp(s, "dsound"))    return AUDIO_SUBSYSTEM_DSOUND;
    if (!strcmp(s, "wasapi"))    return AUDIO_SUBSYSTEM_WASAPI;
    if (!strcmp(s, "oss"))       return AUDIO_SUBSYSTEM_OSS;
    if (!strcmp(s, "coreaudio")) return AUDIO_SUBSYSTEM_COREAUDIO;
    if (!strcmp(s, "aaudio"))    return AUDIO_SUBSYSTEM_AAUDIO;
    if (!strcmp(s, "shm"))       return AUDIO_SUBSYSTEM_SHM;
    return -1;
}

/* Map a capture-channel name to LEFT / RIGHT / STEREO.
 * Returns LEFT for unrecognised strings. */
static int parse_capture_channel(const char *s)
{
    if (!s) return LEFT;
    if (!strcmp(s, "left")   || !strcmp(s, "LEFT"))   return LEFT;
    if (!strcmp(s, "right")  || !strcmp(s, "RIGHT"))  return RIGHT;
    if (!strcmp(s, "stereo") || !strcmp(s, "STEREO")) return STEREO;
    return LEFT;
}

bool cfg_read(mercury_config *cfg, const char *ini_path)
{
    dictionary *ini = iniparser_load(ini_path);
    if (!ini) {
        fprintf(stderr, "cfg_read: cannot parse file: %s\n", ini_path);
        return false;
    }

    int b;
    int i;
    const char *s;

    b = iniparser_getboolean(ini, CFG_KEY_UI_ENABLED, cfg->ui_enabled ? 1 : 0);
    cfg->ui_enabled = (bool) b;

    i = iniparser_getint(ini, CFG_KEY_UI_PORT, cfg->ui_port);
    cfg->ui_port = (uint16_t) i;

    s = iniparser_getstring(ini, CFG_KEY_UI_PROTOCOL, cfg->tls_enabled ? "wss" : "ws");
    cfg->tls_enabled = (s && !strcmp(s, "wss"));

    b = iniparser_getboolean(ini, CFG_KEY_WATERFALL_ENABLED, cfg->waterfall_enabled ? 1 : 0);
    cfg->waterfall_enabled = (bool) b;

    i = iniparser_getint(ini, CFG_KEY_RADIO_MODEL, cfg->radio_type);
    cfg->radio_type = i;

    s = iniparser_getstring(ini, CFG_KEY_RADIO_DEVICE, cfg->radio_device);

    s = iniparser_getstring(ini, CFG_KEY_INPUT_DEVICE, cfg->input_device);

    s = iniparser_getstring(ini, CFG_KEY_OUTPUT_DEVICE, cfg->output_device);

    s = iniparser_getstring(ini, CFG_KEY_CAPTURE_CHANNEL, NULL);
    if (s)
        cfg->capture_channel = parse_capture_channel(s);

    s = iniparser_getstring(ini, CFG_KEY_SOUND_SYSTEM, NULL);
    if (s)
        cfg->sound_system = parse_sound_system(s);

    i = iniparser_getint(ini, CFG_KEY_ARQ_TCP_BASE_PORT, cfg->arq_tcp_base_port);
    cfg->arq_tcp_base_port = i;

    i = iniparser_getint(ini, CFG_KEY_BROADCAST_TCP_PORT, cfg->broadcast_tcp_port);
    cfg->broadcast_tcp_port = i;

    iniparser_freedict(ini);
    return true;
}
