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

#include "iniparser/iniparser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "cfg_utils.h"
#include "../audioio/audioio.h"
#include "../radio_io/radio_io.h"
#include "../data_interfaces/tcp_interfaces.h"
#include "../gui_interface/ui_communication.h"
#include "../datalink_arq/arq_protocol.h"

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
    cfg->sound_system       = -1;  /* auto: resolved by audioio_pick_default_subsystem() */
    cfg->arq_tcp_base_port  = DEFAULT_ARQ_PORT;       /* 8300   */
    cfg->broadcast_tcp_port = DEFAULT_BROADCAST_PORT; /* 8100  */
    cfg->verbose            = false;
    cfg->freedv_verbosity   = 0;
    cfg->hamlib_log_level   = 0;
    cfg->radio_serial_speed = 0;  /* 0 = use hamlib default */
    cfg->no_progress_timeout_s = ARQ_NO_PROGRESS_TIMEOUT_S_DEFAULT;
    cfg->disconnect_drain_timeout_s = ARQ_DISCONNECT_DRAIN_TIMEOUT_S_DEFAULT;
    cfg->tnc_keepalive_s = 60;
    cfg->tnc_buffer_report_ms = 1000;
    cfg->tx_gain_db            = 0.0f;
    cfg->tx_delay_ms           = 10;
    cfg->data_retry_slots            = ARQ_DATA_RETRY_SLOTS_DEFAULT;
    cfg->mode_hold_after_downgrade_s = ARQ_MODE_HOLD_AFTER_DOWNGRADE_S_DEFAULT;
    cfg->ladder_up_successes         = ARQ_LADDER_UP_SUCCESSES_DEFAULT;
    cfg->retry_downgrade_threshold   = ARQ_RETRY_DOWNGRADE_THRESHOLD_DEFAULT;
    cfg->channel_guard_ms            = ARQ_CHANNEL_GUARD_MS_DEFAULT;
    cfg->iss_post_ack_guard_ms       = ARQ_ISS_POST_ACK_GUARD_MS_DEFAULT;
    cfg->peer_payload_hold_s         = ARQ_PEER_PAYLOAD_HOLD_S_DEFAULT;
    cfg->busy_detect                 = false;
    cfg->busy_threshold_db           = 10;
    cfg->busy_hysteresis_db          = 3;
    cfg->busy_on_debounce_ms         = 300;
    cfg->busy_hang_ms                = 1500;
}

/* Map a sound-system name to the AUDIO_SUBSYSTEM_* constant.
 * Returns -1 (auto) for unrecognised strings. */
