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
#include "../modem/modem_mfsk.h"   /* MERCURY_MODE_MFSK, _N32 */

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

/* The source of the host's last CONNECT, under g_conn_lock.  Kept apart from
 * arq_conn.src_addr, which an incoming CALL resets (cb_notify_pending), so the
 * session that CONNECT places still reports the host's source. */
static char g_connect_src[CALLSIGN_MAX_SIZE];

/* The host already has its DISCONNECTED for the session now ending: ABORT
 * answers at once, then the FSM still runs the teardown, and its end must not
 * say it again (the line carries no session id, so a host that has moved on
 * reads a late one as "my current session closed").  Under g_conn_lock; set
 * by ABORT with a session up, consumed by cb_notify_disconnected, cleared
 * when a new session starts (CONNECT, an incoming CALL). */
static bool g_host_told;

static bool notice_owed(void);

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

/* Received bytes this session dropped: the ring was full with nothing
 * reading it.  Once non-zero, nothing more of the session is delivered (the
 * stream would have a hole in it), and the session is ended with the host's
 * ABORT (g_rx_abort_sent once the command bus took it).  Both reset when a
 * session starts or ends.  Event-loop thread only. */
static size_t g_rx_dropped;
static bool   g_rx_abort_sent;

static void rx_dropped_summary(const char *when)
{
    if (g_rx_dropped)
        HLOGW(LOG_COMP, "%s: %zu received bytes were dropped, no application reading them",
              when, g_rx_dropped);
    g_rx_dropped = 0;
    g_rx_abort_sent = false;
}

/* The session can't deliver what it receives any more: end it as the host's
 * ABORT does, so the peer hears a DISCONNECT and its application reports the
 * transfer failed, instead of its bytes being acknowledged and thrown away
 * until it hangs up.  Non-blocking: the command thread takes the session
 * lock this thread holds.  If the command bus is full, rx_abort_tick retries
 * on every pass of the event loop (at least every 500 ms), not only on the
 * next delivery: a peer that stops sending would make none. */
static void rx_end_session_for_no_reader(void)
{
    if (g_rx_abort_sent)
        return;
    arq_cmd_msg_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = ARQ_CMD_ABORT;
    if (arq_channel_bus_try_send_cmd(&g_bus, &cmd) == 0)
    {
        g_rx_abort_sent = true;
        HLOGW(LOG_COMP, "Ending the session (ABORT): it can't deliver what it receives");
    }
    else
        HLOGD(LOG_COMP, "Command bus full: the session's ABORT will be retried");
}

/* Event loop, under g_sess_lock: an ABORT owed for a drop and not yet on the
 * bus.  g_rx_dropped resets when the session ends, so one owed is always for
 * the session still up. */
static void rx_abort_tick(void)
{
    if (g_rx_dropped && !g_rx_abort_sent)
        rx_end_session_for_no_reader();
}

static void cb_notify_connected(const char *remote_call, const char *local_call,
                                bool we_called)
{
    pthread_mutex_lock(&g_conn_lock);
    /* The side comes from the FSM.  It used to be inferred from src_addr (set
     * meant "we called"), but an incoming CALL handled just before a queued
     * CONNECT clears src_addr (cb_notify_pending), and the outgoing session
     * that CONNECT then placed was reported backwards. */
    arq_protocol_session_calls(we_called, remote_call, local_call,
                               g_connect_src, arq_conn.my_call_sign,
                               arq_conn.src_addr, arq_conn.dst_addr,
                               CALLSIGN_MAX_SIZE);
    arq_conn.TRX = RX;
    pthread_mutex_unlock(&g_conn_lock);
    /* Flush any stale RX bytes from the previous session before notifying
     * UUCP.  Moved here from cb_notify_disconnected so that the last bytes
     * of the previous session have time to drain to the TCP socket before
     * the buffer is cleared (clearing on disconnect races with UUCP reads). */
    clear_buffer(data_rx_buffer_arq);
    rx_dropped_summary("Previous session");
    /* A new session starts with a clean slate: drop any partial chat line left
     * over from the previous session's feed buffer. */
    msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_RX);
    msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_TX);
    arq_tnc_send_connected();   /* dispatches to tnc_send_connected, which takes g_conn_lock via arq_conn_get_calls; must be outside our lock */
    HLOGI(LOG_COMP, "Connected to %s", remote_call);
}

/* When PENDING last went to the host for the call we are answering; 0: none.
 * Written by the FSM's callbacks and by pending_hold, all on the event loop
 * under g_sess_lock. */
static uint64_t g_pending_ms;

static void abort_hold_release(uint64_t now);

