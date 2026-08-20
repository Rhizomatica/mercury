/* HERMES Modem
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

// WebSocket-based UI communication between Mercury backend and MercuryQT UI.
// Publisher threads broadcast status/device lists; command callback handles
// incoming UI commands via the websocket server.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <math.h>

#include "../common/os_interop.h"


#include "ui_communication.h"
#include "ui_status.h"

#include "../datalink_arq/arq.h"
#include "../data_interfaces/net.h"
#include "../data_interfaces/tcp_interfaces.h"
#include "../common/hermes_log.h"
#include "../modem/freedv/modem_stats.h"
#include "../modem/modem.h"
#include "../radio_io/radio_io.h"  /* RADIO_TYPE_NONE */
#include "../radio_io/rigctl_parse.h"  /* preload_radio_list */

#include "../audioio/audio_dev_limits.h"   /* AUDIO_DEV_STR_MAX */

/* Hand-written rather than #include "audioio.h": that header pulls in ffbase,
 * which is not on this file's include path. */
extern int get_soundcard_list(int audio_system, int mode,
                              char ids[][AUDIO_DEV_STR_MAX], char dev_names[][AUDIO_DEV_STR_MAX],
                              int max_count);

/* The enumerator writes rows this wide and we copy them straight into
 * ui_device_t; if the UI struct were ever the narrower of the two, long
 * PulseAudio node names would be cut again exactly as in issue #185. */
_Static_assert(AUDIO_DEV_STR_MAX >= UI_DEV_ID_MAX,
               "device id rows must not be narrower than ui_device_t.id");

/* Audio path health; see audioio.h.  Declared here rather than included for
 * the same reason audioio_restart is: audioio.h drags in ffbase. */
extern bool audioio_health_ok(char *reason, size_t reasonlen);

extern int audioio_restart(const char *capture_dev, const char *playback_dev,
                           int audio_subsys, int capture_channel_layout);

// global shutdown flag from main.c
/* Must match the definition in common/mercury_engine.c: _Atomic, not
 * volatile.  This global is written from the termination signal handler and
 * polled by every worker loop; volatile orders nothing between threads, and
 * declaring the same object differently in different translation units is
 * undefined behaviour on top of that.  Plain assignment and test still work. */
extern _Atomic bool shutdown_;

#define UI_LOG_TAG "ui-comm"

// Called by the WebSocket server thread when a new UI client connects.
// Sets pending flags so the publisher sends device lists and radio list.
static void ws_connect_handler(void *user_data)
{
    ui_ctx_t *ctx = (ui_ctx_t *)user_data;
    if (ctx) {
        ctx->soundcard_list_pending = 1;
        ctx->radio_list_pending = 1;
    }
}

/* Latest gathered status, published by ui_publisher_thread and read by the
 * embedded UI across the CGo bridge.  The publisher owns the gathering; the
 * bridge only ever copies out, so a slow or stopped UI can never hold up the
 * engine. */
static pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;
static ui_status_t     g_status;
static bool            g_status_valid = false;

/* The running UI context, so the CGo bridge can route commands through the
 * same handler the websocket uses.  Set by ui_comm_init(), cleared by
 * ui_comm_shutdown(). */
static ui_ctx_t       *g_ui_ctx = NULL;

// ---------------- WS COMMAND CALLBACK ----------------
// Called by the websocket server thread when a command JSON arrives from UI.
static int ws_command_handler(const ws_command_t *cmd, void *user_data)
{
    return ui_comm_handle_command((ui_ctx_t *)user_data, cmd);
}

/* Every UI command lands here, whether it arrived as websocket JSON from a
 * remote client or as a direct call from the embedded UI.  One implementation
 * means local and remote cannot diverge in behaviour. */
