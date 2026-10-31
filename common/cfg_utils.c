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

#include <iniparser.h>
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
#include "message_store.h"

void cfg_set_defaults(mercury_config *cfg)
{
    cfg->ui_enabled         = false;
    cfg->ui_port            = UI_DEFAULT_PORT;       /* 10000  */
    cfg->tls_enabled        = false;                 /* ws     */
    snprintf(cfg->tls_cert, sizeof(cfg->tls_cert), "%s", CFG_SSL_CERT);
    snprintf(cfg->tls_key,  sizeof(cfg->tls_key),  "%s", CFG_SSL_KEY);
    cfg->waterfall_enabled  = true;
    cfg->ptt.method         = PTT_METHOD_NONE;
    cfg->ptt.device[0]      = '\0';
    cfg->ptt.serial_line       = PTT_LINE_RTS;
    cfg->ptt.serial_invert_rts = false;
    cfg->ptt.serial_invert_dtr = false;
    cfg->ptt.cm108_gpio        = PTT_CM108_GPIO_DEFAULT;
    cfg->ptt.hamlib_model   = RADIO_TYPE_NONE;
    cfg->ptt.hamlib_log_level = 0;
    cfg->ptt.hamlib_serial_speed = 0;
    cfg->ptt.hamlib_conf[0] = '\0';
    cfg->input_device[0]    = '\0';
    cfg->output_device[0]   = '\0';
    cfg->capture_channel    = LEFT;
    cfg->sound_system       = -1;  /* auto: resolved by audioio_pick_default_subsystem() */
    cfg->arq_tcp_base_port  = DEFAULT_ARQ_PORT;       /* 8300   */
    cfg->broadcast_tcp_port = DEFAULT_BROADCAST_PORT; /* 8100  */
    cfg->callsign[0]        = '\0';                   /* unset: no default callsign */
    cfg->verbose            = false;
    cfg->trace              = false;
    cfg->log_utc            = false;
    cfg->freedv_verbosity   = 0;
    cfg->disconnect_drain_timeout_s = ARQ_DISCONNECT_DRAIN_TIMEOUT_S_DEFAULT;
    cfg->tnc_keepalive_s = 60;
    cfg->tnc_buffer_report_ms = 1000;
    cfg->tx_gain_db            = 0.0f;
    cfg->tx_delay_ms           = 10;
    cfg->retry_stagger_ms            = ARQ_RETRY_STAGGER_MS_DEFAULT;
    cfg->silence_fallback_s          = ARQ_SILENCE_FALLBACK_S_DEFAULT;
    cfg->removed_keys                = 0;
    cfg->mfsk_tx_only                = false;
    cfg->max_keydown_s               = 0;
    cfg->busy_detect                 = false;
    cfg->busy_threshold_db           = 10;
    cfg->busy_hysteresis_db          = 3;
    cfg->busy_on_debounce_ms         = 300;
    cfg->busy_hang_ms                = 1500;
    cfg->store_enabled               = true;
    cfg->store_path[0]               = '\0';
    cfg->store_max_messages          = MSG_STORE_DEFAULT_MAX_LINES;
    cfg->hl2_frequency_hz            = 0;
    cfg->hl2_rx_gain_db              = 20;
    cfg->hl2_tx_drive                = 128;
    cfg->hl2_pa                      = true;
}

/* Map a sound-system name to the AUDIO_SUBSYSTEM_* constant.
 * Returns -1 (auto) for unrecognised strings. */
int cfg_sound_system_parse(const char *s)
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
    if (!strcmp(s, "rtp"))       return AUDIO_SUBSYSTEM_RTP;
    if (!strcmp(s, "hl2"))       return AUDIO_SUBSYSTEM_HL2;
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

const char *cfg_ptt_method_name(ptt_method_t method)
{
    switch (method) {
    case PTT_METHOD_NONE:       return "none";
    case PTT_METHOD_HAMLIB:     return "hamlib";
    case PTT_METHOD_SERIAL:     return "serial";
    case PTT_METHOD_CM108:      return "cm108";
    case PTT_METHOD_HERMES_SHM: return "hermes_shm";
    default:                    return "unknown";
    }
}