static void cb_notify_pending(const char *remote_call, const char *local_call)
{
    /* An ABORT hold still on is an earlier session's: its CANCELPENDING goes
     * first, or it would cancel this PENDING (abort_hold_release). */
    abort_hold_release(time_now_ms());
    /* An incoming call makes us the callee, whatever an earlier session left
     * behind.  cb_notify_connected takes a set src_addr to mean "we called",
     * so addresses left over from an outgoing session that ended without
     * cb_notify_disconnected reported the next INCOMING session to the host
     * backwards: CONNECTED <our call> <the caller>, which BPQ does not route
     * to the BBS.  On air, KG9NI 1.9.22: CONNECT KG9NI-1 K7EK ended by LISTEN
     * OFF in DISCONNECTING (01:24), then K7EK called in (03:17), and the
     * session sat silent for 2 min until K7EK hung up.
     *
     * cb_notify_connected no longer reads these to tell the side, so the reset
     * only keeps the UI from showing the old session's calls.  It leaves
     * g_connect_src alone: a CONNECT may already be queued behind this CALL. */
    pthread_mutex_lock(&g_conn_lock);
    memset(arq_conn.src_addr, 0, sizeof(arq_conn.src_addr));
    memset(arq_conn.dst_addr, 0, sizeof(arq_conn.dst_addr));
    g_host_told = false;   /* a new session: its end is owed a notice */
    pthread_mutex_unlock(&g_conn_lock);
    arq_tnc_send_pending();
    g_pending_ms = time_now_ms();
    HLOGI(LOG_COMP, "Incoming connection from %s on %s (pending)",
          remote_call, (local_call && local_call[0]) ? local_call : "(primary)");
}

/* PENDING again, for the call already pending (arq_fsm.h).  Not
 * cb_notify_pending: its address reset must happen once, when the call
 * arrives.  A repeat is handled on the FSM thread after a host CONNECT may
 * have written src_addr/dst_addr on the command thread; clearing them then
 * left cb_notify_connected reporting the host's own call backwards (#477). */