static int parse_sound_system(const char *s)
{
    if (!s) return -1;
    if (!strcmp(s, "auto"))      return -1;
    if (!strcmp(s, "alsa"))      return AUDIO_SUBSYSTEM_ALSA;
    if (!strcmp(s, "pulse"))     return AUDIO_SUBSYSTEM_PULSE;
    if (!strcmp(s, "dsound"))    return AUDIO_SUBSYSTEM_DSOUND;
    if (!strcmp(s, "wasapi"))    return AUDIO_SUBSYSTEM_WASAPI;
    if (!strcmp(s, "oss"))       return AUDIO_SUBSYSTEM_OSS;
    if (!strcmp(s, "coreaudio")) return AUDIO_SUBSYSTEM_COREAUDIO;
    if (!strcmp(s, "aaudio"))    return AUDIO_SUBSYSTEM_AAUDIO;
    if (!strcmp(s, "shm"))       return AUDIO_SUBSYSTEM_SHM;
    if (!strcmp(s, "null"))      return AUDIO_SUBSYSTEM_NULL;
    if (!strcmp(s, "fifo"))      return AUDIO_SUBSYSTEM_FIFO;
    if (!strcmp(s, "sock"))      return AUDIO_SUBSYSTEM_SOCK;
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

    s = iniparser_getstring(ini, CFG_KEY_RADIO_DEVICE, NULL);
    if (s) {
        strncpy(cfg->radio_device, s, sizeof(cfg->radio_device) - 1);
        cfg->radio_device[sizeof(cfg->radio_device) - 1] = '\0';
    }

    s = iniparser_getstring(ini, CFG_KEY_INPUT_DEVICE, NULL);
    if (s) {
        strncpy(cfg->input_device, s, sizeof(cfg->input_device) - 1);
        cfg->input_device[sizeof(cfg->input_device) - 1] = '\0';
    }

    s = iniparser_getstring(ini, CFG_KEY_OUTPUT_DEVICE, NULL);
    if (s) {
        strncpy(cfg->output_device, s, sizeof(cfg->output_device) - 1);
        cfg->output_device[sizeof(cfg->output_device) - 1] = '\0';
    }

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

    b = iniparser_getboolean(ini, CFG_KEY_VERBOSE, cfg->verbose ? 1 : 0);
    cfg->verbose = (bool) b;

    i = iniparser_getint(ini, CFG_KEY_FREEDV_VERBOSITY, cfg->freedv_verbosity);
    if (i >= 0 && i <= 3)
        cfg->freedv_verbosity = i;

    i = iniparser_getint(ini, CFG_KEY_HAMLIB_LOG_LEVEL, cfg->hamlib_log_level);
    if (i >= 0 && i <= 6)
        cfg->hamlib_log_level = i;

    i = iniparser_getint(ini, CFG_KEY_RADIO_SERIAL_SPEED, cfg->radio_serial_speed);
    if (i >= 0)
        cfg->radio_serial_speed = i;

    i = iniparser_getint(ini, CFG_KEY_NO_PROGRESS_TIMEOUT_S, cfg->no_progress_timeout_s);
    if (i > 0)
        cfg->no_progress_timeout_s = i;

    i = iniparser_getint(ini, CFG_KEY_DISCONNECT_DRAIN_TIMEOUT_S, cfg->disconnect_drain_timeout_s);
    if (i > 0)
        cfg->disconnect_drain_timeout_s = i;

    i = iniparser_getint(ini, CFG_KEY_TNC_KEEPALIVE_S, cfg->tnc_keepalive_s);
    if (i >= 5 && i <= 600)
        cfg->tnc_keepalive_s = i;

    i = iniparser_getint(ini, CFG_KEY_TNC_BUFFER_REPORT_MS, cfg->tnc_buffer_report_ms);
    if (i >= 100 && i <= 10000)
        cfg->tnc_buffer_report_ms = i;

    i = iniparser_getint(ini, CFG_KEY_ARQ_DATA_RETRY_SLOTS, cfg->data_retry_slots);
    if (i >= 1 && i <= 64)   cfg->data_retry_slots = i;

    i = iniparser_getint(ini, CFG_KEY_ARQ_MODE_HOLD_DOWNGRADE_S, cfg->mode_hold_after_downgrade_s);
    if (i >= 0 && i <= 60)   cfg->mode_hold_after_downgrade_s = i;

    i = iniparser_getint(ini, CFG_KEY_ARQ_LADDER_UP_SUCCESSES, cfg->ladder_up_successes);
    if (i >= 1 && i <= 16)   cfg->ladder_up_successes = i;

    i = iniparser_getint(ini, CFG_KEY_ARQ_RETRY_DOWNGRADE_THRESHOLD, cfg->retry_downgrade_threshold);
    if (i >= 1 && i <= 16)   cfg->retry_downgrade_threshold = i;

    i = iniparser_getint(ini, CFG_KEY_ARQ_CHANNEL_GUARD_MS, cfg->channel_guard_ms);
    if (i >= 200 && i <= 3000) cfg->channel_guard_ms = i;

    i = iniparser_getint(ini, CFG_KEY_ARQ_ISS_POST_ACK_GUARD_MS, cfg->iss_post_ack_guard_ms);
    if (i >= 200 && i <= 3000) cfg->iss_post_ack_guard_ms = i;



    i = iniparser_getint(ini, CFG_KEY_ARQ_PEER_PAYLOAD_HOLD_S, cfg->peer_payload_hold_s);
    if (i >= 1 && i <= 120)  cfg->peer_payload_hold_s = i;


    cfg->busy_detect = (bool) iniparser_getboolean(ini, CFG_KEY_BUSY_DETECT,
                                                   cfg->busy_detect ? 1 : 0);

    i = iniparser_getint(ini, CFG_KEY_BUSY_THRESHOLD_DB, cfg->busy_threshold_db);
    if (i >= 3 && i <= 40)      cfg->busy_threshold_db = i;

    i = iniparser_getint(ini, CFG_KEY_BUSY_HYSTERESIS_DB, cfg->busy_hysteresis_db);
    if (i >= 0 && i <= 20)      cfg->busy_hysteresis_db = i;

    i = iniparser_getint(ini, CFG_KEY_BUSY_ON_DEBOUNCE_MS, cfg->busy_on_debounce_ms);
    if (i >= 0 && i <= 5000)    cfg->busy_on_debounce_ms = i;

    i = iniparser_getint(ini, CFG_KEY_BUSY_HANG_MS, cfg->busy_hang_ms);
    if (i >= 0 && i <= 10000)   cfg->busy_hang_ms = i;

    double d = iniparser_getdouble(ini, CFG_KEY_TX_GAIN_DB, (double)cfg->tx_gain_db);
    if (!isfinite(d)) d = 0.0;  /* malformed/non-finite INI value -> default */
    if (d < -20.0) d = -20.0;
    if (d >  20.0) d =  20.0;
    cfg->tx_gain_db = (float)d;

    i = iniparser_getint(ini, CFG_KEY_TX_DELAY_MS, cfg->tx_delay_ms);
    if (i >= 0 && i <= 2000) cfg->tx_delay_ms = i;

    iniparser_freedict(ini);
    return true;
}