int ui_comm_handle_command(ui_ctx_t *ctx, const ws_command_t *cmd)
{
    if (!ctx || !cmd)
        return -1;

    HLOGI(UI_LOG_TAG, "WS CMD from UI: command=\"%s\" value=\"%s\" value2=\"%s\"",
          cmd->command, cmd->value, cmd->value2);

    if (strcmp(cmd->command, "set_audio_config") == 0) {
        // value = capture_dev, value2 = playback_dev, value3 = input_channel
        if (cmd->value[0])
        {
            strncpy(ctx->selected_capture_dev, cmd->value,
                    sizeof(ctx->selected_capture_dev) - 1);
            ctx->selected_capture_dev[sizeof(ctx->selected_capture_dev) - 1] = '\0';
        }
        HLOGI(UI_LOG_TAG, "Capture device set to: %s", ctx->selected_capture_dev);

        if (cmd->value2[0])
        {
            strncpy(ctx->selected_playback_dev, cmd->value2,
                    sizeof(ctx->selected_playback_dev) - 1);
            ctx->selected_playback_dev[sizeof(ctx->selected_playback_dev) - 1] = '\0';
        }
        HLOGI(UI_LOG_TAG, "Playback device set to: %s", ctx->selected_playback_dev);

        if (strcmp(cmd->value3, "right") == 0)
            ctx->rx_input_channel = 1;  // RIGHT
        else if (strcmp(cmd->value3, "stereo") == 0)
            ctx->rx_input_channel = 2;  // STEREO
        else
            ctx->rx_input_channel = 0;  // LEFT (default)
        HLOGI(UI_LOG_TAG, "Input channel set to: %s (%d)",
              cmd->value3, ctx->rx_input_channel);

        HLOGI(UI_LOG_TAG, "Restarting audioio subsystem (capture=%s playback=%s channel=%d)",
              ctx->selected_capture_dev, ctx->selected_playback_dev, ctx->rx_input_channel);
        audioio_restart(ctx->selected_capture_dev, ctx->selected_playback_dev,
                        ctx->audio_system, ctx->rx_input_channel);
        HLOGI(UI_LOG_TAG, "Audioio subsystem restarted successfully");

        // Persist audio config to INI
        pthread_mutex_lock(&ctx->cfg_mutex);
        strncpy(ctx->cfg.input_device, ctx->selected_capture_dev,
                sizeof(ctx->cfg.input_device) - 1);
        ctx->cfg.input_device[sizeof(ctx->cfg.input_device) - 1] = '\0';
        strncpy(ctx->cfg.output_device, ctx->selected_playback_dev,
                sizeof(ctx->cfg.output_device) - 1);
        ctx->cfg.output_device[sizeof(ctx->cfg.output_device) - 1] = '\0';
        ctx->cfg.capture_channel = ctx->rx_input_channel;
        if (ctx->cfg_path[0] && cfg_write(&ctx->cfg, ctx->cfg_path))
            HLOGI(UI_LOG_TAG, "Config saved to %s", ctx->cfg_path);
        pthread_mutex_unlock(&ctx->cfg_mutex);

    } else if (strcmp(cmd->command, "set_radio_config") == 0) {
        int new_radio_type = atoi(cmd->value);
        const char *dev_path = cmd->value2;
        int new_serial_speed = cmd->value3[0] ? atoi(cmd->value3) : radio_io_get_serial_speed();
        if (new_serial_speed < 0) new_serial_speed = 0;
        HLOGI(UI_LOG_TAG, "Radio set_radio_config command: model_id=%d device_path=\"%s\" baud_rate=%d",
              new_radio_type, dev_path, new_serial_speed);
        if (new_radio_type == RADIO_TYPE_NONE) {
            radio_io_restart(RADIO_TYPE_NONE, NULL, radio_io_get_hamlib_log_level(), new_serial_speed);
            HLOGI(UI_LOG_TAG, "Radio type set to NONE - radio subsystem shut down");
            ctx->radio_list_pending = 1;
        } else {
            int rc = radio_io_restart(new_radio_type, dev_path, radio_io_get_hamlib_log_level(), new_serial_speed);
            if (rc == 0) {
                HLOGI(UI_LOG_TAG, "Radioio subsystem restarted (model=%d, path=%s, baud=%d)",
                      new_radio_type, dev_path, new_serial_speed);
                ctx->radio_list_pending = 1;
            } else {
                HLOGE(UI_LOG_TAG, "Radioio subsystem restart FAILED (model=%d, path=%s, baud=%d, rc=%d)",
                      new_radio_type, dev_path, new_serial_speed, rc);
                return rc;
            }
        }

        // Persist radio config to INI
        pthread_mutex_lock(&ctx->cfg_mutex);
        ctx->cfg.radio_type = new_radio_type;
        strncpy(ctx->cfg.radio_device, dev_path ? dev_path : "",
                sizeof(ctx->cfg.radio_device) - 1);
        ctx->cfg.radio_device[sizeof(ctx->cfg.radio_device) - 1] = '\0';
        ctx->cfg.radio_serial_speed = new_serial_speed;
        if (ctx->cfg_path[0] && cfg_write(&ctx->cfg, ctx->cfg_path))
            HLOGI(UI_LOG_TAG, "Config saved to %s", ctx->cfg_path);
        pthread_mutex_unlock(&ctx->cfg_mutex);

    } else if (strcmp(cmd->command, "set_waterfall") == 0) {
        bool enable = (strcmp(cmd->value, "off") != 0);
        ui_comm_set_waterfall(enable);
        HLOGI(UI_LOG_TAG, "Waterfall %s by UI command", enable ? "enabled" : "disabled");

    } else if (strcmp(cmd->command, "set_tx_gain") == 0) {
        /* value carries the dB string; clamp to plan range and convert to
         * linear before pushing to the modulator.  Persist to INI so the
         * next start picks it up automatically. */
        float db = (float)atof(cmd->value);
        if (!isfinite(db)) db = 0.0f;  /* reject NaN/Inf from a bad client */
        if (db < -20.0f) db = -20.0f;
        if (db >  20.0f) db =  20.0f;
        float linear = powf(10.0f, db / 20.0f);
        modem_set_tx_gain(linear);
        HLOGI(UI_LOG_TAG, "TX gain set to %.2f dB (linear=%.4f)", db, linear);
        pthread_mutex_lock(&ctx->cfg_mutex);
        ctx->cfg.tx_gain_db = db;
        if (ctx->cfg_path[0] && cfg_write(&ctx->cfg, ctx->cfg_path))
            HLOGI(UI_LOG_TAG, "Config saved to %s", ctx->cfg_path);
        pthread_mutex_unlock(&ctx->cfg_mutex);

    } else {
        HLOGW(UI_LOG_TAG, "Unknown UI command: %s", cmd->command);
        return -1;
    }

    return 0;
}

