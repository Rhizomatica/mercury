/* HERMES Modem — ARQ datalink entry point (FSM-based rewrite)
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq.h"
#include "arq_fsm.h"
#include "arq_protocol.h"
#include "arq_timing.h"
#include "arq_modem.h"
#include "arq_channels.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>

#include "../common/hermes_log.h"
#include "../common/defines_modem.h"
#include "../common/ring_buffer_posix.h"
#include "../data_interfaces/tcp_interfaces.h"
#include "../modem/framer.h"
#include "../modem/freedv/freedv_api.h"

#define LOG_COMP "arq"

/* ======================================================================
 * Globals required by arq.h
 * ====================================================================== */

arq_info   arq_conn;

/* ======================================================================
 * Module-private state
 * ====================================================================== */

extern cbuf_handle_t data_tx_buffer_arq;
extern cbuf_handle_t data_tx_buffer_arq_control;
extern cbuf_handle_t data_rx_buffer_arq;

extern void tnc_send_pending(void);
extern void tnc_send_cancelpending(void);
extern void tnc_send_connected(void);
extern void tnc_send_cqframe(const char *source_call, int bw_hz);
extern void tnc_send_disconnected(void);
extern void tnc_send_buffer(uint32_t bytes);

extern void init_model(void);

static arq_session_t    g_sess;
static arq_timing_ctx_t g_timing;

/* Serializes ALL access to g_sess.  g_sess is logically owned by the ARQ
 * event-loop thread (which runs the FSM and fires deadline timers), but
 * several scalar fields are also read by the modem RX/TX, TCP and GUI threads
 * through the getters below and written by the modem threads via
 * arq_update_link_metrics() (local_snr_x10) and arq_set_active_modem_mode()
 * (payload_mode).  Without this lock those concurrent accesses are a C data
 * race (UB): under -O2 the compiler is entitled to assume g_sess is not
 * modified concurrently and may hoist/cache the event loop's deadline_ms read,
 * so "now >= deadline_ms" never re-evaluates true and the deadline timer never
 * fires (the on-air ARQ handshake freeze).  Recursive so a dispatch callback
 * that re-enters a getter cannot self-deadlock; never acquired while holding
 * g_qmtx/g_evq_lock, so there is no lock-order inversion. */
/* glibc spells the static recursive-mutex initializer with the _NP suffix;
 * mingw-w64 winpthreads provides the non-suffixed PTHREAD_RECURSIVE_MUTEX_
 * INITIALIZER (and not the _NP one), so fall back to it for the Windows
 * cross-compile. */
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif
static pthread_mutex_t  g_sess_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

/* App TX ring buffer (data from TCP client) */
#define APP_TX_BUF_SIZE (64 * 1024)
static uint8_t         g_app_tx_storage[APP_TX_BUF_SIZE];
static cbuf_handle_t   g_app_tx_buf;
static pthread_mutex_t g_app_tx_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Internal event queue */
#define ARQ_EV_QUEUE_CAP 64
static arq_event_t     g_evq[ARQ_EV_QUEUE_CAP];
static size_t          g_evq_head;
static size_t          g_evq_tail;
static size_t          g_evq_count;
static pthread_mutex_t g_evq_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_evq_cond  = PTHREAD_COND_INITIALIZER;

static arq_channel_bus_t g_bus;

static pthread_t g_loop_tid;
static pthread_t g_cmd_tid;
static pthread_t g_payload_tid;

static volatile bool g_running;
static volatile bool g_initialized;

/* ======================================================================
 * Event queue helpers
 * ====================================================================== */

static void evq_push(const arq_event_t *ev)
{
    pthread_mutex_lock(&g_evq_lock);
    if (g_evq_count < ARQ_EV_QUEUE_CAP)
    {
        g_evq[g_evq_tail] = *ev;
        g_evq_tail = (g_evq_tail + 1) % ARQ_EV_QUEUE_CAP;
        g_evq_count++;
        pthread_cond_signal(&g_evq_cond);
    }
    else
    {
        HLOGW(LOG_COMP, "Event queue full — dropped %s",
              arq_event_name(ev->id));
    }
    pthread_mutex_unlock(&g_evq_lock);
}

/* Must be called with g_evq_lock held */
static bool evq_pop_locked(arq_event_t *ev)
{
    if (g_evq_count == 0) return false;
    *ev = g_evq[g_evq_head];
    g_evq_head = (g_evq_head + 1) % ARQ_EV_QUEUE_CAP;
    g_evq_count--;
    return true;
}

/* ======================================================================
 * PTT injection
 * ====================================================================== */

static void ptt_event_inject(int mode, bool ptt_on)
{
    arq_event_t ev = {0};
    ev.id   = ptt_on ? ARQ_EV_TX_STARTED : ARQ_EV_TX_COMPLETE;
    ev.mode = mode;
    evq_push(&ev);
}

/* ======================================================================
 * FSM callbacks
 * ====================================================================== */

