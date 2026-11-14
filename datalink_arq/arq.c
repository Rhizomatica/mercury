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
#include "arq_crypto.h"
#include "car_zstd.h"
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

/* The encryption state of the current session (arq_crypto.h).  Guarded by
 * g_app_tx_mtx, the lock the byte-stream callbacks already take: every path to
 * it is either alone or nested inside g_sess_lock, which is the order
 * cb_tx_read has always used.  Anything that reaches another subsystem (TNC
 * notices, the event queue) happens after the lock is released.
 *
 * With [crypto] mode = off none of this is ever touched: the callbacks take
 * their original paths, byte for byte.
 *
 * One of each, as there is one g_sess: the FSM runs one session at a time,
 * and g_neg below is tied to it by its session id.  Sessions side by side
 * would need these per session. */
static arq_xs_t g_xs;
/* The connect's two encryption bits as they went on air, for the session the
 * FSM is setting up (crypto_negotiate): our CALL's offer and the ACCEPT's
 * answer at the caller, the CALL's offer and our answer at the callee.  Kept
 * here, not in the session.  Guarded by g_app_tx_mtx, like g_xs. */
static struct { uint8_t session_id; bool valid, caller, offer, accept; } g_neg;
/* The compression negotiation (#506): the same shape as g_neg, without the
 * key exchange -- offer on our CALL, echo on our ACCEPT, the session's car
 * compresses when both sides said yes. */
static struct { uint8_t session_id; bool valid, caller, offer, accept; } g_cneg;

/* When PENDING last went to the host for the call we are answering; 0: none
 * outstanding (the host was told CONNECTED, CANCELPENDING or DISCONNECTED
 * since).  Written by the FSM's callbacks, cb_deliver_rx_data and
 * pending_hold, all on the event loop under g_sess_lock. */
static uint64_t g_pending_ms;

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

/* End a session that must not continue: the handshake failed, a record failed
 * its tag, or required mode could not be met.  Nothing more is sent or
 * delivered; the client gets DISCONNECTED from the normal teardown, having
 * never been told CONNECTED if the handshake had not finished. */
static void crypto_abort(const char *why)
{
    arq_event_t ev = { .id = ARQ_EV_APP_DISCONNECT };

    HLOGW(LOG_COMP, "Ending the session: %s", why);
    pthread_mutex_lock(&g_app_tx_mtx);
    arq_xs_end(&g_xs);
    g_xs.state = ARQ_XS_FAILED;
    clear_buffer(g_app_tx_buf);
    pthread_mutex_unlock(&g_app_tx_mtx);
    evq_push(&ev);
}

/* Right after the RF connect, on the event loop (under g_sess_lock, which is
 * recursive).  The two bits that went on air decide: both set means Noise_KK,
 * anything else is a clear session -- which required mode refuses. */