static void cb_notify_pending_again(const char *remote_call)
{
    (void)remote_call;   /* the FSM logs the repeat with the callsign */
    arq_tnc_send_pending();
    g_pending_ms = time_now_ms();
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
    bool told = g_host_told;
    g_host_told = false;
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
    rx_dropped_summary("Session ended");
    if (told)
    {
        HLOGI(LOG_COMP, "Disconnected (the host was told at its ABORT)");
        /* The ABORT hold's CANCELPENDING goes here, as the session ends, not
         * after the pass: fsm_disconnecting's RX_CALL ends the session and
         * answers the CALL in one dispatch, and that CALL's PENDING must
         * come after it (abort_hold). */
        abort_hold_release(time_now_ms());
    }
    else
    {
        arq_tnc_send_disconnected();
        HLOGI(LOG_COMP, "Disconnected");
    }
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

/* Into data_rx_buffer_arq, under g_sess_lock.  While something reads the
 * ring (a TCP data client, or the host holding the TNC), a full ring is
 * waited on, as before: the reader drains it, and the TCP reactor never
 * waits on this lock (#590).  With nothing reading -- the host closed its
 * data port mid-session, or was dropped for not reading -- nothing ever
 * would, and waiting stopped the event loop for good: no frames decoded, no
 * timers, the link left to die on the peer's watchdog.
 *
 * So what fits is kept and the rest dropped, with a warning.  The dropped
 * bytes were acknowledged on the air, so from the first one the session's
 * stream has a hole: nothing more of it is delivered, even to a client
 * that reconnects (it would get the bytes before the hole, then the ones
 * after, with nothing marking the gap), and the session is ended with an
 * ABORT, so the peer learns the transfer failed. */
static void cb_deliver_rx_data(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > INT_BUFFER_SIZE)
        return;
    size_t put = 0;
    while (put < len && g_rx_dropped == 0)
    {
        size_t room = circular_buf_free_size(data_rx_buffer_arq);
        size_t n = len - put < room ? len - put : room;
        if (n > 0)
        {
            write_buffer(data_rx_buffer_arq, (uint8_t *)data + put, n);  /* the only writer: it fits */
            put += n;
            continue;
        }
        if (!tnc_rx_reader_attached())
            break;
        hermes_usleep(10000);   /* full, and a reader there to drain it */
    }
    if (put < len)
    {
        if (g_rx_dropped == 0)
            HLOGW(LOG_COMP, "No application is reading the session's received data and its buffer is full: "
                  "dropping the rest of the session's data");
        g_rx_dropped += len - put;
        rx_end_session_for_no_reader();
    }

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

    case ARQ_CMD_CLEAN_TX_BUFFER:
    {
        /* VARA's CLEANTXBUFFER: drop the host's bytes not yet sent, keep the
         * session.  BPQ32 sends it right before DISCONNECT at its session
         * time limit, so the DISCONNECT's drain has nothing left to wait
         * for.  Frames already on the air (awaiting an ACK) stay: the FSM
         * owns those, and clearing them is ABORT's job.  The host's bytes
         * sent before this command count as unsent too.  Clear before
         * waiting as well: with g_app_tx_buf full the payload bridge is
         * parked in write_buffer, and only a clear lets it finish. */
        size_t dropped = 0;
        for (int pass = 0; pass < 2; pass++)
        {
            pthread_mutex_lock(&g_app_tx_mtx);
            dropped += size_buffer(g_app_tx_buf);
            clear_buffer(g_app_tx_buf);
            pthread_mutex_unlock(&g_app_tx_mtx);
            if (pass == 0)
                wait_payload_written();
        }
        HLOGI(LOG_COMP, "CLEANTXBUFFER: dropped %zu unsent bytes", dropped);
        /* The message store took those bytes when they were queued: a
         * partly sent chat line must not be glued onto the next message. */
        if (dropped > 0)
            msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_TX);
        arq_tnc_send_cleantxbuffer(dropped > 0);
        return;
    }

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
        snprintf(g_connect_src, CALLSIGN_MAX_SIZE, "%s", msg->arg0);
        g_host_told = false;   /* a new session: its end is owed a notice */
        arq_conn.session_bw = 0;
        pthread_mutex_unlock(&g_conn_lock);
        /* The FSM defers a CONNECT that arrives while the previous session is
         * ending, and then skips that session's teardown callback -- which is
         * what would normally flush the TX queue.  So flush the leftovers here,
         * before anything for the new call can be queued, so no stale byte
         * reaches a different station.
         *
         * Also while the session is still CONNECTED with its DISCONNECT held to
         * drain: the redial ends that drain (arq_fsm.c, fsm_connected), so the
         * old session's unsent bytes are not delivered either way, and left
         * here they would go out on the new call.
         *
         * Locks are taken one at a time (never nested), keeping the leaf
         * ordering described at g_conn_lock. */
        pthread_mutex_lock(&g_sess_lock);
        bool tearing_down = (g_sess.conn_state == ARQ_CONN_DISCONNECTING) ||
                            (g_sess.conn_state == ARQ_CONN_CONNECTED &&
                             g_sess.teardown == ARQ_TD_DRAINING);
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
        /* DISCONNECTED is the host's "the session is closed", sent when it
         * is: VARA's command reference has DISCONNECT end the link "once the
         * TX buffer is empty" and DISCONNECTED mean "Session closed by either
         * end".  The FSM ends the session -- the drain, then the DISCONNECT
         * exchange -- and its end says DISCONNECTED (cb_notify_disconnected).
         * We deliberately do NOT clear g_app_tx_buf here: the FSM drains any
         * bytes still queued first.  ABORT stays immediate.
         *
         * Saying it at once let a scanning host move the radio mid-teardown:
         * BPQ32 restarts its scan 15 s after DISCONNECTED (SCANSTART 15), and
         * its LISTEN OFF ended the session before the peer had our DISCONNECT
         * (on air, K7EK and KG9NI, 1.9.22: a retry due 20 s after the first
         * cut off, and drains cut with no DISCONNECT sent at all; the peer
         * timed out after 240 s).  BPQ32 keeps the radio while it waits for
         * DISCONNECTED, and ABORTs after 30 s.
         *
         * With no session up there is nothing to end and no later notice to
         * wait for: answer at once.  Not arq_session_active(): a session that
         * ended on the peer's DISCONNECT is no longer active, but its notice
         * waits for our reply to go out, and answering here too said it
         * twice. */
        if (!notice_owed())
            arq_tnc_send_disconnected();
        wait_payload_written();
        ev.id = ARQ_EV_APP_DISCONNECT;
        break;

    case ARQ_CMD_ABORT:
        /* Dirty disconnect: drop everything still held -- the host's unsent
         * bytes here, the frames awaiting an ACK and the re-staged ones in the
         * FSM (ev.abort) -- and go at once, without the drain DISCONNECT waits
         * for.  The peer is still told: the FSM sends one DISCONNECT, never
         * over a frame on the air, and the callee's out of turn if no slot
         * comes before a scanning host takes the radio (abort_teardown), so
         * the peer need not time out (#218: the comment and docs/TNC.md said
         * no frame went out; one always did).
         *
         * DISCONNECTED goes at once: a host that ABORTs is done waiting
         * (BPQ32 releases the radio on it).  That is this session's notice,
         * so its end in the FSM doesn't send another (g_host_told): on air,
         * VE4KLM 5 Nov, a second one came 58 s later, after the unanswered
         * DISCONNECT frames, when JNOS had already moved on. */
        clear_connection_data();
        if (notice_owed())
        {
            pthread_mutex_lock(&g_conn_lock);
            g_host_told = true;
            pthread_mutex_unlock(&g_conn_lock);
        }
        arq_tnc_send_disconnected();
        /* ...and what we are transmitting stops with it.  A carousel round
         * at the floor is a 27 s keydown, and it used to play out to the end:
         * on air, VE4KLM 15 Nov (JNOS "mbox reset"), PTT stayed on 20 s past
         * the ABORT and its DISCONNECTED, carrying data the session had just
         * dropped.  The modem unkeys within its tail time, the FSM sees the
         * keydown end as it would any other, and the one DISCONNECT goes out
         * after it.  A CALL being sent is cut too: the call is over. */
        arq_modem_cut_tx();
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

/* Spells of listening for calls that have ended (arq_fsm_dispatch_ends_listening),
 * for the modem's MFSK call listeners (listen_spells_ended in the snapshot),
 * and those that ended with no session after them (listen_releases), for its
 * control and payload decoders.  Guarded by g_sess_lock. */
static uint32_t g_listen_spells_ended;
static uint32_t g_listen_releases;