/* Map an AUDIO_SUBSYSTEM_* constant back to a name string. */
const char *cfg_sound_system_name(int sys)
{
    switch (sys) {
    case AUDIO_SUBSYSTEM_ALSA:      return "alsa";
    case AUDIO_SUBSYSTEM_PULSE:     return "pulse";
    case AUDIO_SUBSYSTEM_DSOUND:    return "dsound";
    case AUDIO_SUBSYSTEM_WASAPI:    return "wasapi";
    case AUDIO_SUBSYSTEM_OSS:       return "oss";
    case AUDIO_SUBSYSTEM_COREAUDIO: return "coreaudio";
    case AUDIO_SUBSYSTEM_AAUDIO:    return "aaudio";
    case AUDIO_SUBSYSTEM_SHM:       return "shm";
    case AUDIO_SUBSYSTEM_NULL:      return "null";
    case AUDIO_SUBSYSTEM_FIFO:      return "fifo";
    case AUDIO_SUBSYSTEM_SOCK:      return "sock";
    default:                        return "auto";
    }
}

/* Map a capture-channel constant back to a name string. */
static const char *capture_channel_name(int ch)
{
    switch (ch) {
    case RIGHT:  return "right";
    case STEREO: return "stereo";
    default:     return "left";
    }
}

/* Escape \ and " in |in| so the result can be written inside a double-quoted
 * INI value and round-trip through iniparser's parse_quoted_value(). */
static void cfg_escape_str(char *out, size_t out_size, const char *in)
{
    size_t o = 0;
    for (const char *p = in; *p != '\0' && o + 2 < out_size; p++) {
        if (*p == '\\' || *p == '"')
            out[o++] = '\\';
        out[o++] = (char)*p;
    }
    out[o] = '\0';
}

