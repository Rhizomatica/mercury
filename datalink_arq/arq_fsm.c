/* HERMES Modem — ARQ FSM implementation
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_fsm.h"
#include "../modem/modem_mfsk.h"   /* MERCURY_MODE_MFSK, mercury_mode_is_mfsk */
#include "arq_protocol.h"
#include "arq_timing.h"
#include "arq.h"

#include <limits.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>

#include "../common/hermes_log.h"
#include "../common/virtual_clock.h"
#include "../modem/framer.h"
#include "../modem/freedv/freedv_api.h"

#define LOG_COMP  "arq-fsm"
#define INT_BUFFER_SIZE 4096

/* ======================================================================
 * State/event name tables
 * ====================================================================== */

const char *arq_conn_state_name(arq_conn_state_t s)
{
    static const char *names[] = {
        [ARQ_CONN_DISCONNECTED]  = "DISCONNECTED",
        [ARQ_CONN_LISTENING]     = "LISTENING",
        [ARQ_CONN_CALLING]       = "CALLING",
        [ARQ_CONN_ACCEPTING]     = "ACCEPTING",
        [ARQ_CONN_CONNECTED]     = "CONNECTED",
        [ARQ_CONN_DISCONNECTING] = "DISCONNECTING",
    };
    if ((unsigned)s < ARQ_CONN__COUNT) return names[s];
    return "UNKNOWN";
}

bool arq_conn_state_has_peer(arq_conn_state_t s)
{
    return s == ARQ_CONN_CONNECTED || s == ARQ_CONN_DISCONNECTING;
}

const char *arq_event_name(arq_event_id_t ev)
{
    static const char *names[] = {
        [ARQ_EV_APP_LISTEN]         = "APP_LISTEN",
        [ARQ_EV_APP_STOP_LISTEN]    = "APP_STOP_LISTEN",
        [ARQ_EV_APP_CONNECT]        = "APP_CONNECT",
        [ARQ_EV_APP_DISCONNECT]     = "APP_DISCONNECT",
        [ARQ_EV_APP_DATA_READY]     = "APP_DATA_READY",
        [ARQ_EV_RX_CALL]            = "RX_CALL",
        [ARQ_EV_RX_ACCEPT]          = "RX_ACCEPT",
        [ARQ_EV_RX_DISCONNECT]      = "RX_DISCONNECT",
        [ARQ_EV_TIMER_RETRY]        = "TIMER_RETRY",
        [ARQ_EV_TIMER_ACK]          = "TIMER_ACK",
        [ARQ_EV_TX_STARTED]         = "TX_STARTED",
        [ARQ_EV_TX_COMPLETE]        = "TX_COMPLETE",
        [ARQ_EV_RX_CAROUSEL]        = "RX_CAROUSEL",
        [ARQ_EV_TIMER_CAROUSEL]     = "TIMER_CAROUSEL",
        [ARQ_EV_RX_PATTERN]         = "RX_PATTERN",
    };
    if ((unsigned)ev < ARQ_EV__COUNT) return names[ev];
    return "UNKNOWN";
}

/* ======================================================================
 * Callbacks and timing context registry
 * ====================================================================== */

static arq_fsm_callbacks_t g_cbs;
static arq_timing_ctx_t   *g_timing;

static uint16_t car_seed(const arq_session_t *sess, bool is_caller);
static void car_set_seed(arq_session_t *sess, uint16_t seed);
static void car_connect_callee(arq_session_t *sess, const arq_event_t *ev);

void arq_fsm_set_callbacks(const arq_fsm_callbacks_t *cbs)
{
    if (cbs) g_cbs = *cbs;
}

void arq_fsm_set_timing(arq_timing_ctx_t *timing)
{
    g_timing = timing;
}

/* ======================================================================
 * arq_fsm_init / arq_fsm_timeout_ms
 * ====================================================================== */

void arq_fsm_init(arq_session_t *sess)
{
    car_t *car = sess->car;
    memset(sess, 0, sizeof(*sess));
    sess->car = car ? car : calloc(1, sizeof(car_t));
    sess->conn_state     = ARQ_CONN_DISCONNECTED;
    sess->deadline_ms    = UINT64_MAX;
    sess->deadline_event = ARQ_EV_TIMER_RETRY;
    sess->control_mode        = ARQ_CONTROL_MODE;
    sess->payload_mode        = FREEDV_MODE_DATAC15;
    sess->peer_tx_mode        = FREEDV_MODE_DATAC15;
    sess->floor_mode          = MERCURY_MODE_MFSK;
    sess->initial_payload_mode = FREEDV_MODE_DATAC15;  /* overwritten by arq_set_initial_mode */
}

void arq_fsm_release(arq_session_t *sess)
{
    free(sess->car);
    sess->car = NULL;
}

int arq_fsm_timeout_ms(const arq_session_t *sess, uint64_t now)
{
    if (sess->deadline_ms == UINT64_MAX) return INT_MAX;
    if (sess->deadline_ms <= now)        return 0;
    uint64_t diff = sess->deadline_ms - now;
    return (diff > (uint64_t)INT_MAX) ? INT_MAX : (int)diff;
}

/* ======================================================================
 * Internal helpers
 * ====================================================================== */

static void car_stop(arq_session_t *sess);
static bool fsm_connected_carousel(arq_session_t *sess, const arq_event_t *ev);
static void car_begin_disconnect(arq_session_t *sess, uint64_t now);
static bool abort_teardown(arq_session_t *sess, uint64_t now);

/* The station enters a connection state, at teardown step td (ARQ_TD_NONE
 * but for a reply owed to the peer's DISCONNECT): no step outlives its
 * state. */
static void sess_enter_td(arq_session_t *sess, arq_conn_state_t new_state, arq_teardown_t td,
                          uint64_t deadline_ms, arq_event_id_t deadline_event)
{
    HLOGD(LOG_COMP, "conn: %s -> %s",
          arq_conn_state_name(sess->conn_state),
          arq_conn_state_name(new_state));
    sess->teardown = td;
    if (new_state == ARQ_CONN_CONNECTED && sess->conn_state != ARQ_CONN_CONNECTED)
    {
        sess->host_released  = false;        /* a new session: a new reader */
    }
    if ((new_state == ARQ_CONN_DISCONNECTING || new_state == ARQ_CONN_CALLING) &&
        sess->conn_state != new_state)
    {
        sess->defer_count = 0;          /* a fresh teardown or call: a fresh budget */
    }
    /* The carousel runs only in CONNECTED; its seed lasts until the station is
     * idle, so the teardown still hears its peer. */
    if (new_state != ARQ_CONN_CONNECTED && !(new_state == ARQ_CONN_DISCONNECTING && !car_is_master(sess->car)))
    {
        /* The DISCONNECT exchange after it keeps the carousel's view of the
         * peer (session_ctl_mode): here, not in car_stop, which runs only
         * once the station is idle.  A callee ending the session keeps the
         * carousel running: its DISCONNECT goes in the carousel's slots. */
        if (sess->car_active)
            sess->ctl_floor_at_stop = car_peer_ctl_deaf(sess->car);
        sess->car_active = false;
    }
    if (new_state == ARQ_CONN_DISCONNECTED || new_state == ARQ_CONN_LISTENING)
        car_stop(sess);
    sess->conn_state     = new_state;
    sess->state_enter_ms = time_now_ms();
    sess->deadline_ms    = deadline_ms;
    sess->deadline_event = deadline_event;
    /* The right to complete a session from LISTENING is granted only by the
     * ACCEPT-exhaustion fallback, which sets it right after this call. */
    sess->accept_fallback = false;
    /* The right to key an ACCEPT is earned by hearing a CALL, and it does not
     * survive leaving ACCEPTING. */
    if (new_state != ARQ_CONN_ACCEPTING)
        sess->accept_tx_pending = false;
    /* Back to idle: restore peer_tx_mode to initial_payload_mode (= the
     * broadcast mode) so the payload decoder can receive broadcast frames
     * while LISTENING. */
    if (new_state == ARQ_CONN_DISCONNECTED || new_state == ARQ_CONN_LISTENING)
    {
        sess->peer_tx_mode      = sess->initial_payload_mode;
        sess->tx_inflight_bytes = 0;
    }
}

static void sess_enter(arq_session_t *sess, arq_conn_state_t new_state,
                       uint64_t deadline_ms, arq_event_id_t deadline_event)
{
    sess_enter_td(sess, new_state, ARQ_TD_NONE, deadline_ms, deadline_event);
}

/* The callee ends a carousel session in the carousel's slots (car_io_end),
 * and only then does the carousel run through DISCONNECTING (sess_enter). */
static bool callee_ending(const arq_session_t *sess)
{
    return sess->conn_state == ARQ_CONN_DISCONNECTING && sess->car_active;
}

/* A CONNECT that came during the teardown waits to be placed. */
static bool call_queued(const arq_session_t *sess)
{
    return sess->pending_connect_call[0] != '\0';
}

/* The peer's DISCONNECT is still being answered: its reply or, after it, the
 * host's DISCONNECTED. */
static bool answering_peer_end(const arq_session_t *sess)
{
    return sess->teardown == ARQ_TD_REPLYING || sess->teardown == ARQ_TD_NOTIFYING;
}


static void send_frame(int ptype, int mode, size_t len, const uint8_t *frame,
                       int burst_remaining)
{
    if (!g_cbs.send_tx_frame)
        return;

    /* Pad short frames (e.g. 8-byte control headers) to the modem slot size
     * so the action.frame_size check and fallback buffer path both pass. */
    const arq_mode_timing_t *tm = arq_protocol_mode_timing(mode);
    size_t slot = tm ? (size_t)tm->payload_bytes : len;
    if (len < slot) {
        uint8_t padded[INT_BUFFER_SIZE];
        memcpy(padded, frame, len);
        memset(padded + len, 0, slot - len);
        write_frame_header(padded, ptype, frame_header_extension(frame[0]));
        g_cbs.send_tx_frame(ptype, mode, slot, padded, burst_remaining);
        return;
    }

    g_cbs.send_tx_frame(ptype, mode, len, frame, burst_remaining);
}

static uint64_t deadline_from_s(float seconds)
{
    return time_now_ms() + (uint64_t)(seconds * 1000.0f + 0.5f);
}

static uint64_t retry_deadline_from_s(const arq_session_t *sess, float seconds)
{
    char my_call[CALLSIGN_MAX_SIZE];
    arq_conn_get_calls(my_call, NULL, NULL, sizeof(my_call));
    const char *local = sess->local_call[0] ? sess->local_call : my_call;
    int rank = arq_protocol_retry_rank(local, sess->remote_call);
    return arq_protocol_retry_deadline_ms(seconds, rank);
}


/* The application's TX stream: the bytes it has queued and not yet handed to
 * the carousel. */
static int session_tx_backlog(const arq_session_t *sess)
{
    (void)sess;
    return g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0;
}

static int session_tx_read(arq_session_t *sess, uint8_t *buf, size_t len)
{
    (void)sess;
    if (!g_cbs.tx_read)
        return 0;
    int n = g_cbs.tx_read(buf, len);
    return n > 0 ? n : 0;
}

int arq_call_carrier(const arq_session_t *sess)
{
    if (!sess) return ARQ_CONTROL_MODE;
    /* A station that can only transmit MFSK calls on MFSK from the first
     * CALL: its fast DATAC16 slots would never reach the peer. */
    if (ARQ_TX_MFSK_ONLY)
        return arq_floor_mode(arq_get_bw());
    if (sess->call_sends_done < ARQ_CALL_FAST_SLOTS)
        return sess->control_mode;
    /* The floor of my own bandwidth: the callee answers min(its own, mine),
     * and a BW500 station listens on the 500 Hz floor. */
    return ((sess->call_sends_done - ARQ_CALL_FAST_SLOTS) % 2 == 0)
         ? arq_floor_mode(arq_get_bw()) : sess->control_mode;
}

/* The carrier an ACCEPT to my last CALL may come back on: the CALL's own, or
 * my floor for a repeat on the control mode, which the callee may answer there
 * (accept_carrier).  The first CALL is no repeat. */
static int call_answer_carrier(const arq_session_t *sess)
{
    if (mercury_mode_is_mfsk(sess->call_carrier) || sess->call_sends_done < 2)
        return sess->call_carrier;
    return arq_floor_mode(arq_get_bw());
}

/* How long after my last CALL ends the next one is due: the call interval of
 * the carrier its ACCEPT may come back on (call_answer_carrier), not of the
 * CALL's own.  An ACCEPT on the floor to a repeat on the control mode is a
 * 13.5 s frame (26 s at 500 Hz): the control mode's 8 s interval keyed the
 * next CALL over it, held back only while a decoder of ours was on it
 * (call_must_wait), and at the SNR that wants the floor one seldom is.  On
 * air (KG9NI calling K7EK, 1.9.23, 2026-11-10) 5 of K7EK's 6 floor ACCEPTs
 * were keyed over, 3.7-4.9 s of each; no ACCEPT on the control mode got
 * through, so the floor's was the one that could have. */
static float call_interval_s(const arq_session_t *sess)
{
    return arq_protocol_call_interval_for_mode_s(call_answer_carrier(sess));
}

/* The callee's carrier for an ACCEPT to this CALL, of a session it has
 * answered already: the CALL's own, except a repeat on the control mode.  That
 * one says the caller heard none of our ACCEPTs there -- a weak return path:
 * on air (2026-10-21, sBitx calling an IC-7100) 0 of 3 DATAC16 ACCEPTs
 * arrived and every MFSK one did, and the caller's MFSK CALL then cost 22 s
 * more.  Answered on the floor the caller listens on after a repeat
 * (call_answer_carrier), if that is the session's floor too. */
static int accept_carrier(const arq_session_t *sess, const arq_event_t *ev)
{
    int floor = arq_floor_mode(ev->bw_hz);
    /* Not twice running: the caller's floor CALL after this one gets ours
     * on the floor anyway, and the control mode between is the fast path's
     * second chance.  (It was also there because the caller keyed over a
     * floor ACCEPT its decoders missed; it now waits one out,
     * call_interval_s, and specs/connect's Mut_noalt connects too.) */
    if (mercury_mode_is_mfsk(ev->mode) || floor != sess->floor_mode || sess->call_rx_mode == floor)
        return ev->mode;
    return floor;
}

/* Say why the ACCEPT to this CALL goes where it does, when that is the floor:
 * the CALL came on it, or it repeated one on the control mode, which
 * accept_carrier answers on the floor, where such a caller listens.  Logged
 * here, where the CALL is at hand; the ACCEPT goes out one guard later. */
static void log_accept_carrier(const arq_session_t *sess, const arq_event_t *ev)
{
    if (!mercury_mode_is_mfsk(sess->call_rx_mode))
        return;
    if (mercury_mode_is_mfsk(ev->mode))
        HLOGI(LOG_COMP, "CALL from %s on the MFSK floor: the ACCEPT goes there too",
              ev->remote_call);
    else
        HLOGI(LOG_COMP, "CALL from %s repeated on the control mode: the ACCEPT goes "
              "on the MFSK floor, where a repeating caller listens", ev->remote_call);
}