void ui_comm_set_waterfall(bool enabled)
{
    ui_ctx_t *ctx = g_ui_ctx;
    if (!ctx)
        return;
    modem_set_spectrum_enabled(enabled);

    pthread_mutex_lock(&ctx->cfg_mutex);
    ctx->waterfall_enabled = enabled;
    ctx->cfg.waterfall_enabled = enabled;

    if (enabled) {
        if (ctx->spec_tid == 0) {
            atomic_store_explicit(&ctx->spec_run, true, memory_order_relaxed);
            if (pthread_create(&ctx->spec_tid, NULL, spectrum_publisher_thread, ctx) != 0) {
                atomic_store_explicit(&ctx->spec_run, false, memory_order_relaxed);
                ctx->spec_tid = 0;
                HLOGE(UI_LOG_TAG, "pthread_create(spec) failed: %s", strerror(errno));
            } else {
                HLOGI(UI_LOG_TAG, "Spectrum publisher thread started (delayed start)");
            }
        }
    } else {
        if (ctx->spec_tid != 0) {
            atomic_store_explicit(&ctx->spec_run, false, memory_order_relaxed);
            pthread_join(ctx->spec_tid, NULL);
            ctx->spec_tid = 0;
            HLOGI(UI_LOG_TAG, "Spectrum publisher thread stopped");
        }
    }

    if (ctx->cfg_path[0] && cfg_write(&ctx->cfg, ctx->cfg_path))
        HLOGI(UI_LOG_TAG, "Config saved to %s", ctx->cfg_path);
    pthread_mutex_unlock(&ctx->cfg_mutex);
}

void ui_comm_get_tcp_ports(int *arq_base_port, int *broadcast_port)
{
    ui_ctx_t *ctx = g_ui_ctx;
    if (!ctx) {
        if (arq_base_port) *arq_base_port = 8300;
        if (broadcast_port) *broadcast_port = 8100;
        return;
    }
    pthread_mutex_lock(&ctx->cfg_mutex);
    if (arq_base_port) *arq_base_port = ctx->cfg.arq_tcp_base_port;
    if (broadcast_port) *broadcast_port = ctx->cfg.broadcast_tcp_port;
    pthread_mutex_unlock(&ctx->cfg_mutex);
}

// ---------------- UI PUBLISHER THREAD ----------------
// Periodically gathers modem/ARQ/network status and sends it to the UI.

/* ---- Device enumeration ----------------------------------------------------
 * One implementation per list, used by the websocket publisher and by the
 * embedded UI through the bridge.  The websocket path used to build its JSON
 * inline from get_soundcard_list(); routing both through here is what keeps a
 * local UI and a remote one showing the same devices and the same selection. */

