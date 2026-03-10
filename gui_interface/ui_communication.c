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

// json communication between UI and backend
// POSIX UDP JSON sender + blocking receiver, each on its own thread.
// No third-party libs; includes a minimal JSON string escaper.

// To run test main(), compile with -DTEST_MAIN
// Eg. how to run. One side listens on port 5006, other side sends to
// port 5005. Both sides can send and receive.
// ./ui_communication 127.0.0.1 5005 5006
// ./ui_communication 127.0.0.1 5006 5005

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>

#include "../common/os_interop.h"
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

/* Cross-platform microsecond sleep */
#ifdef _WIN32
#define hermes_usleep(us) Sleep((DWORD)((us) / 1000))
#else
#define hermes_usleep(us) usleep(us)
#endif

#include "ui_communication.h"

#ifndef TEST_MAIN
#include "../datalink_arq/arq.h"
#include "../data_interfaces/net.h"
#include "../data_interfaces/tcp_interfaces.h"
#include "../common/hermes_log.h"
#include "../modem/freedv/modem_stats.h"
#include "../modem/modem.h"

// global shutdown flag from main.c
extern bool shutdown_;
#else
// Standalone test mode: provide a local shutdown flag
static bool shutdown_ = false;
// Stub logger macros for standalone test build
#define HLOGD(c, fmt, ...) printf("[DBG] [" c "] " fmt "\n", ##__VA_ARGS__)
#define HLOGI(c, fmt, ...) printf("[INF] [" c "] " fmt "\n", ##__VA_ARGS__)
#define HLOGW(c, fmt, ...) printf("[WRN] [" c "] " fmt "\n", ##__VA_ARGS__)
#define HLOGE(c, fmt, ...) printf("[ERR] [" c "] " fmt "\n", ##__VA_ARGS__)
#endif

#define UI_LOG_TAG "ui-comm"

// ---------------- TX ----------------
int udp_tx_init(udp_tx_t *tx, const char *ip, uint16_t port)
{
    tx->sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (tx->sock < 0)
    {
        HLOGE(UI_LOG_TAG, "socket(): %s", strerror(errno));
        return -1;
    }

    memset(&tx->dest, 0, sizeof(tx->dest));
    tx->dest.sin_family = AF_INET;
    tx->dest.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &tx->dest.sin_addr) <= 0)
    {
        HLOGE(UI_LOG_TAG, "inet_pton(%s): %s", ip, strerror(errno));
        SOCK_CLOSE(tx->sock);
        tx->sock = -1;
        return -1;
    }

    return 0;
}

void udp_tx_close(udp_tx_t *tx)
{
    if (tx->sock >= 0) {
        SOCK_CLOSE(tx->sock);
        tx->sock = -1;
    }
}

int udp_tx_send_json_pairs(udp_tx_t *tx, ...)
{
    char buf[1500];
    char tmp[512];
    buf[0] = '\0';

    strcat(buf, "{");
    va_list ap;
    va_start(ap, tx);
    const char *key;
    int first = 1;
    while ((key = va_arg(ap, const char *)) != NULL)
    {
        const char *val = va_arg(ap, const char *);
        if (!first) strcat(buf, ",");

        // Don't quote arrays, objects, numbers, booleans, null
        // But always quote empty strings (len==0) to produce valid JSON
        if (val[0] != '\0' &&
            (val[0] == '[' || val[0] == '{' ||
            strcmp(val, "true") == 0 || strcmp(val, "false") == 0 ||
            strcmp(val, "null") == 0 || strspn(val, "0123456789.-") == strlen(val))) {
            snprintf(tmp, sizeof(tmp), "\"%s\":%s", key, val);
        } else {
            snprintf(tmp, sizeof(tmp), "\"%s\":\"%s\"", key, val);
        }

        strcat(buf, tmp);
        first = 0;
    }
    va_end(ap);
    strcat(buf, "}");

    ssize_t sent = sendto(tx->sock, buf, strlen(buf), 0,
                          (struct sockaddr *)&tx->dest, sizeof(tx->dest));

    return sent;
}