static void cb_send_tx_frame(int packet_type, int mode,
                              size_t frame_size, const uint8_t *frame,
                              int burst_remaining)
{
    /* Frames of one PTT burst arrive as consecutive calls (FSM event loop
     * is single-threaded); the modem action is enqueued once, when the
     * last frame (burst_remaining == 0) lands in the ring. */
    static int pending_burst_frames = 0;

    if (!frame || frame_size == 0 || frame_size > INT_BUFFER_SIZE)
        return;

    cbuf_handle_t dst = (packet_type == PACKET_TYPE_ARQ_DATA)
                        ? data_tx_buffer_arq
                        : data_tx_buffer_arq_control;

    if (write_buffer(dst, (uint8_t *)frame, frame_size) != 0)
    {
        HLOGW(LOG_COMP, "TX buffer write failed (ptype=%d size=%zu)",
              packet_type, frame_size);
    }
    else
    {
        pending_burst_frames++;
    }

    if (burst_remaining > 0)
        return;  /* more frames of this burst follow */

    if (pending_burst_frames == 0)
        return;  /* every write failed — nothing to transmit */

    arq_action_t action = {
        .type        = (packet_type == PACKET_TYPE_ARQ_DATA)
                       ? ARQ_ACTION_TX_PAYLOAD : ARQ_ACTION_TX_CONTROL,
        .mode        = mode,
        .frame_size  = frame_size,
        .frame_count = pending_burst_frames,
    };
    pending_burst_frames = 0;
    arq_modem_enqueue(&action);
}

static void cb_notify_connected(const char *remote_call)
{
    if (arq_conn.src_addr[0] == '\0')
    {
        snprintf(arq_conn.src_addr, CALLSIGN_MAX_SIZE, "%s", remote_call);
        snprintf(arq_conn.dst_addr, CALLSIGN_MAX_SIZE, "%s", arq_conn.my_call_sign);
    }
    arq_conn.TRX = RX;
    /* Flush any stale RX bytes from the previous session before notifying
     * UUCP.  Moved here from cb_notify_disconnected so that the last bytes
     * of the previous session have time to drain to the TCP socket before
     * the buffer is cleared (clearing on disconnect races with UUCP reads). */
    clear_buffer(data_rx_buffer_arq);
    tnc_send_connected();
    HLOGI(LOG_COMP, "Connected to %s", remote_call);
}

static void cb_notify_pending(const char *remote_call)
{
    tnc_send_pending();
    HLOGI(LOG_COMP, "Incoming connection from %s (pending)", remote_call);
}

static void cb_notify_cancelpending(void)
{
    arq_conn.session_bw = 0;
    tnc_send_cancelpending();
    HLOGI(LOG_COMP, "Incoming connection cancelled");
}

static void cb_notify_disconnected(bool to_no_client)
{
    (void)to_no_client;
    bool was_connected = arq_conn.dst_addr[0] != '\0';
    memset(arq_conn.src_addr, 0, sizeof(arq_conn.src_addr));
    memset(arq_conn.dst_addr, 0, sizeof(arq_conn.dst_addr));
    arq_conn.session_bw = 0;
    arq_conn.TRX = RX;
    /* Flush stale TX bytes from the previous session.  RX bytes are flushed
     * at connection start (cb_notify_connected) instead of here, so that the
     * last delivered bytes have time to drain to the TCP socket before the
     * next session clears the buffer. */
    pthread_mutex_lock(&g_app_tx_mtx);
    clear_buffer(g_app_tx_buf);
    pthread_mutex_unlock(&g_app_tx_mtx);
    tnc_send_disconnected();
    HLOGI(LOG_COMP, "Disconnected");
    /* Return to LISTENING after any disconnection (failed call, cancelled call,
     * or ended session) as long as listen mode is active.  The was_connected
     * guard was thought to prevent spurious APP_LISTEN from APP_DISCONNECT-in-
     * DISCONNECTED, but fsm_disconnected has no APP_DISCONNECT handler, so
     * notify_disconnected is never called from that path. */
    (void)was_connected;
    if (arq_conn.listen && arq_conn.my_call_sign[0] != '\0')
    {
        arq_event_t ev = { .id = ARQ_EV_APP_LISTEN };
        evq_push(&ev);
    }
}

static void cb_deliver_rx_data(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > INT_BUFFER_SIZE)
        return;
    write_buffer(data_rx_buffer_arq, (uint8_t *)data, len);
}

static int cb_tx_backlog(void)
{
    pthread_mutex_lock(&g_app_tx_mtx);
    int n = (int)size_buffer(g_app_tx_buf);
    pthread_mutex_unlock(&g_app_tx_mtx);
    return n;
}

static int cb_tx_read(uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return 0;
    pthread_mutex_lock(&g_app_tx_mtx);
    size_t avail = size_buffer(g_app_tx_buf);
    if (avail > len) avail = len;
    int n = 0;
    if (avail > 0)
        n = (read_buffer(g_app_tx_buf, buf, avail) == 0) ? (int)avail : 0;
    pthread_mutex_unlock(&g_app_tx_mtx);
    return n;
}

static void cb_send_buffer_status(int backlog_bytes)
{
    tnc_send_buffer((uint32_t)(backlog_bytes < 0 ? 0 : backlog_bytes));
}