static void count_listen_end(arq_listen_end_t end)
{
    if (end != ARQ_LISTEN_GOES_ON)
        g_listen_spells_ended++;
    if (end == ARQ_LISTEN_ENDS_RELEASED)
        g_listen_releases++;
}

/* While calling, the host hears a line at least every ARQ_CALL_PROGRESS_MS.
 *
 * BPQ32 closes a session idle for its IDLETIME, and a forwarding connect
 * script may set that as low as it likes ("IDLETIME 60": BBSUtilities.c
 * passes it to ChangeSessionIdletime with no floor, and the BBS does not log
 * the line).  While its port is attached and not connected, BPQ32's VARA
 * driver passes the TNC lines it does not know to the session as data
 * ("VARA} ..."), and that resets the idle timer; PTT, SN, BUSY and BUFFER it
 * takes itself (VARA.c).  Mercury's BITRATE went out only for frames on a
 * payload mode (modem.c, publish_link_bitrate), so during the fast CALLs
 * the host heard nothing: on air (KG9NI, linbpq 6.0.25.41, 2026-11-10/11) 8
 * calls were dropped 61-62 s after the last line BPQ32 had, the first MFSK
 * CALL's BITRATE 55 s in or later.  At BW500 even the floor CALLs leave 64.5
 * s between keydowns (a 27 s frame and its 37.5 s interval), and a CALL held
 * for a frame arriving (call_must_wait) adds up to 32.5 s.
 *
 * So a BITRATE line on entering CALLING and every ARQ_CALL_PROGRESS_MS after,
 * for the carrier of the CALL out last: the modem's rate for it, where it
 * has reported one (tnc_send_bitrate_progress), else the mode's net rate,
 * its payload over its airtime -- the modem never reports the control mode.
 * Once connected, BITRATE is the modem's alone again. */
#define ARQ_CALL_PROGRESS_MS 20000
static uint64_t g_call_progress_ms;   /* the last line's time; 0: none (g_sess_lock) */

static void call_progress(uint64_t now)
{
    if (g_sess.conn_state != ARQ_CONN_CALLING)
    {
        g_call_progress_ms = 0;
        return;
    }
    if (g_call_progress_ms && now - g_call_progress_ms < ARQ_CALL_PROGRESS_MS)
        return;
    g_call_progress_ms = now;
    int mode = g_sess.call_carrier > 0 ? g_sess.call_carrier : ARQ_CONTROL_MODE;
    const arq_mode_timing_t *tm = arq_protocol_mode_timing(mode);
    uint32_t bps = tm && tm->frame_duration_s > 0.0f
                 ? (uint32_t)(tm->payload_bytes * 8 / tm->frame_duration_s + 0.5f) : 0;
    /* The modem's speed levels (modem.c, bitrate_level_from_payload_mode),
     * and 16 for the control mode, which it never reports. */
    uint32_t level = mode == MERCURY_MODE_MFSK ? 100 : mode == MERCURY_MODE_MFSK_N32 ? 103 :
                     mode == MERCURY_MODE_MFSK_W2 ? 104 : 16;
    arq_tnc_send_bitrate(level, bps);
}

/* While we answer a call, PENDING again every ARQ_PENDING_HOLD_MS.
 *
 * A scanning host holds its channel for a fixed time per PENDING: BPQ32 six
 * scan intervals (VARA.c, "Time out after 6 Scanintervals"), about 60-85 s
 * on air.  We answer for as long as our ACCEPT retries last, the host's
 * RETRIES slots from the last CALL heard (RETRIES 20: 20 retry intervals,
 * about 160 s), and only a repeat of the CALL that we decode sent PENDING
 * again (#477).  On a weak path that is not enough: on air (K7EK <- KG9NI-7,
 * 30 m, 2026-11-12 20:49) K7EK decoded 2 of the caller's 13 CALLs, answered
 * the second, and BPQ32 hopped 66 s later while the caller called on for
 * another 2 minutes; also 06:23 (63 s).  Held this way, the host keeps the
 * channel exactly as long as we answer: when the ACCEPT retries run out, or
 * the caller's DISCONNECT ends it, the FSM's CANCELPENDING releases it.
 * Not after a LISTEN OFF we are deferring (ARQ_TD_LISTEN_OFF): the host has
 * asked for the radio back. */
#define ARQ_PENDING_HOLD_MS 20000

static void pending_hold(uint64_t now)
{
    if (g_sess.conn_state != ARQ_CONN_ACCEPTING || g_sess.teardown == ARQ_TD_LISTEN_OFF)
        return;
    if (g_pending_ms && now - g_pending_ms < ARQ_PENDING_HOLD_MS)
        return;
    arq_tnc_send_pending();
    g_pending_ms = now;
}