int ui_comm_get_audio_devices(ui_device_kind_t kind, ui_device_t *out, int max,
                              char *selected, size_t sel_len)
{
    ui_ctx_t *ctx = g_ui_ctx;
    if (!ctx || !out || max <= 0)
        return 0;

    /* get_soundcard_list() takes mode 1 = capture, 0 = playback. */
    int mode = (kind == UI_DEV_CAPTURE) ? 1 : 0;

    /* Heap: 32 rows x AUDIO_DEV_STR_MAX x 2 arrays is 16 KB, and this is
     * called from the websocket publisher thread, whose stack is 512 KB by
     * default on macOS. */
    char (*ids)[AUDIO_DEV_STR_MAX]   = malloc(sizeof(*ids)   * 32);
    char (*names)[AUDIO_DEV_STR_MAX] = malloc(sizeof(*names) * 32);
    if (!ids || !names) {
        free(ids); free(names);
        HLOGE(UI_LOG_TAG, "out of memory enumerating audio devices");
        return 0;
    }

    int cap = (max < 32) ? max : 32;
    int count = get_soundcard_list(ctx->audio_system, mode, ids, names, cap);
    if (count < 0)
        count = 0;

    for (int i = 0; i < count; i++)
    {
        /* Bound the read as well as the write: if a backend ever handed back
         * an unterminated row, "%s" would run on into the next one. */
        snprintf(out[i].id,   sizeof(out[i].id),   "%.*s", (int)sizeof(ids[i]) - 1, ids[i]);
        snprintf(out[i].name, sizeof(out[i].name), "%.*s", (int)sizeof(names[i]) - 1, names[i]);
    }

    ui_devices_disambiguate(out, count);

    if (selected && sel_len)
        snprintf(selected, sel_len, "%s",
                 (kind == UI_DEV_CAPTURE) ? ctx->selected_capture_dev
                                          : ctx->selected_playback_dev);

    free(ids);
    free(names);
    return count;
}

int ui_comm_get_radio_list(ui_device_t *out, int max, char *selected, size_t sel_len,
                           char *device_path, size_t dev_len, int *serial_speed)
{
    ui_ctx_t *ctx = g_ui_ctx;
    if (!ctx || !out || max <= 0)
        return 0;

    if (selected && sel_len)
    {
        int cur_type = radio_io_get_radio_type();
        if (cur_type > 0)
            snprintf(selected, sel_len, "%d", cur_type);
        else
            selected[0] = '\0';
    }
    if (device_path && dev_len)
    {
        const char *cur_dev = radio_io_get_device_path();
        snprintf(device_path, dev_len, "%s", cur_dev ? cur_dev : "");
    }
    if (serial_speed)
        *serial_speed = radio_io_get_serial_speed();

    /* "None" is always first: switching the radio off is a choice, not the
     * absence of one. */
    int n = 0;
    snprintf(out[n].name, sizeof(out[n].name), "None");
    snprintf(out[n].id,   sizeof(out[n].id),   "%d", RADIO_TYPE_NONE);
    n++;

    char (*radio_ids)[16]   = malloc(sizeof(*radio_ids)   * 512);
    char (*radio_names)[64] = malloc(sizeof(*radio_names) * 512);
    if (!radio_ids || !radio_names)
    {
        HLOGE(UI_LOG_TAG, "Failed to allocate radio list arrays");
        free(radio_ids);
        free(radio_names);
        return n;
    }

    int count = radio_io_get_radio_list(radio_ids, radio_names, 512);
    for (int i = 0; i < count && n < max; i++, n++)
    {
        snprintf(out[n].id,   sizeof(out[n].id),   "%s", radio_ids[i]);
        snprintf(out[n].name, sizeof(out[n].name), "%s", radio_names[i]);
    }

    free(radio_ids);
    free(radio_names);
    return n;
}

int ui_comm_get_input_channel(void)
{
    ui_ctx_t *ctx = g_ui_ctx;
    return ctx ? ctx->rx_input_channel : 0;
}

void ui_comm_preload_radio_list(void)
{
#ifdef HAVE_HAMLIB
    preload_radio_list();
#endif
}

/* Gather everything the UI shows into one typed snapshot.  This is the only
 * place that reads the engine for status; the websocket JSON and the embedded
 * UI both render THIS struct, so the two views cannot drift apart. */