static void crypto_session_start(const char *remote_call, const char *local_call)
{
    pthread_mutex_lock(&g_app_tx_mtx);
    bool initiator  = g_neg.caller;
    bool negotiated = g_neg.valid && g_neg.session_id == g_sess.session_id &&
                      g_neg.offer && g_neg.accept;
    pthread_mutex_unlock(&g_app_tx_mtx);
    const char *why = NULL;
    bool secure = false;

    if (negotiated)
    {
        uint8_t peer_pub[ARQ_CRYPTO_KEYLEN];
        char my_call[CALLSIGN_MAX_SIZE];
        arq_xs_params_t p;

        arq_conn_get_calls(my_call, NULL, NULL, sizeof(my_call));

        /* Both sides must bind the same strings.  The initiator is known by
         * the source callsign of its CALL; the responder by the callsign the
         * initiator DIALED -- which, on a station answering several SSIDs, is
         * the one whose CRC matched, not necessarily its primary. */
        p.initiator      = initiator;
        p.initiator_call = initiator ? my_call : remote_call;
        p.responder_call = initiator ? remote_call
                                     : ((local_call && local_call[0]) ? local_call : my_call);
        p.bw_token       = arq_protocol_bw_token_from_hz(arq_reported_bandwidth_hz());
        p.session_id     = g_sess.session_id;
        p.offer_bit      = true;
        p.accept_bit     = true;

        if (arq_crypto_peer_key(remote_call, peer_pub) != 0)
            why = "the peer's key is no longer readable";
        else
        {
            int rc;

            pthread_mutex_lock(&g_app_tx_mtx);
            rc = arq_xs_begin_secure(&g_xs, &p, peer_pub);
            pthread_mutex_unlock(&g_app_tx_mtx);
            if (rc == 0)
                secure = true;
            else
                why = "the handshake could not start";
        }
        memset(peer_pub, 0, sizeof(peer_pub));
    }
    else
    {
        pthread_mutex_lock(&g_app_tx_mtx);
        arq_xs_begin_clear(&g_xs);
        pthread_mutex_unlock(&g_app_tx_mtx);
        if (arq_crypto_mode() == ARQ_CRYPTO_REQUIRED)
            why = "encryption is required and the peer did not agree to it";
    }

    if (why != NULL)
    {
        crypto_abort(why);
        return;
    }

    if (secure)
    {
        /* CONNECTED goes to the client only when the handshake completes
         * (cb_deliver_rx_data), so it cannot send a byte in the clear. */
        HLOGI(LOG_COMP, "Connected to %s; encryption handshake started as %s",
              remote_call, initiator ? "initiator" : "responder");
        return;
    }

    g_pending_ms = 0;
    arq_tnc_send_connected();
    arq_tnc_send_encryption(false);
    HLOGI(LOG_COMP, "Connected to %s (not encrypted)", remote_call);
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
    /* A new session starts with a clean slate: drop any partial chat line left
     * over from the previous session's feed buffer. */
    msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_RX);
    msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_TX);

    /* Payload compression (#506): the car is told after the session is up
     * (the FSM's ARQ_NEG_SESSION_UP step), not here: notify_connected comes
     * before the car exists, and car_init would wipe the setting. */

    if (arq_crypto_mode() == ARQ_CRYPTO_OFF)
    {
        g_pending_ms = 0;
        arq_tnc_send_connected();   /* dispatches to tnc_send_connected, which takes g_conn_lock via arq_conn_get_calls; must be outside our lock */
        HLOGI(LOG_COMP, "Connected to %s", remote_call);
        return;
    }

    crypto_session_start(remote_call, local_call);
}