/* After an ABORT, PENDING holds a scanning host on the channel until our one
 * DISCONNECT is out.
 *
 * ABORT answers the host at once (DISCONNECTED, g_host_told), and BPQ32
 * releases the port on it: LISTEN ON and SCANSTART 15 (VARA.c,
 * VARAReleaseTNC), so its scan's first hop comes about 15 s later, and the
 * hop's LISTEN OFF releases the radio.  Our DISCONNECT is never keyed over
 * the peer (abort_teardown), and at BW500 a peer's floor frame is 26 s: on
 * air (K7EK, callee, ABORT 2026-11-13 15:17:40.8, KG9NI on the air with an
 * MFSK500 frame to about 15:18:08) the hop came at 15:17:57, the DISCONNECT
 * was never sent, and KG9NI sat out its 240 s watchdog.
 *
 * A hop is refused while the port reports a connect pending (VARA.c: scan
 * permission is refused while ConnectPending, set to 6 scan intervals by
 * PENDING, cleared by CANCELPENDING or DISCONNECTED).  So, with the host
 * told and our DISCONNECT not yet out: PENDING, after the DISCONNECTED (it
 * would clear it), again every ARQ_PENDING_HOLD_MS, and CANCELPENDING once
 * the DISCONNECT has gone (its keydown over) or the teardown has ended.  The
 * FSM's ABORT bounds (ARQ_CAR_ABORT_WAIT_MS) end it: the callee's
 * out-of-turn DISCONNECT that never finds a clear channel gives up a
 * longest keydown of the caller's (30 s) and its guard after its key-by time
 * (abort_key_by, then ARQ_ABORT_HOLD_MAX holds, not counted while its own
 * keydown is on the air): about 43 s after the ABORT.  The CANCELPENDING
 * then comes with no DISCONNECT sent.  ARQ_ABORT_SCAN_HOLD_MS is only the
 * backstop.  The repeat is the answering hold's (pending_hold), the same
 * ARQ_PENDING_HOLD_MS.  Mercury already holds a scanner this way while a CQ is
 * on the air (arq_notify_cq_tx_started).  A plain DISCONNECT needs none:
 * its DISCONNECTED comes when the session has closed (#493). */
#define ARQ_ABORT_SCAN_HOLD_MS 60000
static uint64_t g_abort_hold_ms;   /* the hold's first PENDING; 0: none (g_sess_lock) */
static uint64_t g_abort_hold_last; /* its last PENDING */
static bool     g_abort_hold_done; /* this teardown has had its hold */

/* CANCELPENDING for a hold in progress.  From abort_hold once the hold is no
 * longer owed (after the event loop's pass), and earlier where a new call
 * may follow in the same pass: cb_notify_disconnected, as the ABORTed
 * session ends, and cb_notify_pending, before a new call's PENDING.  A
 * CANCELPENDING after that PENDING would cancel it and release a scanner in
 * the middle of the answer.  The FSM answers a CALL in the pass that ended
 * the teardown: fsm_disconnecting's RX_CALL ends the session and dispatches
 * the CALL in one go, and enter_idle_after_call enters LISTENING at once, so
 * a CALL later in the same batch is taken too.  All on the event loop, under
 * g_sess_lock. */
static void abort_hold_release(uint64_t now)
{
    if (!g_abort_hold_ms)
        return;
    arq_tnc_send_cancelpending();
    HLOGI(LOG_COMP, "ABORT: the scanner released (%llu s held)",
          (unsigned long long)((now - g_abort_hold_ms) / 1000));
    g_abort_hold_ms = 0;
}

static void abort_hold(uint64_t now)
{
    pthread_mutex_lock(&g_conn_lock);
    bool told = g_host_told;
    pthread_mutex_unlock(&g_conn_lock);
    bool owed = told && g_sess.conn_state == ARQ_CONN_DISCONNECTING &&
                !(g_sess.teardown == ARQ_TD_SENT && !g_sess.tx_active);
    if (owed && g_abort_hold_ms && now - g_abort_hold_ms >= ARQ_ABORT_SCAN_HOLD_MS)
    {
        /* Not reached while the FSM's ABORT bounds hold (~30 s). */
        HLOGW(LOG_COMP, "ABORT: the hold reached its %d s backstop with the teardown still on",
              ARQ_ABORT_SCAN_HOLD_MS / 1000);
        owed = false;
    }
    if (!owed)
    {
        abort_hold_release(now);
        if (!told || g_sess.conn_state != ARQ_CONN_DISCONNECTING)
            g_abort_hold_done = false;   /* a new teardown may hold again */
        return;
    }
    if (g_abort_hold_ms && now - g_abort_hold_last < ARQ_PENDING_HOLD_MS)
        return;
    if (!g_abort_hold_ms)
    {
        if (g_abort_hold_done)
            return;
        g_abort_hold_done = true;
        g_abort_hold_ms = now;
        HLOGI(LOG_COMP, "ABORT: holding a scanning host on the channel until our DISCONNECT is out (PENDING)");
    }
    arq_tnc_send_pending();
    g_abort_hold_last = now;
}

