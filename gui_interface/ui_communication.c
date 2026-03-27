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

#include "../common/os_interop.h"

/* Cross-platform microsecond sleep */
#ifdef _WIN32
#define hermes_usleep(us) Sleep((DWORD)((us) / 1000))
#else
#define hermes_usleep(us) usleep(us)
#endif

#include "ui_communication.h"

#include "../datalink_arq/arq.h"
#include "../data_interfaces/net.h"
#include "../data_interfaces/tcp_interfaces.h"
#include "../common/hermes_log.h"
#include "../modem/freedv/modem_stats.h"
#include "../modem/modem.h"
#include "../radio_io/radio_io.h"  /* RADIO_TYPE_NONE */

extern int get_soundcard_list(int audio_system, int mode,
                              char ids[][64], char dev_names[][64], int max_count);

extern int audioio_restart(const char *capture_dev, const char *playback_dev,
                           int audio_subsys, int capture_channel_layout);

extern int radio_io_get_radio_list(char ids[][16], char names[][64], int max_count);
extern int radio_io_restart(int new_radio_type, const char *device_path);
extern const char *radio_io_get_device_path(void);
extern int radio_io_get_radio_type(void);

// global shutdown flag from main.c
extern volatile bool shutdown_;

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

// ---------------- WS COMMAND CALLBACK ----------------
// Called by the websocket server thread when a command JSON arrives from UI.
static int ws_command_handler(const ws_command_t *cmd, void *user_data)
{
    ui_ctx_t *ctx = (ui_ctx_t *)user_data;
    if (!ctx || !cmd)
        return -1;

    HLOGI(UI_LOG_TAG, "WS CMD from UI: command=\"%s\" value=\"%s\" value2=\"%s\"",
          cmd->command, cmd->value, cmd->value2);

    if (strcmp(cmd->command, "set_audio_config") == 0) {
        // value = capture_dev, value2 = playback_dev, value3 = input_channel
        if (cmd->value[0])
            strncpy(ctx->selected_capture_dev, cmd->value,
                    sizeof(ctx->selected_capture_dev) - 1);
        HLOGI(UI_LOG_TAG, "Capture device set to: %s", ctx->selected_capture_dev);

        if (cmd->value2[0])
            strncpy(ctx->selected_playback_dev, cmd->value2,
                    sizeof(ctx->selected_playback_dev) - 1);
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
        strncpy(ctx->cfg.input_device, ctx->selected_capture_dev,
                sizeof(ctx->cfg.input_device) - 1);
        ctx->cfg.input_device[sizeof(ctx->cfg.input_device) - 1] = '\0';
        strncpy(ctx->cfg.output_device, ctx->selected_playback_dev,
                sizeof(ctx->cfg.output_device) - 1);
        ctx->cfg.output_device[sizeof(ctx->cfg.output_device) - 1] = '\0';
        ctx->cfg.capture_channel = ctx->rx_input_channel;
        if (ctx->cfg_path[0] && cfg_write(&ctx->cfg, ctx->cfg_path))
            HLOGI(UI_LOG_TAG, "Config saved to %s", ctx->cfg_path);

    } else if (strcmp(cmd->command, "set_radio_config") == 0) {
        int new_radio_type = atoi(cmd->value);
        const char *dev_path = cmd->value2;
        HLOGI(UI_LOG_TAG, "Radio set_radio_config command: model_id=%d device_path=\"%s\"",
              new_radio_type, dev_path);
        if (new_radio_type == RADIO_TYPE_NONE) {
            radio_io_restart(RADIO_TYPE_NONE, NULL);
            HLOGI(UI_LOG_TAG, "Radio type set to NONE - radio subsystem shut down");
            ctx->radio_list_pending = 1;
        } else {
            int rc = radio_io_restart(new_radio_type, dev_path);
            if (rc == 0) {
                HLOGI(UI_LOG_TAG, "Radioio subsystem restarted (model=%d, path=%s)",
                      new_radio_type, dev_path);
                ctx->radio_list_pending = 1;
            } else {
                HLOGE(UI_LOG_TAG, "Radioio subsystem restart FAILED (model=%d, path=%s, rc=%d)",
                      new_radio_type, dev_path, rc);
                return rc;
            }
        }

        // Persist radio config to INI
        ctx->cfg.radio_type = new_radio_type;
        strncpy(ctx->cfg.radio_device, dev_path ? dev_path : "",
                sizeof(ctx->cfg.radio_device) - 1);
        ctx->cfg.radio_device[sizeof(ctx->cfg.radio_device) - 1] = '\0';
        if (ctx->cfg_path[0] && cfg_write(&ctx->cfg, ctx->cfg_path))
            HLOGI(UI_LOG_TAG, "Config saved to %s", ctx->cfg_path);

    } else {
        HLOGW(UI_LOG_TAG, "Unknown UI command: %s", cmd->command);
        return -1;
    }

    return 0;
}