bool cfg_ptt_method_parse(const char *name, ptt_method_t *method)
{
    if (!name || !method) return false;
    if (!strcmp(name, "none"))            *method = PTT_METHOD_NONE;
    else if (!strcmp(name, "hamlib"))     *method = PTT_METHOD_HAMLIB;
    else if (!strcmp(name, "serial"))     *method = PTT_METHOD_SERIAL;
    else if (!strcmp(name, "cm108"))      *method = PTT_METHOD_CM108;
    else if (!strcmp(name, "hermes_shm")) *method = PTT_METHOD_HERMES_SHM;
    /* "serial_rts" was the name before DTR and inversion existed.  Keep it
     * working -- it is already in people's mercury.ini -- as plain RTS. */
    else if (!strcmp(name, "serial_rts")) *method = PTT_METHOD_SERIAL;
    else return false;
    return true;
}

bool cfg_ptt_config_set_method(ptt_config_t *config, const char *name)
{
    if (!config || !cfg_ptt_method_parse(name, &config->method))
        return false;

    if (!strcmp(name, "serial_rts"))
    {
        config->serial_line = PTT_LINE_RTS;
        config->serial_invert_rts = false;
        config->serial_invert_dtr = false;
    }
    return true;
}

const char *cfg_ptt_line_name(ptt_line_t line)
{
    switch (line) {
    case PTT_LINE_DTR:  return "dtr";
    case PTT_LINE_BOTH: return "both";
    case PTT_LINE_RTS:
    default:            return "rts";
    }
}

bool cfg_ptt_line_parse(const char *name, ptt_line_t *line)
{
    if (!name || !line) return false;
    if (!strcmp(name, "rts"))       *line = PTT_LINE_RTS;
    else if (!strcmp(name, "dtr"))  *line = PTT_LINE_DTR;
    else if (!strcmp(name, "both")) *line = PTT_LINE_BOTH;
    else return false;
    return true;
}

/* invert = none | rts | dtr | both -- one key rather than two booleans,
 * because that is how the operator thinks about a cable. */
const char *cfg_ptt_invert_name(bool invert_rts, bool invert_dtr)
{
    if (invert_rts && invert_dtr) return "both";
    if (invert_rts)               return "rts";
    if (invert_dtr)               return "dtr";
    return "none";
}

bool cfg_ptt_invert_parse(const char *name, bool *invert_rts, bool *invert_dtr)
{
    if (!name || !invert_rts || !invert_dtr) return false;
    if (!strcmp(name, "none"))      { *invert_rts = false; *invert_dtr = false; }
    else if (!strcmp(name, "rts"))  { *invert_rts = true;  *invert_dtr = false; }
    else if (!strcmp(name, "dtr"))  { *invert_rts = false; *invert_dtr = true;  }
    else if (!strcmp(name, "both")) { *invert_rts = true;  *invert_dtr = true;  }
    else return false;
    return true;
}

/* [arq] keys this version no longer reads: the stop-and-wait plane's, and
 * the two guard intervals, which are fixed now.  A file that still sets one
 * loads as before; cfg_read notes which (removed_keys) and the engine warns
 * once the logger is up.  Saving the configuration from the UI drops them. */
static const char *const cfg_removed_keys[] = {
    "arq:no_progress_timeout_s",  "arq:data_retry_slots",
    "arq:mode_hold_after_downgrade_s", "arq:ladder_up_successes",
    "arq:retry_downgrade_threshold", "arq:keepalive_interval_s",
    "arq:keepalive_miss_limit",   "arq:peer_payload_hold_s",
    "arq:startup_max_s",          "arq:channel_guard_ms",
    "arq:iss_post_ack_guard_ms",
};
#define CFG_REMOVED_KEYS ((int)(sizeof(cfg_removed_keys) / sizeof(cfg_removed_keys[0])))

