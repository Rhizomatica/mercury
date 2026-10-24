/* HERMES Modem — ARQ datalink entry point (FSM-based rewrite)
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq.h"
#include "arq_fsm.h"
#include "arq_tnc.h"
#include "arq_protocol.h"
#include "arq_trace.h"
#include "arq_timing.h"
#include "arq_modem.h"
#include "arq_channels.h"
#include "carousel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>

#include "../common/hermes_log.h"
#include "../common/os_interop.h"
#include "../common/virtual_clock.h"
#include "../common/defines_modem.h"
#include "../common/ring_buffer_posix.h"
#include "../common/message_store.h"
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

/* Guards the process-global arq_conn.  arq_conn is read and written from five
 * threads (event loop, cmd bridge, modem RX frame handlers, modem TX PTT path,
 * and the GUI status thread), so every field access is a data race without
 * this lock.  It is a LEAF lock: it is never held while acquiring g_sess_lock
 * or g_app_tx_mtx.  That is deliberate -- arq_get_runtime_snapshot() takes
 * g_app_tx_mtx (via cb_tx_backlog) before g_sess_lock, while cb_notify_
 * disconnected() writes arq_conn and then takes g_app_tx_mtx; guarding
 * arq_conn with either of those locks would invert an ordering and deadlock.
 * A separate leaf lock has no ordering relationship to maintain. */
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;

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

/* Read by the modem RX/TX and TCP threads while the main thread writes them
 * at init/teardown.  `volatile` orders nothing between threads and is not
 * atomic in C -- formally a data race, and ThreadSanitizer flags exactly that
 * (arq_init writing g_initialized against arq_get_runtime_snapshot reading it
 * from the RX thread).  C11 atomics keep the plain read/write syntax, so only
 * the declaration changes. */
static _Atomic bool g_running;
static _Atomic bool g_initialized;

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

/* Wake the event loop without queueing an event.  Called by the -x sock
 * lockstep transport (audioio.c) after each virtual_clock_set(): the loop's
 * pthread_cond_timedwait deadline is wall time, so in virtual mode it would
 * otherwise sleep up to its 500 ms cap past a due FSM deadline.  A bare
 * signal makes it re-read time_now_ms() and fire anything now due. */
void arq_notify_virtual_time(void)
{
    pthread_mutex_lock(&g_evq_lock);
    pthread_cond_signal(&g_evq_cond);
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
     * last frame (burst_remaining == 0) lands in the ring.
     *
     * The counter lives in g_sess.pending_burst_frames rather than a
     * function-static so that it is covered by g_sess_lock — both this
     * call path (event loop, already under g_sess_lock) and the cmd-bridge
     * SEND_CQ path (which now holds g_sess_lock across this call) share the
     * same lock, preventing a burst-drop race. */
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
        g_sess.pending_burst_frames++;
    }

    if (burst_remaining > 0)
        return;  /* more frames of this burst follow */

    if (g_sess.pending_burst_frames == 0)
        return;  /* every write failed — nothing to transmit */

    arq_action_t action = {
        .type        = (packet_type == PACKET_TYPE_ARQ_DATA)
                       ? ARQ_ACTION_TX_PAYLOAD : ARQ_ACTION_TX_CONTROL,
        .mode        = mode,
        .frame_size  = frame_size,
        .frame_count = g_sess.pending_burst_frames,
    };
    g_sess.pending_burst_frames = 0;
    arq_modem_enqueue(&action);
}

/* Enqueue a pattern for the modem TX worker: no coded frame, the modem
 * synthesises the tone burst itself (send_pattern_ack in modem.c). */
static void cb_send_pattern(int pattern_kind)
{
    arq_action_t action = {
        .type         = ARQ_ACTION_TX_PATTERN,
        .mode         = -1,
        .frame_count  = 1,
        .pattern_kind = pattern_kind,
    };
    arq_modem_enqueue(&action);
}

/* Carousel keydown: the FSM's frames are only valid during the call, so the
 * modem gets a copy it frees when sent. */
static void cb_send_keydown(const arq_keydown_t *kd)
{
    arq_keydown_t *copy = (arq_keydown_t *)malloc(sizeof(*copy));
    if (!copy)
    {
        HLOGE(LOG_COMP, "keydown dropped: out of memory");
        arq_modem_ptt_off();          /* the carousel waits for its end */
        return;
    }
    *copy = *kd;
    arq_action_t action = {
        .type    = ARQ_ACTION_TX_KEYDOWN,
        .mode    = kd->f[0].mode,
        .keydown = copy,
    };
    if (arq_modem_enqueue(&action) != 0)
    {
        HLOGW(LOG_COMP, "keydown dropped: modem queue full");
        free(copy);
        arq_modem_ptt_off();
    }
}