int udp_tx_send_status(udp_tx_t *tx,
                       int bitrate, double snr,
                       const char *user_callsign,
                       const char *dest_callsign,
                       int sync, modem_direction_t dir,
                       int client_tcp_connected,
                       long bytes_transmitted, long bytes_received,
                       int waterfall_enabled)
{
    char br[32], snrbuf[32], tx_bytes[32], rx_bytes[32];
    snprintf(br, sizeof(br), "%d", bitrate);
    snprintf(snrbuf, sizeof(snrbuf), "%.1f", snr);
    snprintf(tx_bytes, sizeof(tx_bytes), "%ld", bytes_transmitted);
    snprintf(rx_bytes, sizeof(rx_bytes), "%ld", bytes_received);

    return udp_tx_send_json_pairs(tx,
        "type", "status",
        "bitrate", br,
        "snr", snrbuf,
        "user_callsign", user_callsign,
        "dest_callsign", dest_callsign,
        "sync", sync ? "true" : "false",
        "direction", dir == DIR_TX ? "tx" : "rx",
        "client_tcp_connected", client_tcp_connected ? "true" : "false",
        "bytes_transmitted", tx_bytes,
        "bytes_received", rx_bytes,
        "waterfall", waterfall_enabled ? "true" : "false",
        NULL);
}

int udp_tx_send_soundcard_list(udp_tx_t *tx,
                               const char *selected_soundcard,
                               const char *soundcards[], int count) {
    char buf[1500]; // max mtu size
    snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < count; i++) {
        strcat(buf, "\"");
        strcat(buf, soundcards[i]);
        strcat(buf, "\"");
        if (i < count - 1) strcat(buf, ",");
    }
    strcat(buf, "]");

    return udp_tx_send_json_pairs(tx,
        "type", "soundcard_list",
        "selected", selected_soundcard,
        "list", buf,
        NULL);
}

int udp_tx_send_radio_list(udp_tx_t *tx,
                           const char *selected_radio,
                           const char *radios[], int count) {
    char buf[1500];
    snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < count; i++) {
        strcat(buf, "\"");
        strcat(buf, radios[i]);
        strcat(buf, "\"");
        if (i < count - 1) strcat(buf, ",");
    }
    strcat(buf, "]");

    return udp_tx_send_json_pairs(tx,
        "type", "radio_list",
        "selected", selected_radio,
        "list", buf,
        NULL);
}

// ---------------- RX ----------------
static void parse_json(const char *json, char keys[][64], char vals[][1024], int *count)
{
    *count = 0;
    const char *p = json;
    while ((p = strchr(p, '"')) && *count < 32)
    {
        // ---- parse key ----
        const char *q = strchr(p + 1, '"');
        if (!q)
            break;
        int klen = q - (p + 1);
        strncpy(keys[*count], p + 1, klen);
        keys[*count][klen] = '\0';

        // ---- find colon ----
        p = strchr(q + 1, ':');
        if (!p)
            break;
        p++;

        // ---- skip whitespace ----
        while (*p == ' ' || *p == '\t')
            p++;

        // ---- parse value ----
        if (*p == '"')
        {
            // string value
            p++;
            q = strchr(p, '"');
            if (!q) break;
            int vlen = q - p;
            strncpy(vals[*count], p, vlen);
            vals[*count][vlen] = '\0';
            p = q + 1;
        }
        else if (*p == '[')
        {
            // array value
            int depth = 1;
            const char *start = p;
            p++;
            while (*p && depth > 0)
            {
                if (*p == '[') depth++;
                else if (*p == ']') depth--;
                p++;
            }

            if (depth != 0)
                break; // malformed

            int vlen = p - start;
            if (vlen >= (int)sizeof(vals[*count]))
                vlen = sizeof(vals[*count]) - 1;

            strncpy(vals[*count], start, vlen);
            vals[*count][vlen] = '\0';
        }
        else
        {
            // bare value (true, false, null, number)
            const char *start = p;
            while (*p && *p != ',' && *p != '}') p++;
            int vlen = p - start;
            strncpy(vals[*count], start, vlen);
            vals[*count][vlen] = '\0';
        }

        (*count)++;

        // ---- skip to next key ----
        while (*p && *p != '"')
        {
            if (*p == '}') return;
            p++;
        }
    }
}