static int normalize_bandwidth_hz(int bw_hz)
{
    if (bw_hz == ARQ_BANDWIDTH_NARROW_HZ ||
        bw_hz == ARQ_BANDWIDTH_FULL_HZ ||
        bw_hz == ARQ_BANDWIDTH_TACTICAL_HZ)
        return bw_hz;

    return ARQ_BANDWIDTH_FULL_HZ;
}

static int active_session_bandwidth_hz(void)
{
    if (arq_conn.session_bw != 0)
        return normalize_bandwidth_hz(arq_conn.session_bw);

    return normalize_bandwidth_hz(arq_conn.bw);
}

int arq_effective_bandwidth_hz(void)
{
    if (active_session_bandwidth_hz() == ARQ_BANDWIDTH_NARROW_HZ)
        return ARQ_BANDWIDTH_NARROW_HZ;

    return ARQ_BANDWIDTH_FULL_HZ;
}

int arq_reported_bandwidth_hz(void)
{
    return active_session_bandwidth_hz();
}

bool arq_bandwidth_allows_mode(int mode)
{
    /* Wideband payload modes (~1.7-2.2 kHz) need more than the narrow
     * session bandwidth. */
    if (mode == FREEDV_MODE_DATAC1 ||
        mode == FREEDV_MODE_DATAC17 ||
        mode == FREEDV_MODE_QAM16C2)
        return arq_effective_bandwidth_hz() > ARQ_BANDWIDTH_NARROW_HZ;

    return true;
}

/* ======================================================================
 * CMD bridge worker
 * ====================================================================== */

static void handle_cmd(const arq_cmd_msg_t *msg)
{
    arq_event_t ev = {0};

    switch (msg->type)
    {
    case ARQ_CMD_SET_CALLSIGN:
        snprintf(arq_conn.my_call_sign, CALLSIGN_MAX_SIZE, "%s", msg->arg0);
        /* Setting a new primary callsign clears all secondary callsigns */
        memset(arq_conn.secondary_calls, 0, sizeof(arq_conn.secondary_calls));
        arq_conn.secondary_call_count = 0;
        HLOGI(LOG_COMP, "My callsign: %s", arq_conn.my_call_sign);
        return;

    case ARQ_CMD_ADD_SECONDARY_CALLSIGN:
        if (arq_conn.secondary_call_count < CALLSIGN_MAX_SECONDARY)
        {
            snprintf(arq_conn.secondary_calls[arq_conn.secondary_call_count],
                     CALLSIGN_MAX_SIZE, "%s", msg->arg0);
            arq_conn.secondary_call_count++;
            HLOGI(LOG_COMP, "Secondary callsign added: %s (total=%d)",
                  msg->arg0, arq_conn.secondary_call_count);
        }
        else
        {
            HLOGW(LOG_COMP, "Secondary callsign list full (max=%d), ignoring %s",
                  CALLSIGN_MAX_SECONDARY, msg->arg0);
        }
        return;

    case ARQ_CMD_CLEAR_SECONDARY_CALLSIGNS:
        memset(arq_conn.secondary_calls, 0, sizeof(arq_conn.secondary_calls));
        arq_conn.secondary_call_count = 0;
        HLOGI(LOG_COMP, "Secondary callsigns cleared");
        return;

    case ARQ_CMD_SET_BANDWIDTH:
        arq_conn.bw = normalize_bandwidth_hz(msg->value);
        return;

    case ARQ_CMD_SET_RETRY:
        arq_conn.retry_slots = msg->value;
        arq_set_retry_slots(msg->value);
        return;

    case ARQ_CMD_SET_CALLINT:
        if (msg->value <= 0)
        {
            atomic_store(&arq_callint_override_s, ARQ_CALLINT_DEFAULT_S);
            const arq_mode_timing_t *ctrl =
                arq_protocol_mode_timing(ARQ_CONTROL_MODE);
            HLOGI(LOG_COMP, "CALLINT reset to default (%.1fs)",
                  ctrl ? ctrl->retry_interval_s : 8.0f);
        }
        else
        {
            float v = (float)msg->value;
            if (v < ARQ_CALLINT_MIN_S) v = ARQ_CALLINT_MIN_S;
            atomic_store(&arq_callint_override_s, v);
            HLOGI(LOG_COMP, "CALLINT set to %.1fs", v);
        }
        return;

    case ARQ_CMD_LISTEN_ON:
        arq_conn.listen = true;
        ev.id = ARQ_EV_APP_LISTEN;
        break;

    case ARQ_CMD_LISTEN_OFF:
        arq_conn.listen = false;
        ev.id = ARQ_EV_APP_STOP_LISTEN;
        break;

    case ARQ_CMD_CONNECT:
        snprintf(arq_conn.src_addr, CALLSIGN_MAX_SIZE, "%s", msg->arg0);
        snprintf(arq_conn.dst_addr, CALLSIGN_MAX_SIZE, "%s", msg->arg1);
        arq_conn.session_bw = 0;
        snprintf(ev.remote_call, CALLSIGN_MAX_SIZE, "%s", msg->arg1);
        ev.id = ARQ_EV_APP_CONNECT;
        break;

    case ARQ_CMD_SEND_CQ:
    {
        uint8_t frame[INT_BUFFER_SIZE];
        const char *source_call = msg->arg0[0] ? msg->arg0 : arq_conn.my_call_sign;
        int bw_hz = normalize_bandwidth_hz(msg->value);
        int n = arq_protocol_build_cq(frame, sizeof(frame), source_call, bw_hz);
        if (n <= 0)
        {
            HLOGW(LOG_COMP, "Failed to build CQ frame (source=%s bw=%d)",
                  source_call, bw_hz);
            return;
        }

        pthread_mutex_lock(&g_sess_lock);
        int cq_mode = g_sess.control_mode;
        pthread_mutex_unlock(&g_sess_lock);
        cb_send_tx_frame(PACKET_TYPE_ARQ_CQ, cq_mode, (size_t)n, frame, 0);
        HLOGI(LOG_COMP, "Queued CQ frame from %s (%d Hz)", source_call, bw_hz);
        return;
    }

    case ARQ_CMD_DISCONNECT:
        /* VARA TNC spec: send DISCONNECTED to host immediately on receiving
         * DISCONNECT command, before the over-the-air exchange completes.
         * This lets the host close the socket cleanly while the FSM handles
         * the air-side teardown.  We deliberately do NOT clear g_app_tx_buf
         * here: the FSM drains any bytes still queued, then completes a clean
         * air-side DISCONNECT handshake.  The retry-exhaustion path bounds the
         * drain so a dead channel still tears down quickly (see arq_fsm.c). */
        tnc_send_disconnected();
        ev.id = ARQ_EV_APP_DISCONNECT;
        break;

    case ARQ_CMD_ABORT:
        /* Dirty disconnect: flush all buffers immediately so the FSM sees no
         * pending data and transitions to DISCONNECTED without deferral.
         * No air-side DISCONNECT frame is sent — the peer will time out. */
        clear_connection_data();
        tnc_send_disconnected();
        ev.id = ARQ_EV_APP_DISCONNECT;
        break;

    case ARQ_CMD_CLIENT_DISCONNECT:
        ev.id = ARQ_EV_APP_DISCONNECT;
        break;

    case ARQ_CMD_CLIENT_CONNECT:
        HLOGD(LOG_COMP, "Client (re)connected");
        return;

    case ARQ_CMD_SET_PUBLIC:
    case ARQ_CMD_NONE:
    default:
        return;
    }

    evq_push(&ev);
}