static void cb_notify_connected(const char *remote_call, const char *local_call)
{
    pthread_mutex_lock(&g_conn_lock);
    if (arq_conn.src_addr[0] == '\0')
    {
        /* Callee side: report the caller as src and the SSID they dialed as dst.
         * local_call is the callsign (primary or secondary) whose DST CRC16
         * matched the incoming CALL; fall back to the primary if unset. */
        snprintf(arq_conn.src_addr, CALLSIGN_MAX_SIZE, "%s", remote_call);
        snprintf(arq_conn.dst_addr, CALLSIGN_MAX_SIZE, "%s",
                 (local_call && local_call[0]) ? local_call : arq_conn.my_call_sign);
    }
    arq_conn.TRX = RX;
    pthread_mutex_unlock(&g_conn_lock);
    /* Flush any stale RX bytes from the previous session before notifying
     * UUCP.  Moved here from cb_notify_disconnected so that the last bytes
     * of the previous session have time to drain to the TCP socket before
     * the buffer is cleared (clearing on disconnect races with UUCP reads). */
    clear_buffer(data_rx_buffer_arq);
    /* A new session starts with a clean slate: drop any partial chat line left
     * over from the previous session's feed buffer. */
    msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_RX);
    msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_TX);
    arq_tnc_send_connected();   /* dispatches to tnc_send_connected, which takes g_conn_lock via arq_conn_get_calls; must be outside our lock */
    HLOGI(LOG_COMP, "Connected to %s", remote_call);
}

static void cb_notify_pending(const char *remote_call, const char *local_call)
{
    arq_tnc_send_pending();
    HLOGI(LOG_COMP, "Incoming connection from %s on %s (pending)",
          remote_call, (local_call && local_call[0]) ? local_call : "(primary)");
}

static void cb_notify_cancelpending(void)
{
    pthread_mutex_lock(&g_conn_lock);
    arq_conn.session_bw = 0;
    pthread_mutex_unlock(&g_conn_lock);
    arq_tnc_send_cancelpending();
    HLOGI(LOG_COMP, "Incoming connection cancelled");
}

static void cb_notify_disconnected(bool to_no_client)
{
    (void)to_no_client;
    pthread_mutex_lock(&g_conn_lock);
    bool was_connected = arq_conn.dst_addr[0] != '\0';
    memset(arq_conn.src_addr, 0, sizeof(arq_conn.src_addr));
    memset(arq_conn.dst_addr, 0, sizeof(arq_conn.dst_addr));
    arq_conn.session_bw = 0;
    arq_conn.TRX = RX;
    bool relisten = (arq_conn.listen && arq_conn.my_call_sign[0] != '\0');
    pthread_mutex_unlock(&g_conn_lock);   /* release BEFORE g_app_tx_mtx: leaf discipline */
    /* Flush stale TX bytes from the previous session.  RX bytes are flushed
     * at connection start (cb_notify_connected) instead of here, so that the
     * last delivered bytes have time to drain to the TCP socket before the
     * next session clears the buffer. */
    pthread_mutex_lock(&g_app_tx_mtx);
    clear_buffer(g_app_tx_buf);
    pthread_mutex_unlock(&g_app_tx_mtx);
    /* Discard any partial chat line so the next session's first message is not
     * glued onto trailing bytes from this one. */
    msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_RX);
    msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_TX);
    arq_tnc_send_disconnected();
    HLOGI(LOG_COMP, "Disconnected");
    ARQ_TRACE_DUMP("disconnected");
    /* Return to LISTENING after any disconnection (failed call, cancelled call,
     * or ended session) as long as listen mode is active.  The was_connected
     * guard was thought to prevent spurious APP_LISTEN from APP_DISCONNECT-in-
     * DISCONNECTED, but fsm_disconnected has no APP_DISCONNECT handler, so
     * notify_disconnected is never called from that path. */
    (void)was_connected;
    if (relisten)
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

    char my_call[CALLSIGN_MAX_SIZE], src[CALLSIGN_MAX_SIZE], dst[CALLSIGN_MAX_SIZE];
    arq_conn_get_calls(my_call, src, dst, CALLSIGN_MAX_SIZE);
    const char *peer = (dst[0] && strcmp(dst, my_call) != 0) ? dst : src;
    msg_store_feed(MSG_PLANE_ARQ, MSG_DIR_RX, peer, data, len);
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
    arq_tnc_send_buffer((uint32_t)(backlog_bytes < 0 ? 0 : backlog_bytes));
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
    pthread_mutex_lock(&g_conn_lock);
    int session_bw = arq_conn.session_bw;
    int bw         = arq_conn.bw;
    pthread_mutex_unlock(&g_conn_lock);
    if (session_bw != 0)
        return normalize_bandwidth_hz(session_bw);
    return normalize_bandwidth_hz(bw);
}

int arq_reported_bandwidth_hz(void)
{
    return active_session_bandwidth_hz();
}

void arq_set_trx(int trx)
{
    pthread_mutex_lock(&g_conn_lock);
    arq_conn.TRX = trx;
    pthread_mutex_unlock(&g_conn_lock);
}

int arq_get_trx(void)
{
    pthread_mutex_lock(&g_conn_lock);
    int t = arq_conn.TRX;
    pthread_mutex_unlock(&g_conn_lock);
    return t;
}

void arq_conn_get_calls(char *my_call, char *src_addr, char *dst_addr, size_t bufsz)
{
    if (bufsz == 0) return;
    pthread_mutex_lock(&g_conn_lock);
    if (my_call)  { snprintf(my_call,  bufsz, "%s", arq_conn.my_call_sign); }
    if (src_addr) { snprintf(src_addr, bufsz, "%s", arq_conn.src_addr); }
    if (dst_addr) { snprintf(dst_addr, bufsz, "%s", arq_conn.dst_addr); }
    pthread_mutex_unlock(&g_conn_lock);
}