/* Does the ACCEPT go out, on the carrier chosen for it (call_rx_mode)?  Not
 * from a station that transmits only MFSK where that is the control mode:
 * it stays silent there, and answers the caller's next CALL, which the
 * caller sends on the floor or, a repeat, listens for there
 * (call_answer_carrier).  On the floor to a first CALL, the ACCEPT outlasts
 * the call interval and the caller, listening on the control mode only,
 * keys its repeat over it (specs/connect/ConnectCarrier.tla, MfskTx). */
static bool accept_keys(const arq_session_t *sess)
{
    return !ARQ_TX_MFSK_ONLY || mercury_mode_is_mfsk(sess->call_rx_mode);
}

/* The ACCEPT names the caller's start rung and says whether we hear it below
 * the control mode (then the rung is the floor).  A floor-only caller's first
 * control from us would otherwise be its only way to learn that, and at the
 * floor we answer with patterns, which carry no bits: a 1 KB transfer went by
 * without one, and its DISCONNECT then went out on a mode we could not hear
 * (harness, forward below DATAC16). */
static int accept_level(const arq_session_t *sess)
{
    return sess->car_ctl_deaf ? 0 : sess->car_rx_level;
}

/* The callee's wait, from the end of an ACCEPT on this carrier, for the
 * caller's first round: on the BW500 floor that round is a frame 13.5 s
 * longer than on the 1 kHz one. */
static uint64_t accept_rx_window_ms(int call_mode)
{
    if (!mercury_mode_is_mfsk(call_mode))
        return ARQ_ACCEPT_RX_WINDOW_MS;
    const arq_mode_timing_t *tm = arq_protocol_mode_timing(call_mode),
                            *t1 = arq_protocol_mode_timing(MERCURY_MODE_MFSK);
    float longer_s = tm && t1 ? tm->frame_duration_s - t1->frame_duration_s : 0.0f;
    return ARQ_ACCEPT_RX_WINDOW_FLOOR_MS + (uint64_t)(longer_s * 1000.0f + 0.5f);
}

static void send_call_accept(arq_session_t *sess, bool is_accept)
{
    uint8_t frame[INT_BUFFER_SIZE];
    int n;
    /* Read shared arq_conn through the locked accessors (g_conn_lock):
     * this runs on the FSM/event-loop thread while handle_cmd may be writing
     * my_call_sign/bw from the command-bridge thread. */
    char my_call[CALLSIGN_MAX_SIZE];
    arq_conn_get_calls(my_call, NULL, NULL, sizeof(my_call));
    int bw_hz = is_accept ? arq_reported_bandwidth_hz() : arq_get_bw();
    bool crypto = g_cbs.crypto_negotiate &&
                  g_cbs.crypto_negotiate(is_accept ? ARQ_NEG_ACCEPT_OUT : ARQ_NEG_CALL_OUT,
                                         sess->remote_call, sess->session_id, false);
    bool compress = g_cbs.compression_negotiate &&
                    g_cbs.compression_negotiate(is_accept ? ARQ_NEG_ACCEPT_OUT : ARQ_NEG_CALL_OUT,
                                                sess->remote_call, sess->session_id, false);
    if (is_accept)
    {
        /* The callsign the caller dialed: a station answering on a secondary
         * callsign is matched on that one's CRC. */
        const char *called = sess->local_call[0] ? sess->local_call : my_call;
        n = arq_protocol_build_accept(frame, sizeof(frame), sess->session_id,
                                      called, sess->remote_call, bw_hz,
                                      accept_level(sess), sess->car_ctl_deaf,
                                      sess->car_call_snr != 0.0f ? sess->car_call_snr : NAN,
                                      sess->car_nonce, ARQ_TX_MFSK_ONLY, crypto, compress);
        /* The ACCEPT is the carousel's first poll: from here the caller's
         * frames carry the session seed, and its first round comes on the rung
         * the ACCEPT names. */
        car_set_seed(sess, car_seed(sess, false));
        sess->peer_tx_mode = sess->car_rx_level == 0 ? sess->floor_mode
                                                     : car_level_mode(sess->car_rx_level);
    }
    else
        n = arq_protocol_build_call(frame, sizeof(frame), sess->session_id,
                                    my_call, sess->remote_call, bw_hz,
                                    ARQ_TX_MFSK_ONLY, crypto, compress);
    if (n > 0)
    {
        /* A CALL escalates to the MFSK floor once the fast slots are spent; an
         * ACCEPT answers on the carrier the CALL came on, so the two ends
         * never disagree about it.  A station that transmits only MFSK
         * sends both on MFSK (arq_call_carrier, accept_keys). */
        int mode = is_accept ? (sess->call_rx_mode > 0 ? sess->call_rx_mode : sess->control_mode)
                             : arq_call_carrier(sess);
        if (!is_accept) {
            sess->call_carrier = mode;
            sess->call_sends_done++;
            /* An ACCEPT to an MFSK CALL, or to a repeat, comes back on MFSK:
             * listen for it. */
            int answer = call_answer_carrier(sess);
            if (mercury_mode_is_mfsk(answer))
                sess->peer_tx_mode = answer;
        }
        /* Why an ACCEPT goes on the floor is logged where the CALL is at
         * hand (log_accept_carrier): the CALL came on it, or a repeat on the
         * control mode moved it there. */
        if (mode != sess->control_mode)
        {
            if (is_accept)
                HLOGI(LOG_COMP, "ACCEPT on the MFSK floor");
            else
                HLOGI(LOG_COMP, "CALL on the MFSK floor (the fast CALLs went unanswered)");
        }
        send_frame(PACKET_TYPE_ARQ_CALL2, mode, (size_t)n, frame, 0);
    }
    else
        /* Almost always an over-long callsign: the 10-byte SRC slot holds ~14
         * characters at ~5.25 bits each.  The encoder refuses rather than
         * truncating (a truncated arithmetic code decodes to a DIFFERENT
         * callsign), so say why — otherwise this is a CALL that never goes out
         * and a session that retries against silence. */
        HLOGW(LOG_COMP, "%s not sent: cannot encode callsign '%s' (too long?)",
              is_accept ? "ACCEPT" : "CALL", my_call);
}

/* The mode for the session's own control frames (DISCONNECT and the rest):
 * MFSK to a peer the carousel says hears us below the control mode.  On air
 * (car20, gateway at 2 %) the gateway's DISCONNECTs went on DATAC16 to an
 * estacao2 that could not hear them; estacao2 polled a closed session until
 * its uucico was killed. */
static int session_ctl_mode(const arq_session_t *sess)
{
    if (!sess->car)
        return ARQ_TX_MFSK_ONLY ? sess->floor_mode : sess->control_mode;
    bool deaf = sess->car_active ? car_peer_ctl_deaf(sess->car) : sess->ctl_floor_at_stop;
    return (deaf || ARQ_TX_MFSK_ONLY) ? sess->floor_mode : sess->control_mode;
}

/* The session's DISCONNECT: the one control frame a session sends outside
 * the carousel (plain CRC, so a peer that has dropped the seed still hears
 * it). */
static void send_disconnect_frame(arq_session_t *sess)
{
    uint8_t frame[INT_BUFFER_SIZE];
    uint8_t snr_raw = 0;
    if (sess->local_snr_x10 != 0)
        snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);
    int n = arq_protocol_build_disconnect(frame, sizeof(frame), sess->session_id, snr_raw);
    if (n > 0)
        send_frame(PACKET_TYPE_ARQ_CONTROL, session_ctl_mode(sess), (size_t)n, frame, 0);
}


/* ======================================================================
 * Level 1 FSM per-state handlers
 * ====================================================================== */


/* Is the peer transmitting right now, as far as this station can tell?
 *
 * Two independent signals, because neither is sufficient alone:
 *
 *  - A decoder holding sync on an incoming burst.  Precise when available --
 *    it means a frame in a waveform we are listening for is actually arriving.
 *  - Energy on the channel, via the busy detector.  Coarser, but it sees
 *    transmissions sync cannot: the acquisition window before sync is
 *    established, a mode neither decoder happens to be bound to, and a burst
 *    too weak to sync on at all.  Optional -- the detector is off unless the
 *    station enables it, so this is an improvement when present, not a
 *    dependency.
 *
 * What neither covers: if the peer's signal cannot be heard at all -- a deep
 * fade, or simply too little SNR to detect -- then no amount of
 * listen-before-talk helps, and the collision happens.  That is the hidden
 * transmitter problem and it is not solvable by listening; the carousel's
 * answer is turns the caller times and the callee answers in slots
 * (docs/CAROUSEL-ARQ.md).  So this check reduces the collision rate; it does
 * not eliminate it. */
static int decoder_hears_frame(const arq_session_t *sess);
/* By what sign (CAR_KEYED_*), 0: none. */
static int peer_is_transmitting(const arq_session_t *sess)
{
    if (g_cbs.channel_busy && g_cbs.channel_busy())
        return CAR_KEYED_BUSY;
    return decoder_hears_frame(sess);
}

int arq_fsm_channel_keyed(const arq_session_t *sess) { return peer_is_transmitting(sess); }

bool arq_fsm_notice_owed(const arq_session_t *sess)
{
    switch (sess->conn_state)
    {
    case ARQ_CONN_CALLING:
    case ARQ_CONN_ACCEPTING:
    case ARQ_CONN_CONNECTED:
    case ARQ_CONN_DISCONNECTING:
        return true;
    default:
        /* DISCONNECTED, answering the peer's DISCONNECT: the notice goes once
         * the reply is out (TX_COMPLETE in fsm_disconnected). */
        return answering_peer_end(sess);
    }
}

/* The first signal alone: one of our decoders is on a frame. */
static int decoder_hears_frame(const arq_session_t *sess)
{
    uint64_t now = time_now_ms();
    if (now < sess->rx_frame_busy_until_ms)
        return CAR_KEYED_PREAMBLE;   /* a frame whose preamble was caught: see arq_note_rx_preamble */
    if (now < sess->nav_busy_until_ms)
        return CAR_KEYED_NAV;        /* its NAV header was heard: see arq_note_rx_nav */
    if (sess->last_rx_sync_ms == 0)
        return 0;
    if (now < sess->last_rx_sync_ms)
        return CAR_KEYED_SYNC;       /* clock went backwards; treat as busy */
    return (now - sess->last_rx_sync_ms) < (uint64_t)ARQ_CHANNEL_SYNC_HOLD_MS ? CAR_KEYED_SYNC : 0;
}


/* Return to the pre-call idle status after an ARQ call ends: LISTENING if the
 * app has listen mode enabled, otherwise DISCONNECTED.  The connection status
 * is torn down only by the end of a call (or a TCP-client disconnect) and
 * always returns to where it was before the call rather than getting stuck. */
static void enter_idle_after_call(arq_session_t *sess)
{
    sess->disc_req_ms = 0;
    sess_enter(sess,
               sess->listen_enabled ? ARQ_CONN_LISTENING : ARQ_CONN_DISCONNECTED,
               UINT64_MAX, ARQ_EV_TIMER_RETRY);
}

/* Tell the host a session ended -- unless it has already asked for the next
 * call.  Every end-of-session notice goes through here, on every path (peer
 * ack, timeout, drain deadline, retry exhaustion, peer-initiated disconnect),
 * because a CONNECT deferred behind the teardown must not be followed by the
 * OLD session's DISCONNECTED: arriving after the host's new CONNECT, it reads
 * as "your call failed".  The call's own outcome is the next status the host
 * sees.  Skipping the callback also leaves the new call's addresses and TX
 * queue alone (arq.c's disconnect callback clears both). */
static void notify_session_ended(arq_session_t *sess)
{
    if (call_queued(sess))
        return;
    if (g_cbs.notify_disconnected)
        g_cbs.notify_disconnected(false);
}

/* The reply to the peer's DISCONNECT, if it is still owed: an ACK pattern.
 * 0.64 s on the air where a DISCONNECT frame is 3.7 s (13.5 s at the floor),
 * and heard ~10 dB deeper; the peer, its DISCONNECT out, expects one
 * (arq_fsm_expect_pattern).  The session id it lacks is not needed: only
 * an end that has already sent its DISCONNECT takes one as the answer. */
static void send_disconnect_reply(arq_session_t *sess)
{
    if (sess->teardown != ARQ_TD_REPLYING)
        return;
    sess->teardown = ARQ_TD_NOTIFYING;
    if (g_cbs.send_pattern)
        g_cbs.send_pattern(CAR_PATTERN_ACK);
    else
        send_disconnect_frame(sess);
}

/* ======================================================================
 * Carousel data plane
 *
 * CONNECTED runs datalink_arq/carousel.c.  The ACCEPT is the carousel's first
 * poll.  Its frames carry the session in their CRC (crc_seed), so a seeded
 * frame reaching the FSM is this session's by construction.
 * ====================================================================== */


bool arq_fsm_expect_pattern(const arq_session_t *sess, uint64_t now)
{
    (void)now;
    /* My DISCONNECT is out: the answer is an ACK pattern (send_disconnect_reply). */
    if (sess->conn_state == ARQ_CONN_DISCONNECTING && sess->teardown == ARQ_TD_SENT && !sess->tx_active)
        return true;
    return sess->car_active && sess->car && car_expect_pattern(sess->car);
}

/* The peer answered my DISCONNECT. */
static void disconnect_answered(arq_session_t *sess)
{
    HLOGI(LOG_COMP, "Disconnect finalized (peer ack)");
    notify_session_ended(sess);   /* silent if a call is queued behind it */
    if (g_timing) arq_timing_record_disconnect(g_timing, "peer_ack");
    enter_idle_after_call(sess);
}

/* Silence after which the peer is gone -- idle or not.  An idle caller polls
 * about once a minute (the 60 s backoff and the exchange), and when a poll
 * goes unanswered it asks again 3 s after it gives up on the answer
 * (carousel.c, m_decide_recv): a try every 15 s above the floor, 24-35 s at
 * the floor with DATAC16 control, 37 s with MFSK control (sim, frames lost).
 * So 240 s is a run of polls lost, not two or three at the backoff.  It used
 * to apply only with data in flight, and an idle session whose peer had left
 * polled it for ever (#384: 10 min, until the operator aborted). */
#define ARQ_CAR_PEER_LOST_MS 240000
/* Silence after which a DISCONNECT is not worth repeating: the peer misses
 * one, and its own lost-peer watchdog ends the session (#384: three tries,
 * 61 s, for a peer gone 10 min). */
#define ARQ_CAR_PEER_QUIET_MS 120000