static void *arq_cmd_bridge_worker(void *arg)
{
    arq_cmd_msg_t msg;
    (void)arg;
    while (arq_channel_bus_recv_cmd(&g_bus, &msg) == 0)
        handle_cmd(&msg);
    return NULL;
}

/* ======================================================================
 * Payload bridge worker
 * ====================================================================== */

static void *arq_payload_bridge_worker(void *arg)
{
    arq_bytes_msg_t payload;
    (void)arg;
    while (arq_channel_bus_recv_payload(&g_bus, &payload) == 0)
    {
        if (payload.len == 0 || payload.len > INT_BUFFER_SIZE)
            continue;
        write_buffer(g_app_tx_buf, payload.data, payload.len);

        arq_event_t ev = { .id = ARQ_EV_APP_DATA_READY };
        evq_push(&ev);
    }
    return NULL;
}

/* ======================================================================
 * Main ARQ event loop
 * ====================================================================== */

static void *arq_event_loop_worker(void *arg)
{
    (void)arg;
    HLOGI(LOG_COMP, "Event loop started");

    while (g_running)
    {
        uint64_t now   = hermes_uptime_ms();
        pthread_mutex_lock(&g_sess_lock);
        int timeout_ms = arq_fsm_timeout_ms(&g_sess, now);
        pthread_mutex_unlock(&g_sess_lock);
        if (timeout_ms > 500 || timeout_ms < 0)
            timeout_ms = 500;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000LL;
        if (ts.tv_nsec >= 1000000000LL) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000LL;
        }

        pthread_mutex_lock(&g_evq_lock);
        if (g_evq_count == 0)
            pthread_cond_timedwait(&g_evq_cond, &g_evq_lock, &ts);

        arq_event_t events[ARQ_EV_QUEUE_CAP];
        size_t n = 0;
        arq_event_t ev;
        while (evq_pop_locked(&ev) && n < ARQ_EV_QUEUE_CAP)
            events[n++] = ev;
        pthread_mutex_unlock(&g_evq_lock);

        pthread_mutex_lock(&g_sess_lock);
        for (size_t i = 0; i < n; i++)
            arq_fsm_dispatch(&g_sess, &events[i]);

        /* Fire deadline */
        now = hermes_uptime_ms();
        if (g_sess.deadline_ms != UINT64_MAX && now >= g_sess.deadline_ms)
        {
            g_sess.deadline_ms = UINT64_MAX;
            arq_event_t tev = { .id = g_sess.deadline_event };
            arq_fsm_dispatch(&g_sess, &tev);
        }
        pthread_mutex_unlock(&g_sess_lock);
    }

    HLOGI(LOG_COMP, "Event loop stopped");
    return NULL;
}

/* ======================================================================
 * Incoming frame handling (called from modem.c worker)
 * ====================================================================== */