bool cfg_write(const mercury_config *cfg, const char *ini_path)
{
    FILE *f = fopen(ini_path, "w");
    if (!f) {
        fprintf(stderr, "cfg_write: cannot open file for writing: %s\n", ini_path);
        return false;
    }

    // Worst-case escaped length: every byte in the source becomes two bytes.
    char escaped[2049];

    fprintf(f, "[main]\n");
    fprintf(f, "ui_enabled = %s\n",      cfg->ui_enabled ? "true" : "false");
    fprintf(f, "ui_port = %d\n",          cfg->ui_port);
    fprintf(f, "ui_protocol = %s\n",      cfg->tls_enabled ? "wss" : "ws");
    fprintf(f, "waterfall_enabled = %s\n", cfg->waterfall_enabled ? "true" : "false");
    fprintf(f, "radio_model = %d\n",      cfg->radio_type);

    cfg_escape_str(escaped, sizeof(escaped), cfg->radio_device);
    fprintf(f, "radio_device = \"%s\"\n",  escaped);

    cfg_escape_str(escaped, sizeof(escaped), cfg->input_device);
    fprintf(f, "input_device = \"%s\"\n",  escaped);

    cfg_escape_str(escaped, sizeof(escaped), cfg->output_device);
    fprintf(f, "output_device = \"%s\"\n", escaped);

    fprintf(f, "capture_channel = %s\n",  capture_channel_name(cfg->capture_channel));
    fprintf(f, "sound_system = %s\n",     cfg_sound_system_name(cfg->sound_system));
    fprintf(f, "arq_tcp_base_port = %d\n", cfg->arq_tcp_base_port);
    fprintf(f, "broadcast_tcp_port = %d\n", cfg->broadcast_tcp_port);
    fprintf(f, "verbose = %s\n",           cfg->verbose ? "true" : "false");
    fprintf(f, "freedv_verbosity = %d\n",  cfg->freedv_verbosity);
    fprintf(f, "hamlib_log_level = %d\n",  cfg->hamlib_log_level);
    fprintf(f, "radio_serial_speed = %d\n", cfg->radio_serial_speed);

    fprintf(f, "\n[arq]\n");
    fprintf(f, "no_progress_timeout_s = %d\n", cfg->no_progress_timeout_s);
    fprintf(f, "disconnect_drain_timeout_s = %d\n", cfg->disconnect_drain_timeout_s);
    fprintf(f, "data_retry_slots = %d\n", cfg->data_retry_slots);
    fprintf(f, "mode_hold_after_downgrade_s = %d\n", cfg->mode_hold_after_downgrade_s);
    fprintf(f, "ladder_up_successes = %d\n", cfg->ladder_up_successes);
    fprintf(f, "retry_downgrade_threshold = %d\n", cfg->retry_downgrade_threshold);
    fprintf(f, "channel_guard_ms = %d\n", cfg->channel_guard_ms);
    fprintf(f, "iss_post_ack_guard_ms = %d\n", cfg->iss_post_ack_guard_ms);
    fprintf(f, "peer_payload_hold_s = %d\n", cfg->peer_payload_hold_s);

    fprintf(f, "\n[channel]\n");
    fprintf(f, "busy_detect = %s\n", cfg->busy_detect ? "true" : "false");
    fprintf(f, "busy_threshold_db = %d\n", cfg->busy_threshold_db);
    fprintf(f, "busy_hysteresis_db = %d\n", cfg->busy_hysteresis_db);
    fprintf(f, "busy_on_debounce_ms = %d\n", cfg->busy_on_debounce_ms);
    fprintf(f, "busy_hang_ms = %d\n", cfg->busy_hang_ms);

    fprintf(f, "\n[audio]\n");
    fprintf(f, "tx_gain_db = %.2f\n", cfg->tx_gain_db);
    fprintf(f, "tx_delay_ms = %d\n", cfg->tx_delay_ms);

    fprintf(f, "\n[tnc]\n");
    fprintf(f, "keepalive_s = %d\n", cfg->tnc_keepalive_s);
    fprintf(f, "buffer_report_ms = %d\n", cfg->tnc_buffer_report_ms);

    fclose(f);
    return true;
}