/* When the lost-peer watchdog ends the session: ARQ_CAR_PEER_LOST_MS after
 * the last thing heard -- or, if the callee's answer to a keydown of mine
 * already on the air by then is still due, once that answer's slot is over.
 * At the floor an answer is a 27 s keydown, and the 240 s fell inside it: on
 * NW8L's IONOS MPD at -10 dB (1.9.23, BW500) the caller polled at the floor,
 * the callee keyed its answer at once, and the session ended 6 s before the
 * frame was through, the file never sent.  Only a keydown that ended by the
 * 240 s mark: one keyed after it is not waited for, so a caller asking
 * again and again does not hold the session open. */
static uint64_t car_lost_at(const arq_session_t *sess)
{
    uint64_t at = sess->car_last_rx_ms + ARQ_CAR_PEER_LOST_MS;
    uint64_t due = car_answer_due_ms(sess->car, at);
    return due > at ? due : at;
}

static bool car_peer_quiet(const arq_session_t *sess, uint64_t now)
{
    return sess->car_session && now - sess->car_last_rx_ms >= ARQ_CAR_PEER_QUIET_MS;
}

/* Silence after which a callee that has said its application hung up (M_END)
 * is gone.  Its last bytes, still to come, get what the callee itself gives
 * them: a callee hanging up with data held drains it for the configured drain
 * timeout or the carousel's budget for its rung, whichever is longer (the
 * APP_DISCONNECT case in fsm_connected_carousel).  Waiting less, the caller
 * would end while the callee still delivers, and a fade shorter than the
 * callee's drain would drop a frame that was coming.  The budget (78-131 s by
 * rung today) is past the 45 s default, so the timeout decides only when it
 * is configured longer (disconnect_drain_timeout_s), or if the budget
 * shrinks.  Counted from the last frame heard, which comes after the callee's
 * hang-up, so the caller never ends first; at most ARQ_CAR_PEER_QUIET_MS,
 * past which a DISCONNECT is not worth repeating either.  It used to be the
 * lost-peer watchdog, 240 s, on a session both applications had left (on
 * air, N3JXB -> KG9NI-1, 1.9.22: the M_END at 14:36:03, nine polls
 * unanswered, dropped at 14:40:03). */
static uint64_t car_peer_end_wait_ms(const arq_session_t *sess)
{
    uint64_t w = (uint64_t)ARQ_DISCONNECT_DRAIN_TIMEOUT_S * 1000ULL;
    uint64_t car_budget = car_drain_budget_ms(sess->car);
    if (car_budget > w) w = car_budget;
    return w < ARQ_CAR_PEER_QUIET_MS ? w : ARQ_CAR_PEER_QUIET_MS;
}

static void car_io_keydown(void *ctx, const car_frame_t *fr, int n)
{
    arq_session_t *sess = ctx;
    static arq_keydown_t kd;          /* event-loop thread; the callee copies */
    kd.n = n > ARQ_KEYDOWN_FRAMES ? ARQ_KEYDOWN_FRAMES : n;
    kd.crc_seed = sess->crc_seed;
    kd.nav = car_wants_nav(sess->car);
    for (int i = 0; i < kd.n; i++)
    {
        kd.f[i].mode   = fr[i].mode;
        kd.f[i].gap_ms = fr[i].gap_ms;
        kd.f[i].len    = fr[i].len <= ARQ_KEYDOWN_FRAME_MAX ? fr[i].len : ARQ_KEYDOWN_FRAME_MAX;
        memcpy(kd.f[i].bytes, fr[i].bytes, kd.f[i].len);
    }
    if (g_cbs.send_keydown)
        g_cbs.send_keydown(&kd);
    if (g_timing)
    {
        uint64_t now = time_now_ms();
        g_timing->frames_tx += (uint64_t)kd.n;
        arq_timing_record_turn(kd.n,
                               sess->car_last_rx_ms ? (int64_t)(now - sess->car_last_rx_ms) : -1,
                               sess->last_tx_end_ms ? (int64_t)(now - sess->last_tx_end_ms) : -1);
    }
}

static void car_io_bind_rx(void *ctx, int mode) { ((arq_session_t *)ctx)->peer_tx_mode = mode; }

static void car_io_pattern(void *ctx, int kind)
{
    (void)ctx;
    if (g_cbs.send_pattern)
        g_cbs.send_pattern(kind == CAR_PATTERN_BREAK ? ARQ_PATTERN_BREAK : ARQ_PATTERN_ACK);
}
static int peer_is_transmitting(const arq_session_t *sess);
static int car_io_peer_keyed(void *ctx) { return peer_is_transmitting((arq_session_t *)ctx); }

static size_t car_io_tx_read(void *ctx, uint8_t *buf, size_t max)
{
    int n = session_tx_read((arq_session_t *)ctx, buf, max);
    if (n > 0 && g_timing) arq_timing_record_car_tx(g_timing, n);
    return n > 0 ? (size_t)n : 0;
}

static size_t car_io_tx_pending(void *ctx)
{
    int n = session_tx_backlog((arq_session_t *)ctx);
    return n > 0 ? (size_t)n : 0;
}

static void car_io_deliver(void *ctx, const uint8_t *buf, size_t len)
{
    arq_session_t *sess = ctx;
    /* Nothing reaches an application that has already ended the session (see
     * deliver_rx_checked). */
    if (!sess->host_released && g_cbs.deliver_rx_data)
    {
        g_cbs.deliver_rx_data(buf, len);
        if (g_timing) arq_timing_record_car_rx(g_timing, (int)len);
    }
}

/* BUFFER is what the peer has not confirmed: the application's queue plus
 * the carousel's open blocks (tx_inflight_bytes, which the runtime adds to
 * the backlog in its own periodic BUFFER report). */
static void car_io_tx_confirmed(void *ctx, size_t len)
{
    arq_session_t *sess = ctx;
    (void)len;
    sess->tx_inflight_bytes = (int)car_tx_inflight(sess->car);
    if (g_cbs.send_buffer_status)
        g_cbs.send_buffer_status(session_tx_backlog(sess) + sess->tx_inflight_bytes);
}

/* The callee's session nonce: unique, not secret -- a session's frames must
 * not pass as the next one's between the same two stations.  The clock and a
 * counter, mixed (splitmix64). */
static uint16_t session_nonce(void)
{
    static uint64_t count;
    uint64_t z = time_now_ms() + 0x9E3779B97F4A7C15ULL * ++count;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (uint16_t)(z ^ (z >> 31));
}

/* The session's CRC seed: both ends know the session id, the callee's nonce
 * and start rung for the caller (in the ACCEPT) and both callsigns once the
 * ACCEPT has been heard.  Never
 * 0, which means "plain CRC". */