bool arq_handle_incoming_connect_frame(uint8_t *data, size_t frame_size)
{
    if (!data || frame_size < 2) return false;

    bool is_accept = (data[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_ACCEPT_FLAG) != 0;
    uint8_t session_id;
    char src[CALLSIGN_MAX_SIZE] = {0};
    char dst[CALLSIGN_MAX_SIZE] = {0};
    int bw_hz = 0;

    int rc = is_accept
             ? arq_protocol_parse_accept(data, frame_size, &session_id, src, dst, &bw_hz)
             : arq_protocol_parse_call  (data, frame_size, &session_id, src, dst, &bw_hz);

    if (rc < 0)
    {
        HLOGD(LOG_COMP, "CALL/ACCEPT parse failed");
        return false;
    }

    /* Validate that DST CRC16 matches our own callsign or a secondary */
    if (arq_conn.my_call_sign[0] != 0)
    {
        uint16_t frame_crc = (uint16_t)data[ARQ_CONNECT_PAYLOAD_IDX]
                           | ((uint16_t)data[ARQ_CONNECT_PAYLOAD_IDX + 1] << 8);
        bool match = (frame_crc == arq_protocol_callsign_crc16(arq_conn.my_call_sign));
        for (int i = 0; !match && i < arq_conn.secondary_call_count; i++)
            match = (frame_crc == arq_protocol_callsign_crc16(arq_conn.secondary_calls[i]));
        if (!match)
        {
            HLOGD(LOG_COMP, "CALL/ACCEPT not for us (DST CRC16 mismatch)");
            return false;
        }
    }

    arq_event_t ev = {0};
    ev.id         = is_accept ? ARQ_EV_RX_ACCEPT : ARQ_EV_RX_CALL;
    ev.session_id = session_id;
    /* src = transmitting side's callsign */
    snprintf(ev.remote_call, CALLSIGN_MAX_SIZE, "%s", src);
    if (is_accept)
        arq_conn.session_bw = normalize_bandwidth_hz(bw_hz);
    else
        arq_conn.session_bw = normalize_bandwidth_hz(
            bw_hz < normalize_bandwidth_hz(arq_conn.bw) ? bw_hz : normalize_bandwidth_hz(arq_conn.bw));
    evq_push(&ev);
    return true;
}

bool arq_handle_incoming_cq_frame(uint8_t *data, size_t frame_size)
{
    char source_call[CALLSIGN_MAX_SIZE] = {0};
    int bw_hz = 0;

    if (!data || frame_size < ARQ_CONTROL_FRAME_SIZE)
        return false;

    if (arq_protocol_parse_cq(data, frame_size, source_call, &bw_hz) < 0)
    {
        HLOGD(LOG_COMP, "CQ parse failed");
        return false;
    }

    tnc_send_cqframe(source_call, normalize_bandwidth_hz(bw_hz));
    HLOGI(LOG_COMP, "CQ frame decoded from %s (%d Hz)", source_call, bw_hz);
    return true;
}

void arq_notify_cq_tx_started(void)
{
    tnc_send_pending();
    HLOGI(LOG_COMP, "CQ transmission started");
}

void arq_notify_cq_tx_complete(void)
{
    tnc_send_cancelpending();
    HLOGI(LOG_COMP, "CQ transmission completed");
}

void arq_handle_incoming_frame(uint8_t *data, size_t frame_size, float rx_snr)
{
    if (!data || frame_size < ARQ_FRAME_HDR_SIZE) return;

    arq_frame_hdr_t hdr;
    if (arq_protocol_decode_hdr(data, frame_size, &hdr) < 0)
    {
        HLOGD(LOG_COMP, "Frame header decode failed");
        return;
    }

    arq_event_t ev = {0};
    ev.session_id    = hdr.session_id;
    ev.seq           = hdr.tx_seq;
    ev.ack_seq       = hdr.rx_ack_seq;
    ev.rx_flags      = hdr.flags;
    ev.snr_encoded   = (int8_t)hdr.snr_raw;
    ev.ack_delay_raw = hdr.ack_delay_raw;

    if (hdr.packet_type == PACKET_TYPE_ARQ_DATA)
    {
        ev.id = ARQ_EV_RX_DATA;
        /* Infer the FreeDV mode from frame_size by matching the mode table.
         * This lets the FSM track what mode the peer was actually transmitting
         * in (for decoder-sync enforcement on role switch). */
        ev.mode = FREEDV_MODE_DATAC15;   /* safe default */
        for (int i = 0; i < arq_mode_table_count; i++)
        {
            if ((int)frame_size == arq_mode_table[i].payload_bytes)
            {
                ev.mode = arq_mode_table[i].freedv_mode;
                break;
            }
        }
        size_t slot_bytes = (frame_size > ARQ_FRAME_HDR_SIZE)
                            ? (frame_size - ARQ_FRAME_HDR_SIZE) : 0;
        /* ack_delay_raw is repurposed in DATA frames: 0 = full frame (all
         * slot_bytes are valid), else = bits [7:0] of the valid byte count.
         * Bits 8-10 travel in the flags byte (ARQ_FLAG_LEN_HI / LEN_B9 /
         * LEN_B10), allowing counts up to 2047 (QAM16C2 carries 1205 user
         * bytes).  See ARQ_DATA_LEN_FULL in arq_protocol.h. */
        const uint8_t len_flags =
            ARQ_FLAG_LEN_HI | ARQ_FLAG_LEN_B9 | ARQ_FLAG_LEN_B10;
        size_t valid_bytes;
        if (hdr.ack_delay_raw == ARQ_DATA_LEN_FULL &&
            !(hdr.flags & len_flags))
        {
            valid_bytes = slot_bytes;
        }
        else
        {
            valid_bytes = (size_t)hdr.ack_delay_raw;
            if (hdr.flags & ARQ_FLAG_LEN_HI)
                valid_bytes |= 0x100u;
            if (hdr.flags & ARQ_FLAG_LEN_B9)
                valid_bytes |= 0x200u;
            if (hdr.flags & ARQ_FLAG_LEN_B10)
                valid_bytes |= 0x400u;
        }
        if (valid_bytes > slot_bytes)
            valid_bytes = slot_bytes;   /* sanity cap */
        ev.data_bytes = valid_bytes;
        ev.rx_snr     = rx_snr;
        if (valid_bytes > 0 && valid_bytes <= sizeof(ev.payload))
        {
            memcpy(ev.payload, data + ARQ_FRAME_HDR_SIZE, valid_bytes);
            ev.payload_len = valid_bytes;
        }
    }
    else if (hdr.packet_type == PACKET_TYPE_ARQ_CONTROL)
    {
        switch (hdr.subtype)
        {
        case ARQ_SUBTYPE_ACK:          ev.id = ARQ_EV_RX_ACK;           break;
        case ARQ_SUBTYPE_DISCONNECT:   ev.id = ARQ_EV_RX_DISCONNECT;    break;
        case ARQ_SUBTYPE_TURN_REQ:     ev.id = ARQ_EV_RX_TURN_REQ;      break;
        case ARQ_SUBTYPE_TURN_ACK:     ev.id = ARQ_EV_RX_TURN_ACK;      break;
        case ARQ_SUBTYPE_KEEPALIVE:    ev.id = ARQ_EV_RX_KEEPALIVE;     break;
        case ARQ_SUBTYPE_KEEPALIVE_ACK: ev.id = ARQ_EV_RX_KEEPALIVE_ACK; break;
        case ARQ_SUBTYPE_MODE_REQ:
            ev.id   = ARQ_EV_RX_MODE_REQ;
            ev.mode = (frame_size > ARQ_FRAME_HDR_SIZE)
                      ? (int)data[ARQ_FRAME_HDR_SIZE] : 0;
            break;
        case ARQ_SUBTYPE_MODE_ACK:
            ev.id   = ARQ_EV_RX_MODE_ACK;
            ev.mode = (frame_size > ARQ_FRAME_HDR_SIZE)
                      ? (int)data[ARQ_FRAME_HDR_SIZE] : 0;
            break;
        default:
            return;
        }
    }
    else
    {
        return;
    }

    evq_push(&ev);
}

/* ======================================================================
 * Public arq.h API
 * ====================================================================== */

int arq_init(size_t frame_size, int mode)
{
    if (frame_size == 0 || frame_size > INT_BUFFER_SIZE)
    {
        HLOGE(LOG_COMP, "Init failed: bad frame_size=%zu", frame_size);
        return -1;
    }

    memset(&arq_conn, 0, sizeof(arq_conn));
    arq_conn.frame_size      = frame_size;
    arq_conn.mode            = mode;
    arq_conn.call_burst_size = 1;
    arq_conn.bw              = ARQ_BANDWIDTH_FULL_HZ;
    arq_conn.session_bw      = 0;

    init_model();

    g_app_tx_buf = circular_buf_init(g_app_tx_storage, APP_TX_BUF_SIZE);
    if (!g_app_tx_buf)
    {
        HLOGE(LOG_COMP, "Failed to init app TX buffer");
        return -1;
    }

    arq_timing_init(&g_timing);
    arq_fsm_init(&g_sess);
    /* Record the startup payload mode (= broadcast RX mode) so that
     * sess_enter() can restore peer_tx_mode on disconnect, allowing
     * the payload decoder to receive broadcast frames while LISTENING. */
    g_sess.initial_payload_mode = mode;
    g_sess.peer_tx_mode         = mode;  /* match broadcast mode at startup */

    static const arq_fsm_callbacks_t cbs = {
        .send_tx_frame       = cb_send_tx_frame,
        .notify_connected    = cb_notify_connected,
        .notify_pending      = cb_notify_pending,
        .notify_cancelpending = cb_notify_cancelpending,
        .notify_disconnected = cb_notify_disconnected,
        .deliver_rx_data     = cb_deliver_rx_data,
        .tx_backlog          = cb_tx_backlog,
        .tx_read             = cb_tx_read,
        .send_buffer_status  = cb_send_buffer_status,
    };
    arq_fsm_set_callbacks(&cbs);
    arq_fsm_set_timing(&g_timing);
    arq_modem_set_event_fn(ptt_event_inject);
    arq_modem_queue_init(64);

    if (arq_channel_bus_init(&g_bus) < 0)
    {
        HLOGE(LOG_COMP, "Channel bus init failed");
        return -1;
    }

    g_running = true;
    if (pthread_create(&g_loop_tid, NULL, arq_event_loop_worker, NULL) != 0)
    {
        HLOGE(LOG_COMP, "Failed to start event loop thread");
        arq_channel_bus_dispose(&g_bus);
        return -1;
    }

    if (pthread_create(&g_cmd_tid, NULL, arq_cmd_bridge_worker, NULL) != 0 ||
        pthread_create(&g_payload_tid, NULL, arq_payload_bridge_worker, NULL) != 0)
    {
        HLOGE(LOG_COMP, "Failed to start bridge threads");
        g_running = false;
        pthread_mutex_lock(&g_evq_lock);
        pthread_cond_broadcast(&g_evq_cond);
        pthread_mutex_unlock(&g_evq_lock);
        arq_channel_bus_close(&g_bus);
        pthread_join(g_loop_tid, NULL);
        arq_channel_bus_dispose(&g_bus);
        return -1;
    }

    g_initialized = true;
    HLOGI(LOG_COMP, "ARQ initialized (frame=%zu mode=%d)", frame_size, mode);
    return 0;
}

void arq_shutdown(void)
{
    if (!g_initialized) return;
    g_initialized = false;
    g_running     = false;

    arq_channel_bus_close(&g_bus);

    pthread_mutex_lock(&g_evq_lock);
    pthread_cond_broadcast(&g_evq_cond);
    pthread_mutex_unlock(&g_evq_lock);

    arq_modem_queue_shutdown();

    pthread_join(g_loop_tid, NULL);
    pthread_join(g_cmd_tid, NULL);
    pthread_join(g_payload_tid, NULL);

    arq_channel_bus_dispose(&g_bus);

    if (g_app_tx_buf)
    {
        circular_buf_free(g_app_tx_buf);
        g_app_tx_buf = NULL;
    }

    HLOGI(LOG_COMP, "ARQ shutdown complete");
}

void arq_tick_1hz(void) { }

void arq_post_event(int event) { (void)event; }

bool arq_is_link_connected(void)
{
    pthread_mutex_lock(&g_sess_lock);
    bool connected = (g_sess.conn_state == ARQ_CONN_CONNECTED);
    pthread_mutex_unlock(&g_sess_lock);
    return connected;
}

int arq_queue_data(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return -1;
    int rc = write_buffer(g_app_tx_buf, (uint8_t *)data, len);
    if (rc == 0)
    {
        arq_event_t ev = { .id = ARQ_EV_APP_DATA_READY };
        evq_push(&ev);
    }
    return rc;
}

int arq_get_tx_backlog_bytes(void)  { return cb_tx_backlog(); }

#define SESS_READ(expr) do { \
        pthread_mutex_lock(&g_sess_lock); \
        int _v = (expr); \
        pthread_mutex_unlock(&g_sess_lock); \
        return _v; \
    } while (0)