static void fill_modem_message(modem_message_t *msg, char keys[][64], char vals[][1024], int pairs)
{
    memset(msg, 0, sizeof(*msg));
    msg->type = MSG_UNKNOWN;

    for (int i = 0; i < pairs; i++)
    {
        if (strcmp(keys[i], "type") == 0)
        {
            if (strcmp(vals[i], "status") == 0) msg->type = MSG_STATUS;
            else if (strcmp(vals[i], "config") == 0) msg->type = MSG_CONFIG;
            else if (strcmp(vals[i], "soundcard_list") == 0) msg->type = MSG_SOUNDCARD_LIST;
            else if (strcmp(vals[i], "radio_list") == 0) msg->type = MSG_RADIO_LIST;
        }
    }

    for (int i = 0; i < pairs; i++)
    {
        switch (msg->type) {
        case MSG_UNKNOWN:
            // No specific fields to parse
            break;
        case MSG_STATUS:
            if (strcmp(keys[i], "bitrate") == 0)
                msg->status.bitrate = atoi(vals[i]);
            else if (strcmp(keys[i], "snr") == 0)
                msg->status.snr = atof(vals[i]);
            else if (strcmp(keys[i], "user_callsign") == 0)
                strncpy(msg->status.user_callsign, vals[i],
                        sizeof msg->status.user_callsign - 1);
            else if (strcmp(keys[i], "dest_callsign") == 0)
                strncpy(msg->status.dest_callsign, vals[i],
                        sizeof msg->status.dest_callsign - 1);
            else if (strcmp(keys[i], "sync") == 0)
                msg->status.sync = (strcmp(vals[i], "true") == 0);
            else if (strcmp(keys[i], "direction") == 0)
                msg->status.dir = (strcmp(vals[i], "tx") == 0) ? DIR_TX : DIR_RX;
            else if (strcmp(keys[i], "client_tcp_connected") == 0)
                msg->status.client_tcp_connected = (strcmp(vals[i], "true") == 0);
            else if (strcmp(keys[i], "bytes_transmitted") == 0)
                msg->status.bytes_transmitted = atol(vals[i]);
            else if (strcmp(keys[i], "bytes_received") == 0)
                msg->status.bytes_received = atol(vals[i]);
                    break;
        case MSG_SOUNDCARD_LIST:
            if (msg->type == MSG_SOUNDCARD_LIST)
            {
                if (strcmp(keys[i], "selected") == 0)
                    strncpy(msg->soundcard_list.selected, vals[i], sizeof msg->soundcard_list.selected - 1);
                else
                    if (strcmp(keys[i], "list") == 0)
                        strncpy(msg->soundcard_list.list, vals[i], sizeof msg->soundcard_list.list - 1);
            }
            break;
        case MSG_RADIO_LIST:
            if (strcmp(keys[i], "selected") == 0)
                strncpy(msg->radio_list.selected, vals[i], sizeof msg->radio_list.selected - 1);
            else
                if (strcmp(keys[i], "list") == 0)
                    strncpy(msg->radio_list.list, vals[i], sizeof msg->radio_list.list - 1);
            break;
        default:
            HLOGW(UI_LOG_TAG, "Unknown message type, raw: %s", vals[i]);
        }
    }
}


void *rx_thread_main(void *arg)
{
    rx_args_t *rxa = (rx_args_t *)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        HLOGE(UI_LOG_TAG, "socket(rx): %s", strerror(errno));
        free(rxa);
        return NULL;
    }

    // Set receive timeout so we can check shutdown_ periodically