/* A CALL on its way: PENDING as soon as a decoder has locked onto a frame,
 * not when the CALL has decoded.
 *
 * A scanning host leaves the channel on its own schedule -- RMS Trimode stays
 * on a frequency for 4 s and is deaf for 0.5 s while it changes -- and it
 * stops only on PENDING, which it takes for "a connect request has been
 * detected".  Ours came when the CALL had decoded: a 3.74 s frame (13.5 s on
 * the MFSK floor) that falls whole inside one 4 s stay on about one visit in
 * twenty-five (#527).  A decoder's lock comes about a second into the frame
 * (the modem keeps it: modem_rx_frame_in_flight), so a stay that catches the
 * frame's start holds the channel for the rest of it.
 *
 * So, while LISTENING and not keyed: PENDING when a lock begins, again every
 * ARQ_PENDING_HOLD_MS while it lasts (a floor frame at BW500 is 27 s), and
 * CANCELPENDING once it is gone and the FSM has not taken a CALL out of it.
 * That last wait is ARQ_ARRIVAL_GRACE_MS: a CALL for us decodes as the lock
 * ends and the FSM answers it a pass later, with the PENDING of the call
 * (cb_notify_pending), and a CANCELPENDING in between would let the host go.
 * A frame that is not a CALL for us -- another session's, a station calling
 * someone else -- holds the host for its length and the grace, no more.
 * A lock that outlasts any frame (ARQ_ARRIVAL_MAX_MS) is noise: released, and
 * not held again until it has gone.
 *
 * No state beyond these statics: nothing is added to the session, and the
 * hold is a function of the lock, the connection state and the clock. */
#define ARQ_ARRIVAL_GRACE_MS 2000
#define ARQ_ARRIVAL_MAX_MS   45000
/* The modem says so on every pass of its loop in which a decoder was locked,
 * and a pass in which the decoder published nothing says nothing: a lock that
 * was seen within ARQ_ARRIVAL_STALE_MS is still there.  The loop passes every
 * 20 ms (RX_DECODE_CHUNK_SAMPLES); an RX loop stalled for longer than this and
 * the grace together (2.75 s) reads as a lock gone, and the host is released
 * while the CALL may still be on the air -- as such a stall loses that CALL's
 * audio anyway. */
#define ARQ_ARRIVAL_STALE_MS 750
static _Atomic uint64_t g_call_seen_ms;     /* the last time a CALL decoder was locked; 0: never */
static uint64_t        g_arrival_ms;       /* the hold's first PENDING; 0: none (g_sess_lock) */
static uint64_t        g_arrival_last_ms;  /* its last PENDING */
static uint64_t        g_arrival_gone_ms;  /* when the lock was first seen gone; 0: it is there */
static bool            g_arrival_noise;    /* the lock outlasted any frame: not held again until gone */

void arq_note_call_in_flight(void)
{
    uint64_t now = time_now_ms();
    uint64_t was = atomic_exchange(&g_call_seen_ms, now);
    if (!was || now - was >= ARQ_ARRIVAL_STALE_MS)
    {
        /* Wake the event loop: its timeout is 500 ms and the host's stay is 4 s. */
        pthread_mutex_lock(&g_evq_lock);
        pthread_cond_signal(&g_evq_cond);
        pthread_mutex_unlock(&g_evq_lock);
    }
}

static void arrival_hold(uint64_t now)
{
    uint64_t seen = atomic_load(&g_call_seen_ms);
    bool on = seen && now - seen < ARQ_ARRIVAL_STALE_MS;
    bool idle = g_sess.conn_state == ARQ_CONN_LISTENING && !g_sess.tx_active;

    if (!on)
        g_arrival_noise = false;
    if (!idle)
    {
        /* A session has taken over (its own notices follow), or we are keyed,
         * or the host has asked for the radio back: nothing of this hold left
         * to say. */
        g_arrival_ms = g_arrival_gone_ms = 0;
        return;
    }
    if (on)
    {
        g_arrival_gone_ms = 0;
        if (g_arrival_noise)
            return;
        if (!g_arrival_ms)
        {
            arq_tnc_send_pending();
            g_arrival_ms = g_arrival_last_ms = now;
            HLOGI(LOG_COMP, "A CALL is arriving: holding a scanning host (PENDING)");
        }
        else if (now - g_arrival_ms >= ARQ_ARRIVAL_MAX_MS)
        {
            arq_tnc_send_cancelpending();
            HLOGI(LOG_COMP, "A lock that outlasts any frame: noise, the scanner released");
            g_arrival_ms = 0;
            g_arrival_noise = true;
        }
        else if (now - g_arrival_last_ms >= ARQ_PENDING_HOLD_MS)
        {
            arq_tnc_send_pending();
            g_arrival_last_ms = now;
        }
        return;
    }
    if (!g_arrival_ms)
        return;
    if (!g_arrival_gone_ms)
        g_arrival_gone_ms = now;
    if (now - g_arrival_gone_ms >= ARQ_ARRIVAL_GRACE_MS)
    {
        arq_tnc_send_cancelpending();
        HLOGI(LOG_COMP, "The frame was no CALL for us: the scanner released (%llu s held)",
              (unsigned long long)((now - g_arrival_ms) / 1000));
        g_arrival_ms = g_arrival_gone_ms = 0;
    }
}

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
            count_listen_end(arq_fsm_dispatch_ends_listening(&g_sess, &events[i]));

        /* Fire deadline */
        now = time_now_ms();
        if (g_sess.deadline_ms != UINT64_MAX && now >= g_sess.deadline_ms)
        {
            g_sess.deadline_ms = UINT64_MAX;
            arq_event_t tev = { .id = g_sess.deadline_event };
            count_listen_end(arq_fsm_dispatch_ends_listening(&g_sess, &tev));
        }
        call_progress(now);
        pending_hold(now);
        abort_hold(now);
        rx_abort_tick();
        arrival_hold(now);
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