int arq_get_speed_level(void)       { SESS_READ(g_sess.speed_level); }
int arq_get_payload_mode(void)      { SESS_READ(g_sess.payload_mode); }
int arq_get_control_mode(void)      { SESS_READ(g_sess.control_mode); }
int arq_get_preferred_rx_mode(void) { SESS_READ(arq_modem_preferred_rx_mode(&g_sess)); }
int arq_get_preferred_tx_mode(void) { SESS_READ(arq_modem_preferred_tx_mode(&g_sess)); }

void arq_set_active_modem_mode(int mode, size_t frame_size)
{
    /* Only record payload_mode for data modes.  Control-mode TX switches
     * (DATAC16 for ACK/TURN_REQ/etc.) must not overwrite the negotiated
     * payload_mode that the RX decoder uses when the peer transmits data. */
    pthread_mutex_lock(&g_sess_lock);
    if (mode != g_sess.control_mode)
        g_sess.payload_mode = mode;
    pthread_mutex_unlock(&g_sess_lock);
    arq_conn.mode       = mode;
    arq_conn.frame_size = frame_size;
}

void arq_update_link_metrics(int sync, float snr, int rx_status, bool frame_decoded)
{
    (void)sync; (void)rx_status;
    if (frame_decoded && snr > -100.0f && snr < 100.0f)
    {
        pthread_mutex_lock(&g_sess_lock);
        if (g_sess.local_snr_x10 == 0)
            g_sess.local_snr_x10 = (int)(snr * 10.0f);
        else
            g_sess.local_snr_x10 = (g_sess.local_snr_x10 * 3 + (int)(snr * 10.0f)) / 4;
        pthread_mutex_unlock(&g_sess_lock);
    }
}