static void cb_notify_pending(const char *remote_call, const char *local_call)
{
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
    g_pending_ms = 0;
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
    /* Keys go, and so does any partial inbound record: its plaintext prefix
     * is never delivered. */
    arq_xs_end(&g_xs);
    g_neg.valid = false;
    g_cneg.valid = false;
    pthread_mutex_unlock(&g_app_tx_mtx);
    /* The car's compression streams go with the session (car_set_zstd(false)
     * frees them; a build without the backend never allocated any). */
    if (g_sess.car)
        car_set_zstd(g_sess.car, false);
    /* Discard any partial chat line so the next session's first message is not
     * glued onto trailing bytes from this one. */
    msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_RX);
    msg_store_reset(MSG_PLANE_ARQ, MSG_DIR_TX);
    /* A PENDING still outstanding: the session ended in its encryption
     * handshake, before the host was told CONNECTED (crypto_session_start).
     * The host hears the call is off, as when we stop answering one. */
    bool pending = g_pending_ms != 0;
    g_pending_ms = 0;
    if (told)
        HLOGI(LOG_COMP, "Disconnected (the host was told at its ABORT)");
    else
    {
        if (pending)
            arq_tnc_send_cancelpending();
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

static void deliver_plaintext(const uint8_t *data, size_t len)
{
    write_buffer(data_rx_buffer_arq, (uint8_t *)data, len);

    char my_call[CALLSIGN_MAX_SIZE], src[CALLSIGN_MAX_SIZE], dst[CALLSIGN_MAX_SIZE];
    arq_conn_get_calls(my_call, src, dst, CALLSIGN_MAX_SIZE);
    const char *peer = (dst[0] && strcmp(dst, my_call) != 0) ? dst : src;
    msg_store_feed(MSG_PLANE_ARQ, MSG_DIR_RX, peer, data, len);
}

static void cb_deliver_rx_data(const uint8_t *data, size_t len)
{
    /* Only ever called from the event loop, so one static buffer is enough;
     * it holds a record's worth more than the input (arq_xs_rx_feed). */
    static uint8_t plain[INT_BUFFER_SIZE + ARQ_CRYPTO_REC_MAX];
    char fp[ARQ_CRYPTO_FP_HEXLEN + 1] = { 0 };
    arq_xs_event_t ev = ARQ_XS_EV_NONE;
    size_t n;

    if (!data || len == 0 || len > INT_BUFFER_SIZE)
        return;

    if (arq_crypto_mode() == ARQ_CRYPTO_OFF)
    {
        deliver_plaintext(data, len);
        return;
    }

    pthread_mutex_lock(&g_app_tx_mtx);
    n = arq_xs_rx_feed(&g_xs, data, len, plain, sizeof(plain), &ev);
    if (ev == ARQ_XS_EV_SECURE)
        memcpy(fp, g_xs.fingerprint, sizeof(fp));
    pthread_mutex_unlock(&g_app_tx_mtx);

    if (ev == ARQ_XS_EV_SECURE)
    {
        arq_event_t e = { .id = ARQ_EV_APP_DATA_READY };

        g_pending_ms = 0;
        arq_tnc_send_connected();
        arq_tnc_send_encryption(true);
        HLOGI(LOG_COMP, "Session encrypted; peer key fingerprint %s", fp);
        /* The responder's reply is queued now, and the client may start
         * sending: either way ARQ has something to transmit. */
        evq_push(&e);
    }

    if (n > 0)
        deliver_plaintext(plain, n);

    if (ev == ARQ_XS_EV_FAILED)
        crypto_abort("authentication failed");
}

/* The crypto layer's view of the client queue.  g_app_tx_mtx is held. */
static size_t xs_pull(uint8_t *dst, size_t max, void *ctx)
{
    size_t avail = size_buffer(g_app_tx_buf);

    (void) ctx;
    if (avail > max)
        avail = max;
    if (avail == 0)
        return 0;
    return read_buffer(g_app_tx_buf, dst, avail) == 0 ? avail : 0;
}

static int cb_tx_backlog(void)
{
    pthread_mutex_lock(&g_app_tx_mtx);
    size_t plain = size_buffer(g_app_tx_buf);
    int n = (arq_crypto_mode() == ARQ_CRYPTO_OFF)
            ? (int)plain
            : (int)arq_xs_tx_pending(&g_xs, plain);
    pthread_mutex_unlock(&g_app_tx_mtx);
    return n;
}

static int cb_tx_read(uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return 0;
    pthread_mutex_lock(&g_app_tx_mtx);
    int n = 0;
    if (arq_crypto_mode() == ARQ_CRYPTO_OFF)
    {
        size_t avail = size_buffer(g_app_tx_buf);
        if (avail > len) avail = len;
        if (avail > 0)
            n = (read_buffer(g_app_tx_buf, buf, avail) == 0) ? (int)avail : 0;
    }
    else
    {
        /* `len` is the room left in the carousel block being opened, which a
         * record fills (arq_crypto.h, RECORD SIZE). */
        n = (int)arq_xs_tx_read(&g_xs, buf, len, xs_pull, NULL);
    }
    pthread_mutex_unlock(&g_app_tx_mtx);
    return n;
}

/* The compression negotiation (#506): the same four steps as crypto's,
 * without the key exchange -- offer on our CALL, echo on our ACCEPT, and
 * the session's car compresses when both sides said yes. */

static bool s_compression_on;          /* [compression] mode = optional */

void arq_compression_configure(bool on)
{
    s_compression_on = on;
}

bool arq_compression_enabled(void)
{
    return s_compression_on;
}

static bool cb_compression_negotiate(arq_neg_step_t step, const char *remote_call,
                                     uint8_t session_id, bool peer_bit)
{
    bool bit = false;
    if (!s_compression_on || !car_zstd_available())
        return false;
    switch (step)
    {
    case ARQ_NEG_CALL_OUT:
        bit = true;
        g_cneg.valid = true; g_cneg.session_id = session_id; g_cneg.caller = true;
        g_cneg.offer = true; g_cneg.accept = false;
        break;
    case ARQ_NEG_CALL_IN:
        g_cneg.valid = true; g_cneg.session_id = session_id; g_cneg.caller = false;
        g_cneg.offer = peer_bit;
        g_cneg.accept = peer_bit;          /* optional: take whatever is offered */
        bit = g_cneg.accept;
        break;
    case ARQ_NEG_ACCEPT_OUT:
        bit = g_cneg.valid && g_cneg.session_id == session_id && g_cneg.accept;
        break;
    case ARQ_NEG_ACCEPT_IN:
        if (g_cneg.valid && g_cneg.session_id == session_id)
            g_cneg.accept = g_cneg.offer && peer_bit;
        break;
    case ARQ_NEG_SESSION_UP:
        bit = g_cneg.valid && g_cneg.session_id == session_id && g_cneg.accept;
        break;
    }
    return bit;
}

static bool cb_crypto_negotiate(arq_neg_step_t step, const char *remote_call,
                                uint8_t session_id, bool peer_bit)
{
    bool bit = false;

    if (arq_crypto_mode() == ARQ_CRYPTO_OFF)
        return false;
    pthread_mutex_lock(&g_app_tx_mtx);
    switch (step)
    {
    case ARQ_NEG_CALL_OUT:
        bit = arq_crypto_should_offer(remote_call);
        g_neg.valid = true; g_neg.session_id = session_id; g_neg.caller = true;
        g_neg.offer = bit; g_neg.accept = false;
        break;
    case ARQ_NEG_CALL_IN:
        g_neg.valid = true; g_neg.session_id = session_id; g_neg.caller = false;
        g_neg.offer = peer_bit;
        g_neg.accept = arq_crypto_answer_call(remote_call, peer_bit) == ARQ_CRYPTO_ANSWER_SECURE;
        break;
    case ARQ_NEG_ACCEPT_OUT:
        bit = g_neg.valid && g_neg.session_id == session_id && g_neg.accept;
        break;
    case ARQ_NEG_ACCEPT_IN:
        if (g_neg.valid && g_neg.session_id == session_id)
            g_neg.accept = g_neg.offer && peer_bit;
        break;
    }
    pthread_mutex_unlock(&g_app_tx_mtx);
    return bit;
}

static void cb_send_buffer_status(int backlog_bytes)
{
    /* During the handshake the client has not been told CONNECTED, and the
     * backlog is only our own handshake message: nothing of the client's. */
    if (arq_crypto_mode() != ARQ_CRYPTO_OFF)
    {
        pthread_mutex_lock(&g_app_tx_mtx);
        bool handshaking = (g_xs.state == ARQ_XS_HANDSHAKE);
        pthread_mutex_unlock(&g_app_tx_mtx);
        if (handshaking)
            return;
    }

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
        /* Required mode: refuse up front a call that could only go clear,
         * rather than spend a connect on it and hang up. */
        const char *why = NULL;
        if (!arq_crypto_may_call(msg->arg1, &why))
        {
            HLOGW(LOG_COMP, "Not calling %s: %s", msg->arg1, why);
            arq_tnc_send_disconnected();
            return;
        }

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
        /* ENCRYPT OFF belongs to the client that sent it. */
        (void)arq_crypto_set_client_off(false);
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

/* The session is up on the air and its encryption handshake still runs: the
 * host has not been told CONNECTED (crypto_session_start).  On the floor that
 * is one more exchange, 48 s in the harness at -10 dB.  *caller: our side. */
static bool crypto_handshaking(bool *caller)
{
    if (g_sess.conn_state != ARQ_CONN_CONNECTED || g_sess.teardown != ARQ_TD_NONE ||
        arq_crypto_mode() == ARQ_CRYPTO_OFF)
        return false;
    pthread_mutex_lock(&g_app_tx_mtx);
    bool hs = g_xs.state == ARQ_XS_HANDSHAKE;
    *caller = g_neg.caller;
    pthread_mutex_unlock(&g_app_tx_mtx);
    return hs;
}

static void call_progress(uint64_t now)
{
    /* Also through the handshake of a call we placed: to its host the call
     * is still being placed. */
    bool caller = false;
    if (g_sess.conn_state != ARQ_CONN_CALLING && !(crypto_handshaking(&caller) && caller))
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
    uint32_t level = mode == MERCURY_MODE_MFSK ? 100 : mode == MERCURY_MODE_MFSK_N32 ? 103 : 16;
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
    /* Also through the handshake of a call we answered: the host was told
     * PENDING, and CONNECTED waits for the handshake. */
    bool caller = true;
    bool answering = g_sess.conn_state == ARQ_CONN_ACCEPTING ||
                     (g_pending_ms && crypto_handshaking(&caller) && !caller);
    if (!answering || g_sess.teardown == ARQ_TD_LISTEN_OFF)
        return;
    if (g_pending_ms && now - g_pending_ms < ARQ_PENDING_HOLD_MS)
        return;
    arq_tnc_send_pending();
    g_pending_ms = now;
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

    /* Required mode never answers a CALL it cannot encrypt: there is no
     * session to disconnect yet, so silence is the refusal.  Checked only
     * once the DST CRC has matched, so it applies only to calls for us. */
    bool crypto = arq_protocol_connect_crypto(data);
    bool compress = arq_protocol_connect_compress(data);
    /* An ACCEPT taking a compression offer we did not make answers an
     * earlier CONNECT of the same session id: not ours, as with crypto. */
    if (is_accept && compress)
    {
        pthread_mutex_lock(&g_app_tx_mtx);
        bool offered = g_cneg.valid && g_cneg.caller && g_cneg.session_id == session_id && g_cneg.offer;
        pthread_mutex_unlock(&g_app_tx_mtx);
        if (!offered)
        {
            HLOGI(LOG_COMP, "ACCEPT taking a compression offer we did not make: ignored");
            compress = false;
        }
    }
    /* An ACCEPT that takes an offer we did not make answers an earlier
     * CONNECT of the same session id: not ours (specs/connect, Agree). */
    if (is_accept && crypto)
    {
        pthread_mutex_lock(&g_app_tx_mtx);
        bool offered = g_neg.valid && g_neg.caller && g_neg.session_id == session_id && g_neg.offer;
        pthread_mutex_unlock(&g_app_tx_mtx);
        if (!offered)
        {
            HLOGI(LOG_COMP, "ACCEPT taking an encryption offer we did not make: ignored");
            return false;
        }
    }
    if (!is_accept &&
        arq_crypto_answer_call(caller, crypto) == ARQ_CRYPTO_ANSWER_REFUSE)
    {
        HLOGI(LOG_COMP, "Not answering %s: encryption is required and %s",
              caller, crypto ? "there is no key for that station"
                             : "the CALL did not offer it");
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
    ev.crypto     = crypto;
    ev.compress   = compress;
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
        .crypto_negotiate    = cb_crypto_negotiate,
        .compression_negotiate = cb_compression_negotiate,
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