#ifdef _WIN32
    DWORD rcvtimeo = 1000;  /* 1 second in milliseconds */
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcvtimeo, sizeof(rcvtimeo));
#else
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(rxa->listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        HLOGE(UI_LOG_TAG, "bind(port %u): %s", rxa->listen_port, strerror(errno));
        SOCK_CLOSE(sock);
        free(rxa);
        return NULL;
    }

    free(rxa); // no longer needed

    char buf[1500];
    while (!shutdown_)
    {
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&src, &srclen);
        if (n <= 0) continue;
        buf[n] = '\0';

        HLOGD(UI_LOG_TAG, "RX %d bytes: %s", n, buf);
        char keys[32][64], vals[32][1024];
        int pairs = 0;
        parse_json(buf, keys, vals, &pairs);

        modem_message_t msg;
        fill_modem_message(&msg, keys, vals, pairs);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
        HLOGD(UI_LOG_TAG, "RX from %s:%u", ip, ntohs(src.sin_port));

        switch (msg.type) {
        case MSG_STATUS:
            HLOGD(UI_LOG_TAG, "STATUS bitrate=%d snr=%.1f call=%s dest=%s sync=%s dir=%s tcp=%s tx=%ld rx=%ld",
                  msg.status.bitrate, msg.status.snr,
                  msg.status.user_callsign, msg.status.dest_callsign,
                  msg.status.sync ? "true" : "false",
                  msg.status.dir == DIR_TX ? "tx" : "rx",
                  msg.status.client_tcp_connected ? "true" : "false",
                  msg.status.bytes_transmitted, msg.status.bytes_received);
            break;

        case MSG_SOUNDCARD_LIST:
            HLOGD(UI_LOG_TAG, "SOUNDCARD_LIST selected=%s list=%s",
                  msg.soundcard_list.selected, msg.soundcard_list.list);
            break;

        case MSG_RADIO_LIST:
            HLOGD(UI_LOG_TAG, "RADIO_LIST selected=%s list=%s",
                  msg.radio_list.selected, msg.radio_list.list);
            break;

        default:
            HLOGW(UI_LOG_TAG, "Unknown message type, raw: %s", buf);
            break;
        }
    }

    SOCK_CLOSE(sock);
    return NULL;
}

// ---------------- UI PUBLISHER THREAD ----------------
// Periodically gathers modem/ARQ/network status and sends it to the UI.

#ifndef TEST_MAIN