bool arq_try_dequeue_action(arq_action_t *action)
{
    if (!action) return false;
    return arq_modem_dequeue(action, 0);
}

bool arq_wait_dequeue_action(arq_action_t *action, int timeout_ms)
{
    if (!action) return false;
    return arq_modem_dequeue(action, timeout_ms);
}

bool arq_get_runtime_snapshot(arq_runtime_snapshot_t *snapshot)
{
    if (!snapshot || !g_initialized) return false;
    int backlog = cb_tx_backlog();   /* takes g_app_tx_mtx; compute before g_sess_lock */
    pthread_mutex_lock(&g_sess_lock);
    snapshot->initialized      = true;
    snapshot->connected        = (g_sess.conn_state == ARQ_CONN_CONNECTED);
    snapshot->trx              = arq_conn.TRX;
    snapshot->tx_backlog_bytes = backlog + g_sess.tx_inflight_bytes;
    snapshot->speed_level      = g_sess.speed_level;
    snapshot->payload_mode      = g_sess.payload_mode;
    snapshot->peer_tx_mode      = g_sess.peer_tx_mode;
    snapshot->control_mode      = g_sess.control_mode;
    snapshot->preferred_rx_mode = arq_modem_preferred_rx_mode(&g_sess);
    snapshot->preferred_tx_mode = arq_modem_preferred_tx_mode(&g_sess);
    snapshot->tx_bytes          = g_timing.tx_bytes;
    snapshot->rx_bytes          = g_timing.rx_bytes;
    pthread_mutex_unlock(&g_sess_lock);
    return true;
}