// ---------------- UI PUBLISHER THREAD ----------------
// Periodically gathers modem/ARQ/network status and sends it to the UI.

void *ui_publisher_thread(void *arg)
{
    ui_ctx_t *ctx = (ui_ctx_t *)arg;

    HLOGI(UI_LOG_TAG, "Publisher started - sending status every %dms via WebSocket (port %u)",
           UI_PUBLISH_INTERVAL_US / 1000, ctx->ws_port);

    while (!shutdown_)
    {
        // --- Gather status from ARQ snapshot ---
        arq_runtime_snapshot_t snap;
        int have_snap = arq_get_runtime_snapshot(&snap);

        int bitrate = (int)tnc_get_last_bitrate_bps();
        double snr = (double)tnc_get_last_snr();
        const char *user_call = arq_conn.my_call_sign;
        /* Determine remote peer for UI display.
         * src_addr/dst_addr follow TNC convention (initiator/target):
         *   ISS (caller):   src_addr = self,   dst_addr = remote
         *   IRS (receiver): src_addr = remote,  dst_addr = self
         * Pick whichever is NOT our own callsign. */
        const char *dest_call;
        if (arq_conn.dst_addr[0] != '\0' &&
            strcmp(arq_conn.dst_addr, arq_conn.my_call_sign) != 0)
            dest_call = arq_conn.dst_addr;   /* ISS: dst is remote */
        else
            dest_call = arq_conn.src_addr;   /* IRS: src is remote */
        int sync = 0;
        modem_direction_t dir = DIR_RX;
        int tcp_connected = 0;
        long bytes_tx = 0;
        long bytes_rx = 0;

        if (have_snap && snap.initialized)
        {
            dir = (snap.trx == 1) ? DIR_TX : DIR_RX;
            sync = snap.connected ? 1 : 0;
            bytes_tx = (long)snap.tx_bytes;
            bytes_rx = (long)snap.rx_bytes;
        }

        // --- Check TCP client connected status ---
        int ctl_status = net_get_status(CTL_TCP_PORT);
        int data_status = net_get_status(DATA_TCP_PORT);
        tcp_connected = (ctl_status == NET_CONNECTED || data_status == NET_CONNECTED) ? 1 : 0;

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

        // --- Build and broadcast status JSON via WebSocket ---
        {
            char buf[4096];
            int pos = 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                "{\"type\":\"status\","
                "\"bitrate\":%d,"
                "\"snr\":%.1f,"
                "\"user_callsign\":\"%s\","
                "\"dest_callsign\":\"%s\","
                "\"sync\":%s,"
                "\"direction\":\"%s\","
                "\"client_tcp_connected\":%s,"
                "\"bytes_transmitted\":%ld,"
                "\"bytes_received\":%ld,"
                "\"waterfall\":%s}",
                bitrate, snr,
                user_call ? user_call : "",
                dest_call ? dest_call : "",
                sync ? "true" : "false",
                dir == DIR_TX ? "tx" : "rx",
                tcp_connected ? "true" : "false",
                bytes_tx, bytes_rx,
                ctx->waterfall_enabled ? "true" : "false");

            ws_broadcast_json(&ctx->ws, buf);
        }

        // --- Send capture/playback device lists and input channel when a new UI client connects ---
        if (ctx->soundcard_list_pending)
        {
            ctx->soundcard_list_pending = 0;

            // Capture (input) devices - mode 1 = FFAUDIO_DEV_CAPTURE
            char cap_ids[32][64], cap_names[32][64];
            int cap_count = get_soundcard_list(ctx->audio_system, 1, cap_ids, cap_names, 32);
            if (cap_count > 0)
            {
                char buf[4096];
                int pos = 0;
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"type\":\"capture_dev_list\",\"selected\":\"%s\",\"list\":[",
                    ctx->selected_capture_dev);
                for (int i = 0; i < cap_count && pos < (int)sizeof(buf) - 128; i++) {
                    if (i > 0) buf[pos++] = ',';
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "{\"name\":\"%s\",\"id\":\"%s\"}", cap_names[i], cap_ids[i]);
                }
                pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
                ws_broadcast_json(&ctx->ws, buf);
            }

            // Playback (output) devices - mode 0 = FFAUDIO_DEV_PLAYBACK
            char pb_ids[32][64], pb_names[32][64];
            int pb_count = get_soundcard_list(ctx->audio_system, 0, pb_ids, pb_names, 32);
            if (pb_count > 0)
            {
                char buf[4096];
                int pos = 0;
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{\"type\":\"playback_dev_list\",\"selected\":\"%s\",\"list\":[",
                    ctx->selected_playback_dev);
                for (int i = 0; i < pb_count && pos < (int)sizeof(buf) - 128; i++) {
                    if (i > 0) buf[pos++] = ',';
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "{\"name\":\"%s\",\"id\":\"%s\"}", pb_names[i], pb_ids[i]);
                }
                pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
                ws_broadcast_json(&ctx->ws, buf);
            }

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
            char (*radio_ids)[16] = malloc(sizeof(*radio_ids) * 512);
            char (*radio_names)[64] = malloc(sizeof(*radio_names) * 512);
            if (!radio_ids || !radio_names)
            {
                HLOGE(UI_LOG_TAG, "Failed to allocate radio list arrays");
                free(radio_ids);
                free(radio_names);
                continue;
            }
            int radio_count = radio_io_get_radio_list(radio_ids, radio_names, 512);
            if (radio_count > 0)
            {
                char sel_buf[16] = "";
                int cur_type = radio_io_get_radio_type();
                if (cur_type > 0)
                    snprintf(sel_buf, sizeof(sel_buf), "%d", cur_type);
                const char *cur_dev = radio_io_get_device_path();

                const size_t buf_size = 65536;
                char *buf = malloc(buf_size);
                if (!buf) {
                    HLOGE(UI_LOG_TAG, "Failed to allocate radio_list buffer");
                    free(radio_ids);
                    free(radio_names);
                    continue;
                }
                int pos = 0;
                pos += snprintf(buf + pos, buf_size - pos,
                    "{\"type\":\"radio_list\",\"selected\":\"%s\",\"device_path\":\"%s\",\"list\":[",
                    sel_buf, cur_dev ? cur_dev : "");
                // First entry always "None"
                pos += snprintf(buf + pos, buf_size - pos,
                    "{\"name\":\"None\",\"id\":\"%d\"}", RADIO_TYPE_NONE);
                for (int i = 0; i < radio_count && pos < (int)buf_size - 128; i++) {
                    buf[pos++] = ',';
                    pos += snprintf(buf + pos, buf_size - pos,
                        "{\"name\":\"%s\",\"id\":\"%s\"}", radio_names[i], radio_ids[i]);
                }
                pos += snprintf(buf + pos, buf_size - pos, "]}");
                ws_broadcast_json(&ctx->ws, buf);
                free(buf);
            }
            free(radio_ids);
            free(radio_names);
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

    while (!shutdown_)
    {
        float spec_dB[MODEM_STATS_NSPEC];
        int sr = modem_get_rx_spectrum(spec_dB, MODEM_STATS_NSPEC);
        if (sr > 0)
        {
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

// ---------------- HIGH-LEVEL INIT / SHUTDOWN ----------------

int ui_comm_init(ui_ctx_t *ctx, uint16_t ws_port, bool tls_enabled,
                 int waterfall_enabled, int audio_system,
                 const char *selected_capture, const char *selected_playback,
                 int rx_input_channel,
                 const mercury_config *initial_cfg, const char *cfg_path)
{
    memset(ctx, 0, sizeof(*ctx));

    ctx->waterfall_enabled = waterfall_enabled;
    ctx->audio_system = audio_system;
    ctx->rx_input_channel = rx_input_channel;
    ctx->ws_port = ws_port;
    ctx->tls_enabled = tls_enabled;
    if (selected_capture)
        strncpy(ctx->selected_capture_dev, selected_capture, sizeof(ctx->selected_capture_dev) - 1);
    else
        ctx->selected_capture_dev[0] = '\0';
    if (selected_playback)
        strncpy(ctx->selected_playback_dev, selected_playback, sizeof(ctx->selected_playback_dev) - 1);
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
    // Serve static test page from websocket/web/ directory
    if (ws_init(&ctx->ws, ws_port, "gui_interface/websocket/web",
                ws_command_handler, ctx, tls_enabled) != 0) {
        HLOGE(UI_LOG_TAG, "Failed to init WebSocket server on port %u", ws_port);
        return -1;
    }
    // Register connect callback - sends all device lists and radio list on each new UI connection
    ctx->ws.connect_callback = ws_connect_handler;
    ctx->ws.connect_callback_data = ctx;
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
        if (pthread_create(&ctx->spec_tid, NULL, spectrum_publisher_thread, ctx) != 0) {
            HLOGE(UI_LOG_TAG, "pthread_create(spec) failed: %s", strerror(errno));
            HLOGW(UI_LOG_TAG, "Spectrum publisher thread not started (waterfall may not update)");
        } else {
            pthread_detach(ctx->spec_tid);
            HLOGI(UI_LOG_TAG, "Spectrum publisher thread started");
        }
    }

    return 0;
}

void ui_comm_shutdown(ui_ctx_t *ctx)
{
    // Threads check shutdown_ flag and will exit on their own.
    // Shut down the WebSocket server.
    ws_shutdown(&ctx->ws);
    HLOGI(UI_LOG_TAG, "Shut down");
}