void arq_set_callsign(const char *callsign)
{
    pthread_mutex_lock(&g_conn_lock);
    snprintf(arq_conn.my_call_sign, CALLSIGN_MAX_SIZE, "%s",
             callsign ? callsign : "");
    /* Setting a new primary callsign clears all secondary callsigns. */
    memset(arq_conn.secondary_calls, 0, sizeof(arq_conn.secondary_calls));
    arq_conn.secondary_call_count = 0;
    pthread_mutex_unlock(&g_conn_lock);
    /* Log the argument, not the shared field: the field may have changed by
     * the time the lock is released. */
    HLOGI(LOG_COMP, "My callsign: %s", callsign ? callsign : "");
}

int arq_get_bw(void)
{
    pthread_mutex_lock(&g_conn_lock);
    int bw = arq_conn.bw;
    pthread_mutex_unlock(&g_conn_lock);
    return bw;
}

/* ======================================================================
 * CMD bridge worker
 * ====================================================================== */

/* Payload messages put on the bus, and those the payload bridge has written
 * to g_app_tx_buf.  The two bridges run apart, so a DISCONNECT right behind
 * the application's last bytes could reach the FSM before them: nothing
 * queued, the session ended at once and the bytes were dropped (on air,
 * 2026-10-18: a reply, then DISCONNECT, the same millisecond). */
static atomic_uint g_payload_sent, g_payload_written;

/* Until every payload put on the bus before now is in g_app_tx_buf.  Bounded:
 * a full buffer holds the bridge, and then the FSM sees bytes queued anyway. */
static void wait_payload_written(void)
{
    unsigned target = atomic_load(&g_payload_sent);
    for (int i = 0; i < 1000 && (int)(atomic_load(&g_payload_written) - target) < 0; i++)
        hermes_usleep(1000);
}