static uint16_t car_seed(const arq_session_t *sess, bool is_caller)
{
    char me[CALLSIGN_MAX_SIZE];
    arq_conn_get_calls(me, NULL, NULL, sizeof(me));
    /* The callee may have been dialed on a secondary callsign: the one the
     * caller wrote in its CALL is what both ends hash. */
    const char *callee_me = sess->local_call[0] ? sess->local_call : me;
    const char *caller = is_caller ? me : sess->remote_call;
    const char *callee = is_caller ? sess->remote_call : callee_me;
    unsigned char buf[4 + 2 * CALLSIGN_MAX_SIZE];
    int n = 0;
    buf[n++] = sess->session_id;
    buf[n++] = (unsigned char)sess->car_nonce;
    buf[n++] = (unsigned char)(sess->car_nonce >> 8);
    /* The piece size the carousel will cut this session into: two ends
     * that would read each other's pieces at another size get different
     * seeds, and hear nothing (car_piece_for_start). */
    buf[n++] = (unsigned char)car_piece_for_start(is_caller ? sess->car_tx_level : sess->car_rx_level);
    for (const char *p = caller; *p && n < (int)sizeof(buf) - 1; p++)
        buf[n++] = (unsigned char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
    buf[n++] = '/';
    for (const char *p = callee; *p && n < (int)sizeof(buf); p++)
        buf[n++] = (unsigned char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
    uint16_t seed = freedv_gen_crc16(buf, n);
    return seed ? seed : 0x5A5A;
}

/* A session's seed comes once both ends know its bandwidth (the callee from
 * the CALL, the caller from the ACCEPT), and with it the session's floor:
 * its patterns, too, go at that geometry. */
static void car_set_seed(arq_session_t *sess, uint16_t seed)
{
    sess->crc_seed = seed;
    if (seed) {
        sess->floor_mode = arq_floor_mode(arq_reported_bandwidth_hz());
        if (g_cbs.set_floor_mode)
            g_cbs.set_floor_mode(sess->floor_mode);
    }
    if (g_cbs.set_crc_seed)
        g_cbs.set_crc_seed(seed);
}

static void abort_fallback(arq_session_t *sess, uint64_t now);

/* The callee's DISCONNECT, in the slot the carousel was given (car_request_end). */
static void car_io_end(void *ctx)
{
    arq_session_t *sess = ctx;
    /* ABORT's one DISCONNECT, keyed outside the slots, is out already. */
    if (sess->tx_retries_left == 0 && sess->teardown == ARQ_TD_SENT)
        return;
    HLOGI(LOG_COMP, "carousel: DISCONNECT in my slot");
    send_disconnect_frame(sess);
    sess->teardown = ARQ_TD_SENT;
    /* ABORT's: it ends at its TX_COMPLETE, not at the key-by time left in
     * teardown_deadline_ms (abort_teardown); this is the fallback. */
    if (sess->tx_retries_left == 0)
        abort_fallback(sess, time_now_ms());
}

static void car_io_trace(void *ctx, const char *line)
{
    (void)ctx;
    HLOGD("carousel", "%s", line);
}

/* The top rung this session's negotiated bandwidth allows (BW500 stops at
 * DATAC3).  Both ends hold the same bandwidth once the ACCEPT is through:
 * the callee answers with min(its own, the CALL's), the caller takes it. */
static int session_max_level(void)
{
    return car_max_level_for_bw(arq_reported_bandwidth_hz());
}

static int session_start_level(float snr_db)
{
    int lv = car_start_level(snr_db), top = session_max_level();
    return lv > top ? top : lv;
}

static void car_start(arq_session_t *sess, bool is_caller, uint64_t now)
{
    car_io_t io = {
        .keydown = car_io_keydown, .bind_rx = car_io_bind_rx, .peer_keyed = car_io_peer_keyed,
        .tx_read = car_io_tx_read, .tx_pending = car_io_tx_pending, .deliver = car_io_deliver,
        .tx_confirmed = car_io_tx_confirmed, .ctx = sess,
        .pattern = g_cbs.send_pattern ? car_io_pattern : NULL,
        .trace = car_io_trace,
        .end = car_io_end,
        .block_key = sess->crc_seed,
    };
    /* NAV headers below 0 dB (as this end hears the peer: the path is
     * reciprocal), where the peer's decoders may not sync on my keydowns.
     * MERCURY_NAV=0 turns them off (A/B on air). */
    const char *nav_env = getenv("MERCURY_NAV");
    bool nav_on = !(nav_env && !strcmp(nav_env, "0"));
    car_set_nav_ms(nav_on ? ARQ_NAV_LEAD_MS : 0);
    HLOGI(LOG_COMP, "carousel: NAV headers %s", nav_on ? "below 0 dB" : "off (MERCURY_NAV=0)");
    car_set_nav_below_db(0.0f);
    /* Small backlogs on the shortest rung (#384); MERCURY_SHORT_RUNG=0 turns
     * it off (A/B on air).  Off, the hint bits stay zero: the peer sees an
     * older station. */
    const char *sr_env = getenv("MERCURY_SHORT_RUNG");
    car_set_short_rung(!(sr_env && !strcmp(sr_env, "0")));
    car_set_nav_loss(2.0);
    sess->nav_busy_until_ms = 0;
    car_init(sess->car, &io, sess->car_rx_level, sess->car_tx_level);
    car_set_max_level(sess->car, session_max_level());
    car_set_floor_mode(sess->car, sess->floor_mode);
    car_seed_ctl_deaf(sess->car, sess->car_ctl_deaf,
                      ARQ_TX_MFSK_ONLY ? CAR_DEAF_MFSK_TX : sess->car_peer_ctl_deaf);
    car_seed_snr(sess->car, sess->car_my_snr, is_caller ? sess->car_call_snr : 0.0f);
    sess->ctl_floor_at_stop = false;
    sess->car_active = true;
    sess->car_session = true;
    sess->car_last_rx_ms = now;
    if (is_caller)
        car_start_sender(sess->car, now);
    else
        car_start_receiver(sess->car, now);
}

static void car_stop(arq_session_t *sess)
{
    sess->car_active = false;
    sess->car_session = false;
    /* The reply to the peer's DISCONNECT is an ACK pattern, and patterns are
     * the session's (the seed keys them: modem_apply_crc_seed): the seed
     * stays until it has gone out.  Cleared first, the reply went out on the
     * station-wide patterns and the peer, still in the session, never
     * detected it (harness, a callee ending at the floor: 3 runs of 3). */
    if (sess->crc_seed && sess->teardown != ARQ_TD_REPLYING)
        car_set_seed(sess, 0);
}

/* The carousel can no longer trust the session (car_failed): a block failed
 * its check or did not decode.  Part of it may already be with the
 * application, so end the session -- as ABORT does, dropping what is not
 * delivered -- and let the application see the link go rather than carry on
 * with wrong data.  True when it did. */
static bool car_fail_closed(arq_session_t *sess, uint64_t now)
{
    const char *why = car_failed(sess->car);
    if (!why) return false;
    HLOGE(LOG_COMP, "carousel: %s -- ending the session", why);
    car_begin_disconnect(sess, now);
    return true;
}


/* A carousel session ends (docs/CAROUSEL-TURNS.md).  The caller keys its
 * DISCONNECT clear of every slot the callee could be using (see
 * fsm_disconnecting); the callee keys nothing on timers of its own: its
 * DISCONNECT goes in its next slot, again in the slots after any keydown of
 * the caller's that did not hear it, and it gives up silently at the end. */
#define ARQ_CAR_END_WAIT_MS 180000
static void car_begin_disconnect(arq_session_t *sess, uint64_t now)
{
    /* disc_req_ms is 0 when the host has not hung up: the peer's
     * application did, a drain gave up, the link failed.  The line then
     * claimed "0 ms after the host's DISCONNECT" for a DISCONNECT that came
     * later or never (N3JXB -> VE4KLM, 2026-11-10 18:23:51: the host's came
     * 30 s later, during the teardown). */
    if (sess->disc_req_ms)
        HLOGI(LOG_COMP, "Teardown starts (%s), %llu ms after the host's DISCONNECT",
              car_is_master(sess->car) ? "caller" : "callee",
              (unsigned long long)(now - sess->disc_req_ms));
    else
        HLOGI(LOG_COMP, "Teardown starts (%s)",
              car_is_master(sess->car) ? "caller" : "callee");
    sess->teardown_deadline_ms = 0;
    bool quiet = car_peer_quiet(sess, now);
    sess->tx_retries_left = quiet ? 0 : ARQ_DISCONNECT_RETRY_SLOTS;
    if (car_is_master(sess->car))
    {
        if (quiet)
            HLOGI(LOG_COMP, "nothing from the peer in %llu s: one DISCONNECT, no retries",
                  (unsigned long long)((now - sess->car_last_rx_ms) / 1000));
        sess_enter(sess, ARQ_CONN_DISCONNECTING, now + ARQ_CHANNEL_GUARD_MS, ARQ_EV_TIMER_ACK);
        return;
    }
    /* The callee keys only in slots of the caller's keydowns: with none heard
     * in this long there is no slot to wait for. */
    if (quiet)
    {
        HLOGI(LOG_COMP, "Disconnect finalized (nothing from the caller in %llu s)",
              (unsigned long long)((now - sess->car_last_rx_ms) / 1000));
        notify_session_ended(sess);
        if (g_timing) arq_timing_record_disconnect(g_timing, "timeout");
        enter_idle_after_call(sess);
        return;
    }
    sess->teardown_deadline_ms = now + ARQ_CAR_END_WAIT_MS;
    car_request_end(sess->car);
    sess_enter(sess, ARQ_CONN_DISCONNECTING, sess->teardown_deadline_ms, ARQ_EV_TIMER_CAROUSEL);
}

/* ABORT: VARA's "disconnect the link immediately".  The peer is still told
 * (#218), but once: one DISCONNECT, no retries, and nothing waited for past
 * it.  Going through the retries kept the rig keying for a peer that had
 * gone quiet (on air, N3JXB -> KG9NI-1, 1.9.23: ABORT 80 s into the
 * silence, under ARQ_CAR_PEER_QUIET_MS, so three DISCONNECTs over 53 s; the
 * CONNECT to K7EK typed meanwhile waited 34 s of them).  An ABORT during a
 * teardown already running did nothing at all, and hosts send exactly that
 * when the DISCONNECTED is slow to come: BPQ32 30 s after its DISCONNECT,
 * Pat 60 s.
 *
 * The caller: its DISCONNECT goes once, clear of the peer's slots as ever,
 * and the session ends when its answer comes or one retry interval passes.
 * With one already out it is the same: no more retries, and the interval
 * running is waited out.  Ending at once there, a redial keyed its CALL
 * over the peer's reply, to a peer still replying that takes no CALL (sim:
 * both keyed 640 ms, connected after 24.0 s against 13.8 s; specs/teardown,
 * Mut_abortnow).
 * The callee: its DISCONNECT goes in its next slot only, not again in the
 * slots after.  One already out and off the air has told the caller: the
 * session ends now.  tx_retries_left at 0 marks it: the callee's DISCONNECT
 * is not retried on the FSM's timers, and its 0 otherwise means a quiet
 * caller, which ends the session at once (car_begin_disconnect).
 *
 * If no slot comes by abort_key_by(), the callee's goes out of turn, on a
 * clear channel only: the host takes the radio soon after its ABORT.  BPQ32
 * does so as it sends it: CheckForDetach (linbpq CommonCode.c) sends the
 * ABORT through ForcedClose and goes straight on to CloseComplete, whose
 * VARAReleaseTNC sends LISTEN ON and SCANSTART 15, whatever our DISCONNECTED
 * says.  On air (K7EK, 2026-11-10) LISTEN ON came in the ABORT's millisecond
 * and the first hop's LISTEN OFF 16.6 s later.  The callee's slot wait ran to
 * 30 s, and the peer was never told.  teardown_deadline_ms holds the key-by
 * time.
 *
 * The ABORT's one DISCONNECT is never keyed over a peer we hear or know to be
 * keyed: a half-duplex peer can't hear it, and it is not retried (#531
 * review).  So the caller's keeps to the peer's slots as ever (keyed before
 * them, it told no peer the wait did not), and its channel deferral waits
 * out a keydown it hears past the usual cap (ARQ_ABORT_HEARD_MAX); the
 * callee's out-of-turn one is held while the channel is busy, and the slot
 * after the caller's keydown carries it.  True when the session has ended
 * here. */
#define ARQ_CAR_ABORT_WAIT_MS 30000   /* the callee's DISCONNECT on the air at the ABORT: its end at most */
#define ARQ_ABORT_RELEASE_MS  15000   /* BPQ32: the scan's first hop after an ABORT (SCANSTART 15) */
#define ARQ_ABORT_MARGIN_MS    1000

/* When ABORT's DISCONNECT keys at the latest: off the air a margin before
 * the host's scan takes the radio.  A frame too long for that (the MFSK
 * floor at 500 Hz: 26 s) keys at once. */
static uint64_t abort_key_by(const arq_session_t *sess, uint64_t now)
{
    const arq_mode_timing_t *tm = arq_protocol_mode_timing(session_ctl_mode(sess));
    uint64_t air = tm ? (uint64_t)(tm->frame_duration_s * 1000.0f) : 4400;
    return air + ARQ_ABORT_MARGIN_MS < ARQ_ABORT_RELEASE_MS
         ? now + ARQ_ABORT_RELEASE_MS - air - ARQ_ABORT_MARGIN_MS : now;
}

/* The callee's ABORTed DISCONNECT is keyed: the session ends at its
 * TX_COMPLETE; this deadline is the fallback. */
static void abort_fallback(arq_session_t *sess, uint64_t now)
{
    const arq_mode_timing_t *tm = arq_protocol_mode_timing(session_ctl_mode(sess));
    sess->teardown_deadline_ms = now + (tm ? (uint64_t)(tm->frame_duration_s * 1000.0f) : 4400) +
                                 ARQ_CAR_ABORT_WAIT_MS;
}

/* The callee's ABORTed DISCONNECT, not yet out, has reached its key-by time. */
static bool abort_must_key(const arq_session_t *sess, uint64_t now)
{
    return callee_ending(sess) && sess->tx_retries_left == 0 &&
           sess->teardown != ARQ_TD_SENT && sess->teardown_deadline_ms != 0 &&
           now >= sess->teardown_deadline_ms;
}

/* When the channel is ours to key on, as far as we can tell: our last
 * keydown's reply window over (a frame we invited has started by then and a
 * decoder holds sync on it), and the peer quiet for one reply guard.  With
 * peer_is_transmitting() false, as disconnect_must_wait() uses it. */
static uint64_t channel_clear_at(const arq_session_t *sess)
{
    uint64_t guard  = (uint64_t)ARQ_CHANNEL_GUARD_MS;
    uint64_t window = (uint64_t)(ARQ_ISS_POST_ACK_GUARD_MS > ARQ_CHANNEL_GUARD_MS
                                     ? ARQ_ISS_POST_ACK_GUARD_MS : ARQ_CHANNEL_GUARD_MS) +
                      ARQ_REPLY_WINDOW_MARGIN_MS;
    uint64_t clear_at = 0;
    if (sess->last_tx_end_ms)
        clear_at = sess->last_tx_end_ms + window;
    if (sess->last_rx_sync_ms && sess->last_rx_sync_ms + guard > clear_at)
        clear_at = sess->last_rx_sync_ms + guard;
    return clear_at;
}

/* How many ARQ_CHANNEL_DEFER_MS holds the out-of-turn DISCONNECT waits for a
 * clear channel past its key-by time: to about the 30 s mercuryv2 gave the
 * slot wait. */
#define ARQ_ABORT_HOLD_MAX ((ARQ_CAR_ABORT_WAIT_MS - ARQ_ABORT_RELEASE_MS) / ARQ_CHANNEL_DEFER_MS)
/* ...and how many the caller's waits while it hears the peer on the air. */
#define ARQ_ABORT_HEARD_MAX (ARQ_CAR_ABORT_WAIT_MS / ARQ_CHANNEL_DEFER_MS)

static bool abort_teardown(arq_session_t *sess, uint64_t now)
{
    if (sess->conn_state != ARQ_CONN_DISCONNECTING)
        return false;
    sess->tx_retries_left = 0;
    if (callee_ending(sess) && sess->teardown == ARQ_TD_SENT && !sess->tx_active)
    {
        HLOGI(LOG_COMP, "ABORT: our DISCONNECT is out -- ending now, the peer's answer not awaited");
        notify_session_ended(sess);
        if (g_timing) arq_timing_record_disconnect(g_timing, "abort");
        enter_idle_after_call(sess);
        return true;
    }
    if (sess->teardown == ARQ_TD_SENT)
    {
        /* On the air now (the callee), or waiting out its answer (the
         * caller): nothing more to key. */
        if (callee_ending(sess) && sess->teardown_deadline_ms > now + ARQ_CAR_ABORT_WAIT_MS)
            sess->teardown_deadline_ms = now + ARQ_CAR_ABORT_WAIT_MS;
        HLOGI(LOG_COMP, "ABORT: our DISCONNECT is out -- no more retries");
        return false;
    }
    if (!callee_ending(sess))
    {
        HLOGI(LOG_COMP, "ABORT: one DISCONNECT, no retries");
        return false;
    }
    sess->teardown_deadline_ms = abort_key_by(sess, now);
    sess->defer_count = 0;
    HLOGI(LOG_COMP, "ABORT: one DISCONNECT, no retries: in my next slot, or out of turn in %llu ms "
          "on a clear channel", (unsigned long long)(sess->teardown_deadline_ms - now));
    return false;
}

/* The caller's DISCONNECT: when it would run into a slot of the callee's,
 * later.  True when it waits. */
static bool car_disconnect_must_wait(arq_session_t *sess, const arq_event_t *ev, uint64_t now)
{
    if (!sess->car_session || !car_is_master(sess->car))
        return false;
    /* A peer silent ARQ_CAR_PEER_QUIET_MS has no slots in use to keep clear,
     * and its one DISCONNECT has no retry to lose (car_begin_disconnect).
     * Waiting for them held a DISCONNECT 17 s after the host's at the floor,
     * for a peer silent 128 s (on air, N3JXB -> VE4KLM, 1.9.22). */
    if (car_peer_quiet(sess, now))
        return false;
    const arq_mode_timing_t *tm = arq_protocol_mode_timing(session_ctl_mode(sess));
    uint64_t len = 110 + (tm ? (uint64_t)(tm->frame_duration_s * 1000.0f) : 4400) + 200;
    uint64_t at = car_free_at(sess->car, now, len);
    if (at <= now)
        return false;
    HLOGD(LOG_COMP, "DISCONNECT waits %llu ms for the peer's slots", (unsigned long long)(at - now));
    sess->deadline_ms    = at;
    sess->deadline_event = ev->id;
    return true;
}

/* Once the carousel has nothing in flight either way, a deferred DISCONNECT
 * can go. */
static bool car_drained(const arq_session_t *sess)
{
    return car_is_drained(sess->car) && session_tx_backlog(sess) == 0;
}

static void fsm_disconnected(arq_session_t *sess, const arq_event_t *ev)
{
    switch (ev->id)
    {
    case ARQ_EV_TIMER_ACK:
        send_disconnect_reply(sess);    /* the reply guard has expired */
        break;

    case ARQ_EV_TX_COMPLETE:
        /* Deferred from RX_DISCONNECT: fire now that DISCONNECT ACK is sent,
         * giving the TCP data thread time to drain data_rx_buffer_arq. */
        if (sess->teardown == ARQ_TD_NOTIFYING)
        {
            notify_session_ended(sess);
            /* Call fully torn down — restore the pre-call idle status. */
            enter_idle_after_call(sess);
        }
        break;

    case ARQ_EV_APP_LISTEN:
        /* Still finishing the peer's teardown: the listen intent is recorded
         * (listen_enabled) and enter_idle_after_call() restores LISTENING once
         * our reply is out.  Entering it now would drop the reply's timer. */
        if (answering_peer_end(sess))
            break;
        sess_enter(sess, ARQ_CONN_LISTENING, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        break;

    case ARQ_EV_APP_CONNECT:
        send_disconnect_reply(sess);    /* owed first; the CALL queues behind it */
        snprintf(sess->remote_call, CALLSIGN_MAX_SIZE, "%s", ev->remote_call);
        /* Our call goes out from the primary: drop the callsign the last
         * incoming CALL dialed, which retry_deadline_from_s would otherwise
         * rank this call with. */
        sess->local_call[0] = '\0';
        sess->session_id      = (uint8_t)(time_now_ms() & ARQ_CONNECT_SESSION_MASK) | 0x01;
        sess->tx_retries_left = ARQ_CALL_RETRY_SLOTS;
        sess->call_sends_done = 0;
        sess->call_carrier    = 0;
        sess->payload_mode       = FREEDV_MODE_DATAC15;
        sess->peer_tx_mode       = FREEDV_MODE_DATAC15;
        send_call_accept(sess, false);
        /* the first CALL's interval: the floor's from a station that
         * transmits only MFSK, or its next CALL keys behind this one */
        sess_enter(sess, ARQ_CONN_CALLING,
                   deadline_from_s(call_interval_s(sess)),
                   ARQ_EV_TIMER_RETRY);
        break;

    default:
        break;
    }
}


/* Stop hearing a caller we answered until our ACCEPTs ran out (fsm_accepting,
 * TIMER_RETRY): its seed drops every plain frame, broadcast included. */
static void end_accept_fallback(arq_session_t *sess)
{
    if (!sess->accept_fallback || !sess->crc_seed)
        return;
    sess->accept_fallback = false;
    car_set_seed(sess, 0);
    sess->peer_tx_mode = sess->initial_payload_mode;   /* back to broadcast */
}

static void fsm_listening(arq_session_t *sess, const arq_event_t *ev)
{
    switch (ev->id)
    {
    case ARQ_EV_RX_CALL:
    {
        /* The same caller calling again in the same session (back here after
         * our ACCEPTs ran out): it may have decoded one of them, so it gets
         * the same ACCEPT -- the rung, the deafness and the nonce it named.
         * Ends that read different ACCEPTs would cut different pieces and
         * seed different CRCs (docs/CAROUSEL-ARQ.md, "Wire formats"). */
        bool same_session = sess->accept_fallback && ev->session_id == sess->session_id &&
                            strcasecmp(ev->remote_call, sess->remote_call) == 0;
        snprintf(sess->remote_call, CALLSIGN_MAX_SIZE, "%s", ev->remote_call);
        snprintf(sess->local_call, CALLSIGN_MAX_SIZE, "%s", ev->local_call);
        sess->session_id      = ev->session_id;
        /* a repeat after our ACCEPTs ran out: answered as in ACCEPTING */
        sess->call_rx_mode    = same_session ? accept_carrier(sess, ev) : ev->mode;
        log_accept_carrier(sess, ev);
        if (!same_session)
        {
            /* The answer to its encryption offer, once for the session: a
             * repeat gets the same, like the nonce (specs/connect, Agree:
             * answered anew, a re-CONNECT now offering decoded the old
             * answer's ACCEPT still in the air). */
            if (g_cbs.crypto_negotiate)
                g_cbs.crypto_negotiate(ARQ_NEG_CALL_IN, ev->remote_call, ev->session_id, ev->crypto);
            if (g_cbs.compression_negotiate)
                g_cbs.compression_negotiate(ARQ_NEG_CALL_IN, ev->remote_call, ev->session_id, ev->compress);
            /* A new session takes the CALL's bandwidth; a repeat keeps the
             * session's, like its nonce and rung: the caller adopts whatever
             * an ACCEPT of it says (specs/connect/ConnectHandshake.tla). */
            arq_set_session_bw(ev->bw_hz, true);
            sess->car_nonce    = session_nonce();
            sess->car_rx_level = session_start_level(ev->rx_snr);   /* the ACCEPT names it */
            /* How the caller hears us it reports in its first frame, which
             * comes before we send any control.  (Not "an MFSK CALL means it
             * cannot hear us": the caller escalates just the same when it is
             * we who cannot hear its control-mode CALLs.)  But we may not
             * hear them: a CALL that reached us only on MFSK says so whatever
             * its SNR -- under a Doppler spread no OFDM mode decodes, and a
             * session started on DATAC1 by the SNR sent its first round into
             * a callee that never heard it (Go harness, ccir-flutter, itur-ld,
             * -hm, -hd: 0 bytes at every SNR).  The floor then, until an OFDM
             * frame proves otherwise (carousel.c, CAR_DEAF_HARD).  For good
             * from a caller that transmits only MFSK (CAR_DEAF_MFSK_TX). */
            sess->car_ctl_deaf = ev->tx_mfsk ? CAR_DEAF_MFSK_TX
                               : ev->rx_snr < ARQ_SNR_MIN_DATAC15_DB || mercury_mode_is_mfsk(ev->mode);
            if (sess->car_ctl_deaf) sess->car_rx_level = 0;
            sess->car_call_snr = ev->rx_snr;
            sess->car_my_snr   = ev->rx_snr;
        }
        sess->car_peer_ctl_deaf = false;
        sess->tx_retries_left = ARQ_ACCEPT_RETRY_SLOTS;
        sess->accept_tx_pending = accept_keys(sess);   /* answering a CALL we just heard */
        /* Off the listen mode: here, not in sess_enter, because LISTENING
         * keeps peer_tx_mode on the broadcast mode.  The carousel binds the
         * decoder to the peer's rung once it runs. */
        sess->payload_mode       = FREEDV_MODE_DATAC15;
        sess->peer_tx_mode       = FREEDV_MODE_DATAC15;
        /* Do NOT send ACCEPT immediately: the caller's PTT-OFF may not have
         * happened yet when we decode the last samples of their CALL frame.
         * Wait ARQ_CHANNEL_GUARD_MS so their relay is in RX before we TX. */
        sess_enter(sess, ARQ_CONN_ACCEPTING,
                   time_now_ms() + ARQ_CHANNEL_GUARD_MS,
                   ARQ_EV_TIMER_RETRY);
        if (g_cbs.notify_pending)
            g_cbs.notify_pending(ev->remote_call, ev->local_call);
        break;
    }

    case ARQ_EV_RX_ACCEPT:
        /* We gave up CALLING (retries exhausted) and returned to LISTENING,
         * but the callee is still retrying ACCEPT from our earlier CALL.
         * We already told the TNC "DISCONNECTED", so we can't reconnect.
         * Send a DISCONNECT to tell the peer to stop retrying. */
        if (ev->session_id == sess->session_id)
        {
            HLOGI(LOG_COMP, "Stale ACCEPT in LISTENING — sending DISCONNECT to peer");
            send_disconnect_frame(sess);
        }
        break;

    case ARQ_EV_APP_CONNECT:
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        fsm_disconnected(sess, ev);
        break;

    case ARQ_EV_APP_STOP_LISTEN:
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        break;

    case ARQ_EV_RX_CAROUSEL:
        if (sess->accept_fallback && sess->crc_seed)
            car_connect_callee(sess, ev);
        break;

    case ARQ_EV_TIMER_RETRY:
        /* The ACCEPT-exhaustion fallback ran out: the caller never came. */
        end_accept_fallback(sess);
        break;

    case ARQ_EV_RX_DISCONNECT:
        /* The caller we stopped answering has given up: no frame of that
         * session is coming, so stop waiting for one (see fsm_accepting). */
        if (sess->accept_fallback && sess->crc_seed && ev->session_id == sess->session_id)
        {
            HLOGI(LOG_COMP, "DISCONNECT from the caller we answered: no longer waiting for it");
            end_accept_fallback(sess);
        }
        break;

    default:
        break;
    }
}

/* Has this call gone on past arq:call_max_s (0: no cap)?  From entering
 * CALLING, which happens once per call (APP_CONNECT). */
static bool call_over_time(const arq_session_t *sess)
{
    int cap_s = atomic_load(&arq_call_max_s);
    return cap_s > 0 && time_now_ms() - sess->state_enter_ms >= (uint64_t)cap_s * 1000;
}

/* A CALL waits while a frame we caught is arriving (decoder_hears_frame; not
 * the energy busy detector, off by default and not to hold CALLs): an ACCEPT
 * that came late, or another station's frame, is no place for a CALL.  (The
 * ACCEPT to a repeat, on the floor, no longer outlasts the interval:
 * call_interval_s.)  Once held, also until that frame could have decoded: a CALL keyed
 * as the ACCEPT ended went out from a caller connected a moment later, and the
 * callee answered it over the caller's first round (sim, asym:-4:-9).  At most
 * one floor frame's airtime and its decode, then we key regardless. */
static bool call_must_wait(arq_session_t *sess)
{
    const arq_mode_timing_t *tm = arq_protocol_mode_timing(arq_floor_mode(arq_get_bw()));
    float frame_ms = (tm ? tm->frame_duration_s : 13.5f) * 1000.0f;
    unsigned cap = (unsigned)((frame_ms + ARQ_CALL_DECODE_WAIT_MS) / ARQ_CHANNEL_DEFER_MS) + 1;
    uint64_t now = time_now_ms();
    bool busy = decoder_hears_frame(sess) ||
                (sess->defer_count > 0 && sess->last_rx_sync_ms &&
                 now < sess->last_rx_sync_ms + ARQ_CALL_DECODE_WAIT_MS);
    if (!busy || sess->defer_count >= cap)
    {
        sess->defer_count = 0;
        return false;
    }
    if (sess->defer_count++ == 0)
        HLOGD(LOG_COMP, "CALL held: a frame is arriving");
    sess->deadline_ms = now + ARQ_CHANNEL_DEFER_MS;
    return true;
}

static void fsm_calling(arq_session_t *sess, const arq_event_t *ev)
{
    switch (ev->id)
    {
    case ARQ_EV_RX_ACCEPT:
        /* Ours only if it answers this session and comes from the station we
         * called: the session id, our own CRC (checked on arrival) and the
         * called station's CRC -- the callee's callsign is not on the air. */
        if (ev->session_id != sess->session_id ||
            ev->car_called_crc != arq_protocol_callsign_crc16(sess->remote_call))
            break;
        arq_set_session_bw(ev->bw_hz, false);
        sess->car_peer_ctl_deaf = ev->car_ctl_deaf;
        sess->car_tx_level = sess->car_peer_ctl_deaf ? 0 : ev->car_level;
        sess->car_call_snr = ev->car_call_snr;
        if (sess->car_tx_level > session_max_level())
            sess->car_tx_level = session_max_level();
        sess->car_rx_level = session_start_level(ev->rx_snr);
        /* An ACCEPT heard only on MFSK, to an MFSK CALL: as the callee's MFSK
         * CALL above.  One to a repeat on the control mode (accept_carrier)
         * says only that an ACCEPT there was lost -- the callee decoded our
         * control-mode CALLs -- so the SNR decides: taken as deafness, a lost
         * frame's worth of bad luck put a good link's control on the floor
         * (sim, nvis: a callee's DISCONNECT 12.4 -> 17.1 s).  For good from a
         * callee that transmits only MFSK (CAR_DEAF_MFSK_TX). */
        sess->car_ctl_deaf = ev->tx_mfsk ? CAR_DEAF_MFSK_TX
                           : ev->rx_snr < ARQ_SNR_MIN_DATAC15_DB ||
                             (mercury_mode_is_mfsk(ev->mode) && mercury_mode_is_mfsk(sess->call_carrier));
        if (sess->car_ctl_deaf) sess->car_rx_level = 0;
        sess->car_my_snr   = ev->rx_snr;
        if (g_cbs.crypto_negotiate)
            g_cbs.crypto_negotiate(ARQ_NEG_ACCEPT_IN, sess->remote_call, sess->session_id, ev->crypto);
        if (g_cbs.compression_negotiate)
            g_cbs.compression_negotiate(ARQ_NEG_ACCEPT_IN, sess->remote_call, sess->session_id, ev->compress);
        if (g_cbs.notify_connected)
            g_cbs.notify_connected(sess->remote_call, sess->local_call, true);
        if (g_timing)
            arq_timing_record_connect(g_timing, sess->control_mode);
        sess_enter(sess, ARQ_CONN_CONNECTED, UINT64_MAX, ARQ_EV_TIMER_CAROUSEL);
        sess->car_nonce = ev->car_nonce;
        car_set_seed(sess, car_seed(sess, true));
        car_start(sess, true, time_now_ms());
        /* The car exists now (car_start car_init's it), so the session's
         * compression decision reaches it (#506). */
        if (g_cbs.compression_negotiate &&
            g_cbs.compression_negotiate(ARQ_NEG_SESSION_UP, sess->remote_call, sess->session_id, false))
            car_set_zstd(sess->car, true);
        break;

    case ARQ_EV_TX_COMPLETE:
        /* CALL frame just finished transmitting: re-anchor the retry deadline
         * to PTT-OFF rather than to whenever the frame was queued.
         *
         * fsm_accepting already does this for ACCEPT; CALLING never got the
         * same treatment, and the arithmetic is unforgiving.  The retry
         * interval is measured from the TIMER_RETRY that *queued* the CALL,
         * but the CALL itself occupies ~3.7 s of DATAC16 airtime, and the peer
         * cannot even begin its ACCEPT until our PTT drops.  With the default
         * interval that leaves the retransmission firing at roughly the same
         * moment the ACCEPT is arriving -- so the retry keys the transmitter on
         * top of the reply it was waiting for, on essentially every connect.
         * Measuring the interval from here gives the peer a full turnaround. */
        sess->deadline_ms = retry_deadline_from_s(sess, call_interval_s(sess));
        break;

    case ARQ_EV_TIMER_RETRY:
        /* No new CALL past arq:call_max_s, whatever RETRIES left: BPQ32's
         * RETRIES 20 kept a station calling an unanswering path for ~8 min
         * (K7EK, 18 times in two days).  Checked when a CALL comes due, so
         * the last one keyed still has its full wait for the ACCEPT. */
        if (sess->tx_retries_left > 0 && call_over_time(sess))
        {
            HLOGI(LOG_COMP, "CALL: %d s of calling (arq:call_max_s), %d CALLs not sent -- giving up",
                  (int)((time_now_ms() - sess->state_enter_ms) / 1000), sess->tx_retries_left);
            sess->tx_retries_left = 0;
        }
        if (sess->tx_retries_left > 0 && call_must_wait(sess))
            break;
        if (sess->tx_retries_left > 0)
        {
            sess->tx_retries_left--;
            send_call_accept(sess, false);
            /* Deadline is re-anchored on TX_COMPLETE above; this is the
             * fallback if that event is ever missed. */
            sess->deadline_ms = retry_deadline_from_s(sess, call_interval_s(sess));
        }
        else
        {
            notify_session_ended(sess);
            enter_idle_after_call(sess);
        }
        break;

    /* VARA-compatible: LISTEN OFF drops the link, it does not merely stop
     * accepting new ones.  A host asserting a transmitter interlock (BPQ32
     * INTERLOCK, frequency scanners) sends LISTEN OFF to mean "release the
     * radio now" — honouring it only in LISTENING left us retrying CALL/ACCEPT
     * over another port that had already taken the channel.
     *
     * Acted on immediately here, with no grace period.  The grace in ACCEPTING
     * exists for one specific race: a scanning host can send LISTEN OFF just
     * before it processes the PENDING we sent it.  PENDING announces an INCOMING
     * call, so it is never sent while we are CALLING — there is nothing for the
     * host to be racing against, and delaying its channel release would only
     * keep a radio it asked for. */
    case ARQ_EV_APP_STOP_LISTEN:
    case ARQ_EV_APP_DISCONNECT:
        notify_session_ended(sess);
        enter_idle_after_call(sess);
        break;

    default:
        break;
    }
}

/* The callee hears its caller's first carousel frame: the session is up.
 * From ACCEPTING, or from LISTENING after the ACCEPT-exhaustion fallback. */
static void car_connect_callee(arq_session_t *sess, const arq_event_t *ev)
{
    sess->accept_fallback = false;
    sess->car_tx_level = -1;          /* the caller's frames will say */
    if (g_cbs.notify_connected)
        g_cbs.notify_connected(sess->remote_call, sess->local_call, false);
    if (g_timing)
        arq_timing_record_connect(g_timing, sess->control_mode);
    /* A LISTEN OFF deferred by the grace period is honoured here, read before
     * entering CONNECTED resets it: dropped on the way, the successful
     * handshake silently swallowed the host's channel release. */
    bool release = sess->teardown == ARQ_TD_LISTEN_OFF;
    sess_enter(sess, ARQ_CONN_CONNECTED, UINT64_MAX, ARQ_EV_TIMER_CAROUSEL);
    if (release)
    {
        HLOGI(LOG_COMP, "connected with a deferred LISTEN OFF — releasing the radio");
        if (g_timing) arq_timing_record_disconnect(g_timing, "listen_off");
        notify_session_ended(sess);
        enter_idle_after_call(sess);
        return;
    }
    uint64_t now = time_now_ms();
    car_start(sess, false, now);
    if (g_cbs.compression_negotiate &&
        g_cbs.compression_negotiate(ARQ_NEG_SESSION_UP, sess->remote_call, sess->session_id, false))
        car_set_zstd(sess->car, true);
    if (g_timing) g_timing->frames_rx++;
    car_on_frame(sess->car, now, ev->payload, ev->payload_len, ev->mode, ev->from_control, ev->rx_snr,
                     ev->rx_age_ms);
    car_fail_closed(sess, now);
}

static void fsm_accepting(arq_session_t *sess, const arq_event_t *ev)
{
    switch (ev->id)
    {
    case ARQ_EV_RX_CAROUSEL:
        if (sess->crc_seed)
            car_connect_callee(sess, ev);
        break;

    case ARQ_EV_RX_CALL:
        /* Another station's CALL, or another session's: we are answering this
         * one, so it goes unanswered. */
        if (ev->session_id != sess->session_id ||
            strcasecmp(ev->remote_call, sess->remote_call) != 0)
        {
            HLOGI(LOG_COMP, "CALL from %s while answering %s: ignored", ev->remote_call, sess->remote_call);
            break;
        }
        /* Caller is still retrying CALL (our previous ACCEPT was lost). Reset
         * the retry counter so the ACCEPTING window stays open long enough for
         * the caller to decode the next ACCEPT and start sending data.
         *
         * ...and re-anchor the retry to NOW, rather than letting the deadline
         * set at TX_COMPLETE run its full course.  That deadline is sized to
         * hold the RX window open for the caller's first DATA burst, so it is
         * many seconds long.  Waiting it out here is wrong twice over:
         *
         *   - It is the wrong question.  A CALL just arrived, which is proof
         *     the caller is still in CALLING and has NOT begun a data burst,
         *     so there is nothing for the long window to protect.
         *   - It desynchronises us from the caller.  The caller retries CALL
         *     every arq_protocol_call_interval_s(), so a retransmitted ACCEPT
         *     drifts into the middle of a CALL, and the caller is deaf over
         *     its own transmission.
         *
         * Answering one channel guard after the CALL puts the ACCEPT in the
         * gap the caller has just opened by dropping PTT -- the same schedule
         * fsm_listening uses for the first ACCEPT, and for the same reason. */
        sess->tx_retries_left   = ARQ_ACCEPT_RETRY_SLOTS;
        sess->deadline_ms       = time_now_ms() + ARQ_CHANNEL_GUARD_MS;
        /* The same ACCEPT again -- the rung, the deafness and the nonce of the
         * first: the caller may decode any of them, and ends that read
         * different ones would cut different pieces and seed different CRCs.
         * (It re-measured here once, and the ends could disagree: spec R25.)
         * Answer on the carrier THIS CALL came on: a caller escalates when it
         * hears none of our ACCEPTs -- a weak return path -- and an answer
         * kept on DATAC16 kept failing the same way (#235).  A repeat on
         * DATAC16 says the same thing, sooner: the floor (accept_carrier). */
        sess->call_rx_mode      = accept_carrier(sess, ev);
        log_accept_carrier(sess, ev);
        sess->accept_tx_pending = accept_keys(sess);   /* answering a CALL we just heard */
        /* And PENDING again.  A scanning host holds its channel for a fixed
         * time per PENDING -- BPQ32 six scan intervals (VARA.c: "Time out
         * after 6 Scanintervals"), then hops, and the hop's LISTEN OFF
         * cancels us.  On air (K7EK <- KG9NI-7, 30 m, 2026-11-05) BPQ hopped
         * 84 s after the first PENDING, 17 s after we had answered a repeat
         * of the CALL: the caller was still there.  A CALL we hear is the
         * proof it is, so it holds the channel for another stretch. */
        HLOGI(LOG_COMP, "CALL from %s again: PENDING again (holds a scanning host's channel)",
              sess->remote_call);
        if (g_cbs.notify_pending_again)
            g_cbs.notify_pending_again(sess->remote_call);
        break;

    case ARQ_EV_TX_COMPLETE:
        /* ACCEPT frame just finished transmitting.  The peer (caller) will
         * start its first DATA frame (DATAC15, ~4400 ms) almost immediately
         * after our PTT drops.  The deadline that was set in TIMER_RETRY was
         * relative to when TIMER_RETRY fired — not to TX_COMPLETE — so it
         * only left ~4400 ms of RX window after PTT-OFF, which is barely
         * one DATAC15 frame.  Reset the deadline here so we always have
         * a full ARQ_ACCEPT_RX_WINDOW_MS window (guard + DATAC15 frame +
         * margin) measured from the moment our TX actually ends. */
        sess->deadline_ms = time_now_ms() + accept_rx_window_ms(sess->call_rx_mode);
        /* This deadline is a LISTENING window, not a retransmission timer.  An
         * ACCEPT is only ever correct one channel guard after a CALL we heard,
         * because that is the only moment we know the caller has dropped PTT
         * and is listening.  Firing one when this window expires has no phase
         * relationship to the caller at all, and lands in the middle of its
         * next CALL, where a half-duplex radio is deaf.
         *
         * This is a protocol invariant, not a reliability fix: the caller is
         * deaf over its own transmission, so a blind ACCEPT is wrong whether
         * or not it happens to collide in any given run.
         *
         * So mark the pending timer as "window", not "ACCEPT".  If the caller
         * is still calling we will hear it and answer that; if we cannot hear
         * it, transmitting blind only destroys someone else's burst. */
        sess->accept_tx_pending = false;
        break;

    case ARQ_EV_TIMER_RETRY:
        if (sess->teardown == ARQ_TD_LISTEN_OFF &&
            time_now_ms() - sess->state_enter_ms >= ARQ_LISTEN_OFF_GRACE_MS)
        {
            if (g_cbs.notify_cancelpending)
                g_cbs.notify_cancelpending();
            notify_session_ended(sess);
            enter_idle_after_call(sess);
            break;
        }
        if (sess->tx_retries_left > 0)
        {
            sess->tx_retries_left--;
            if (!sess->accept_tx_pending)
            {
                /* The RX window closed without the caller coming back.  Stay
                 * off the air -- see the TX_COMPLETE comment above -- and wait
                 * again, so the budget still bounds how long we hold the
                 * pending call open. */
                sess->deadline_ms = retry_deadline_from_s(sess, arq_protocol_call_interval_s());
                break;
            }
            /* Answering a CALL we heard: the caller dropped PTT one channel
             * guard ago and is listening now. */
            sess->accept_tx_pending = false;
            send_call_accept(sess, true);
            /* deadline is now managed via TX_COMPLETE above; set a generous
             * fallback here in case TX_COMPLETE is missed for any reason */
            sess->deadline_ms = retry_deadline_from_s(sess, arq_protocol_call_interval_s());
        }
        else
        {
            if (g_cbs.notify_cancelpending)
                g_cbs.notify_cancelpending();
            uint16_t seed = sess->crc_seed;
            int rx_mode = sess->peer_tx_mode;
            sess_enter(sess, ARQ_CONN_LISTENING, UINT64_MAX, ARQ_EV_TIMER_RETRY);
            sess->accept_fallback = true;
            /* Keep hearing the caller's carousel frames: it may be connected
             * already, and its first round is what completes the session. */
            if (seed)
            {
                car_set_seed(sess, seed);
                sess->peer_tx_mode = rx_mode;
                /* ...but not past the point where a connected caller would
                 * have given up on us: a seed left set drops every plain
                 * frame, broadcast included. */
                sess->deadline_ms = time_now_ms() + ARQ_CAR_PEER_LOST_MS;
            }
        }
        break;

    case ARQ_EV_RX_DISCONNECT:
        /* The caller has given up on this call.  Its host hung up while it was
         * still CALLING, and our ACCEPT reached it afterwards: it answers that
         * stale ACCEPT with a DISCONNECT (fsm_listening, RX_ACCEPT).  Nothing
         * more is coming, so release the channel now, as a host DISCONNECT
         * would.  On air (K7EK <- KG9NI-7, 2026-11-07 06:06 and 2026-11-08
         * 10:43) the DISCONNECT was decoded and ignored, and the callee held a
         * scanning host's channel 47 s and 104 s more, until BPQ32's PENDING
         * hold ran out.  No reply: the caller is back in LISTENING, expecting
         * none.  Only for this session: another station's DISCONNECT, or an
         * older session's, says nothing about this call. */
        if (ev->session_id != sess->session_id)
            break;
        HLOGI(LOG_COMP, "DISCONNECT from %s while answering its CALL: the caller gave up",
              sess->remote_call);
        if (g_cbs.notify_cancelpending)
            g_cbs.notify_cancelpending();
        notify_session_ended(sess);
        enter_idle_after_call(sess);
        break;

    case ARQ_EV_APP_CONNECT:
        /* UUCP retried CONNECT while we're still accepting a previous call.
         * Abort the accept cycle and start calling.  The remote has likely
         * given up its CALLING attempt already (its retries exhausted), so
         * continuing to send ACCEPTs is pointless.  Transition through
         * DISCONNECTED → CALLING so the new session gets a fresh ID. */
        if (g_cbs.notify_cancelpending)
            g_cbs.notify_cancelpending();
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        fsm_disconnected(sess, ev);
        break;

    /* See fsm_calling: LISTEN OFF releases the radio.  This is the state the
     * BPQ32 interlock report lands in — an inbound CALL we are answering with
     * ACCEPT retries. */
    case ARQ_EV_APP_STOP_LISTEN:
        if (time_now_ms() - sess->state_enter_ms < ARQ_LISTEN_OFF_GRACE_MS)
        {
            sess->teardown = ARQ_TD_LISTEN_OFF;
            sess->deadline_ms = sess->state_enter_ms + ARQ_LISTEN_OFF_GRACE_MS;
            break;
        }
        /* fall through */
    case ARQ_EV_APP_DISCONNECT:
        if (g_cbs.notify_cancelpending)
            g_cbs.notify_cancelpending();
        notify_session_ended(sess);
        enter_idle_after_call(sess);
        break;

    default:
        break;
    }
}

/* A CONNECT that arrived during teardown: place it now that the station is idle.
 * Called from arq_fsm_dispatch() once the station is DISCONNECTED or LISTENING,
 * both of which handle APP_CONNECT as a fresh call -- one place, so every way a
 * session can end places the call, not only the ones that were thought of. */
static void fire_deferred_connect(arq_session_t *sess)
{
    if (!call_queued(sess))
        return;
    arq_event_t ev = { .id = ARQ_EV_APP_CONNECT };
    snprintf(ev.remote_call, CALLSIGN_MAX_SIZE, "%s", sess->pending_connect_call);
    sess->pending_connect_call[0] = '\0';
    HLOGI(LOG_COMP, "Teardown complete: placing deferred call to %s", ev.remote_call);
    arq_fsm_dispatch(sess, &ev);
}

/* Hold a DISCONNECT back until it cannot land on a frame we invited.
 *
 * On air (bench run 12) the host hung up while a frame handing the peer the
 * turn was on the air.  The DISCONNECT keyed 42 ms after it ended -- and the
 * peer keyed its reply 0.9 s later; both were lost, and the DISCONNECT retry
 * clipped the reply's tail.  Listening alone cannot fix that: the invited
 * frame starts after the guard, so at the moment we would key there is
 * nothing yet to hear.
 *
 * So: never while we are still keyed; not before our last transmission's
 * reply window has passed, by which time an invited frame has started and a
 * decoder holds sync on it; and not until the peer has been quiet for one
 * reply guard, so its radio is back on receive.  Bounded like the other
 * deferrals: after ARQ_CHANNEL_DEFER_MAX steps we key regardless -- counted
 * from the end of our own keydown (ARQ_CHANNEL_DEFER_KEYED_MAX). */
static bool disconnect_must_wait(arq_session_t *sess)
{
    uint64_t now = time_now_ms();
    unsigned cap = sess->tx_active ? ARQ_CHANNEL_DEFER_KEYED_MAX : ARQ_CHANNEL_DEFER_MAX;
    /* ABORT's one DISCONNECT is not retried: keyed over a peer we can hear,
     * it is lost, and the peer sits out its watchdog.  It waits out a long
     * keydown of the peer's instead (sim, #531 review: a 19.5 s round of the
     * callee's, the cap reached 10 s into it). */
    if (sess->tx_retries_left == 0 && !sess->tx_active && peer_is_transmitting(sess))
        cap = ARQ_ABORT_HEARD_MAX;
    if (sess->defer_count >= cap)
    {
        HLOGW(LOG_COMP, "DISCONNECT deferral cap reached (%u) - keying anyway",
              (unsigned)sess->defer_count);
        sess->defer_count = 0;
        return false;
    }
    uint64_t clear_at = channel_clear_at(sess);

    if (!sess->tx_active && !peer_is_transmitting(sess) && now >= clear_at)
    {
        sess->defer_count = 0;
        return false;
    }
    sess->defer_count++;
    uint64_t next = now + ARQ_CHANNEL_DEFER_MS;
    if (!sess->tx_active && !peer_is_transmitting(sess) && clear_at > now)
        next = clear_at;           /* the only thing left is the window */
    HLOGD(LOG_COMP, "DISCONNECT deferred (%s) %u/%u",
          sess->tx_active ? "still keyed" :
          peer_is_transmitting(sess) ? "peer transmitting" : "reply window",
          (unsigned)sess->defer_count, (unsigned)ARQ_CHANNEL_DEFER_MAX);
    sess->deadline_ms = next;
    return true;
}

static void fsm_disconnecting(arq_session_t *sess, const arq_event_t *ev)
{
    const arq_mode_timing_t *tm;
    uint64_t now = time_now_ms();

    /* Our keydown is over: the DISCONNECT's deferral budget starts now
     * (ARQ_CHANNEL_DEFER_KEYED_MAX). */
    if (ev->id == ARQ_EV_TX_COMPLETE)
        sess->defer_count = 0;

    /* The callee ending a carousel session: the carousel keeps its slots, and
     * its next one carries our DISCONNECT (car_io_end). */
    if (callee_ending(sess))
    {
        switch (ev->id)
        {
        case ARQ_EV_RX_CAROUSEL:
            car_on_frame(sess->car, now, ev->payload, ev->payload_len, ev->mode, ev->from_control, ev->rx_snr,
                     ev->rx_age_ms);
            return;
        case ARQ_EV_RX_PATTERN:
            /* My DISCONNECT went in a slot instead of an answer, so the
             * caller's ACK now is its answer to it: the caller acks only
             * rounds, and I send none while ending. */
            if (sess->teardown == ARQ_TD_SENT && !ev->pattern_break)
            {
                disconnect_answered(sess);
                return;
            }
            car_on_pattern(sess->car, now, ev->pattern_break ? CAR_PATTERN_BREAK
                                                                              : CAR_PATTERN_ACK);
            return;
        case ARQ_EV_TX_COMPLETE:
            /* ABORT's one DISCONNECT is off the air (abort_teardown). */
            if (sess->teardown == ARQ_TD_SENT && sess->tx_retries_left == 0)
            {
                HLOGI(LOG_COMP, "Disconnect finalized (ABORT: my one DISCONNECT is out)");
                notify_session_ended(sess);
                if (g_timing) arq_timing_record_disconnect(g_timing, "abort");
                enter_idle_after_call(sess);
                return;
            }
            car_on_tx_done(sess->car, now);
            return;
        case ARQ_EV_TIMER_CAROUSEL:
            /* ABORT's DISCONNECT, no slot of the caller's in time: it goes
             * now, before the host's scan takes the radio (abort_key_by). */
            if (abort_must_key(sess, now))
            {
                /* Never over a keydown, ours or the caller's: a half-duplex
                 * caller can't hear it, and with no retries it would be lost.
                 * Held, the slots go on: the caller's keydown is followed by
                 * my slot, which carries it (car_io_end). */
                if (!sess->tx_active && !peer_is_transmitting(sess) && now >= channel_clear_at(sess))
                {
                    HLOGI(LOG_COMP, "ABORT: DISCONNECT keyed out of turn on a clear channel, the host's scan due");
                    send_disconnect_frame(sess);
                    sess->teardown = ARQ_TD_SENT;
                    abort_fallback(sess, now);
                    return;
                }
                if (sess->defer_count++ >= ARQ_ABORT_HOLD_MAX)
                {
                    HLOGI(LOG_COMP, "Disconnect finalized (ABORT: the channel never came clear, the caller not told)");
                    notify_session_ended(sess);
                    if (g_timing) arq_timing_record_disconnect(g_timing, "abort");
                    enter_idle_after_call(sess);
                    return;
                }
                sess->teardown_deadline_ms = now + ARQ_CHANNEL_DEFER_MS;
            }
            else if (now >= sess->teardown_deadline_ms)
            {
                HLOGI(LOG_COMP, "Disconnect finalized (timeout: the caller never answered)");
                notify_session_ended(sess);
                if (g_timing) arq_timing_record_disconnect(g_timing, "timeout");
                enter_idle_after_call(sess);
                return;
            }
            car_on_time(sess->car, now);
            return;
        default:
            break;
        }
    }

    switch (ev->id)
    {
    case ARQ_EV_TX_COMPLETE:
        /* The caller's DISCONNECT is out: keep the callee's reply slot clear
         * of the next one. */
        if (sess->car_session && car_is_master(sess->car))
            car_log_ctl(sess->car, now, ARQ_CHANNEL_GUARD_MS);
        break;

    case ARQ_EV_APP_CONNECT:
        /* A new call while the previous one is still tearing down.
         *
         * This used to fall through to default and be DROPPED -- no call, no
         * reply -- while the control port had already answered the command
         * with OK.  A client that redials promptly (uucp does; so did the
         * loopsim driver) waited for a CONNECTED that could never come.
         *
         * Placing the call over the teardown is not an option: the DISCONNECT
         * exchange is still on the air.  So remember it and place it the
         * moment the teardown completes, which gives the client the ordinary
         * outcome of a CONNECT -- CONNECTED, or DISCONNECTED if the call itself
         * fails.  The old session's own teardown notice is suppressed then
         * (see fire_deferred_connect below): arriving after the new CONNECT,
         * it would read as "your call failed". */
        snprintf(sess->pending_connect_call, CALLSIGN_MAX_SIZE, "%s", ev->remote_call);
        HLOGI(LOG_COMP, "CONNECT to %s deferred until the teardown completes",
              sess->pending_connect_call);
        break;

    case ARQ_EV_APP_DISCONNECT:
        /* The host changed its mind about a deferred call: drop it.  A
         * DISCONNECT lets the teardown in progress carry on; an ABORT cuts
         * it short (abort_teardown). */
        if (call_queued(sess))
        {
            HLOGI(LOG_COMP, "Deferred CONNECT to %s cancelled", sess->pending_connect_call);
            sess->pending_connect_call[0] = '\0';
        }
        if (ev->abort && abort_teardown(sess, now))
            return;
        break;

    case ARQ_EV_APP_STOP_LISTEN:
        /* Stop retransmitting DISCONNECT frames: the host wants the radio
         * free, and the peer will time out without them.  That also rules
         * out a deferred call, so drop it and say so -- otherwise the client
         * would wait for a CONNECTED that is not coming.
         *
         * The session ends here as on every other way out of DISCONNECTING:
         * through notify_session_ended, which logs "Disconnected" and clears
         * the connection's addresses and queue.  Called only for a queued
         * call, it left an end with no "Disconnected" in the log at all (on
         * air, KG9NI -> KE8CGS, 1.9.22: the DISCONNECT keyed, LISTEN OFF 2 s
         * later, and the log just stops). */
        HLOGI(LOG_COMP, "LISTEN OFF during the DISCONNECT exchange: releasing the radio, "
              "the peer's ack not awaited");
        if (g_timing) arq_timing_record_disconnect(g_timing, "listen_off");
        sess->pending_connect_call[0] = '\0';
        notify_session_ended(sess);
        enter_idle_after_call(sess);
        return;

    case ARQ_EV_TIMER_ACK:
        /* Initial DISCONNECT send after channel guard. */
        if (car_disconnect_must_wait(sess, ev, now) || disconnect_must_wait(sess))
            break;
        send_disconnect_frame(sess);
        sess->teardown = ARQ_TD_SENT;
        tm = arq_protocol_mode_timing(session_ctl_mode(sess));
        sess->deadline_ms    = retry_deadline_from_s(sess, tm ? tm->retry_interval_s : 7.0f);
        sess->deadline_event = ARQ_EV_TIMER_RETRY;
        HLOGD(LOG_COMP, "Disconnect tx (initial, after guard)");
        break;

    case ARQ_EV_RX_DISCONNECT:
        /* Both ends hung up.  If ours has gone out the peer has it, and its
         * DISCONNECT is our answer.  If not -- held back while the peer's was
         * on the air -- the peer has nothing from us: finalizing silently left
         * it retrying for 21 s, deaf to the CALL that came next (on air, car24,
         * gateway <- IC-7100: the redial failed).  Answer it as CONNECTED
         * does, one reply guard later. */
        if (sess->teardown != ARQ_TD_SENT)
        {
            HLOGI(LOG_COMP, "Disconnect crossed the peer's before ours went out: replying");
            if (g_timing) arq_timing_record_disconnect(g_timing, "peer_ack");
            sess_enter_td(sess, ARQ_CONN_DISCONNECTED, ARQ_TD_REPLYING,
                          time_now_ms() + ARQ_CHANNEL_GUARD_MS, ARQ_EV_TIMER_ACK);
            break;
        }
        disconnect_answered(sess);
        break;

    case ARQ_EV_RX_PATTERN:
        /* The answer to my DISCONNECT (send_disconnect_reply). */
        if (sess->teardown == ARQ_TD_SENT && !ev->pattern_break)
            disconnect_answered(sess);
        break;

    case ARQ_EV_RX_CALL:
        /* The peer we are tearing down with is calling again: it has left the
         * old session, so there is nothing left to wait for.  Retrying on, the
         * CALL was ignored, and our next DISCONNECT keyed over its retry.  A
         * callee whose DISCONNECT is out waits only for its answer, and a
         * station calls with another MYCALL per application: on air the
         * gateway's UUCP called as PU2UIT, its NNCP as PU2UIT-3, and a callee
         * whose answer was lost ignored the NNCP CALLs for the 180 s of its
         * end wait.  So that one takes a CALL for it from anyone
         * (specs/teardown, Mut_samecall).  Not with a call of our own queued:
         * that one goes out first. */
        if (!call_queued(sess) && sess->listen_enabled &&
            (strncmp(ev->remote_call, sess->remote_call, CALLSIGN_MAX_SIZE) == 0 ||
             (callee_ending(sess) && sess->teardown == ARQ_TD_SENT)))
        {
            HLOGI(LOG_COMP, "Disconnect finalized (%s is calling)", ev->remote_call);
            notify_session_ended(sess);
            if (g_timing) arq_timing_record_disconnect(g_timing, "peer_recall");
            enter_idle_after_call(sess);
            arq_fsm_dispatch(sess, ev);   /* the CALL, now in LISTENING */
            return;
        }
        break;

    case ARQ_EV_TIMER_RETRY:
        if (sess->tx_retries_left > 0)
        {
            if (car_disconnect_must_wait(sess, ev, now) || disconnect_must_wait(sess))
                break;
            sess->tx_retries_left--;
            send_disconnect_frame(sess);
            sess->teardown = ARQ_TD_SENT;
            tm = arq_protocol_mode_timing(session_ctl_mode(sess));
            sess->deadline_ms = retry_deadline_from_s(sess, tm ? tm->retry_interval_s : 7.0f);
            HLOGD(LOG_COMP, "Disconnect tx retry=%d", sess->tx_retries_left);
        }
        else
        {
            HLOGI(LOG_COMP, "Disconnect finalized (timeout)");
            notify_session_ended(sess);
            if (g_timing) arq_timing_record_disconnect(g_timing, "timeout");
            enter_idle_after_call(sess);
        }
        break;

    default:
        break;
    }
}

/* CONNECTED with the carousel.  True when the event was handled here; the
 * rest (LISTEN OFF, a redial, the peer's DISCONNECT) take the common paths
 * below, and the data-flow sub-FSM is never entered. */
static bool fsm_connected_carousel(arq_session_t *sess, const arq_event_t *ev)
{
    uint64_t now = time_now_ms();
    bool peer_end = false;            /* the callee's application has hung up (M_END) */
    switch (ev->id)
    {
    case ARQ_EV_RX_CAROUSEL:
        sess->car_last_rx_ms = now;
        if (g_timing) g_timing->frames_rx++;
        peer_end = car_on_frame(sess->car, now, ev->payload, ev->payload_len, ev->mode, ev->from_control,
                                ev->rx_snr, ev->rx_age_ms);
        if (car_fail_closed(sess, now)) return true;
        break;
    case ARQ_EV_TX_COMPLETE:
        car_on_tx_done(sess->car, now);
        break;
    case ARQ_EV_RX_PATTERN:
        sess->car_last_rx_ms = now;
        car_on_pattern(sess->car, now, ev->pattern_break ? CAR_PATTERN_BREAK
                                                                          : CAR_PATTERN_ACK);
        break;
    case ARQ_EV_APP_DATA_READY:
        car_on_app_data(sess->car, now);
        break;
    case ARQ_EV_TIMER_CAROUSEL:
        if (now >= car_lost_at(sess))
        {
            HLOGW(LOG_COMP, "carousel: nothing from the peer in %d s%s — link lost",
                  (int)((now - sess->car_last_rx_ms) / 1000), car_is_idle(sess->car) ? "" : " with data in flight");
            if (g_timing) arq_timing_record_disconnect(g_timing, "timeout");
            notify_session_ended(sess);
            enter_idle_after_call(sess);
            return true;
        }
        if (car_peer_ending(sess->car) && now - sess->car_last_rx_ms >= car_peer_end_wait_ms(sess))
        {
            /* The callee has all of ours (it says M_END only then) and its
             * application has hung up: what it still holds is its last word,
             * lost now as it was at the watchdog, only later.  One DISCONNECT
             * tells it, in case it still hears us; no retries to a peer this
             * quiet. */
            HLOGW(LOG_COMP, "carousel: the peer hung up and has been silent %llu s -- "
                  "ending the session, its last data not received",
                  (unsigned long long)((now - sess->car_last_rx_ms) / 1000));
            car_begin_disconnect(sess, now);
            sess->tx_retries_left = 0;
            return true;
        }
        car_on_time(sess->car, now);
        break;
    case ARQ_EV_APP_DISCONNECT:
    {
        /* A DISCONNECT while one is already held to drain: the operator is
         * still watching the rig key for a session they have hung up.  End it
         * now, as ABORT does.  Re-arming the drain here pushed the end out by
         * another full budget each time (on air, N3JXB at the MFSK floor:
         * three DISCONNECTs over 69 s, the last 148 s deadline still ahead). */
        bool repeat = sess->teardown == ARQ_TD_DRAINING;
        if (!repeat)
            sess->disc_req_ms = now;
        if (call_queued(sess))
        {
            HLOGI(LOG_COMP, "Deferred CONNECT to %s cancelled", sess->pending_connect_call);
            sess->pending_connect_call[0] = '\0';
        }
        /* Deliver what is queued or in flight first (bounded by the drain
         * timeout in fsm_connected).  ABORT (#218) delivers nothing: what the
         * carousel holds is dropped, and DISCONNECTING still keeps our
         * DISCONNECT off a keydown on the air (disconnect_must_wait). */
        if (ev->abort)
            HLOGI(LOG_COMP, "ABORT: dropping what the carousel has not delivered");
        else if (repeat)
            HLOGI(LOG_COMP, "DISCONNECT again while draining: ending now, %zu bytes undelivered",
                  car_tx_inflight(sess->car) + (size_t)session_tx_backlog(sess));
        else if (car_peer_quiet(sess, now) && !car_drained(sess))
            HLOGI(LOG_COMP, "DISCONNECT: the peer is silent, not waiting to deliver");
        /* A callee that owes the caller an ack (its application hung up on
         * the caller's last bytes, as B2F's FQ) answers first, its answer
         * saying it has hung up (M_END): the caller then has the ack and the
         * hang-up, and ends the session itself.  Its DISCONNECT keyed at once
         * took that answer's slot, and the caller never heard that its last
         * bytes arrived (KG9NI-K7EK, 2026-11-04: "3 bytes of ours
         * undelivered" at every end, each FQ delivered). */
        else if (!car_drained(sess) || sess->tx_active || car_owes_ack(sess->car))
        {
            /* The configured drain is the least it gets; the carousel's
             * budget for its rung stretches it where an exchange is slow. */
            uint64_t budget = (uint64_t)ARQ_DISCONNECT_DRAIN_TIMEOUT_S * 1000ULL;
            uint64_t car_budget = car_drain_budget_ms(sess->car);
            if (car_budget > budget) budget = car_budget;
            sess->teardown = ARQ_TD_DRAINING;
            sess->teardown_deadline_ms = now + budget;
            if (car_drained(sess) && !sess->tx_active)
                HLOGI(LOG_COMP, "DISCONNECT deferred until the peer's last data is acknowledged (up to %llu s)",
                      (unsigned long long)(budget / 1000));
            else
                HLOGI(LOG_COMP, "DISCONNECT deferred until delivered (up to %llu s)",
                      (unsigned long long)(budget / 1000));
            car_wish_end(sess->car);      /* the callee's polls say so: M_END */
            break;
        }
        if (!ev->abort && !repeat)
            HLOGI(LOG_COMP, "DISCONNECT: nothing left to deliver, ending now");
        car_begin_disconnect(sess, now);
        if (ev->abort)
            abort_teardown(sess, now);
        return true;
    }
    case ARQ_EV_RX_ACCEPT:
        return true;                  /* not part of the carousel's session */
    default:
        return false;
    }
    /* A deferred DISCONNECT goes once everything has been delivered. */
    if (sess->teardown == ARQ_TD_DRAINING && car_drained(sess) && !sess->tx_active &&
        !car_owes_ack(sess->car))
        car_begin_disconnect(sess, now);
    /* The callee's application has hung up (M_END), and everything is
     * delivered both ways: the caller ends the session now, its DISCONNECT
     * answered by a pattern.  Left to the callee, the DISCONNECT waited for a
     * poll to give it a slot, and one to acknowledge its last data: below the
     * inline poll, two keydowns more (sim, conn_bench cliff:-5, the callee
     * hanging up: 16 s).  Not drained yet, the callee's own DISCONNECT ends
     * it later, as before. */
    else if (peer_end && car_drained(sess) && !sess->tx_active)
    {
        HLOGI(LOG_COMP, "The peer's application has hung up: ending the session");
        car_begin_disconnect(sess, now);
    }
    return true;
}

static void fsm_connected(arq_session_t *sess, const arq_event_t *ev)
{
    /* A deferred APP_DISCONNECT whose drain deadline has passed: tear down
     * now, whatever is still undelivered, so the rig is never keyed on after
     * the host has disconnected (K7EK "Mercury kept hanging on"). */
    if (sess->teardown == ARQ_TD_DRAINING && time_now_ms() >= sess->teardown_deadline_ms)
    {
        /* the budget it had, not the configured least: the carousel's for
         * its rung may be longer (148 s at the floor) */
        uint64_t budget = sess->disc_req_ms && sess->teardown_deadline_ms > sess->disc_req_ms
                              ? sess->teardown_deadline_ms - sess->disc_req_ms
                              : (uint64_t)ARQ_DISCONNECT_DRAIN_TIMEOUT_S * 1000ULL;
        HLOGW(LOG_COMP,
              "Deferred DISCONNECT drain timeout (%llu s) — forcing teardown",
              (unsigned long long)((budget + 500) / 1000));
        car_begin_disconnect(sess, time_now_ms());
        return;
    }
    /* ...nor while the peer is silent: past ARQ_CAR_PEER_QUIET_MS nothing
     * held is going to be delivered, and the rest of the budget only keeps
     * the rig keying for a session the host has hung up (on air, N3JXB ->
     * VE4KLM, 1.9.22: nothing heard after the ACCEPT, the drain up to 148 s
     * at the floor).  car_begin_disconnect sends the one DISCONNECT a quiet
     * peer gets.  Not on a frame of the peer's: it is not silent. */
    if (sess->teardown == ARQ_TD_DRAINING && car_peer_quiet(sess, time_now_ms()) &&
        ev->id != ARQ_EV_RX_CAROUSEL && ev->id != ARQ_EV_RX_PATTERN)
    {
        HLOGI(LOG_COMP, "Deferred DISCONNECT: nothing from the peer in %llu s, not draining on",
              (unsigned long long)((time_now_ms() - sess->car_last_rx_ms) / 1000));
        car_begin_disconnect(sess, time_now_ms());
        return;
    }

    if (fsm_connected_carousel(sess, ev))
        return;

    switch (ev->id)
    {
    case ARQ_EV_RX_CALL:
        /* Our peer calling: it has left this session (it stops calling before
         * its first frame, the one that connects a callee), most likely on its
         * lost-peer watchdog, which fires first at the end that heard the
         * other last.  Its application's redial went unanswered until ours
         * fired too (sim, nvis: 104 s).  End ours and answer it.  Only the
         * peer's callsign: another station's CALL must not end a session
         * (specs/connect, Mut_ignorecall). */
        if (strcasecmp(ev->remote_call, sess->remote_call) != 0)
            break;
        HLOGI(LOG_COMP, "Session ended (%s is calling again)", ev->remote_call);
        if (g_timing) arq_timing_record_disconnect(g_timing, "peer_recall");
        notify_session_ended(sess);
        enter_idle_after_call(sess);
        arq_fsm_dispatch(sess, ev);   /* the CALL, now in LISTENING */
        return;

    case ARQ_EV_APP_STOP_LISTEN:
        /* LISTEN OFF on a live link means "release the radio", so unlike
         * APP_DISCONNECT below we do NOT drain the TX backlog: the host is
         * telling us another port needs the channel, and queued bytes must not
         * buy more airtime.  No air-side DISCONNECT frame either — that is one
         * more keydown on a channel we were just told to give up, and the peer
         * times out on its own (same reasoning as a dirty ABORT).  A frame
         * already in the air finishes; truncating a keydown mid-frame only
         * leaves the peer decoding garbage, since the transmitter is keyed
         * either way.
         *
         * Said in the log, with what it costs: the session ends without a
         * word to the peer, and whatever of ours is undelivered is dropped.
         * A host whose scanner steps after its own DISCONNECT lands here
         * mid-drain (on air, KG9NI -> N3JXB, 1.9.22: 18 bytes dropped, the
         * peer left to time out, nothing in the log but "Disconnected"). */
        {
            size_t left = (sess->car_active ? car_tx_inflight(sess->car) : 0) +
                          (size_t)session_tx_backlog(sess);
            const char *when = sess->teardown == ARQ_TD_DRAINING
                             ? "while draining after the host's DISCONNECT"
                             : "on a live link";
            if (left)
                HLOGW(LOG_COMP, "LISTEN OFF %s: releasing the radio, %zu bytes of ours "
                      "dropped, no DISCONNECT sent", when, left);
            else
                HLOGI(LOG_COMP, "LISTEN OFF %s: releasing the radio, no DISCONNECT sent", when);
        }
        if (g_timing) arq_timing_record_disconnect(g_timing, "listen_off");
        sess->pending_connect_call[0] = '\0';   /* releasing the radio rules out a queued call */
        notify_session_ended(sess);
        enter_idle_after_call(sess);
        return;

    case ARQ_EV_APP_CONNECT:
        /* A redial that arrives right after DISCONNECT usually lands HERE, not
         * in DISCONNECTING: the disconnect is deferred while the last frame
         * waits for its ACK, so the station is still CONNECTED.  Found on the
         * loopback -- a fix that only handled DISCONNECTING passed every unit
         * test and still dropped a real redial.  Record the call; it is placed
         * once the session has fully ended (see arq_fsm_dispatch).
         *
         * Without a disconnect already under way this is a CONNECT on a live
         * session, which is a host error; it stays ignored. */
        if (sess->teardown == ARQ_TD_DRAINING)
        {
            snprintf(sess->pending_connect_call, CALLSIGN_MAX_SIZE, "%s", ev->remote_call);
            HLOGI(LOG_COMP, "CONNECT to %s deferred until the teardown completes",
                  sess->pending_connect_call);
            /* The host redialled without waiting for DISCONNECTED (which
             * comes when the session ends, arq.c): to it the old session is
             * over and this call is the only one.  Draining on meant the old session kept keying while
             * the new call waited unseen, for up to the whole drain budget
             * (on air, N3JXB at the MFSK floor: "it continued to send in
             * MFSK and did not understand that there was a new connection
             * being attempted").  So the redial ends the drain: the old
             * session's DISCONNECT goes now and the call follows it.  arq.c
             * has flushed the old session's TX queue already. */
            HLOGI(LOG_COMP, "Ending the old session's drain for the new call (%zu bytes undelivered)",
                  car_tx_inflight(sess->car) + (size_t)session_tx_backlog(sess));
            car_begin_disconnect(sess, time_now_ms());
        }
        else
        {
            HLOGW(LOG_COMP, "CONNECT to %s ignored: a session is already up", ev->remote_call);
        }
        return;

    case ARQ_EV_RX_DISCONNECT:
        /* Reply one reply guard later, like every other answer.  A frame
         * decodes before its tail has played out -- on air (bench run 14) the
         * DISCONNECT decoded 186 ms before the peer's TX_COMPLETE -- and a
         * reply keyed at once (40 ms) clipped it. */
        if (g_timing) arq_timing_record_disconnect(g_timing, "rx_disconnect");
        if (sess->disc_req_ms)
            HLOGI(LOG_COMP, "Peer's DISCONNECT received %llu ms after the host's",
                  (unsigned long long)(time_now_ms() - sess->disc_req_ms));
        /* What of ours the peer never got is dropped, not drained: its
         * DISCONNECT means its application hung up (from then on its end
         * delivers nothing: host_released) or its carousel stopped trusting
         * the session (car_fail_closed).  Said here, so a log of the end that
         * did not hang up tells which one lost data (#392). */
        {
            size_t left = (sess->car_active ? car_tx_inflight(sess->car) : 0) +
                          (size_t)session_tx_backlog(sess);
            if (left)
                HLOGW(LOG_COMP, "Peer ended the session: %zu bytes of ours unconfirmed: "
                      "the peer hung up before acknowledging them", left);
        }
        /* It supersedes a DISCONNECT of ours held to drain.  The host hears
         * DISCONNECTED once the reply is out (ARQ_TD_NOTIFYING), so
         * data_rx_buffer_arq has drained to the TCP socket first. */
        sess_enter_td(sess, ARQ_CONN_DISCONNECTED, ARQ_TD_REPLYING,
                      time_now_ms() + ARQ_CHANNEL_GUARD_MS, ARQ_EV_TIMER_ACK);
        return;

    default:
        break;
    }
}

/* ======================================================================
 * Level 2 data-flow sub-FSM
 * ====================================================================== */


/* ======================================================================
 * Top-level dispatch
 * ====================================================================== */

static bool is_session_bound_rx_event(arq_event_id_t id)
{
    switch (id)
    {
    case ARQ_EV_RX_DISCONNECT:
        return true;
    default:
        return false;
    }
}

void arq_fsm_dispatch(arq_session_t *sess, const arq_event_t *ev)
{
    if (!sess || !ev)
        return;

    /* The application ended the session (DISCONNECT, ABORT, or its TCP client
     * went away): whatever the air side still does, nothing more is delivered
     * to it.  See deliver_rx_checked(). */
    if (ev->id == ARQ_EV_APP_DISCONNECT)
        sess->host_released = true;

    HLOGD(LOG_COMP, "state=%s ev=%s",
          arq_conn_state_name(sess->conn_state),
          arq_event_name(ev->id));

    /* Once a session is established, every session-bound frame must carry
     * exactly its ID.  In particular, zero is not a wildcard: transmitters
     * never create a zero session ID, and accepting one would let a malformed
     * or stale frame affect the active link.  Validate before updating
     * last_rx_ms so foreign traffic cannot keep the session alive. */
    if ((sess->conn_state == ARQ_CONN_CONNECTED ||
         sess->conn_state == ARQ_CONN_DISCONNECTING) &&
        is_session_bound_rx_event(ev->id) &&
        ev->session_id != sess->session_id)
    {
        HLOGD(LOG_COMP, "Session ID mismatch: got %d expected %d — dropped",
              (int)ev->session_id, (int)sess->session_id);
        return;
    }

    /* Track last RX time from any received frame */
    switch (ev->id)
    {
    case ARQ_EV_RX_CALL:
    case ARQ_EV_RX_ACCEPT:
    case ARQ_EV_RX_DISCONNECT:
    case ARQ_EV_RX_CAROUSEL:
        sess->last_rx_ms = time_now_ms();
        break;
    default:
        break;
    }

    /* Whether we are keyed, and when we last stopped: the DISCONNECT has to
     * wait for both (see disconnect_must_wait). */
    if (ev->id == ARQ_EV_TX_STARTED)
        sess->tx_active = true;
    else if (ev->id == ARQ_EV_TX_COMPLETE)
    {
        sess->tx_active = false;
        sess->last_tx_end_ms = time_now_ms();
    }

    /* Track the app's listen intent before the per-state dispatch so LISTEN
     * ON/OFF is honoured in any state.  enter_idle_after_call() uses it to
     * restore the correct post-call idle status (LISTENING vs DISCONNECTED). */
    if (ev->id == ARQ_EV_APP_LISTEN)
        sess->listen_enabled = true;
    else if (ev->id == ARQ_EV_APP_STOP_LISTEN)
        sess->listen_enabled = false;

    switch (sess->conn_state)
    {
    case ARQ_CONN_DISCONNECTED:  fsm_disconnected(sess, ev);  break;
    case ARQ_CONN_LISTENING:     fsm_listening(sess, ev);     break;
    case ARQ_CONN_CALLING:       fsm_calling(sess, ev);       break;
    case ARQ_CONN_ACCEPTING:     fsm_accepting(sess, ev);     break;
    case ARQ_CONN_CONNECTED:     fsm_connected(sess, ev);     break;
    case ARQ_CONN_DISCONNECTING: fsm_disconnecting(sess, ev); break;
    default:                                                   break;
    }

    /* Place a CONNECT that was deferred behind a teardown, once the station is
     * idle -- whichever way the session ended.  Not while a peer-initiated
     * disconnect's notice is still pending: that waits for the DISCONNECT ack
     * frame to finish transmitting, and a CALL must not be queued over it. */
    if (call_queued(sess) && !answering_peer_end(sess) &&
        (sess->conn_state == ARQ_CONN_DISCONNECTED ||
         sess->conn_state == ARQ_CONN_LISTENING))
        fire_deferred_connect(sess);

    /* CONNECTED with the carousel has no deadline of its own: the runtime
     * fires the carousel's (and the lost-peer and drain watchdogs') through
     * the one deadline it knows. */
    if (callee_ending(sess))
    {
        uint64_t d = car_next_deadline(sess->car);
        if (sess->teardown_deadline_ms < d) d = sess->teardown_deadline_ms;
        sess->deadline_ms    = d;
        sess->deadline_event = ARQ_EV_TIMER_CAROUSEL;
    }
    if (sess->car_active && sess->conn_state == ARQ_CONN_CONNECTED)
    {
        sess->tx_inflight_bytes = (int)car_tx_inflight(sess->car);
        uint64_t d = car_next_deadline(sess->car);
        if (car_lost_at(sess) < d)
            d = car_lost_at(sess);
        if (car_peer_ending(sess->car) && sess->car_last_rx_ms + car_peer_end_wait_ms(sess) < d)
            d = sess->car_last_rx_ms + car_peer_end_wait_ms(sess);
        if (sess->teardown == ARQ_TD_DRAINING && sess->teardown_deadline_ms < d)
            d = sess->teardown_deadline_ms;
        if (sess->teardown == ARQ_TD_DRAINING && sess->car_last_rx_ms + ARQ_CAR_PEER_QUIET_MS < d)
            d = sess->car_last_rx_ms + ARQ_CAR_PEER_QUIET_MS;
        sess->deadline_ms    = d;
        sess->deadline_event = ARQ_EV_TIMER_CAROUSEL;
    }
}

static bool listening_for_calls(arq_conn_state_t s)
{
    return s == ARQ_CONN_LISTENING || s == ARQ_CONN_ACCEPTING;
}

arq_listen_end_t arq_fsm_dispatch_ends_listening(arq_session_t *sess, const arq_event_t *ev)
{
    if (!sess)
        return ARQ_LISTEN_GOES_ON;
    bool was = listening_for_calls(sess->conn_state);
    arq_fsm_dispatch(sess, ev);
    if (!was || listening_for_calls(sess->conn_state))
        return ARQ_LISTEN_GOES_ON;
    return sess->conn_state == ARQ_CONN_DISCONNECTED ? ARQ_LISTEN_ENDS_RELEASED
                                                     : ARQ_LISTEN_ENDS_IN_SESSION;
}