bool arq_handle_incoming_connect_frame(uint8_t *data, size_t frame_size, float rx_snr, int mode,
                                       uint32_t age_ms)
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
    else if (!arq_protocol_connect_compatible(data))
        rc = -3;   /* an older 2.x minor: likewise */
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
    if (rc == -3)
    {
        /* Minors 0 and 1 (1.9.20-1.9.24) map the ladder's levels to other
         * modes (ARQ_CONNECT_MINOR_MIN): a session would lose its rounds. */
        HLOGW(LOG_COMP, "%s for %s from Mercury 1.9.24 or older (protocol minor %d): the versions do not connect, %s",
              is_accept ? "ACCEPT" : "CALL", dialed_call, arq_protocol_connect_minor(data),
              is_accept ? "ignored" : "not answered");
        return false;
    }

    arq_event_t ev = {0};
    ev.id         = is_accept ? ARQ_EV_RX_ACCEPT : ARQ_EV_RX_CALL;
    ev.session_id = session_id;
    ev.rx_snr     = rx_snr;
    ev.mode       = mode;          /* the carrier: an answer goes back on it */
    ev.tx_mfsk    = tx_mfsk;       /* the peer can only transmit MFSK */
    ev.rx_age_ms  = age_ms;        /* a CALL decoded too late is not answered */
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
    g_connect_src[0] = '\0';
    g_host_told = false;
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
        .notify_pending_again = cb_notify_pending_again,
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

/* The FSM will still say DISCONNECTED for the session (arq_fsm_notice_owed):
 * then a host DISCONNECT waits for it, and ABORT's own answer stands for it. */
static bool notice_owed(void)
{
    pthread_mutex_lock(&g_sess_lock);
    bool owed = arq_fsm_notice_owed(&g_sess);
    pthread_mutex_unlock(&g_sess_lock);
    return owed;
}

bool arq_session_active(void)
{
    pthread_mutex_lock(&g_sess_lock);
    int st = g_sess.conn_state;
    pthread_mutex_unlock(&g_sess_lock);
    return st == ARQ_CONN_CALLING || st == ARQ_CONN_ACCEPTING ||
           st == ARQ_CONN_CONNECTED || st == ARQ_CONN_DISCONNECTING;
}

bool arq_channel_heard(void)
{
    pthread_mutex_lock(&g_sess_lock);
    bool heard = arq_fsm_channel_keyed(&g_sess) != 0;
    pthread_mutex_unlock(&g_sess_lock);
    return heard;
}

bool arq_link_has_peer(void)
{
    pthread_mutex_lock(&g_sess_lock);
    bool has_peer = arq_conn_state_has_peer(g_sess.conn_state);
    pthread_mutex_unlock(&g_sess_lock);
    return has_peer;
}

int arq_goodbye_progress(void)
{
    pthread_mutex_lock(&g_sess_lock);
    int p = !arq_conn_state_has_peer(g_sess.conn_state) ? 0
          : (g_sess.conn_state == ARQ_CONN_DISCONNECTING && g_sess.teardown == ARQ_TD_SENT) ? 2 : 1;
    pthread_mutex_unlock(&g_sess_lock);
    return p;
}

bool arq_is_link_connected(void)
{
    pthread_mutex_lock(&g_sess_lock);
    bool connected = (g_sess.conn_state == ARQ_CONN_CONNECTED);
    pthread_mutex_unlock(&g_sess_lock);
    return connected;
}

/* For the TCP reactor, which must never wait on g_sess_lock: the event loop
 * holds it while it delivers received bytes, and a delivery larger than the
 * free room in data_rx_buffer_arq waits there for the reactor to drain it.
 * A reactor waiting on the lock then drained nothing, and both stopped for
 * good (Go harness, CCIR Poor 20 dB, a 30 kB transfer: the receiver's
 * delivery of 6 blocks at once, its periodic BUFFER report due).  So these
 * take the lock only if it is free. */
bool arq_call_in_progress(void)
{
    if (pthread_mutex_trylock(&g_sess_lock) != 0)
        return true;             /* the event loop is at work: say a call, the safe answer */
    bool call = g_initialized && g_sess.conn_state != ARQ_CONN_DISCONNECTED &&
                g_sess.conn_state != ARQ_CONN_LISTENING;
    pthread_mutex_unlock(&g_sess_lock);
    return call;
}

bool arq_link_connected_now(bool *connected)
{
    if (!connected || pthread_mutex_trylock(&g_sess_lock) != 0)
        return false;
    *connected = (g_sess.conn_state == ARQ_CONN_CONNECTED);
    pthread_mutex_unlock(&g_sess_lock);
    return true;
}

int arq_buffer_bytes_nowait(void)
{
    if (!g_initialized)
        return 0;
    return arq_get_tx_backlog_bytes() + arq_fsm_tx_inflight_published();
}

/* g_sess_lock for the TCP reactor: never waits on it for long, since the
 * event loop holds it while it waits on the reactor (above).  Up to ~1 s of
 * tries; false if it stayed busy. */