void *ui_publisher_thread(void *arg)
{
    ui_ctx_t *ctx = (ui_ctx_t *)arg;
    udp_tx_t *tx = &ctx->tx;

    HLOGI(UI_LOG_TAG, "Publisher started - sending status every %d ms to port %d",
           UI_PUBLISH_INTERVAL_US / 1000, ntohs(tx->dest.sin_port));

    while (!shutdown_)
    {
        // --- Gather status from ARQ snapshot ---
        arq_runtime_snapshot_t snap;
        int have_snap = arq_get_runtime_snapshot(&snap);

        int bitrate = (int)tnc_get_last_bitrate_bps();
        double snr = (double)tnc_get_last_snr();
        const char *user_call = arq_conn.my_call_sign;
        const char *dest_call = arq_conn.dst_addr;
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

        // --- Send status to UI ---
        udp_tx_send_status(tx,
                           bitrate, snr,
                           user_call ? user_call : "",
                           dest_call ? dest_call : "",
                           sync, dir,
                           tcp_connected,
                           bytes_tx, bytes_rx,
                           ctx->waterfall_enabled);

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

    HLOGI(UI_LOG_TAG, "Spectrum publisher started - sending spectrum every %d ms",
          SPECTRUM_PUBLISH_INTERVAL_US / 1000);

    while (!shutdown_)
    {
        float spec_dB[MODEM_STATS_NSPEC];
        int sr = modem_get_rx_spectrum(spec_dB, MODEM_STATS_NSPEC);
        if (sr > 0)
        {
            spectrum_tx_send(&ctx->spectrum_tx, spec_dB,
                             (uint16_t)MODEM_STATS_NSPEC, (uint16_t)sr);
        }

        hermes_usleep(SPECTRUM_PUBLISH_INTERVAL_US);
    }

    HLOGI(UI_LOG_TAG, "Spectrum publisher shutting down");
    return NULL;
}

// ---------------- HIGH-LEVEL INIT / SHUTDOWN ----------------

int ui_comm_init(ui_ctx_t *ctx, const char *ip, uint16_t tx_port, int waterfall_enabled)
{
    memset(ctx, 0, sizeof(*ctx));

    ctx->waterfall_enabled = waterfall_enabled;

    // Initialize TX socket - sends status TO the UI
    if (udp_tx_init(&ctx->tx, ip, tx_port) != 0) {
        HLOGE(UI_LOG_TAG, "Failed to init TX socket to %s:%u", ip, tx_port);
        return -1;
    }
    HLOGI(UI_LOG_TAG, "TX socket ready - sending to %s:%u", ip, tx_port);

    if (waterfall_enabled) {
        // Spectrum is sent on tx_port + 2 (spectrum UDP port = UI base port + 2)
        uint16_t spectrum_port = tx_port + 2;
        if (spectrum_tx_init(&ctx->spectrum_tx, ip, spectrum_port) != 0) {
            HLOGW(UI_LOG_TAG, "Failed to init spectrum TX socket (waterfall will not work)");
            // Non-fatal: continue without spectrum
        } else {
            HLOGI(UI_LOG_TAG, "Spectrum TX ready - sending to %s:%u", ip, spectrum_port);
        }
    } else {
        HLOGI(UI_LOG_TAG, "Waterfall disabled - spectrum data wont be sent to the UI");
        ctx->spectrum_tx.sock = -1;
    }

    // Start RX thread - listens for commands FROM the UI
    uint16_t rx_port = tx_port + 1;
    ctx->rx_port = rx_port;
    rx_args_t *rxa = malloc(sizeof(rx_args_t));
    if (!rxa) {
        udp_tx_close(&ctx->tx);
        return -1;
    }
    rxa->listen_port = rx_port;
    if (pthread_create(&ctx->rx_tid, NULL, rx_thread_main, rxa) != 0) {
        HLOGE(UI_LOG_TAG, "pthread_create(rx) failed: %s", strerror(errno));
        free(rxa);
        udp_tx_close(&ctx->tx);
        return -1;
    }
    pthread_detach(ctx->rx_tid);
    HLOGI(UI_LOG_TAG, "RX thread started - listening on port %u", rx_port);

    // Start publisher thread - periodic status broadcaster
    if (pthread_create(&ctx->pub_tid, NULL, ui_publisher_thread, ctx) != 0) {
        HLOGE(UI_LOG_TAG, "pthread_create(pub) failed: %s", strerror(errno));
        udp_tx_close(&ctx->tx);
        return -1;
    }
    pthread_detach(ctx->pub_tid);
    HLOGI(UI_LOG_TAG, "Publisher thread started");

    if (waterfall_enabled) {
        // Start spectrum publisher thread - high-rate FFT/waterfall broadcaster
        if (pthread_create(&ctx->spec_tid, NULL, spectrum_publisher_thread, ctx) != 0) {
            HLOGE(UI_LOG_TAG, "pthread_create(spec) failed: %s", strerror(errno));
            // Non-fatal: status still works without spectrum
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
    // Close the TX sockets.
    udp_tx_close(&ctx->tx);
    spectrum_tx_close(&ctx->spectrum_tx);
    HLOGI(UI_LOG_TAG, "Shut down");
}

#endif /* !TEST_MAIN */

// ---------------- MAIN TEST ----------------

#ifdef TEST_MAIN
int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <tx_ip> <tx_port> <rx_port>\n", argv[0]);
        return 1;
    }

    const char *tx_ip = argv[1];
    int tx_port = atoi(argv[2]);
    int rx_port = atoi(argv[3]);

    pthread_t rx_thread;
    rx_args_t rxa = { .listen_port = (uint16_t)rx_port };
    pthread_create(&rx_thread, NULL, rx_thread_main, &rxa);
    pthread_detach(rx_thread);

    udp_tx_t tx;
    if (udp_tx_init(&tx, tx_ip, (uint16_t)tx_port) != 0) {
        fprintf(stderr, "TX init failed\n");
        return 1;
    }

    srand((unsigned)time(NULL));
    int counter = 0;

    while (1) {
        // Send status
        int bitrate = (rand() % 2) ? 1200 : 2400;
        double snr = 5.0 - (rand() % 100) / 10.0;
        int sync = rand() % 2;
        modem_direction_t dir = (counter % 2) ? DIR_TX : DIR_RX;
        int client = rand() % 2;
        long bytes_transmitted = rand() % 100000;
        long bytes_received = rand() % 100000;

        udp_tx_send_status(&tx, bitrate, snr, "K1ABC", "N0XYZ", sync, dir, client,
                           bytes_transmitted, bytes_received, 1 /* waterfall=true for test */);

        // Occasionally send a soundcard list
        if (counter % 3 == 0) {
            const char *soundcards[] = { "hw:0,0", "hw:1,0", "hw:2,0" };
            udp_tx_send_soundcard_list(&tx, "hw:1,0", soundcards, 3);
        }

        // Occasionally send a radio list
        if (counter % 5 == 0) {
            const char *radios[] = { "Radio A", "Radio B", "Radio C" };
            udp_tx_send_radio_list(&tx, "Radio B", radios, 3);
        }

        counter++;
        hermes_usleep(500 * 1000); // 500 ms
    }

    udp_tx_close(&tx);
    return 0;
}
#endif