static void ui_gather_status(ui_ctx_t *ctx, ui_status_t *out)
{
    memset(out, 0, sizeof(*out));

    arq_runtime_snapshot_t snap;
    int have_snap = arq_get_runtime_snapshot(&snap);

    char my_call[CALLSIGN_MAX_SIZE], src_call[CALLSIGN_MAX_SIZE], dst_call[CALLSIGN_MAX_SIZE];
    arq_conn_get_calls(my_call, src_call, dst_call, CALLSIGN_MAX_SIZE);

    /* src_addr/dst_addr follow the TNC convention (initiator/target):
     *   ISS (caller):   src = self,   dst = remote
     *   IRS (receiver): src = remote, dst = self
     * The peer is whichever is not us. */
    const char *dest_call = (dst_call[0] != '\0' && strcmp(dst_call, my_call) != 0)
                            ? dst_call : src_call;

    snprintf(out->user_callsign, sizeof(out->user_callsign), "%s", my_call);
    snprintf(out->dest_callsign, sizeof(out->dest_callsign), "%s", dest_call);

    out->bitrate_bps = (int)tnc_get_last_bitrate_bps();
    out->snr_db      = (double)tnc_get_last_snr();

    if (have_snap && snap.initialized)
    {
        out->transmitting      = (snap.trx == 1);
        out->sync              = snap.connected ? true : false;
        out->bytes_transmitted = (long)snap.tx_bytes;
        out->bytes_received    = (long)snap.rx_bytes;
    }

    int ctl_status  = net_get_status(CTL_TCP_PORT);
    int data_status = net_get_status(DATA_TCP_PORT);
    out->client_tcp_connected =
        (ctl_status == NET_CONNECTED || data_status == NET_CONNECTED);

    float tx_gain_linear = modem_get_tx_gain();
    out->tx_gain_db   = (tx_gain_linear > 0.0f) ? 20.0f * log10f(tx_gain_linear) : -120.0f;
    out->tx_peak_dbfs = modem_get_tx_peak_dbfs();

    out->waterfall_enabled = ctx->waterfall_enabled ? true : false;

    /* Audio health.  A card that cannot be opened, or that negotiates a rate
     * the modem cannot use, kills only its own thread -- mercury stays up so
     * the operator can pick a different device.  That is the right behaviour,
     * but it means this status is the ONLY place the failure becomes visible:
     * without it the UI shows a perfectly healthy station that happens to hear
     * nothing. */
    out->audio_ok = audioio_health_ok(out->audio_error, sizeof(out->audio_error));
}

/* Copy the latest gathered status out for the embedded UI.  Returns false
 * until the publisher has produced its first snapshot. */
bool ui_comm_get_status(ui_status_t *out)
{
    if (!out)
        return false;
    pthread_mutex_lock(&g_status_lock);
    bool ok = g_status_valid;
    if (ok)
        *out = g_status;
    pthread_mutex_unlock(&g_status_lock);
    return ok;
}

