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
#define CFG_KEY_TX_DELAY_MS         "audio:tx_delay_ms"
#define CFG_KEY_TNC_KEEPALIVE_S     "tnc:keepalive_s"
#define CFG_KEY_TNC_BUFFER_REPORT_MS "tnc:buffer_report_ms"
#define CFG_KEY_ARQ_DATA_RETRY_SLOTS          "arq:data_retry_slots"
#define CFG_KEY_ARQ_MODE_HOLD_DOWNGRADE_S     "arq:mode_hold_after_downgrade_s"
#define CFG_KEY_ARQ_LADDER_UP_SUCCESSES       "arq:ladder_up_successes"
#define CFG_KEY_ARQ_RETRY_DOWNGRADE_THRESHOLD "arq:retry_downgrade_threshold"
#define CFG_KEY_ARQ_CHANNEL_GUARD_MS          "arq:channel_guard_ms"
#define CFG_KEY_ARQ_ISS_POST_ACK_GUARD_MS     "arq:iss_post_ack_guard_ms"
#define CFG_KEY_ARQ_KEEPALIVE_INTERVAL_S      "arq:keepalive_interval_s"
#define CFG_KEY_ARQ_KEEPALIVE_MISS_LIMIT      "arq:keepalive_miss_limit"
#define CFG_KEY_ARQ_PEER_PAYLOAD_HOLD_S       "arq:peer_payload_hold_s"
#define CFG_KEY_ARQ_STARTUP_MAX_S             "arq:startup_max_s"
#define CFG_KEY_ARQ_RETRY_STAGGER_MS          "arq:retry_stagger_ms"
#define CFG_KEY_BUSY_DETECT                   "channel:busy_detect"
#define CFG_KEY_BUSY_THRESHOLD_DB             "channel:busy_threshold_db"
#define CFG_KEY_BUSY_HYSTERESIS_DB            "channel:busy_hysteresis_db"
#define CFG_KEY_BUSY_ON_DEBOUNCE_MS           "channel:busy_on_debounce_ms"
#define CFG_KEY_BUSY_HANG_MS                  "channel:busy_hang_ms"

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
    int      data_retry_slots;          /* ARQ: DATA retries before a downgrade
                                         * cycle. Default 10, clamped 1..64.   */
    int      mode_hold_after_downgrade_s;/* ARQ: hold lower mode after a forced
                                         * downgrade. Default 6, clamped 0..60. */
    int      ladder_up_successes;       /* ARQ: clean ACKs to step the mode
                                         * ladder up. Default 2, clamped 1..16. */
    int      retry_downgrade_threshold; /* ARQ: consecutive retries that force a
                                         * downgrade. Default 2, clamped 1..16. */
    int      channel_guard_ms;          /* ARQ: IRS response guard after decode.
                                         * Default 700, clamped 200..3000.      */
    int      iss_post_ack_guard_ms;     /* ARQ: ISS guard before resuming DATA
                                         * TX after ACK. Default 900, 200..3000.*/
    int      keepalive_interval_s;      /* ARQ: keepalive TX interval. Default
                                         * 20, clamped 5..120.                  */
    int      keepalive_miss_limit;      /* ARQ: missed keepalives before drop.
                                         * Default 5, clamped 2..20.            */
    int      peer_payload_hold_s;       /* ARQ: hold peer payload mode after
                                         * activity. Default 15, clamped 1..120.*/
    int      startup_max_s;             /* ARQ: control-mode-only startup
                                         * window. Default 10, clamped 2..60.   */
    int      retry_stagger_ms;          /* ARQ: deterministic per-node retry
                                         * offset. 0 disables. Default 2000,
                                         * clamped 0..5000.                  */
    float    tx_gain_db;            /* Linear-equivalent gain on the modulator
                                    * TX samples, in dB. 0.0 = no change.
                                    * Range -20.0 .. +20.0 (clamped). */
    int      tx_delay_ms;           /* Delay (ms) after PTT ON before audio is
                                    * sent to the playback device on TX.
                                    * Default 10 covers the local radio
                                    * relay's switch time; raise it to give a
                                    * slow external PTT more time to key up
                                    * before the signal starts. Clamped
                                    * 0..2000. */
    int      tnc_keepalive_s;       /* IAMALIVE interval on the TNC control
                                    * port. Default 60, clamped 5..600.   */
    int      tnc_buffer_report_ms;  /* BUFFER report interval on the TNC
                                    * control port. Default 1000, clamped
                                    * 100..10000.                          */
    bool     busy_detect;           /* Channel-busy detector on/off. Default
                                    * false (opt-in). Emits VARA "BUSY ON/OFF". */
    int      busy_threshold_db;     /* Passband peak dB above noise floor to
                                    * call BUSY. Default 10, clamped 3..40.  */
    int      busy_hysteresis_db;    /* Release hysteresis in dB. Default 3,
                                    * clamped 0..20.                          */
    int      busy_on_debounce_ms;   /* Sustain above threshold before BUSY.
                                    * Default 300, clamped 0..5000.           */
    int      busy_hang_ms;          /* Sustain below release before CLEAR.
                                    * Default 1500, clamped 0..10000.         */
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

/* Map an AUDIO_SUBSYSTEM_* constant back to its CLI name ("alsa", "oss", ...).
 * Returns a static string; never NULL. */
const char *cfg_sound_system_name(int sys);

#endif /* CFG_UTILS_H_ */