int arq_submit_tcp_cmd(const arq_cmd_msg_t *cmd)
{
    if (!cmd || !g_initialized) return -1;
    return arq_channel_bus_try_send_cmd(&g_bus, cmd);
}

int arq_submit_tcp_payload(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || !g_initialized) return -1;
    return arq_channel_bus_try_send_payload(&g_bus, data, len);
}

void clear_connection_data(void)
{
    pthread_mutex_lock(&g_app_tx_mtx);
    clear_buffer(g_app_tx_buf);
    pthread_mutex_unlock(&g_app_tx_mtx);
    clear_buffer(data_rx_buffer_arq);
    clear_buffer(data_tx_buffer_arq);
    clear_buffer(data_tx_buffer_arq_control);
}

void arq_set_no_progress_timeout_s(int seconds)
{
    if (seconds > 0)
        atomic_store(&arq_no_progress_timeout_s, seconds);
    else
        atomic_store(&arq_no_progress_timeout_s, ARQ_NO_PROGRESS_TIMEOUT_S_DEFAULT);
    HLOGI(LOG_COMP, "ARQ no-progress timeout: %ds",
          atomic_load(&arq_no_progress_timeout_s));
}

void arq_set_disconnect_drain_timeout_s(int seconds)
{
    if (seconds > 0)
        atomic_store(&arq_disconnect_drain_timeout_s, seconds);
    else
        atomic_store(&arq_disconnect_drain_timeout_s,
                     ARQ_DISCONNECT_DRAIN_TIMEOUT_S_DEFAULT);
    HLOGI(LOG_COMP, "ARQ disconnect drain timeout: %ds",
          atomic_load(&arq_disconnect_drain_timeout_s));
}

void arq_set_retry_slots(int slots)
{
    if (slots <= 0)
    {
        atomic_store(&arq_call_retry_slots,       ARQ_CALL_RETRY_SLOTS_DEFAULT);
        atomic_store(&arq_accept_retry_slots,     ARQ_ACCEPT_RETRY_SLOTS_DEFAULT);
        atomic_store(&arq_data_retry_slots,       ARQ_DATA_RETRY_SLOTS_DEFAULT);
        atomic_store(&arq_disconnect_retry_slots, ARQ_DISCONNECT_RETRY_SLOTS_DEFAULT);
    }
    else
    {
        atomic_store(&arq_call_retry_slots,   slots);
        atomic_store(&arq_accept_retry_slots, slots);
        atomic_store(&arq_data_retry_slots,   slots);
        /* Leave disconnect retries at default — no benefit to long teardown */
        atomic_store(&arq_disconnect_retry_slots, ARQ_DISCONNECT_RETRY_SLOTS_DEFAULT);
    }
    HLOGI(LOG_COMP, "Retry slots: call=%d accept=%d data=%d disconnect=%d",
          atomic_load(&arq_call_retry_slots), atomic_load(&arq_accept_retry_slots),
          atomic_load(&arq_data_retry_slots), atomic_load(&arq_disconnect_retry_slots));
}

void reset_arq_info(arq_info *conn)
{
    if (!conn) return;
    char my_call[CALLSIGN_MAX_SIZE];
    snprintf(my_call, CALLSIGN_MAX_SIZE, "%s", conn->my_call_sign);
    char secondary[CALLSIGN_MAX_SECONDARY][CALLSIGN_MAX_SIZE];
    int  secondary_count = conn->secondary_call_count;
    memcpy(secondary, conn->secondary_calls, sizeof(secondary));
    int bw = conn->bw;
    bool listen = conn->listen;
    memset(conn, 0, sizeof(*conn));
    snprintf(conn->my_call_sign, CALLSIGN_MAX_SIZE, "%s", my_call);
    memcpy(conn->secondary_calls, secondary, sizeof(secondary));
    conn->secondary_call_count = secondary_count;
    conn->bw              = bw;
    conn->listen          = listen;
    conn->call_burst_size = 1;
}

void call_remote(void)         { }
void callee_accept_connection(void) { }