void *ui_publisher_thread(void *arg)
{
    ui_ctx_t *ctx = (ui_ctx_t *)arg;

    HLOGI(UI_LOG_TAG, "Publisher started - sending status every %dms via WebSocket (port %u)",
           UI_PUBLISH_INTERVAL_US / 1000, ctx->ws_port);

    while (!shutdown_)
    {
        /* One gather, published two ways: struct to the embedded UI, JSON to
         * remote clients. */
        ui_status_t st;
        ui_gather_status(ctx, &st);

        pthread_mutex_lock(&g_status_lock);
        g_status       = st;
        g_status_valid = true;
        pthread_mutex_unlock(&g_status_lock);

        int    bitrate       = st.bitrate_bps;
        double snr           = st.snr_db;
        int    sync          = st.sync ? 1 : 0;
        modem_direction_t dir = st.transmitting ? DIR_TX : DIR_RX;
        int    tcp_connected = st.client_tcp_connected ? 1 : 0;
        long   bytes_tx      = st.bytes_transmitted;
        long   bytes_rx      = st.bytes_received;
        const char *user_call = st.user_callsign;
        const char *dest_call = st.dest_callsign;

        // --- Log only if something meaningful changed ---
        if (bitrate != ctx->last_sent_status.bitrate ||
            snr != ctx->last_sent_status.snr ||
            sync != ctx->last_sent_status.sync ||
            dir != ctx->last_sent_status.dir ||
            tcp_connected != ctx->last_sent_status.client_tcp_connected ||
            bytes_tx != ctx->last_sent_status.bytes_transmitted ||
            bytes_rx != ctx->last_sent_status.bytes_received ||
            (user_call && strcmp(user_call, ctx->last_sent_status.user_callsign) != 0) ||
            (dest_call && strcmp(dest_call, ctx->last_sent_status.dest_callsign) != 0))
        {
            HLOGD(UI_LOG_TAG, "Status changed: bitrate=%d snr=%.1f sync=%d dir=%d tcp=%d tx=%ld rx=%ld call=%s dest=%s",
                  bitrate, snr, sync, dir, tcp_connected, bytes_tx, bytes_rx,
                  user_call ? user_call : "", dest_call ? dest_call : "");
            
            // update last sent status
            ctx->last_sent_status.bitrate = bitrate;
            ctx->last_sent_status.snr = snr;
            ctx->last_sent_status.sync = sync;
            ctx->last_sent_status.dir = dir;
            ctx->last_sent_status.client_tcp_connected = tcp_connected;
            ctx->last_sent_status.bytes_transmitted = bytes_tx;
            ctx->last_sent_status.bytes_received = bytes_rx;
            if (user_call) strncpy(ctx->last_sent_status.user_callsign, user_call, sizeof(ctx->last_sent_status.user_callsign)-1);
            if (dest_call) strncpy(ctx->last_sent_status.dest_callsign, dest_call, sizeof(ctx->last_sent_status.dest_callsign)-1);
        }

        // --- Render the same snapshot as JSON for remote clients ---
        {
            char buf[4096];
            if (ui_status_to_json(&st, buf, sizeof(buf)) > 0)
                ws_broadcast_json(&ctx->ws, buf);
        }

        // --- Send capture/playback device lists and input channel when a new UI client connects ---
        if (ctx->soundcard_list_pending)
        {
            ctx->soundcard_list_pending = 0;

            /* Same enumerator the embedded UI calls, rendered as JSON.
             * devs[] and the JSON scratch go on the heap: 32 ui_device_t is
             * 16 KB and buf another 18 KB, which is far too much for a thread
             * stack that is 512 KB by default on macOS. */
            ui_device_t *devs = malloc(sizeof(*devs) * 32);
            /* Big enough that 32 rows cannot overflow it even at the widest
             * name and id the struct allows, plus the JSON scaffolding.  It
             * used to be 8 KB, which was comfortable only while names were
             * short: ui_devices_disambiguate() appends the id to any name two
             * devices share, and ui_device_list_to_json() refuses to emit
             * truncated JSON — so an overflow does not corrupt the list, it
             * silently publishes no list at all. */
            const size_t json_cap = 32 * (UI_DEV_NAME_MAX + UI_DEV_ID_MAX + 32) + 256;
            char *buf = malloc(json_cap);
            char sel[UI_DEV_ID_MAX];

            if (devs && buf)
            {
                int cap_count = ui_comm_get_audio_devices(UI_DEV_CAPTURE, devs, 32, sel, sizeof(sel));
                if (cap_count > 0)
                {
                    if (ui_device_list_to_json("capture_dev_list", devs, cap_count, sel, buf, json_cap) > 0)
                        ws_broadcast_json(&ctx->ws, buf);
                    else
                        HLOGW(UI_LOG_TAG, "capture device list did not fit its JSON buffer "
                                          "(%d devices) — not published", cap_count);
                }

                int pb_count = ui_comm_get_audio_devices(UI_DEV_PLAYBACK, devs, 32, sel, sizeof(sel));
                if (pb_count > 0)
                {
                    if (ui_device_list_to_json("playback_dev_list", devs, pb_count, sel, buf, json_cap) > 0)
                        ws_broadcast_json(&ctx->ws, buf);
                    else
                        HLOGW(UI_LOG_TAG, "playback device list did not fit its JSON buffer "
                                          "(%d devices) — not published", pb_count);
                }
            }
            else
            {
                HLOGE(UI_LOG_TAG, "out of memory building the audio device list");
            }
            free(devs);
            free(buf);

            // Input channel selection
            const char *ch_str;
            switch (ctx->rx_input_channel) {
                case 1:  ch_str = "right"; break;
                case 2:  ch_str = "stereo"; break;
                default: ch_str = "left"; break;
            }
            char ch_buf[256];
            snprintf(ch_buf, sizeof(ch_buf),
                "{\"type\":\"input_channel\",\"selected\":\"%s\","
                "\"list\":[\"left\",\"right\",\"stereo\"]}", ch_str);
            ws_broadcast_json(&ctx->ws, ch_buf);
        }

        // --- Send radio list to UI (once at startup, and after set_radio_config) ---
        if (ctx->radio_list_pending)
        {
            ctx->radio_list_pending = 0;

            /* Same enumerator the embedded UI calls (hamlib's list is long, so
             * this one is heap-allocated), rendered as JSON. */
            const int   max_radios = 513;          /* 512 rigs + "None" */
            ui_device_t *radios = malloc(sizeof(*radios) * max_radios);
            char        *buf    = malloc(65536);
            if (!radios || !buf)
            {
                HLOGE(UI_LOG_TAG, "Failed to allocate radio list buffers");
                free(radios);
                free(buf);
            }
            else
            {
                char sel[16] = "", dev_path[512] = "";
                int  serial_speed = 0;
                int  count = ui_comm_get_radio_list(radios, max_radios, sel, sizeof(sel),
                                                    dev_path, sizeof(dev_path), &serial_speed);
                if (count > 0)
                {
                    /* radio_list carries two fields no other list has, so it
                     * gets the shared body plus its own preamble. */
                    int pos = snprintf(buf, 65536,
                        "{\"type\":\"radio_list\",\"selected\":\"%s\",\"device_path\":\"%s\","
                        "\"serial_speed\":%d,\"list\":[", sel, dev_path, serial_speed);
                    for (int i2 = 0; i2 < count && pos < 65536 - 160; i2++)
                        pos += snprintf(buf + pos, 65536 - pos, "%s{\"name\":\"%s\",\"id\":\"%s\"}",
                                        i2 ? "," : "", radios[i2].name, radios[i2].id);
                    pos += snprintf(buf + pos, 65536 - pos, "]}");
                    ws_broadcast_json(&ctx->ws, buf);
                }
                free(radios);
                free(buf);
            }
        }

        hermes_usleep(UI_PUBLISH_INTERVAL_US);
    }

    HLOGI(UI_LOG_TAG, "Publisher shutting down");
    return NULL;
}