static bool sess_lock_for_reactor(void)
{
    for (int i = 0; i < 1000; i++)
    {
        if (pthread_mutex_trylock(&g_sess_lock) == 0)
            return true;
        hermes_usleep(1000);
    }
    return false;
}

void arq_discard_stale_rx(void)
{
    /* Every FSM dispatch -- so every CONNECTED transition and every delivery
     * into the buffer -- runs under g_sess_lock.  Holding it across the check
     * and the clear means a session cannot start in between and have its
     * first bytes wiped. */
    if (!sess_lock_for_reactor())
        return;          /* the event loop is busy delivering: a session is up */
    if (g_sess.conn_state != ARQ_CONN_CONNECTED)
        clear_buffer(data_rx_buffer_arq);
    pthread_mutex_unlock(&g_sess_lock);
}

size_t arq_discard_stale_tx(void)
{
    if (!g_initialized)
        return 0;

    /* Only with no call at all: idle or listening.  While calling or
     * accepting, the bytes belong to the session being set up (a host may
     * write as soon as it sends CONNECT, and may reopen its data socket);
     * with a session up, the session ends and flushes them itself.
     * g_sess_lock across the check and the clear, as arq_discard_stale_rx:
     * a CONNECT or an incoming CALL taken by the ARQ thread cannot slip in
     * between.  g_app_tx_mtx inside it, the order the FSM's dispatch takes
     * them in (cb_tx_read).
     *
     * Not covered: a CONNECT still on the command bus.  It reaches the ARQ
     * thread asynchronously, so for a few ms after the host sends it the
     * state is still DISCONNECTED, and a host that writes its first bytes
     * and closes its data socket inside that window loses them.  No known
     * host reopens its data socket right after CONNECT.
     *
     * Bytes the client wrote just before it closed may still be on the
     * payload bus, and must land before the last clear or they go out on the
     * next session anyway.  The bridge may also be blocked on a full queue
     * (an idle write beyond APP_TX_BUF_SIZE: nothing drains it with no
     * session); one clear would wake it to write the rest after the clear.
     * So clear until everything sent before now has landed: each clear makes
     * room for the next messages.  The TCP thread calls this, so no new
     * payload joins the bus meanwhile.  Bounded as wait_payload_written. */
    unsigned target = atomic_load(&g_payload_sent);
    size_t dropped = 0;
    for (int i = 0; i < 1000; i++)
    {
        /* Read before the clear: if everything had landed, this clear is
         * the last one needed. */
        bool landed = (int)(atomic_load(&g_payload_written) - target) >= 0;
        bool idle;

        if (!sess_lock_for_reactor())
            break;       /* busy for a second: a session is at work, not idle */
        idle = (g_sess.conn_state == ARQ_CONN_DISCONNECTED ||
                g_sess.conn_state == ARQ_CONN_LISTENING);
        if (idle)
        {
            pthread_mutex_lock(&g_app_tx_mtx);
            size_t n = size_buffer(g_app_tx_buf);
            if (n > 0)
            {
                clear_buffer(g_app_tx_buf);
                dropped += n;
            }
            pthread_mutex_unlock(&g_app_tx_mtx);
        }
        pthread_mutex_unlock(&g_sess_lock);

        if (landed || !idle)
            break;
        hermes_usleep(1000);
    }

    if (dropped > 0)
    {
        /* A partial chat line from those bytes must not be glued onto the
         * next session's first message. */
        msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_TX);
        HLOGW(LOG_COMP, "Data client left with no session up: discarded %zu "
              "queued byte(s) that no session would carry", dropped);
    }
    return dropped;
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
    snapshot->listen_spells_ended = g_listen_spells_ended;
    snapshot->listen_releases     = g_listen_releases;
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
        arq_protocol_set_call_fast_slots(0);
        atomic_store(&arq_accept_retry_slots,     ARQ_ACCEPT_RETRY_SLOTS_DEFAULT);
        atomic_store(&arq_disconnect_retry_slots, ARQ_DISCONNECT_RETRY_SLOTS_DEFAULT);
    }
    else
    {
        atomic_store(&arq_call_retry_slots,   slots);
        /* More retries mean more fast CALLs first (#442), never fewer than
         * the default. */
        arq_protocol_set_call_fast_slots(slots);
        atomic_store(&arq_accept_retry_slots, slots);
        /* Leave disconnect retries at default — no benefit to long teardown */
        atomic_store(&arq_disconnect_retry_slots, ARQ_DISCONNECT_RETRY_SLOTS_DEFAULT);
    }
    HLOGI(LOG_COMP, "Retry slots: call=%d (fast=%d) accept=%d disconnect=%d",
          atomic_load(&arq_call_retry_slots), atomic_load(&arq_call_fast_slots), atomic_load(&arq_accept_retry_slots),
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

void arq_set_max_keydown_s(int s)
{
    car_set_max_keydown_ms(s > 0 ? (uint32_t)s * 1000u : 0);
    if (s > 0)
        HLOGI(LOG_COMP, "Carousel rounds held to %d s of airtime", s);
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