const char *cfg_removed_key_name(int k)
{
    if (k < 0 || k >= CFG_REMOVED_KEYS)
        return NULL;
    return cfg_removed_keys[k] + strlen("arq:");
}

bool cfg_read(mercury_config *cfg, const char *ini_path)
{
    dictionary *ini = iniparser_load(ini_path);
    if (!ini) {
        fprintf(stderr, "cfg_read: cannot parse file: %s\n", ini_path);
        return false;
    }
    cfg->removed_keys = 0;
    for (int k = 0; k < CFG_REMOVED_KEYS; k++)
        if (iniparser_find_entry(ini, cfg_removed_keys[k]))
            cfg->removed_keys |= 1u << k;

    int b;
    int i;
    const char *s;

    b = iniparser_getboolean(ini, CFG_KEY_UI_ENABLED, cfg->ui_enabled ? 1 : 0);
    cfg->ui_enabled = (bool) b;

    i = iniparser_getint(ini, CFG_KEY_UI_PORT, cfg->ui_port);
    cfg->ui_port = (uint16_t) i;

    s = iniparser_getstring(ini, CFG_KEY_UI_PROTOCOL, cfg->tls_enabled ? "wss" : "ws");
    cfg->tls_enabled = (s && !strcmp(s, "wss"));

    /* NULL as the fallback, not cfg->tls_cert: iniparser hands the default
     * straight back when the key is absent, and snprintf'ing a buffer onto
     * itself is undefined -- it emptied the path instead of keeping it. */
    s = iniparser_getstring(ini, CFG_KEY_UI_TLS_CERT, NULL);
    if (s && *s)
        snprintf(cfg->tls_cert, sizeof(cfg->tls_cert), "%s", s);

    s = iniparser_getstring(ini, CFG_KEY_UI_TLS_KEY, NULL);
    if (s && *s)
        snprintf(cfg->tls_key, sizeof(cfg->tls_key), "%s", s);

    b = iniparser_getboolean(ini, CFG_KEY_WATERFALL_ENABLED, cfg->waterfall_enabled ? 1 : 0);
    cfg->waterfall_enabled = (bool) b;

    /* Read the legacy radio-oriented keys first, then let an explicit [ptt]
     * section override them.  This supports old files as well as a simple
     * migration where only ptt.method was added by hand. */
    s = iniparser_getstring(ini, CFG_KEY_RADIO_MODEL, NULL);
    if (s) {
        i = iniparser_getint(ini, CFG_KEY_RADIO_MODEL, cfg->ptt.hamlib_model);
        if (i == RADIO_TYPE_SHM)
        {
            cfg->ptt.method = PTT_METHOD_HERMES_SHM;
            cfg->ptt.hamlib_model = RADIO_TYPE_NONE;
        }
        else if (i > 0)
        {
            cfg->ptt.method = PTT_METHOD_HAMLIB;
            cfg->ptt.hamlib_model = i;
        }
        else
        {
            cfg->ptt.method = PTT_METHOD_NONE;
            cfg->ptt.hamlib_model = RADIO_TYPE_NONE;
        }
    }

    s = iniparser_getstring(ini, CFG_KEY_RADIO_DEVICE, NULL);
    if (s) {
        strncpy(cfg->ptt.device, s, sizeof(cfg->ptt.device) - 1);
        cfg->ptt.device[sizeof(cfg->ptt.device) - 1] = '\0';
    }

    i = iniparser_getint(ini, CFG_KEY_HAMLIB_LOG_LEVEL,
                         cfg->ptt.hamlib_log_level);
    if (i >= 0 && i <= 6)
        cfg->ptt.hamlib_log_level = i;

    i = iniparser_getint(ini, CFG_KEY_RADIO_SERIAL_SPEED,
                         cfg->ptt.hamlib_serial_speed);
    if (i >= 0)
        cfg->ptt.hamlib_serial_speed = i;

    s = iniparser_getstring(ini, CFG_KEY_PTT_DEVICE, NULL);
    if (s) {
        strncpy(cfg->ptt.device, s, sizeof(cfg->ptt.device) - 1);
        cfg->ptt.device[sizeof(cfg->ptt.device) - 1] = '\0';
    }

    s = iniparser_getstring(ini, CFG_KEY_PTT_LINE, NULL);
    if (s && !cfg_ptt_line_parse(s, &cfg->ptt.serial_line)) {
        fprintf(stderr, "Invalid ptt.line '%s' in %s. Use rts, dtr, or both.\n",
                s, ini_path);
        iniparser_freedict(ini);
        return false;
    }

    s = iniparser_getstring(ini, CFG_KEY_PTT_INVERT, NULL);
    if (s && !cfg_ptt_invert_parse(s, &cfg->ptt.serial_invert_rts,
                                       &cfg->ptt.serial_invert_dtr)) {
        fprintf(stderr, "Invalid ptt.invert '%s' in %s. Use none, rts, dtr, or both.\n",
                s, ini_path);
        iniparser_freedict(ini);
        return false;
    }

    /* Parse the method after its serial options so the legacy serial_rts
     * spelling retains its fixed meaning: RTS only, not inverted. */
    s = iniparser_getstring(ini, CFG_KEY_PTT_METHOD, NULL);
    if (s && !cfg_ptt_config_set_method(&cfg->ptt, s)) {
        fprintf(stderr, "cfg_read: invalid PTT method '%s'\n", s);
        iniparser_freedict(ini);
        return false;
    }

    i = iniparser_getint(ini, CFG_KEY_PTT_CM108_GPIO, cfg->ptt.cm108_gpio);
    if (i >= 1 && i <= 4)
        cfg->ptt.cm108_gpio = i;

    i = iniparser_getint(ini, CFG_KEY_PTT_HAMLIB_MODEL,
                         cfg->ptt.hamlib_model);
    cfg->ptt.hamlib_model = i;

    i = iniparser_getint(ini, CFG_KEY_PTT_HAMLIB_LOG,
                         cfg->ptt.hamlib_log_level);
    if (i >= 0 && i <= 6)
        cfg->ptt.hamlib_log_level = i;

    i = iniparser_getint(ini, CFG_KEY_PTT_HAMLIB_SPEED,
                         cfg->ptt.hamlib_serial_speed);
    if (i >= 0)
        cfg->ptt.hamlib_serial_speed = i;

    s = iniparser_getstring(ini, CFG_KEY_PTT_HAMLIB_CONF, NULL);
    if (s) {
        strncpy(cfg->ptt.hamlib_conf, s, sizeof(cfg->ptt.hamlib_conf) - 1);
        cfg->ptt.hamlib_conf[sizeof(cfg->ptt.hamlib_conf) - 1] = '\0';
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
        cfg->sound_system = cfg_sound_system_parse(s);

    i = iniparser_getint(ini, CFG_KEY_ARQ_TCP_BASE_PORT, cfg->arq_tcp_base_port);
    cfg->arq_tcp_base_port = i;

    i = iniparser_getint(ini, CFG_KEY_BROADCAST_TCP_PORT, cfg->broadcast_tcp_port);
    cfg->broadcast_tcp_port = i;

    s = iniparser_getstring(ini, CFG_KEY_CALLSIGN, NULL);
    if (s) {
        strncpy(cfg->callsign, s, sizeof(cfg->callsign) - 1);
        cfg->callsign[sizeof(cfg->callsign) - 1] = '\0';
    }

    b = iniparser_getboolean(ini, CFG_KEY_VERBOSE, cfg->verbose ? 1 : 0);
    cfg->verbose = (bool) b;

    b = iniparser_getboolean(ini, CFG_KEY_TRACE, cfg->trace ? 1 : 0);
    cfg->trace = (bool) b;
    if (cfg->trace)
        cfg->verbose = true;   /* trace implies verbose */

    b = iniparser_getboolean(ini, CFG_KEY_LOG_UTC, cfg->log_utc ? 1 : 0);
    cfg->log_utc = (bool) b;

    i = iniparser_getint(ini, CFG_KEY_FREEDV_VERBOSITY, cfg->freedv_verbosity);
    if (i >= 0 && i <= 3)
        cfg->freedv_verbosity = i;

    i = iniparser_getint(ini, CFG_KEY_DISCONNECT_DRAIN_TIMEOUT_S, cfg->disconnect_drain_timeout_s);
    if (i > 0)
        cfg->disconnect_drain_timeout_s = i;

    i = iniparser_getint(ini, CFG_KEY_TNC_KEEPALIVE_S, cfg->tnc_keepalive_s);
    if (i >= 5 && i <= 600)
        cfg->tnc_keepalive_s = i;

    i = iniparser_getint(ini, CFG_KEY_TNC_BUFFER_REPORT_MS, cfg->tnc_buffer_report_ms);
    if (i >= 100 && i <= 10000)
        cfg->tnc_buffer_report_ms = i;

    i = iniparser_getint(ini, CFG_KEY_ARQ_RETRY_STAGGER_MS, cfg->retry_stagger_ms);
    if (i >= 0 && i <= ARQ_RETRY_STAGGER_MS_MAX) cfg->retry_stagger_ms = i;

    i = iniparser_getint(ini, CFG_KEY_ARQ_SILENCE_FALLBACK_S, cfg->silence_fallback_s);
    if (i >= ARQ_SILENCE_FALLBACK_S_MIN && i <= ARQ_SILENCE_FALLBACK_S_MAX)
        cfg->silence_fallback_s = i;

    cfg->mfsk_tx_only = (bool) iniparser_getboolean(ini, CFG_KEY_ARQ_MFSK_TX_ONLY,
                                                    cfg->mfsk_tx_only ? 1 : 0);

    i = iniparser_getint(ini, CFG_KEY_ARQ_MAX_KEYDOWN_S, cfg->max_keydown_s);
    if (i == 0 || (i >= 5 && i <= 30)) cfg->max_keydown_s = i;

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

    cfg->store_enabled = (bool) iniparser_getboolean(ini, CFG_KEY_STORE_ENABLED,
                                                     cfg->store_enabled ? 1 : 0);

    s = iniparser_getstring(ini, CFG_KEY_STORE_PATH, NULL);
    if (s) {
        strncpy(cfg->store_path, s, sizeof(cfg->store_path) - 1);
        cfg->store_path[sizeof(cfg->store_path) - 1] = '\0';
    }

    i = iniparser_getint(ini, CFG_KEY_STORE_MAX_MESSAGES, cfg->store_max_messages);
    if (i >= 1 && i <= 10000) cfg->store_max_messages = i;

    i = iniparser_getint(ini, CFG_KEY_HL2_FREQUENCY_HZ, (int) cfg->hl2_frequency_hz);
    if (i >= 0 && i <= 60000000) cfg->hl2_frequency_hz = (uint32_t) i;

    i = iniparser_getint(ini, CFG_KEY_HL2_RX_GAIN_DB, cfg->hl2_rx_gain_db);
    if (i >= -12 && i <= 48) cfg->hl2_rx_gain_db = i;

    i = iniparser_getint(ini, CFG_KEY_HL2_TX_DRIVE, cfg->hl2_tx_drive);
    if (i >= 0 && i <= 255) cfg->hl2_tx_drive = i;

    cfg->hl2_pa = (bool) iniparser_getboolean(ini, CFG_KEY_HL2_PA, cfg->hl2_pa ? 1 : 0);

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
    case AUDIO_SUBSYSTEM_RTP:       return "rtp";
    case AUDIO_SUBSYSTEM_HL2:       return "hl2";
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
    fprintf(f, "ui_tls_cert = %s\n",      cfg->tls_cert);
    fprintf(f, "ui_tls_key = %s\n",       cfg->tls_key);
    fprintf(f, "waterfall_enabled = %s\n", cfg->waterfall_enabled ? "true" : "false");
    cfg_escape_str(escaped, sizeof(escaped), cfg->input_device);
    fprintf(f, "input_device = \"%s\"\n",  escaped);

    cfg_escape_str(escaped, sizeof(escaped), cfg->output_device);
    fprintf(f, "output_device = \"%s\"\n", escaped);

    fprintf(f, "capture_channel = %s\n",  capture_channel_name(cfg->capture_channel));
    fprintf(f, "sound_system = %s\n",     cfg_sound_system_name(cfg->sound_system));
    fprintf(f, "arq_tcp_base_port = %d\n", cfg->arq_tcp_base_port);
    fprintf(f, "broadcast_tcp_port = %d\n", cfg->broadcast_tcp_port);
    cfg_escape_str(escaped, sizeof(escaped), cfg->callsign);
    fprintf(f, "callsign = \"%s\"\n", escaped);
    fprintf(f, "verbose = %s\n",           cfg->verbose ? "true" : "false");
    fprintf(f, "trace = %s\n",             cfg->trace ? "true" : "false");
    fprintf(f, "log_utc = %s\n",           cfg->log_utc ? "true" : "false");
    fprintf(f, "freedv_verbosity = %d\n",  cfg->freedv_verbosity);
    fprintf(f, "\n[ptt]\n");
    fprintf(f, "method = %s\n", cfg_ptt_method_name(cfg->ptt.method));
    cfg_escape_str(escaped, sizeof(escaped), cfg->ptt.device);
    fprintf(f, "device = \"%s\"\n", escaped);
    fprintf(f, "line = %s\n", cfg_ptt_line_name(cfg->ptt.serial_line));
    fprintf(f, "invert = %s\n",
            cfg_ptt_invert_name(cfg->ptt.serial_invert_rts,
                                cfg->ptt.serial_invert_dtr));
    fprintf(f, "cm108_gpio = %d\n", cfg->ptt.cm108_gpio);
    fprintf(f, "hamlib_model = %d\n", cfg->ptt.hamlib_model);
    fprintf(f, "hamlib_serial_speed = %d\n", cfg->ptt.hamlib_serial_speed);
    cfg_escape_str(escaped, sizeof(escaped), cfg->ptt.hamlib_conf);
    fprintf(f, "hamlib_conf = \"%s\"\n", escaped);
    fprintf(f, "hamlib_log_level = %d\n", cfg->ptt.hamlib_log_level);

    fprintf(f, "\n[arq]\n");
    fprintf(f, "disconnect_drain_timeout_s = %d\n", cfg->disconnect_drain_timeout_s);
    fprintf(f, "retry_stagger_ms = %d\n", cfg->retry_stagger_ms);
    fprintf(f, "silence_fallback_s = %d\n", cfg->silence_fallback_s);
    fprintf(f, "mfsk_tx_only = %s\n", cfg->mfsk_tx_only ? "true" : "false");
    fprintf(f, "max_keydown_s = %d\n", cfg->max_keydown_s);

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

    fprintf(f, "\n[store]\n");
    fprintf(f, "enabled = %s\n", cfg->store_enabled ? "true" : "false");
    cfg_escape_str(escaped, sizeof(escaped), cfg->store_path);
    fprintf(f, "path = \"%s\"\n", escaped);
    fprintf(f, "max_messages = %d\n", cfg->store_max_messages);

    fprintf(f, "\n[hl2]\n");
    fprintf(f, "frequency_hz = %u\n", (unsigned) cfg->hl2_frequency_hz);
    fprintf(f, "rx_gain_db = %d\n", cfg->hl2_rx_gain_db);
    fprintf(f, "tx_drive = %d\n", cfg->hl2_tx_drive);
    fprintf(f, "pa = %s\n", cfg->hl2_pa ? "true" : "false");

    fclose(f);
    return true;
}