// ---------------- SPECTRUM PUBLISHER THREAD ----------------
// Sends FFT spectrum data to the UI at ~20 fps (50 ms interval),
// decoupled from the slower status publisher (500 ms).

void *spectrum_publisher_thread(void *arg)
{
    ui_ctx_t *ctx = (ui_ctx_t *)arg;

    HLOGI(UI_LOG_TAG, "Spectrum publisher started - sending spectrum every %d ms via WebSocket",
          SPECTRUM_PUBLISH_INTERVAL_US / 1000);

    uint64_t last_seq = 0;

    while (!shutdown_ && atomic_load_explicit(&ctx->spec_run, memory_order_relaxed))
    {
        float spec_dB[MODEM_STATS_NSPEC];
        uint64_t seq = 0;
        int sr = modem_get_rx_spectrum_seq(spec_dB, MODEM_STATS_NSPEC, &seq);
        /* Skip frames already sent: the read no longer consumes, so this
         * thread and any embedded UI each see every frame. */
        if (sr > 0 && seq != last_seq)
        {
            last_seq = seq;
            // Build binary spectrum frame: magic(4) + fft_size(2) + sample_rate(2) + floats
            uint8_t frame[8 + MODEM_STATS_NSPEC * sizeof(float)];
            uint16_t fft_size = (uint16_t)MODEM_STATS_NSPEC;
            uint16_t sample_rate = (uint16_t)sr;
            /* All fields are little-endian on the wire for compatibility with mercury-qt */
#define SPECTRUM_MAGIC 0x4D435259U
            frame[0] = (uint8_t)( SPECTRUM_MAGIC        & 0xFF);
            frame[1] = (uint8_t)((SPECTRUM_MAGIC >>  8) & 0xFF);
            frame[2] = (uint8_t)((SPECTRUM_MAGIC >> 16) & 0xFF);
            frame[3] = (uint8_t)((SPECTRUM_MAGIC >> 24) & 0xFF);
#undef SPECTRUM_MAGIC
            frame[4] = (uint8_t)(fft_size & 0xFF);
            frame[5] = (uint8_t)((fft_size >> 8) & 0xFF);
            frame[6] = (uint8_t)(sample_rate & 0xFF);
            frame[7] = (uint8_t)((sample_rate >> 8) & 0xFF);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            for (int _i = 0; _i < MODEM_STATS_NSPEC; _i++) {
                uint32_t _w;
                memcpy(&_w, &spec_dB[_i], 4);
                _w = ((_w >> 24) & 0xFF) | ((_w >> 8) & 0xFF00) |
                     ((_w & 0xFF00) << 8) | ((_w & 0xFF) << 24);
                memcpy(frame + 8 + _i * 4, &_w, 4);
            }
#else
            memcpy(frame + 8, spec_dB, MODEM_STATS_NSPEC * sizeof(float));
#endif
            ws_broadcast_binary(&ctx->ws, frame, 8 + MODEM_STATS_NSPEC * sizeof(float));
        }

        hermes_usleep(SPECTRUM_PUBLISH_INTERVAL_US);
    }

    HLOGI(UI_LOG_TAG, "Spectrum publisher shutting down");
    return NULL;
}

/* Command entry point for the embedded UI: builds the same ws_command_t the
 * websocket path builds, then runs the shared handler. */
int ui_comm_command(const char *command, const char *value,
                    const char *value2, const char *value3)
{
    ui_ctx_t *ctx = g_ui_ctx;
    if (!ctx || !command)
        return -1;

    ws_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.command, sizeof(cmd.command), "%s", command);
    if (value)  snprintf(cmd.value,  sizeof(cmd.value),  "%s", value);
    if (value2) snprintf(cmd.value2, sizeof(cmd.value2), "%s", value2);
    if (value3) snprintf(cmd.value3, sizeof(cmd.value3), "%s", value3);

    return ui_comm_handle_command(ctx, &cmd);
}

// ---------------- HIGH-LEVEL INIT / SHUTDOWN ----------------