static void handle_cmd(const arq_cmd_msg_t *msg)
{
    arq_event_t ev = {0};

    switch (msg->type)
    {
    case ARQ_CMD_SET_CALLSIGN:
        arq_set_callsign(msg->arg0);
        /* REGISTERED for this path is sent by the MYCALL handler itself, so it
         * cannot overtake the OK that answers the command -- see
         * data_interfaces/tcp_interfaces.c.  Reconnect still notifies below. */
        return;

    case ARQ_CMD_ADD_SECONDARY_CALLSIGN:
    {
        int new_count = -1;
        pthread_mutex_lock(&g_conn_lock);
        if (arq_conn.secondary_call_count < CALLSIGN_MAX_SECONDARY)
        {
            snprintf(arq_conn.secondary_calls[arq_conn.secondary_call_count],
                     CALLSIGN_MAX_SIZE, "%s", msg->arg0);
            arq_conn.secondary_call_count++;
            new_count = arq_conn.secondary_call_count;
        }
        pthread_mutex_unlock(&g_conn_lock);
        if (new_count >= 0)
            HLOGI(LOG_COMP, "Secondary callsign added: %s (total=%d)", msg->arg0, new_count);
        else
            HLOGW(LOG_COMP, "Secondary callsign list full (max=%d), ignoring %s",
                  CALLSIGN_MAX_SECONDARY, msg->arg0);
        return;
    }

    case ARQ_CMD_CLEAR_SECONDARY_CALLSIGNS:
        pthread_mutex_lock(&g_conn_lock);
        memset(arq_conn.secondary_calls, 0, sizeof(arq_conn.secondary_calls));
        arq_conn.secondary_call_count = 0;
        pthread_mutex_unlock(&g_conn_lock);
        HLOGI(LOG_COMP, "Secondary callsigns cleared");
        return;

    case ARQ_CMD_SET_BANDWIDTH:
        pthread_mutex_lock(&g_conn_lock);
        arq_conn.bw = normalize_bandwidth_hz(msg->value);
        pthread_mutex_unlock(&g_conn_lock);
        return;

    case ARQ_CMD_SET_RETRY:
        pthread_mutex_lock(&g_conn_lock);
        arq_conn.retry_slots = msg->value;
        pthread_mutex_unlock(&g_conn_lock);
        arq_set_retry_slots(msg->value);   /* outside lock: takes g_sess_lock internally */
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
        pthread_mutex_lock(&g_conn_lock);
        arq_conn.listen = true;
        pthread_mutex_unlock(&g_conn_lock);
        ev.id = ARQ_EV_APP_LISTEN;
        break;

    case ARQ_CMD_LISTEN_OFF:
        pthread_mutex_lock(&g_conn_lock);
        arq_conn.listen = false;
        pthread_mutex_unlock(&g_conn_lock);
        ev.id = ARQ_EV_APP_STOP_LISTEN;
        break;

    case ARQ_CMD_CONNECT:
    {
        pthread_mutex_lock(&g_conn_lock);
        snprintf(arq_conn.src_addr, CALLSIGN_MAX_SIZE, "%s", msg->arg0);
        snprintf(arq_conn.dst_addr, CALLSIGN_MAX_SIZE, "%s", msg->arg1);
        arq_conn.session_bw = 0;
        pthread_mutex_unlock(&g_conn_lock);
        /* The FSM defers a CONNECT that arrives while the previous session is
         * ending, and then skips that session's teardown callback -- which is
         * what would normally flush the TX queue.  So flush the leftovers here,
         * before anything for the new call can be queued, so no stale byte
         * reaches a different station.
         *
         * Only in DISCONNECTING.  A redial can also arrive while the session is
         * still CONNECTED with its DISCONNECT deferred; there the old session
         * is still delivering its last bytes (that is why the disconnect was
         * deferred), and flushing would cut them off.  In that case nothing
         * flushes at all: normally the drain empties the queue before teardown,
         * but if the drain deadline forces the teardown first, bytes it did not
         * send would go out on the new call.  Accepted as a narrow edge: it
         * needs a DISCONNECT with data still unsent, a redial before the ACK,
         * and a drain that times out.
         *
         * Locks are taken one at a time (never nested), keeping the leaf
         * ordering described at g_conn_lock. */
        pthread_mutex_lock(&g_sess_lock);
        bool tearing_down = (g_sess.conn_state == ARQ_CONN_DISCONNECTING);
        pthread_mutex_unlock(&g_sess_lock);
        if (tearing_down)
        {
            pthread_mutex_lock(&g_app_tx_mtx);
            clear_buffer(g_app_tx_buf);
            pthread_mutex_unlock(&g_app_tx_mtx);
            HLOGI(LOG_COMP, "CONNECT during teardown: flushed previous session's TX queue");
        }

        snprintf(ev.remote_call, CALLSIGN_MAX_SIZE, "%s", msg->arg1);
        ev.id = ARQ_EV_APP_CONNECT;
        break;
    }

    case ARQ_CMD_SEND_CQ:
    {
        uint8_t frame[INT_BUFFER_SIZE];
        char cq_my_call[CALLSIGN_MAX_SIZE];
        pthread_mutex_lock(&g_conn_lock);
        snprintf(cq_my_call, sizeof(cq_my_call), "%s", arq_conn.my_call_sign);
        pthread_mutex_unlock(&g_conn_lock);
        const char *source_call = msg->arg0[0] ? msg->arg0 : cq_my_call;
        int bw_hz = normalize_bandwidth_hz(msg->value);
        int n = arq_protocol_build_cq(frame, sizeof(frame), source_call, bw_hz);
        if (n <= 0)
        {
            HLOGW(LOG_COMP, "Failed to build CQ frame (source=%s bw=%d)",
                  source_call, bw_hz);
            return;
        }

        pthread_mutex_lock(&g_sess_lock);
        /* On the floor from a station that transmits only MFSK: a
         * listener routes it by its packet type off any decoder. */
        int cq_mode = ARQ_TX_MFSK_ONLY ? arq_floor_mode(bw_hz) : g_sess.control_mode;
        /* Hold g_sess_lock across the call so that g_sess.pending_burst_frames
         * is protected — the event loop also holds this lock during dispatch. */
        cb_send_tx_frame(PACKET_TYPE_ARQ_CQ, cq_mode, (size_t)n, frame, 0);
        pthread_mutex_unlock(&g_sess_lock);
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
        arq_tnc_send_disconnected();
        wait_payload_written();
        ev.id = ARQ_EV_APP_DISCONNECT;
        break;

    case ARQ_CMD_ABORT:
        /* Dirty disconnect: drop everything still held -- the host's unsent
         * bytes here, the frames awaiting an ACK and the re-staged ones in the
         * FSM (ev.abort) -- and go at once, without the drain DISCONNECT waits
         * for.  The peer is still told: the FSM sends its DISCONNECT frames,
         * after any frame on the air ends, so it need not time out (#218: the
         * comment and docs/TNC.md said no frame went out; one always did). */
        clear_connection_data();
        arq_tnc_send_disconnected();
        ev.id = ARQ_EV_APP_DISCONNECT;
        ev.abort = true;
        break;

    case ARQ_CMD_CLIENT_DISCONNECT:
        wait_payload_written();
        ev.id = ARQ_EV_APP_DISCONNECT;
        break;

    case ARQ_CMD_CLIENT_CONNECT:
    {
        char call_copy[CALLSIGN_MAX_SIZE];
        bool has_callsign;
        HLOGD(LOG_COMP, "Client (re)connected");
        pthread_mutex_lock(&g_conn_lock);
        has_callsign = (arq_conn.my_call_sign[0] != '\0');
        if (has_callsign)
            snprintf(call_copy, sizeof(call_copy), "%s", arq_conn.my_call_sign);
        pthread_mutex_unlock(&g_conn_lock);
        if (has_callsign)
            arq_tnc_send_registered(call_copy);
        return;
    }

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
        {
            atomic_fetch_add(&g_payload_written, 1);
            continue;
        }
        write_buffer(g_app_tx_buf, payload.data, payload.len);
        atomic_fetch_add(&g_payload_written, 1);

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
        uint64_t now   = time_now_ms();
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
        now = time_now_ms();
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

/* A frame that passed the session's seeded CRC: the carousel data plane's. */
void arq_handle_carousel_frame(const uint8_t *data, size_t frame_size, int mode,
                               bool from_control, float rx_snr, uint32_t age_ms)
{
    if (!data || frame_size == 0)
        return;
    arq_event_t ev = {0};
    ev.id           = ARQ_EV_RX_CAROUSEL;
    ev.mode         = mode;
    ev.from_control = from_control;
    ev.rx_snr       = rx_snr;
    ev.rx_age_ms    = age_ms;
    ev.payload_len  = frame_size < sizeof(ev.payload) ? frame_size : sizeof(ev.payload);
    memcpy(ev.payload, data, ev.payload_len);
    ARQ_TRACE(ARQ_TR_RX_FRAME, 0xCA, (uint8_t)(from_control ? 1 : 0), (uint16_t)frame_size);
    evq_push(&ev);
}

/* Dump a connect frame's raw bytes at TRACE level.  A CALL/ACCEPT control
 * frame is ARQ_CONTROL_FRAME_SIZE bytes (the two CRC bytes have already been
 * stripped by the caller), so a fixed 3-chars-per-byte buffer is plenty. */
static void log_connect_frame_hex(const char *kind, const uint8_t *data, size_t len)
{
    char buf[3 * ARQ_CONTROL_FRAME_SIZE + 1];
    size_t off = 0;

    if (len > ARQ_CONTROL_FRAME_SIZE)
        len = ARQ_CONTROL_FRAME_SIZE;
    for (size_t i = 0; i < len; i++)
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                                "%s%02x", i ? " " : "", data[i]);
    HLOGTR(LOG_COMP, "%s frame bytes: %s", kind, buf);
}

bool arq_handle_incoming_connect_frame(uint8_t *data, size_t frame_size, float rx_snr, int mode)
{
    if (!data || frame_size < 2) return false;

    bool is_accept = (data[ARQ_CONNECT_SESSION_IDX] & ARQ_CONNECT_ACCEPT_FLAG) != 0;
    uint8_t session_id = 0;
    char caller[CALLSIGN_MAX_SIZE] = {0};
    arq_accept_t acc;
    int bw_hz = 0;
    bool tx_mfsk = false;
    int rc;

    if (!arq_protocol_connect_version_ok(data, frame_size))
        rc = -2;   /* another protocol version: below, once we know it is for us */
    else if (is_accept)
    {
        rc = arq_protocol_parse_accept(data, frame_size, &acc);
        session_id = acc.session_id;
        bw_hz = acc.bw_hz;
        tx_mfsk = acc.tx_mfsk;
    }
    else
        rc = arq_protocol_parse_call(data, frame_size, &session_id, caller, &bw_hz, &tx_mfsk);

    /* CALL/ACCEPT do not pass through arq_handle_incoming_frame, so without
     * this the connect exchange is invisible to the trace -- which is exactly
     * the exchange whose reliability decides whether a link comes up. */
    ARQ_TRACE(ARQ_TR_RX_FRAME, is_accept ? 2 : 1, (uint8_t)(rc < 0 ? 0xff : 0),
              (uint16_t)frame_size);
    log_connect_frame_hex(is_accept ? "ACCEPT" : "CALL", data, frame_size);

    if (rc == -1)
    {
        HLOGD(LOG_COMP, "CALL/ACCEPT parse failed");
        return false;
    }

    /* Copy callsigns out under g_conn_lock, compare CRCs unlocked */
    char my_call[CALLSIGN_MAX_SIZE];
    char sec[CALLSIGN_MAX_SECONDARY][CALLSIGN_MAX_SIZE];
    int  sec_count;
    pthread_mutex_lock(&g_conn_lock);
    snprintf(my_call, sizeof(my_call), "%s", arq_conn.my_call_sign);
    sec_count = arq_conn.secondary_call_count;
    memcpy(sec, arq_conn.secondary_calls, sizeof(sec));
    pthread_mutex_unlock(&g_conn_lock);

    /* Validate that DST CRC16 matches our own callsign or a secondary.
     * Record WHICH of our callsigns matched (the SSID the caller dialed) so the
     * callee reports it as the local address — a station listening on several
     * SSIDs must show the dialed one, not just the primary. */
    const char *dialed_call = my_call;
    if (my_call[0] != 0)
    {
        bool match = arq_protocol_connect_dst_matches(data, frame_size, my_call);
        for (int i = 0; !match && i < sec_count; i++)
        {
            if (arq_protocol_connect_dst_matches(data, frame_size, sec[i]))
            {
                match       = true;
                dialed_call = sec[i];
            }
        }
        if (!match)
        {
            /* Bytes 2-3 are the recipient's CRC in every version (the called
             * station's on a CALL, the caller's on an ACCEPT), so the intended
             * recipient is always identifiable (unless the frame is too short
             * to hold them).  The sender is only ever a full callsign on a
             * CALL; an ACCEPT carries both ends as CRCs. */
            char dst[16];
            if (frame_size >= ARQ_CONNECT_PAYLOAD_IDX + 2)
                snprintf(dst, sizeof(dst), "crc16:0x%04X",
                         (unsigned)(data[ARQ_CONNECT_PAYLOAD_IDX] |
                                    (data[ARQ_CONNECT_PAYLOAD_IDX + 1] << 8)));
            else
                snprintf(dst, sizeof(dst), "?");
            const char *kind = is_accept ? "ACCEPT" : "CALL";
            if (rc == 0 && is_accept)
                HLOGD(LOG_COMP, "ACCEPT not for us: from=crc16:0x%04X to=%s",
                      acc.called_crc, dst);
            else if (rc == 0)
                HLOGD(LOG_COMP, "CALL not for us: from=%s to=%s",
                      caller[0] ? caller : "?", dst);
            else
                HLOGD(LOG_COMP, "%s not for us: from=? to=%s (not our protocol version)",
                      kind, dst);
            return false;
        }
    }
    if (rc == -2)
    {
        /* Bytes 2-3 are the called CRC in every version, so this is a call
         * for us from an older release, 1.9.19, 1.9.18 or 1.9.x: the
         * versions do not connect (docs/CAROUSEL-ARQ.md, "Wire formats"). */
        if (!is_accept)
            HLOGW(LOG_COMP, "CALL for %s from another protocol version (Mercury 1.9.x?): not answered",
                  dialed_call);
        return false;
    }
    if (ARQ_TX_MFSK_ONLY && arq_protocol_connect_minor(data) < 1)
    {
        /* Minor 0, 1.9.20, does not read the TX-MFSK-only bit: it would put
         * my rounds on rungs I cannot send. */
        HLOGW(LOG_COMP, "%s for %s from Mercury 1.9.20, which cannot reach a TX-MFSK-only station: ignored",
              is_accept ? "ACCEPT" : "CALL", dialed_call);
        return false;
    }

    arq_event_t ev = {0};
    ev.id         = is_accept ? ARQ_EV_RX_ACCEPT : ARQ_EV_RX_CALL;
    ev.session_id = session_id;
    ev.rx_snr     = rx_snr;
    ev.mode       = mode;          /* the carrier: an answer goes back on it */
    ev.tx_mfsk    = tx_mfsk;       /* the peer can only transmit MFSK */
    if (is_accept)
    {
        /* The callee's callsign is not on the air, only its CRC: the FSM
         * checks it against the station it called. */
        ev.car_level      = acc.start_level;
        ev.car_ctl_deaf   = acc.ctl_deaf;
        ev.car_call_snr   = isnan(acc.call_snr_db) ? 0.0f : acc.call_snr_db;
        ev.car_nonce      = acc.nonce;
        ev.car_called_crc = acc.called_crc;
    }
    else
        snprintf(ev.remote_call, CALLSIGN_MAX_SIZE, "%s", caller);
    /* local = the one of our callsigns the caller dialed (primary or secondary) */
    snprintf(ev.local_call, CALLSIGN_MAX_SIZE, "%s", dialed_call);
    /* The bandwidth the frame advertises: the FSM makes it the session's when
     * it takes the frame (arq_set_session_bw), not here -- another station's
     * CALL, or a stray ACCEPT, would change the bandwidth of the session being
     * answered, and its ends then capped their rungs and chose their floor
     * apart. */
    ev.bw_hz = bw_hz;
    evq_push(&ev);
    return true;
}

void arq_set_session_bw(int bw_hz, bool from_call)
{
    pthread_mutex_lock(&g_conn_lock);
    int local_bw = normalize_bandwidth_hz(arq_conn.bw);
    /* the callee answers with min(its own, the CALL's); the caller takes it */
    arq_conn.session_bw = normalize_bandwidth_hz(from_call && local_bw < bw_hz ? local_bw : bw_hz);
    pthread_mutex_unlock(&g_conn_lock);
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

    arq_tnc_send_cqframe(source_call, normalize_bandwidth_hz(bw_hz));
    HLOGI(LOG_COMP, "CQ frame decoded from %s (%d Hz)", source_call, bw_hz);
    return true;
}

void arq_notify_cq_tx_started(void)
{
    arq_tnc_send_pending();
    HLOGI(LOG_COMP, "CQ transmission started");
}

void arq_notify_cq_tx_complete(void)
{
    arq_tnc_send_cancelpending();
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

    ARQ_TRACE(ARQ_TR_RX_FRAME, hdr.packet_type, hdr.subtype, (uint16_t)frame_size);

    /* The one plain session frame there is: the DISCONNECT.  Everything a
     * session moves goes on the carousel's seeded frames (ARQ_EV_RX_CAROUSEL);
     * 1.9.x's DATA, ACK, MODE, TURN and KEEPALIVE frames are not read. */
    if (hdr.packet_type != PACKET_TYPE_ARQ_CONTROL || hdr.subtype != ARQ_SUBTYPE_DISCONNECT)
        return;
    arq_event_t ev = {0};
    ev.id         = ARQ_EV_RX_DISCONNECT;
    ev.session_id = hdr.session_id;

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

    /* single-threaded: before worker threads start -- no lock needed */
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
        /* Energy-based channel occupancy, so the FSM can avoid keying over a
         * transmission it cannot decode.  modem_channel_busy() returns false
         * when the detector is disabled (the default), which degrades this to
         * the decoder-sync signal alone rather than breaking anything. */
        .channel_busy        = arq_modem_channel_busy,
        .send_keydown        = cb_send_keydown,
        .send_pattern        = cb_send_pattern,
        .set_crc_seed        = arq_modem_crc_seed,
        .set_floor_mode      = arq_modem_floor_mode,
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
        arq_fsm_release(&g_sess);
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
        arq_fsm_release(&g_sess);
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

void arq_post_pattern_ack(bool is_break)
{
    arq_event_t ev = {0};
    ev.id       = ARQ_EV_RX_PATTERN;
    ev.pattern_break = is_break;
    evq_push(&ev);
}

bool arq_session_active(void)
{
    pthread_mutex_lock(&g_sess_lock);
    int st = g_sess.conn_state;
    pthread_mutex_unlock(&g_sess_lock);
    return st == ARQ_CONN_CALLING || st == ARQ_CONN_ACCEPTING ||
           st == ARQ_CONN_CONNECTED || st == ARQ_CONN_DISCONNECTING;
}

bool arq_is_link_connected(void)
{
    pthread_mutex_lock(&g_sess_lock);
    bool connected = (g_sess.conn_state == ARQ_CONN_CONNECTED);
    pthread_mutex_unlock(&g_sess_lock);
    return connected;
}

void arq_discard_stale_rx(void)
{
    /* Every FSM dispatch -- so every CONNECTED transition and every delivery
     * into the buffer -- runs under g_sess_lock.  Holding it across the check
     * and the clear means a session cannot start in between and have its
     * first bytes wiped. */
    pthread_mutex_lock(&g_sess_lock);
    if (g_sess.conn_state != ARQ_CONN_CONNECTED)
        clear_buffer(data_rx_buffer_arq);
    pthread_mutex_unlock(&g_sess_lock);
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

/* Operator-selected idle ("listen") payload mode.  initial_payload_mode is
 * what sess_enter() restores peer_tx_mode to whenever the session falls back
 * to DISCONNECTED/LISTENING, so it is the single field that decides which
 * payload mode the RX decoder sits on while no ARQ session is up (and hence
 * which broadcast frames we can hear).
 *
 * Refused unless the link is actually idle: while a session is up the
 * carousel owns peer_tx_mode and would overwrite an operator setting within one
 * RX-loop iteration, so accepting the command there would silently do nothing.
 * The state test is inside the lock deliberately -- checking it in the caller
 * would race the event loop, which is the same unsynchronized-g_sess class of
 * bug as the on-air handshake freeze.
 *
 * A session that starts later is unaffected: every session-start path
 * (APP_CONNECT, RX_ACCEPT, and the ACCEPTING data/ack paths) hard-resets
 * payload_mode and peer_tx_mode to DATAC15, so the listen mode cannot leak
 * into a connection's mode ladder. */
int arq_set_listen_mode(int mode)
{
    int rc = -1;
    pthread_mutex_lock(&g_sess_lock);
    if (g_sess.conn_state == ARQ_CONN_DISCONNECTED ||
        g_sess.conn_state == ARQ_CONN_LISTENING)
    {
        g_sess.initial_payload_mode = mode;
        /* Apply now rather than waiting for the next sess_enter(): we are
         * already in an idle state, so no further transition is guaranteed. */
        g_sess.peer_tx_mode = mode;
        rc = 0;
    }
    pthread_mutex_unlock(&g_sess_lock);
    return rc;
}

void arq_set_active_modem_mode(int mode, size_t frame_size)
{
    /* Only record payload_mode for data modes: a control-mode TX switch
     * (DATAC16) must not overwrite it. */
    pthread_mutex_lock(&g_sess_lock);
    if (mode != g_sess.control_mode)
        g_sess.payload_mode = mode;
    pthread_mutex_unlock(&g_sess_lock);
    pthread_mutex_lock(&g_conn_lock);
    arq_conn.mode       = mode;
    arq_conn.frame_size = frame_size;
    pthread_mutex_unlock(&g_conn_lock);
}

/* Carrier sense is a decoder holding sync, and a decoder can lose it inside a
 * frame that is still on the air: on air (car28) the gateway caught a 7.4 s
 * DATAC17 frame, lost it 1.2 s in, and its receiver polled 2.7 s into the
 * frame.  A caught preamble starts a frame of known length; the channel is
 * busy for that long.  When a frame decodes it is over. */
void arq_note_rx_preamble(int mode)
{
    const arq_mode_timing_t *tm = arq_protocol_mode_timing(mode);
    if (!tm || tm->frame_duration_s <= 0.0f)
        return;
    uint64_t until = time_now_ms() + (uint64_t)(tm->frame_duration_s * 1000.0f);
    pthread_mutex_lock(&g_sess_lock);
    if (until > g_sess.rx_frame_busy_until_ms)
        g_sess.rx_frame_busy_until_ms = until;
    pthread_mutex_unlock(&g_sess_lock);
}

void arq_note_rx_nav(uint32_t age_ms, uint32_t hold_ms)
{
    if (age_ms >= hold_ms)
        return;
    uint64_t until = time_now_ms() + (uint64_t)(hold_ms - age_ms);
    pthread_mutex_lock(&g_sess_lock);
    if (g_sess.car_active && until > g_sess.nav_busy_until_ms)
        g_sess.nav_busy_until_ms = until;
    pthread_mutex_unlock(&g_sess_lock);
}

void arq_note_rx_frame_done(void)
{
    pthread_mutex_lock(&g_sess_lock);
    g_sess.rx_frame_busy_until_ms = 0;
    pthread_mutex_unlock(&g_sess_lock);
}

void arq_update_link_metrics(int sync, float snr, int rx_status, bool frame_decoded)
{
    (void)rx_status;
    /* A decoder holding sync means a burst is arriving, i.e. the peer is
     * transmitting.  Record when we last saw that so the FSM can avoid keying
     * on top of it (see ARQ_CHANNEL_SYNC_HOLD_MS).  This argument used to be
     * discarded, which is why nothing in the FSM could tell a busy channel
     * from an idle one. */
    if (sync)
    {
        pthread_mutex_lock(&g_sess_lock);
        g_sess.last_rx_sync_ms = time_now_ms();
        pthread_mutex_unlock(&g_sess_lock);
    }
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
    int trx     = arq_get_trx();     /* takes g_conn_lock (leaf); before g_sess_lock */
    int own_bw  = arq_get_bw();      /* likewise */
    pthread_mutex_lock(&g_sess_lock);
    snapshot->initialized      = true;
    snapshot->connected        = (g_sess.conn_state == ARQ_CONN_CONNECTED);
    snapshot->expect_pattern_ack = arq_fsm_expect_pattern(&g_sess, time_now_ms());
    snapshot->in_carousel = g_sess.car_active;
    /* ACCEPTING too: a caller that hears none of our ACCEPTs -- a strong
     * forward path, a weak return one -- escalates to MFSK CALLs, and those
     * are what we must answer (on MFSK, #235).  With the listener off while
     * ACCEPTING, the callee only ever heard the DATAC16 CALLs it could not
     * usefully answer: on air (car17, st2 calling the gateway at 2 %), both
     * MFSK CALLs arrived mid-ACCEPTING, and the call never connected. */
    snapshot->listening_for_calls = (g_sess.conn_state == ARQ_CONN_LISTENING ||
                                     g_sess.conn_state == ARQ_CONN_ACCEPTING);
    snapshot->listen_floor_mode = arq_floor_mode(normalize_bandwidth_hz(own_bw));
    snapshot->trx              = trx;
    snapshot->tx_backlog_bytes = backlog + g_sess.tx_inflight_bytes;
    snapshot->payload_mode      = g_sess.payload_mode;
    snapshot->peer_tx_mode      = g_sess.peer_tx_mode;
    snapshot->control_mode      = g_sess.control_mode;
    snapshot->preferred_rx_mode = arq_modem_preferred_rx_mode(&g_sess);
    snapshot->preferred_tx_mode = arq_modem_preferred_tx_mode(&g_sess);
    snapshot->tx_bytes          = g_timing.tx_bytes;
    snapshot->rx_bytes          = g_timing.rx_bytes;
    bool car_up = g_sess.car_active && g_sess.car;
    snapshot->car_tx_mode   = car_up ? car_ui_tx_mode(g_sess.car) : -1;
    snapshot->car_rx_mode   = car_up ? car_ui_rx_mode(g_sess.car) : -1;
    snapshot->car_peer_mode = car_up ? car_ui_peer_mode(g_sess.car) : -1;
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

    /* Persist only bytes that were actually accepted for transmission: the bus
     * rejects on EAGAIN (full) and EINVAL (oversized), and a message that never
     * went out must not be replayed as if it had. */
    int rc = arq_channel_bus_try_send_payload(&g_bus, data, len);
    if (rc == 0)
    {
        atomic_fetch_add(&g_payload_sent, 1);
        char my_call[CALLSIGN_MAX_SIZE], src[CALLSIGN_MAX_SIZE], dst[CALLSIGN_MAX_SIZE];
        arq_conn_get_calls(my_call, src, dst, CALLSIGN_MAX_SIZE);
        const char *peer = (dst[0] && strcmp(dst, my_call) != 0) ? dst : src;
        msg_store_feed(MSG_PLANE_ARQ, MSG_DIR_TX, peer, data, len);
    }

    return rc;
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
        atomic_store(&arq_disconnect_retry_slots, ARQ_DISCONNECT_RETRY_SLOTS_DEFAULT);
    }
    else
    {
        atomic_store(&arq_call_retry_slots,   slots);
        atomic_store(&arq_accept_retry_slots, slots);
        /* Leave disconnect retries at default — no benefit to long teardown */
        atomic_store(&arq_disconnect_retry_slots, ARQ_DISCONNECT_RETRY_SLOTS_DEFAULT);
    }
    HLOGI(LOG_COMP, "Retry slots: call=%d accept=%d disconnect=%d",
          atomic_load(&arq_call_retry_slots), atomic_load(&arq_accept_retry_slots),
          atomic_load(&arq_disconnect_retry_slots));
}

void arq_set_retry_stagger_ms(int ms)
{
    /* 0 is a valid (and useful) value: it disables the retry stagger entirely. */
    if (ms < 0) ms = 0;
    if (ms > ARQ_RETRY_STAGGER_MS_MAX) ms = ARQ_RETRY_STAGGER_MS_MAX;
    atomic_store(&arq_retry_stagger_ms, ms);
    HLOGI(LOG_COMP, "retry_stagger_ms = %d", atomic_load(&arq_retry_stagger_ms));
}

void arq_set_tx_mfsk_only(bool on)
{
    atomic_store(&arq_tx_mfsk_only, on ? 1 : 0);
    HLOGI(LOG_COMP, "MFSK-only transmit %s", on ? "enabled" : "disabled");
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