int ui_comm_init(ui_ctx_t *ctx, uint16_t ws_port, bool tls_enabled,
                 int waterfall_enabled, int audio_system,
                 const char *selected_capture, const char *selected_playback,
                 int rx_input_channel,
                 const mercury_config *initial_cfg, const char *cfg_path)
{
    g_ui_ctx = ctx;

    memset(ctx, 0, sizeof(*ctx));

    pthread_mutex_init(&ctx->cfg_mutex, NULL);

    ctx->waterfall_enabled = waterfall_enabled;
    ctx->audio_system = audio_system;
    ctx->rx_input_channel = rx_input_channel;
    ctx->ws_port = ws_port;
    ctx->tls_enabled = tls_enabled;
    if (selected_capture)
    {
        strncpy(ctx->selected_capture_dev, selected_capture, sizeof(ctx->selected_capture_dev) - 1);
        ctx->selected_capture_dev[sizeof(ctx->selected_capture_dev) - 1] = '\0';
    }
    else
        ctx->selected_capture_dev[0] = '\0';
    if (selected_playback)
    {
        strncpy(ctx->selected_playback_dev, selected_playback, sizeof(ctx->selected_playback_dev) - 1);
        ctx->selected_playback_dev[sizeof(ctx->selected_playback_dev) - 1] = '\0';
    }
    else
        ctx->selected_playback_dev[0] = '\0';

    // Store config snapshot and path for persisting UI changes
    if (initial_cfg)
        ctx->cfg = *initial_cfg;
    else
        cfg_set_defaults(&ctx->cfg);
    if (cfg_path) {
        strncpy(ctx->cfg_path, cfg_path, sizeof(ctx->cfg_path) - 1);
        ctx->cfg_path[sizeof(ctx->cfg_path) - 1] = '\0';
    }

    // Initialize WebSocket server (bidirectional: status TX + command RX)
    // Serve static test page from websocket/web/ directory.
    // connect callback is registered before the server thread starts to avoid
    // a race on aarch64 where the callback ptr and data could be seen
    // out-of-order by the server thread.
    if (ws_init(&ctx->ws, ws_port,
                ws_command_handler, ctx,
                ws_connect_handler, ctx,
                tls_enabled) != 0) {
        HLOGE(UI_LOG_TAG, "Failed to init WebSocket server on port %u", ws_port);
        return -1;
    }
    HLOGI(UI_LOG_TAG, "WebSocket server ready on port %u", ws_port);

    // Start publisher thread - periodic status broadcaster (via WebSocket)
    if (pthread_create(&ctx->pub_tid, NULL, ui_publisher_thread, ctx) != 0) {
        HLOGE(UI_LOG_TAG, "pthread_create(pub) failed: %s", strerror(errno));
        ws_shutdown(&ctx->ws);
        return -1;
    }
    pthread_detach(ctx->pub_tid);
    HLOGI(UI_LOG_TAG, "Publisher thread started");

    if (waterfall_enabled) {
        // Start spectrum publisher thread - high-rate FFT/waterfall broadcaster (via WebSocket)
        atomic_store_explicit(&ctx->spec_run, true, memory_order_relaxed);
        if (pthread_create(&ctx->spec_tid, NULL, spectrum_publisher_thread, ctx) != 0) {
            atomic_store_explicit(&ctx->spec_run, false, memory_order_relaxed);
            ctx->spec_tid = 0;
            HLOGE(UI_LOG_TAG, "pthread_create(spec) failed: %s", strerror(errno));
            HLOGW(UI_LOG_TAG, "Spectrum publisher thread not started (waterfall may not update)");
        } else {
            HLOGI(UI_LOG_TAG, "Spectrum publisher thread started");
        }
    }

    return 0;
}

void ui_comm_shutdown(ui_ctx_t *ctx)
{
    g_ui_ctx = NULL;

    // Stop and join the spectrum publisher thread before the websocket server
    // goes down: it was made joinable for the runtime disable path, so leave
    // no joinable thread unjoined at teardown.  Take cfg_mutex so this cannot
    // race ui_comm_set_waterfall()'s disable path, which joins the same thread
    // under the same lock — two pthread_join calls on one tid is undefined
    // behaviour.  This closes the window the NOTE below describes.
    pthread_mutex_lock(&ctx->cfg_mutex);
    atomic_store_explicit(&ctx->spec_run, false, memory_order_relaxed);
    if (ctx->spec_tid != 0) {
        pthread_join(ctx->spec_tid, NULL);
        ctx->spec_tid = 0;
    }
    pthread_mutex_unlock(&ctx->cfg_mutex);

    ws_shutdown(&ctx->ws);
    // NOTE: cfg_mutex is deliberately NOT destroyed.  ui_comm_set_waterfall
    // may have already read g_ui_ctx before the NULL above and could still
    // lock it, so destroying it here would race a live user.  The process is
    // exiting anyway.
    HLOGI(UI_LOG_TAG, "Shut down");
}
