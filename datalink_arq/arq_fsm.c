/* HERMES Modem — ARQ FSM implementation
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>   /* getenv/atoi for the MERCURY_PIN_LADDER test hook */

static int ladder_pin_level(void);   /* MERCURY_PIN_LADDER test hook */
#include "arq_fsm.h"
#include "arq_protocol.h"
#include "arq_timing.h"
#include "arq.h"

#include <limits.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "../common/hermes_log.h"
#include "../common/virtual_clock.h"
#include "../modem/framer.h"
#include "../modem/freedv/freedv_api.h"
#include "../modem/modem_mfsk.h"   /* MERCURY_MODE_MFSK */

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

const char *arq_dflow_state_name(arq_dflow_state_t s)
{
    static const char *names[] = {
        [ARQ_DFLOW_IDLE_ISS]       = "IDLE_ISS",
        [ARQ_DFLOW_DATA_TX]        = "DATA_TX",
        [ARQ_DFLOW_WAIT_ACK]       = "WAIT_ACK",
        [ARQ_DFLOW_IDLE_IRS]       = "IDLE_IRS",
        [ARQ_DFLOW_ACK_TX]         = "ACK_TX",
    };
    if ((unsigned)s < ARQ_DFLOW__COUNT) return names[s];
    return "UNKNOWN";
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
        [ARQ_EV_RX_ACK]             = "RX_ACK",
        [ARQ_EV_RX_DATA]            = "RX_DATA",
        [ARQ_EV_RX_DISCONNECT]      = "RX_DISCONNECT",
        [ARQ_EV_TIMER_RETRY]        = "TIMER_RETRY",
        [ARQ_EV_TIMER_ACK]          = "TIMER_ACK",
        [ARQ_EV_TIMER_PEER_BACKLOG] = "TIMER_PEER_BACKLOG",
        [ARQ_EV_TX_STARTED]         = "TX_STARTED",
        [ARQ_EV_TX_COMPLETE]        = "TX_COMPLETE",
    };
    if ((unsigned)ev < ARQ_EV__COUNT) return names[ev];
    return "UNKNOWN";
}

/* ======================================================================
 * Callbacks and timing context registry
 * ====================================================================== */

static arq_fsm_callbacks_t g_cbs;
static arq_timing_ctx_t   *g_timing;

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
    memset(sess, 0, sizeof(*sess));
    sess->conn_state     = ARQ_CONN_DISCONNECTED;
    sess->dflow_state    = ARQ_DFLOW_IDLE_ISS;
    sess->role           = ARQ_ROLE_NONE;
    sess->deadline_ms    = UINT64_MAX;
    sess->deadline_event = ARQ_EV_TIMER_RETRY;
    sess->control_mode        = ARQ_CONTROL_MODE;
    {
        int pin_ = ladder_pin_level();
        int start_ = (pin_ >= 0) ? pin_ : 0;
        sess->speed_level          = start_;
        sess->rx_speed_level       = start_;
        sess->payload_mode         = arq_mode_ladder[start_];  /* ladder floor = MFSK */
        sess->peer_tx_mode         = arq_mode_ladder[start_];  /* RX decoder starts at floor */
        sess->initial_payload_mode = arq_mode_ladder[start_];
    }
    sess->speed_level    = 0;
    sess->tx_success_count = 0;
    sess->fast_ramp      = true;
    sess->proven_level   = 0;
    sess->rx_speed_level = 0;
    sess->rx_success_count = 0;
    sess->rx_fast_ramp   = true;
}

int arq_fsm_timeout_ms(const arq_session_t *sess, uint64_t now)
{
    if (sess->deadline_ms == UINT64_MAX) return INT_MAX;
    if (sess->deadline_ms <= now)        return 0;
    uint64_t diff = sess->deadline_ms - now;
    return (diff > (uint64_t)INT_MAX) ? INT_MAX : (int)diff;
}

/* IRS idle hold before the peer-backlog timer re-arms (was tied to the removed
 * ARQ_PEER_PAYLOAD_HOLD_S knob).  Fixed: the no-progress budget is the real
 * liveness net; this only paces the idle re-arm. */
#define ARQ_IRS_IDLE_HOLD_S   15

/* Silence (since last RX) after which an IRS that holds data self-promotes to
 * ISS — the piggyback-only handoff cannot start a reverse transfer against a
 * peer that never sends.  Must exceed one full slow-frame + turnaround (MFSK
 * ~13.5s + guards) so it never fires mid-transfer and collides with a peer
 * that IS still sending; the peer's own frames refresh last_rx_ms and reset
 * this clock, so it only trips when the peer is genuinely quiet. */
#define ARQ_IRS_SELFPROMOTE_S 40

/* Reset the per-session data-flow / ladder state at (re)connect.  Starts the
 * ladder at the MFSK floor with the fast initial ramp armed. */
static void reset_session_data_state(arq_session_t *sess)
{
    sess->tx_seq             = 0;
    sess->rx_expected        = 0;
    sess->tx_frame_present   = false;
    sess->tx_frame_len       = 0;
    sess->tx_frame_retx      = false;
    sess->tx_retries_left    = ARQ_DATA_RETRY_SLOTS;
    sess->speed_level        = 0;
    sess->tx_success_count   = 0;
    sess->fast_ramp          = true;
    sess->proven_level       = 0;
    sess->rx_speed_level     = 0;
    sess->rx_success_count   = 0;
    sess->rx_fast_ramp       = true;
    {
        int pin_ = ladder_pin_level();
        int start_ = (pin_ >= 0) ? pin_ : 0;
        sess->speed_level    = start_;
        sess->rx_speed_level = start_;
        sess->payload_mode   = arq_mode_ladder[start_];   /* MFSK floor */
        sess->peer_tx_mode   = arq_mode_ladder[start_];
    }
    sess->pending_disconnect = false;
    sess->irs_data_wait_ms   = 0;
}

/* ======================================================================
 * Internal helpers
 * ====================================================================== */

static void sess_enter(arq_session_t *sess, arq_conn_state_t new_state,
                       uint64_t deadline_ms, arq_event_id_t deadline_event)
{
    HLOGD(LOG_COMP, "conn: %s -> %s",
          arq_conn_state_name(sess->conn_state),
          arq_conn_state_name(new_state));
    sess->conn_state     = new_state;
    sess->state_enter_ms = time_now_ms();
    /* The confirm correlator belongs to ACCEPTING and nothing else: any state
     * change closes it, including the successful one into CONNECTED, where the
     * caller's first data burst wants the whole sample budget. */
    if (new_state != ARQ_CONN_ACCEPTING)
        sess->confirm_listen_until_ms = 0;
    sess->deadline_ms    = deadline_ms;
    sess->deadline_event = deadline_event;
    /* A deferred LISTEN OFF must SURVIVE the transition that the grace period
     * exists to protect.  Clearing it here meant the successful case — the
     * handshake completes and we enter CONNECTED — silently swallowed the
     * host's channel release: the interlock was honoured only when the call
     * failed anyway.  Carry it into CONNECTED and act on it there; drop it only
     * when the session is already going idle, where it has nothing left to
     * release. */
    if (new_state == ARQ_CONN_DISCONNECTED || new_state == ARQ_CONN_LISTENING)
        sess->deferred_listen_off = false;
    if (new_state != ARQ_CONN_CONNECTED)
    {
        sess->pending_connect_confirm = false;
        sess->need_initial_guard = false;
    }
    else
    {
        /* Seed the no-progress clock at connection establishment so the
         * wall-clock disconnect budget always has a baseline.  Without this,
         * a session that never lands an advancing ACK (e.g. one-way TX
         * failure while peer keepalives still arrive) would leave
         * last_tx_progress_ms at 0 and persist forever after retry
         * exhaustion.  Advancing ACKs refresh this timestamp during data
         * flow (see fsm_dflow). */
        sess->last_tx_progress_ms = time_now_ms();
    }
    /* Reset data-flow and mode state when returning to idle connection states.
     * Restore peer_tx_mode to initial_payload_mode (= broadcast mode) so the
     * payload decoder can receive broadcast frames while LISTENING.  The
     * session-start paths (RX_CALL, APP_CONNECT) override this to DATAC15
     * before entering ACCEPTING/CALLING. */
    if (new_state == ARQ_CONN_DISCONNECTED || new_state == ARQ_CONN_LISTENING)
    {
        sess->dflow_state      = ARQ_DFLOW_IDLE_ISS;
        sess->peer_tx_mode     = sess->initial_payload_mode;
        sess->rx_speed_level   = 0;   /* mirror back to the floor for a fresh session */
        sess->rx_success_count = 0;
        sess->rx_fast_ramp     = true;
        sess->tx_frame_present = false;
        sess->tx_frame_len     = 0;
        sess->tx_frame_retx    = false;
    }
}

static void dflow_enter(arq_session_t *sess, arq_dflow_state_t new_state,
                        uint64_t deadline_ms, arq_event_id_t deadline_event)
{
    if (sess->dflow_state != new_state)
        HLOGD(LOG_COMP, "dflow: %s -> %s",
              arq_dflow_state_name(sess->dflow_state),
              arq_dflow_state_name(new_state));
    sess->dflow_state    = new_state;
    sess->deadline_ms    = deadline_ms;
    sess->deadline_event = deadline_event;
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

/** Update local_snr_x10 EMA from the SNR carried in a received frame event.
 *  Called in all RX_DATA handlers to avoid cross-thread race with the modem
 *  thread's arq_update_link_metrics() call. */
static void update_local_snr(arq_session_t *sess, const arq_event_t *ev)
{
    if (ev->rx_snr <= -100.0f || ev->rx_snr >= 100.0f || ev->rx_snr == 0.0f)
        return;
    int snr_x10 = (int)(ev->rx_snr * 10.0f);
    if (sess->local_snr_x10 == 0)
        sess->local_snr_x10 = snr_x10;
    else
        sess->local_snr_x10 = (sess->local_snr_x10 * 3 + snr_x10) / 4;
}

static int clamp_payload_mode_to_bandwidth(int mode)
{
    /* All bandwidth-restricted modes clamp to the fastest narrow mode. */
    if (!arq_bandwidth_allows_mode(mode) &&
        (mode == FREEDV_MODE_DATAC1 ||
         mode == FREEDV_MODE_DATAC17 ||
         mode == FREEDV_MODE_QAM16C2))
        return FREEDV_MODE_DATAC3;

    return mode;
}

/* Total pending TX bytes (just the app ring now — no restage buffer). */
static int session_tx_backlog(const arq_session_t *sess)
{
    (void)sess;
    return g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0;
}

/* Set payload_mode from the current speed_level (clamped to the active BW). */
/* TEST HOOK (MERCURY_PIN_LADDER): pin the ladder to one rung so a chosen
 * payload mode is exercised on every run. Used here to ask a single question:
 * does the data plane work with MFSK out of the picture? */
static int ladder_pin_level(void)
{
    static int cached = -2;
    if (cached == -2)
    {
        const char *e = getenv("MERCURY_PIN_LADDER");
        cached = -1;
        if (e && *e)
        {
            int v = atoi(e);
            if (v >= 0 && v < ARQ_LADDER_LEVELS)
            {
                cached = v;
                HLOGW(LOG_COMP, "TEST HOOK: ladder pinned to level %d (mode %d)",
                      v, arq_mode_ladder[v]);
            }
        }
    }
    return cached;
}

static void apply_speed_level(arq_session_t *sess)
{
    int pin = ladder_pin_level();
    if (pin >= 0)
        sess->speed_level = pin;
    if (sess->speed_level < 0)
        sess->speed_level = 0;
    if (sess->speed_level > ARQ_LADDER_LEVELS - 1)
        sess->speed_level = ARQ_LADDER_LEVELS - 1;
    sess->payload_mode =
        clamp_payload_mode_to_bandwidth(arq_mode_ladder[sess->speed_level]);
}

/** Apply one delivery-driven ladder step to a (level, success_count, fast_ramp)
 *  triple.  Shared by the ISS (TX outcomes) and the IRS mode mirror (RX
 *  outcomes) so both ends climb/drop by exactly the same rule and stay locked
 *  in step without any on-wire mode negotiation.
 *
 *  clean == the frame was delivered/received first try: a run of clean outcomes
 *  climbs — the fast initial ramp climbs one rung per clean outcome until the
 *  first miss, after which it settles to ARQ_LADDER_UP_SUCCESSES clean outcomes
 *  per step.  A miss/retry steps DOWN one rung immediately (toward the MFSK
 *  floor) and ends the fast ramp, so a deep fade parks at the floor with no
 *  over-climb oscillation.  Returns the signed level change (for logging). */
static int ladder_step(int *level, int *success_count, bool *fast_ramp, bool clean)
{
    int before = *level;
    if (!clean)
    {
        *fast_ramp = false;
        if (*level > 0)
            (*level)--;
        *success_count = 0;
    }
    else
    {
        (*success_count)++;
        int need = *fast_ramp ? 1 : ARQ_LADDER_UP_SUCCESSES;
        if (*success_count >= need && *level < ARQ_LADDER_LEVELS - 1)
        {
            (*level)++;
            *success_count = 0;
        }
    }
    return *level - before;
}

/** Record the outcome of the retained TX frame once its fate is known.
 *  Delivery-driven, no SNR/OLLA/reverse-hold: clean == the frame was delivered
 *  with no retransmission; else it needed at least one retry. */
static void record_tx_outcome(arq_session_t *sess, bool clean)
{
    /* A rung is "proven" once it has actually put a frame across.  Captured
     * before the step, so a clean delivery proves the rung that carried it and
     * not the one we are about to climb to.  Fresh frames are sized to the
     * proven rung (send_data_burst), which is what makes a failed probe
     * recoverable: the retained frame is immutable, so one read at the size of
     * an unproven rung can be stranded there with no smaller mode able to
     * carry it. */
    if (clean)
    {
        if (sess->speed_level > sess->proven_level)
            sess->proven_level = sess->speed_level;
    }

    int delta = ladder_step(&sess->speed_level, &sess->tx_success_count,
                            &sess->fast_ramp, clean);

    /* Shrink the size budget with the ladder: a rung that just failed is no
     * longer evidence that the next frame may be read that large. */
    if (sess->proven_level > sess->speed_level)
        sess->proven_level = sess->speed_level;
    if (delta < 0)
        HLOGD(LOG_COMP, "Ladder step-down to %d (retry)", sess->speed_level);
    else if (delta > 0)
        HLOGD(LOG_COMP, "Ladder step-up to %d", sess->speed_level);
    apply_speed_level(sess);
}

/** IRS: mirror the peer's (ISS) ladder from the outcome of a received DATA
 *  frame so our payload decoder is already on the mode the peer's NEXT burst
 *  will use.  clean_new == a new in-order frame decoded first try (mirrors the
 *  sender's clean delivery); a duplicate (our ACK was lost, the sender retried
 *  and stepped down) mirrors the sender's step-down.  Keeps peer_tx_mode ==
 *  arq_mode_ladder[rx_speed_level]. */
static void irs_mirror_peer_ladder(arq_session_t *sess, bool clean_new)
{
    int pin = ladder_pin_level();
    if (pin >= 0)
    {
        sess->rx_speed_level = pin;
        sess->peer_tx_mode = clamp_payload_mode_to_bandwidth(arq_mode_ladder[pin]);
        return;
    }

    int delta = ladder_step(&sess->rx_speed_level, &sess->rx_success_count,
                            &sess->rx_fast_ramp, clean_new);
    sess->peer_tx_mode =
        clamp_payload_mode_to_bandwidth(arq_mode_ladder[sess->rx_speed_level]);
    if (delta != 0)
        HLOGD(LOG_COMP, "IRS RX-mode mirror %s to level %d (mode=%d)",
              delta > 0 ? "climb" : "step-down",
              sess->rx_speed_level, sess->peer_tx_mode);
}

/* IRS: arm the ACK deadline after a received DATA frame.  Stop-and-wait: one
 * frame per burst, so we always wait the channel guard before emitting the
 * pattern ACK (lets the ISS relay switch TX->RX before our tones arrive). */
static void irs_arm_ack_deadline(arq_session_t *sess, const arq_event_t *ev)
{
    (void)ev;
    dflow_enter(sess, ARQ_DFLOW_ACK_TX,
                time_now_ms() + ARQ_CHANNEL_GUARD_MS, ARQ_EV_TIMER_ACK);
}

static bool deliver_rx_checked(arq_session_t *sess, const arq_event_t *ev)
{
    if (ev->seq != sess->rx_expected)
    {
        HLOGD(LOG_COMP, "Duplicate data seq=%d (expected=%d) — suppressed",
              (int)ev->seq, (int)sess->rx_expected);
        return false;
    }
    if (ev->payload_len > 0 && g_cbs.deliver_rx_data)
        g_cbs.deliver_rx_data(ev->payload, ev->payload_len);
    sess->rx_expected = ev->seq + 1;
    return true;
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
    if (is_accept)
        n = arq_protocol_build_accept(frame, sizeof(frame), sess->session_id,
                                      my_call, sess->remote_call, bw_hz);
    else
        n = arq_protocol_build_call(frame, sizeof(frame), sess->session_id,
                                    my_call, sess->remote_call, bw_hz);
    if (n > 0)
        send_frame(PACKET_TYPE_ARQ_CALL, sess->control_mode, (size_t)n, frame, 0);
    else
        /* Almost always an over-long callsign: the 10-byte SRC slot holds ~14
         * characters at ~5.25 bits each.  The encoder refuses rather than
         * truncating (a truncated arithmetic code decodes to a DIFFERENT
         * callsign), so say why — otherwise this is a CALL that never goes out
         * and a session that retries against silence. */
        HLOGW(LOG_COMP, "%s not sent: cannot encode callsign '%s' (too long?)",
              is_accept ? "ACCEPT" : "CALL", my_call);
}

static void send_ctrl_frame(arq_session_t *sess, arq_subtype_t subtype)
{
    uint8_t frame[INT_BUFFER_SIZE];
    uint8_t snr_raw = 0;
    if (sess->local_snr_x10 != 0)
        snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);

    int n = -1;
    switch (subtype)
    {
    case ARQ_SUBTYPE_DISCONNECT:
        n = arq_protocol_build_disconnect(frame, sizeof(frame),
                                          sess->session_id, snr_raw); break;
    default:
        return;
    }
    if (n > 0)
        send_frame(PACKET_TYPE_ARQ_CONTROL, sess->control_mode, (size_t)n, frame, 0);
}

/* Emit the pattern ACK.  ACK+TURN (break) when we have reverse data queued
 * (== HAS_DATA piggyback), else a plain ACK.  No coded frame, no seq: in
 * stop-and-wait only one frame is outstanding, so "an ACK was heard" ACKs it
 * unambiguously.  ack_delay_raw is unused (kept for the DATA_RX call site). */
static void send_ack(arq_session_t *sess, uint8_t ack_delay_raw)
{
    (void)ack_delay_raw;
    int kind = (session_tx_backlog(sess) > 0) ? ARQ_PATTERN_BREAK
                                               : ARQ_PATTERN_ACK;
    sess->acktx_had_has_data = (kind == ARQ_PATTERN_BREAK);
    if (g_cbs.send_pattern_ack)
        g_cbs.send_pattern_ack(sess->payload_mode, kind);
}

/* Smallest ladder mode whose usable payload can carry `len` user bytes, at or
 * above the current speed_level.  Used so an already-outstanding frame (whose
 * seq<->bytes identity is immutable) is never re-framed too small after a mode
 * drop — which would double-deliver on the peer.  Falls back to the fastest
 * mode if none fits (should not happen: reads are sized to the mode at
 * creation, so len always fits some mode >= the creation mode). */
static int mode_that_fits(int from_level, int len)
{
    for (int lvl = from_level; lvl < ARQ_LADDER_LEVELS; lvl++)
    {
        int m = clamp_payload_mode_to_bandwidth(arq_mode_ladder[lvl]);
        const arq_mode_timing_t *tm = arq_protocol_mode_timing(m);
        if (tm && (int)tm->payload_bytes - ARQ_FRAME_HDR_SIZE >= len)
            return m;
    }
    return clamp_payload_mode_to_bandwidth(arq_mode_ladder[ARQ_LADDER_LEVELS - 1]);
}

/* Build and transmit the single retained DATA frame.  A FRESH frame reads raw
 * user bytes from the app ring once, sized to the current payload_mode, and
 * caches them in sess->tx_frame with a fixed seq.  A retransmit re-frames the
 * SAME cached bytes under the SAME seq — never resized — so the seq<->content
 * mapping is immutable and a duplicate is idempotent on the peer.  If the mode
 * dropped below the retained frame's length, we transmit at the smallest mode
 * that still fits it (mode_that_fits) rather than splitting it.  No window,
 * no restage. */
static void send_data_burst(arq_session_t *sess)
{
    if (!g_cbs.tx_read || !g_cbs.tx_backlog)
        return;

    const arq_mode_timing_t *tm = arq_protocol_mode_timing(sess->payload_mode);
    if (!tm)
        return;
    if ((int)tm->payload_bytes <= ARQ_FRAME_HDR_SIZE)
        return;
    size_t user_bytes = (size_t)tm->payload_bytes - ARQ_FRAME_HDR_SIZE;

    /* Cap a FRESH read at the proven rung's slot.  The retained frame is
     * immutable — it is never re-framed smaller, because the seq<->bytes
     * identity has to stay fixed for a duplicate to be idempotent on the peer.
     * That makes an oversized read a trap: read 502 bytes while probing DATAC1
     * and no lower rung's slot can hold them, so mode_that_fits() pins every
     * retransmission to DATAC1 even as the ladder steps down.  If the channel
     * cannot carry that rung, the session transmits the same undecodable burst
     * until the no-progress timeout, while the peer's mirror follows the
     * (purely notional) step-downs away from the mode actually on the air.
     * Sizing the read to a rung that has already delivered keeps the retreat
     * open: the frame always fits the rung we fall back to.
     *
     * Only while PROBING above the proven rung.  Slot sizes are not monotonic
     * along the ladder (MFSK carries 90 user bytes, DATAC15 22), so capping
     * unconditionally would shrink a floor frame to a fifth of its payload
     * during exactly the deep fade the floor exists for. */
    if (!sess->tx_frame_present && sess->speed_level > sess->proven_level)
    {
        int pl = sess->proven_level;
        if (pl < 0) pl = 0;
        if (pl > ARQ_LADDER_LEVELS - 1) pl = ARQ_LADDER_LEVELS - 1;
        const arq_mode_timing_t *tp = arq_protocol_mode_timing(
            clamp_payload_mode_to_bandwidth(arq_mode_ladder[pl]));
        if (tp && (int)tp->payload_bytes > ARQ_FRAME_HDR_SIZE)
        {
            size_t proven_slot = (size_t)tp->payload_bytes - ARQ_FRAME_HDR_SIZE;
            if (proven_slot < user_bytes)
                user_bytes = proven_slot;
        }
    }

    /* Fetch a new frame's worth of user bytes iff none is outstanding. */
    if (!sess->tx_frame_present)
    {
        size_t want = user_bytes;
        if (want > sizeof(sess->tx_frame))
            want = sizeof(sess->tx_frame);
        int got = g_cbs.tx_read(sess->tx_frame, want);
        if (got <= 0)
            return;  /* backlog drained */
        sess->tx_frame_len     = got;
        sess->tx_frame_seq     = sess->tx_seq;
        sess->tx_frame_present = true;
        sess->tx_frame_retx    = false;
    }

    /* Choose the TX mode: the current mode if the (immutable) retained frame
     * fits, else the smallest mode that does — the frame is never resized. */
    int tx_mode = sess->payload_mode;
    const arq_mode_timing_t *tmm = arq_protocol_mode_timing(tx_mode);
    size_t slot = (tmm && (int)tmm->payload_bytes > ARQ_FRAME_HDR_SIZE)
                  ? (size_t)tmm->payload_bytes - ARQ_FRAME_HDR_SIZE : 0;
    if (sess->tx_frame_len > (int)slot)
    {
        tx_mode = mode_that_fits(sess->speed_level, sess->tx_frame_len);
        tmm  = arq_protocol_mode_timing(tx_mode);
        slot = (tmm && (int)tmm->payload_bytes > ARQ_FRAME_HDR_SIZE)
               ? (size_t)tmm->payload_bytes - ARQ_FRAME_HDR_SIZE : slot;
    }

    int this_len = sess->tx_frame_len;
    uint8_t payload[INT_BUFFER_SIZE];
    memset(payload, 0, slot);
    memcpy(payload, sess->tx_frame, (size_t)this_len);

    uint16_t payload_valid;
    uint8_t  data_flags = 0;
    if ((size_t)this_len == slot)
    {
        payload_valid = ARQ_DATA_LEN_FULL;
    }
    else
    {
        payload_valid = (uint16_t)this_len;
        if (this_len & 0x100) data_flags |= ARQ_FLAG_LEN_HI;
        if (this_len & 0x200) data_flags |= ARQ_FLAG_LEN_B9;
        if (this_len & 0x400) data_flags |= ARQ_FLAG_LEN_B10;
    }
    /* HAS_DATA piggyback: more app bytes are queued behind this frame. */
    if (session_tx_backlog(sess) > 0)
        data_flags |= ARQ_FLAG_HAS_DATA;

    uint8_t snr_raw = 0;
    if (sess->local_snr_x10 != 0)
        snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);

    uint8_t frame[INT_BUFFER_SIZE];
    int n = arq_protocol_build_data(frame, sizeof(frame),
                                    sess->session_id, sess->tx_frame_seq,
                                    sess->rx_expected, data_flags, snr_raw,
                                    payload_valid, payload, slot);
    if (n <= 0)
        return;

    send_frame(PACKET_TYPE_ARQ_DATA, tx_mode, (size_t)n, frame, 0);
    if (g_timing)
        arq_timing_record_tx_queue(g_timing, (int)sess->tx_frame_seq,
                                   tx_mode,
                                   session_tx_backlog(sess), this_len);
}

/* ======================================================================
 * Level 1 FSM per-state handlers
 * ====================================================================== */

static void fsm_dflow(arq_session_t *sess, const arq_event_t *ev);

static void enter_idle_iss(arq_session_t *sess, bool gained_turn)
{
    (void)gained_turn;  /* per-direction mode: my TX mode evolves independently */
    sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;  /* fresh counter on ISS role entry */
    if (session_tx_backlog(sess) > 0 || sess->tx_frame_present)
    {
        dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        send_data_burst(sess);
    }
    else if (sess->pending_disconnect)
    {
        /* TX buffer is empty and last ACK received — fire the deferred DISCONNECT. */
        HLOGD(LOG_COMP, "Deferred DISCONNECT: TX buffer drained — disconnecting now");
        sess->pending_disconnect      = false;
        sess->tx_retries_left         = ARQ_DISCONNECT_RETRY_SLOTS;
        sess_enter(sess, ARQ_CONN_DISCONNECTING,
                   time_now_ms() + ARQ_CHANNEL_GUARD_MS,
                   ARQ_EV_TIMER_ACK);
    }
    else
    {
        dflow_enter(sess, ARQ_DFLOW_IDLE_ISS, UINT64_MAX, ARQ_EV_TIMER_RETRY);
    }
}

/* Called when a remote frame grants ISS role.  Defers DATA_TX by
 * ARQ_ISS_POST_ACK_GUARD_MS so the peer's decoder has enough time to
 * switch from TX back to RX and re-acquire OFDM sync before our preamble
 * arrives.  Larger than ARQ_CHANNEL_GUARD_MS because ack_rx fires ~168ms
 * before the peer's ACK PTT-OFF, so the effective gap at the peer is only
 * (guard + 100ms head) - 168ms; at 500ms that was only 432ms — too tight
 * for DATAC1 re-sync, causing ~39% first-frame misses. */
static void enter_idle_iss_guarded(arq_session_t *sess, bool gained_turn)
{
    (void)gained_turn;  /* per-direction mode: my TX mode evolves independently */
    sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;  /* fresh counter on ISS role entry */
    if (session_tx_backlog(sess) > 0 || sess->tx_frame_present)
    {
        /* Guard before resuming DATA TX so the peer's decoder can switch
         * TX->RX and re-sync before our preamble.  The mode is chosen purely
         * by delivery feedback (record_tx_outcome) — no negotiation. */
        dflow_enter(sess, ARQ_DFLOW_DATA_TX,
                    time_now_ms() + ARQ_ISS_POST_ACK_GUARD_MS,
                    ARQ_EV_TIMER_ACK);
    }
    else if (sess->pending_disconnect)
    {
        /* TX buffer is empty — honour a DISCONNECT that was deferred while
         * a frame was in flight.  Same path as enter_idle_iss(). */
        HLOGD(LOG_COMP, "Deferred DISCONNECT: TX buffer drained — disconnecting now");
        sess->pending_disconnect      = false;
        sess->tx_retries_left         = ARQ_DISCONNECT_RETRY_SLOTS;
        sess_enter(sess, ARQ_CONN_DISCONNECTING,
                   time_now_ms() + ARQ_CHANNEL_GUARD_MS,
                   ARQ_EV_TIMER_ACK);
    }
    else
        dflow_enter(sess, ARQ_DFLOW_IDLE_ISS, UINT64_MAX, ARQ_EV_TIMER_RETRY);
}

static void enter_idle_irs(arq_session_t *sess)
{
    dflow_enter(sess, ARQ_DFLOW_IDLE_IRS,
                deadline_from_s(ARQ_IRS_IDLE_HOLD_S),
                ARQ_EV_TIMER_PEER_BACKLOG);
}

/* Return to the pre-call idle status after an ARQ call ends: LISTENING if the
 * app has listen mode enabled, otherwise DISCONNECTED.  The connection status
 * is torn down only by the end of a call (or a TCP-client disconnect) and
 * always returns to where it was before the call rather than getting stuck. */
static void enter_idle_after_call(arq_session_t *sess)
{
    sess_enter(sess,
               sess->listen_enabled ? ARQ_CONN_LISTENING : ARQ_CONN_DISCONNECTED,
               UINT64_MAX, ARQ_EV_TIMER_RETRY);
}

static void fsm_disconnected(arq_session_t *sess, const arq_event_t *ev)
{
    switch (ev->id)
    {
    case ARQ_EV_TX_COMPLETE:
        /* Deferred from RX_DISCONNECT: fire now that DISCONNECT ACK is sent,
         * giving the TCP data thread time to drain data_rx_buffer_arq. */
        if (sess->pending_disconnect_notify)
        {
            sess->pending_disconnect_notify = false;
            if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
            /* Call fully torn down — restore the pre-call idle status. */
            enter_idle_after_call(sess);
        }
        break;

    case ARQ_EV_APP_LISTEN:
        sess_enter(sess, ARQ_CONN_LISTENING, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        break;

    case ARQ_EV_APP_CONNECT:
        snprintf(sess->remote_call, CALLSIGN_MAX_SIZE, "%s", ev->remote_call);
        sess->session_id      = (uint8_t)(time_now_ms() & 0x7F) | 0x01;
        reset_session_data_state(sess);  /* MFSK-start ladder, clean retransmit */
        sess->tx_retries_left = ARQ_CALL_RETRY_SLOTS;
        sess->disconnect_deadline_ms = 0;
        send_call_accept(sess, false);
        sess_enter(sess, ARQ_CONN_CALLING,
                   deadline_from_s(arq_protocol_call_interval_s()),
                   ARQ_EV_TIMER_RETRY);
        break;

    default:
        break;
    }
}

static void arm_connect_confirm(arq_session_t *sess)
{
    sess->pending_connect_confirm = true;
    dflow_enter(sess, ARQ_DFLOW_ACK_TX,
                time_now_ms() + ARQ_ISS_POST_ACK_GUARD_MS,
                ARQ_EV_TIMER_ACK);
}

static void fsm_listening(arq_session_t *sess, const arq_event_t *ev)
{
    switch (ev->id)
    {
    case ARQ_EV_RX_CALL:
        snprintf(sess->remote_call, CALLSIGN_MAX_SIZE, "%s", ev->remote_call);
        snprintf(sess->local_call, CALLSIGN_MAX_SIZE, "%s", ev->local_call);
        sess->session_id      = ev->session_id;
        /* Reset mode state so the payload decoder matches the new caller's
         * initial MFSK floor.  This must happen here (not in sess_enter for
         * DISCONNECTED/LISTENING) because LISTENING needs peer_tx_mode to
         * stay at the broadcast mode for receiving broadcast frames. */
        reset_session_data_state(sess);
        sess->tx_retries_left = ARQ_ACCEPT_RETRY_SLOTS;
        /* Do NOT send ACCEPT immediately: the caller's PTT-OFF may not have
         * happened yet when we decode the last samples of their CALL frame.
         * Wait ARQ_CHANNEL_GUARD_MS so their relay is in RX before we TX. */
        sess_enter(sess, ARQ_CONN_ACCEPTING,
                   time_now_ms() + ARQ_CHANNEL_GUARD_MS,
                   ARQ_EV_TIMER_RETRY);
        if (g_cbs.notify_pending)
            g_cbs.notify_pending(ev->remote_call, ev->local_call);
        break;

    case ARQ_EV_RX_ACCEPT:
        /* We gave up CALLING (retries exhausted) and returned to LISTENING,
         * but the callee is still retrying ACCEPT from our earlier CALL.
         * We already told the TNC "DISCONNECTED", so we can't reconnect.
         * Send a DISCONNECT to tell the peer to stop retrying. */
        if (ev->session_id == sess->session_id)
        {
            HLOGI(LOG_COMP, "Stale ACCEPT in LISTENING — sending DISCONNECT to peer");
            send_ctrl_frame(sess, ARQ_SUBTYPE_DISCONNECT);
        }
        break;

    case ARQ_EV_APP_CONNECT:
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        fsm_disconnected(sess, ev);
        break;

    case ARQ_EV_APP_STOP_LISTEN:
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        break;

    case ARQ_EV_RX_DATA:
    case ARQ_EV_RX_ACK:
        /* Safety net: if IRS fell from ACCEPTING→LISTENING (ACCEPT retries
         * exhausted) but the ISS is already sending DATA/ACK, accept the
         * connection now — same logic as fsm_accepting RX_DATA handler. */
        if (ev->session_id == sess->session_id)
        {
            sess->role        = ARQ_ROLE_CALLEE;
            reset_session_data_state(sess);
            if (g_cbs.notify_connected)
                g_cbs.notify_connected(sess->remote_call, sess->local_call);
            if (g_timing)
                arq_timing_record_connect(g_timing, sess->control_mode);
            sess_enter(sess, ARQ_CONN_CONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
            enter_idle_irs(sess);
            if (ev->id == ARQ_EV_RX_DATA)
                fsm_dflow(sess, ev);
        }
        break;

    default:
        break;
    }
}

static void fsm_calling(arq_session_t *sess, const arq_event_t *ev)
{
    switch (ev->id)
    {
    case ARQ_EV_RX_ACCEPT:
        if (ev->session_id == sess->session_id)
        {
            bool has_tx_backlog = session_tx_backlog(sess) > 0;
            sess->role        = ARQ_ROLE_CALLER;
            reset_session_data_state(sess);  /* discard stale retransmit buf; MFSK-start */
            if (g_cbs.notify_connected)
                g_cbs.notify_connected(sess->remote_call, sess->local_call);
            if (g_timing)
                arq_timing_record_connect(g_timing, sess->control_mode);
            /* The callee does not enter CONNECTED until it sees the caller's
             * first DATA or ACK.  If the app has no payload queued yet, send
             * an initial ACK after the post-ACCEPT guard so the peer does not
             * sit in ACCEPTING retrying ACCEPT forever. */
            sess->pending_connect_confirm = false;
            sess->need_initial_guard = has_tx_backlog;
            sess_enter(sess, ARQ_CONN_CONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
            if (!has_tx_backlog)
            {
                arm_connect_confirm(sess);
            }
            else
            {
                enter_idle_iss_guarded(sess, false);   /* caller sends data first */
            }
        }
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
        sess->deadline_ms = deadline_from_s(arq_protocol_call_interval_s());
        break;

    case ARQ_EV_TIMER_RETRY:
        if (sess->tx_retries_left > 0)
        {
            sess->tx_retries_left--;
            send_call_accept(sess, false);
            /* Deadline is re-anchored on TX_COMPLETE above; this is the
             * fallback if that event is ever missed. */
            sess->deadline_ms = deadline_from_s(arq_protocol_call_interval_s());
        }
        else
        {
            if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
            enter_idle_after_call(sess);
        }
        break;

    case ARQ_EV_APP_STOP_LISTEN:  /* host wants the radio back — abandon the call */
    case ARQ_EV_APP_DISCONNECT:
        if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
        enter_idle_after_call(sess);
        break;

    default:
        break;
    }
}

static void fsm_accepting(arq_session_t *sess, const arq_event_t *ev)
{
    switch (ev->id)
    {
    case ARQ_EV_RX_DATA:
    case ARQ_EV_RX_ACK:
        sess->role        = ARQ_ROLE_CALLEE;
        /* Which of the two legs finished the handshake is worth a line in the
         * log: "confirm" means the caller's 0.64 s pattern was heard inside the
         * bounded correlator window (the fast path), "first data" means it was
         * not and we fell through to the caller's first burst — costing the
         * time the pattern exists to save.  On a real link the ratio between
         * these two is the measurement that says whether the fast path is
         * carrying its weight. */
        HLOGI(LOG_COMP, "handshake completed on %s",
              ev->id == ARQ_EV_RX_ACK ? "connect confirm (pattern)"
                                      : "first data frame");
        reset_session_data_state(sess);  /* discard stale retransmit buf; MFSK-start */
        if (g_cbs.notify_connected)
            g_cbs.notify_connected(sess->remote_call, sess->local_call);
        if (g_timing)
            arq_timing_record_connect(g_timing, sess->control_mode);
        sess_enter(sess, ARQ_CONN_CONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        enter_idle_irs(sess);       /* callee receives first; process incoming data */
        if (sess->deferred_listen_off)
        {
            /* The host asked for the radio back while we were answering, and
             * the answer then succeeded.  Honour the request now rather than
             * starting a session on a channel we were told to give up. */
            HLOGI(LOG_COMP, "connected with a deferred LISTEN OFF — releasing the radio");
            sess->deferred_listen_off = false;
            sess->pending_disconnect  = false;
            if (g_timing) arq_timing_record_disconnect(g_timing, "listen_off");
            if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
            enter_idle_after_call(sess);
            break;
        }
        if (ev->id == ARQ_EV_RX_DATA)
            fsm_dflow(sess, ev);
        break;

    case ARQ_EV_RX_CALL:
        /* Caller is still retrying CALL (our previous ACCEPT was lost). Reset
         * the retry counter so the ACCEPTING window stays open long enough for
         * the caller to decode the next ACCEPT and start sending data. */
        sess->tx_retries_left = ARQ_ACCEPT_RETRY_SLOTS;
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
        sess->deadline_ms = time_now_ms() + ARQ_ACCEPT_RX_WINDOW_MS;
        /* If the caller has nothing queued it answers with a 0.64 s pattern
         * rather than a DATA burst.  Nothing else decodes a pattern, so open
         * the correlator here — and only here, for a bounded few seconds, so
         * it is shut again well before the caller's first data burst needs the
         * whole sample budget. */
        sess->confirm_listen_until_ms = time_now_ms() + ARQ_CONNECT_CONFIRM_LISTEN_MS;
        break;

    case ARQ_EV_TIMER_RETRY:
        if (sess->deferred_listen_off &&
            time_now_ms() - sess->state_enter_ms >= ARQ_LISTEN_OFF_GRACE_MS)
        {
            sess->deferred_listen_off = false;
            if (g_cbs.notify_cancelpending)
                g_cbs.notify_cancelpending();
            if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
            enter_idle_after_call(sess);
            break;
        }
        if (sess->tx_retries_left > 0)
        {
            sess->tx_retries_left--;
            send_call_accept(sess, true);
            /* deadline is now managed via TX_COMPLETE above; set a generous
             * fallback here in case TX_COMPLETE is missed for any reason */
            sess->deadline_ms = deadline_from_s(arq_protocol_call_interval_s());
        }
        else
        {
            if (g_cbs.notify_cancelpending)
                g_cbs.notify_cancelpending();
            sess_enter(sess, ARQ_CONN_LISTENING, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        }
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

    /* LISTEN OFF releases the radio — but not instantly in this one state.
     * A scanning host (BPQ32 interlock) sends LISTEN OFF at dwell expiry and
     * needs a moment to process the PENDING we just sent it and cancel its own
     * timer; acting immediately turns that race into a dropped inbound call.
     * Defer inside the grace window and honour it on the next TIMER_RETRY, or
     * on reaching CONNECTED (both handled above).  CALLING has no such grace:
     * PENDING announces an INCOMING call, so it is never sent while we are the
     * caller and there is nothing to race against. */
    case ARQ_EV_APP_STOP_LISTEN:
        if (time_now_ms() - sess->state_enter_ms < ARQ_LISTEN_OFF_GRACE_MS)
        {
            sess->deferred_listen_off = true;
            sess->deadline_ms = sess->state_enter_ms + ARQ_LISTEN_OFF_GRACE_MS;
            break;
        }
        /* fall through */
    case ARQ_EV_APP_DISCONNECT:
        if (g_cbs.notify_cancelpending)
            g_cbs.notify_cancelpending();
        if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
        enter_idle_after_call(sess);
        break;

    default:
        break;
    }
}

static void fsm_disconnecting(arq_session_t *sess, const arq_event_t *ev)
{
    const arq_mode_timing_t *tm;

    switch (ev->id)
    {
    case ARQ_EV_TIMER_ACK:
        /* Initial DISCONNECT send after channel guard. */
        send_ctrl_frame(sess, ARQ_SUBTYPE_DISCONNECT);
        tm = arq_protocol_mode_timing(sess->control_mode);
        sess->deadline_ms    = deadline_from_s(tm ? tm->retry_interval_s : 7.0f);
        sess->deadline_event = ARQ_EV_TIMER_RETRY;
        HLOGD(LOG_COMP, "Disconnect tx (initial, after guard)");
        break;

    case ARQ_EV_RX_DISCONNECT:
        HLOGI(LOG_COMP, "Disconnect finalized (peer ack)");
        if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
        if (g_timing) arq_timing_record_disconnect(g_timing, "peer_ack");
        enter_idle_after_call(sess);
        break;

    case ARQ_EV_APP_STOP_LISTEN:
        /* The radio was asked for back mid-teardown: stop retransmitting
         * DISCONNECT.  The peer times out on its own. */
        HLOGI(LOG_COMP, "LISTEN OFF while disconnecting — stopping retransmits");
        if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
        if (g_timing) arq_timing_record_disconnect(g_timing, "listen_off");
        enter_idle_after_call(sess);
        break;

    case ARQ_EV_TIMER_RETRY:
        if (sess->tx_retries_left > 0)
        {
            sess->tx_retries_left--;
            send_ctrl_frame(sess, ARQ_SUBTYPE_DISCONNECT);
            tm = arq_protocol_mode_timing(sess->control_mode);
            sess->deadline_ms = deadline_from_s(tm ? tm->retry_interval_s : 7.0f);
            HLOGD(LOG_COMP, "Disconnect tx retry=%d", sess->tx_retries_left);
        }
        else
        {
            HLOGI(LOG_COMP, "Disconnect finalized (timeout)");
            if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
            if (g_timing) arq_timing_record_disconnect(g_timing, "timeout");
            enter_idle_after_call(sess);
        }
        break;

    default:
        break;
    }
}


static void fsm_connected(arq_session_t *sess, const arq_event_t *ev)
{
    /* Fallback for a deferred APP_DISCONNECT that the normal fire points
     * (idle-ISS entry, WAIT_ACK ack-timer, retry exhaustion) never reach —
     * e.g. a session pinned as IRS.  Once the drain deadline elapses, force a
     * clean air-side teardown regardless of role or backlog so the rig is
     * never keyed indefinitely after the host has disconnected. */
    if (sess->pending_disconnect && sess->disconnect_deadline_ms != 0 &&
        time_now_ms() >= sess->disconnect_deadline_ms)
    {
        HLOGW(LOG_COMP,
              "Deferred DISCONNECT drain timeout (%ds) — forcing teardown",
              ARQ_DISCONNECT_DRAIN_TIMEOUT_S);
        sess->pending_disconnect      = false;
        sess->disconnect_deadline_ms  = 0;
        sess->tx_retries_left         = ARQ_DISCONNECT_RETRY_SLOTS;
        sess_enter(sess, ARQ_CONN_DISCONNECTING,
                   time_now_ms() + ARQ_CHANNEL_GUARD_MS,
                   ARQ_EV_TIMER_ACK);
        return;
    }

    switch (ev->id)
    {
    case ARQ_EV_APP_STOP_LISTEN:
        /* LISTEN OFF is a host interlock ("release the radio now"), not a
         * polite disconnect: drop the link without draining the TX backlog and
         * without a DISCONNECT frame on the air.  Queued bytes buy no more
         * airtime on a channel we were just told to give up, and the peer
         * times out normally.  A frame already in flight finishes; nothing new
         * is keyed.  Contrast APP_DISCONNECT below, which defers to drain. */
        HLOGI(LOG_COMP, "LISTEN OFF while connected — releasing the radio");
        sess->pending_disconnect = false;
        if (g_timing) arq_timing_record_disconnect(g_timing, "listen_off");
        if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
        enter_idle_after_call(sess);
        return;

    case ARQ_EV_APP_DISCONNECT:
        /* Defer DISCONNECT while a frame is physically being transmitted
         * (DATA_TX), still awaiting its ACK (WAIT_ACK), or the TX buffer has
         * unsent bytes, so the last application bytes get delivered before
         * teardown.  Bounded three ways: (1) retry exhaustion with a pending
         * disconnect tears down immediately, (2) the absolute drain deadline
         * armed here forces teardown regardless of state, (3) a drained buffer
         * fires the deferred disconnect at the next idle-ISS entry. */
        if ((session_tx_backlog(sess) > 0) ||
            sess->tx_frame_present ||
            sess->dflow_state == ARQ_DFLOW_DATA_TX ||
            sess->dflow_state == ARQ_DFLOW_WAIT_ACK)
        {
            HLOGD(LOG_COMP,
                  "APP_DISCONNECT deferred — backlog=%d dflow=%s",
                  session_tx_backlog(sess),
                  arq_dflow_state_name(sess->dflow_state));
            sess->pending_disconnect = true;
            sess->disconnect_deadline_ms =
                time_now_ms() +
                (uint64_t)ARQ_DISCONNECT_DRAIN_TIMEOUT_S * 1000ULL;
            return;
        }
        sess->pending_disconnect      = false;
        sess->tx_retries_left         = ARQ_DISCONNECT_RETRY_SLOTS;
        sess_enter(sess, ARQ_CONN_DISCONNECTING,
                   time_now_ms() + ARQ_CHANNEL_GUARD_MS,
                   ARQ_EV_TIMER_ACK);
        return;

    case ARQ_EV_RX_DISCONNECT:
        send_ctrl_frame(sess, ARQ_SUBTYPE_DISCONNECT);
        /* Peer-initiated disconnect supersedes any locally deferred one. */
        sess->pending_disconnect = false;
        /* Defer notify until TX_COMPLETE so data_rx_buffer_arq has time to
         * drain to the TCP socket before UUCP sees the DISCONNECTED signal. */
        sess->pending_disconnect_notify = true;
        if (g_timing) arq_timing_record_disconnect(g_timing, "rx_disconnect");
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        return;

    case ARQ_EV_RX_ACCEPT:
        if (sess->role == ARQ_ROLE_CALLER &&
            ev->session_id == sess->session_id &&
            sess->dflow_state == ARQ_DFLOW_IDLE_ISS &&
            !sess->pending_connect_confirm)
        {
            /* The callee is retrying ACCEPT because it did not decode our
             * earlier post-ACCEPT confirmation. Re-send the confirmation ACK
             * so the peer can leave ACCEPTING without restarting the session. */
            arm_connect_confirm(sess);
            return;
        }
        break;

    default:
        break;
    }

    fsm_dflow(sess, ev);
}

/* ======================================================================
 * Level 2 data-flow sub-FSM (5 states, delivery-driven, pattern ACK)
 *
 *   ISS: IDLE_ISS -> DATA_TX -> WAIT_ACK
 *   IRS: IDLE_IRS -> ACK_TX
 *
 * Turn handoff is piggyback only: the IRS sets HAS_DATA on its pattern ACK
 * (an ACK+TURN "break") when it has reverse data; the ISS yields on seeing it.
 * There is no TURN_REQ/MODE_REQ/KEEPALIVE — the mode ladder is delivery-driven
 * and the no-progress budget is the liveness net.
 * ====================================================================== */

/* IRS: receive a DATA frame (dup-checked), track the peer's TX mode, and note
 * whether the sender has more data.  A duplicate means our previous ACK was
 * lost and the ISS is still active — force peer_has_data so we re-ACK and
 * stay IRS rather than take a spurious piggyback turn. */
static void irs_receive_data(arq_session_t *sess, const arq_event_t *ev)
{
    update_local_snr(sess, ev);
    bool new_frame = deliver_rx_checked(sess, ev);
    /* Follow the sender's delivery-driven ladder: a new in-order frame is a
     * clean delivery on its side (climb); a duplicate means it retried and
     * stepped down.  This sets peer_tx_mode to the mode of the peer's NEXT
     * burst so the payload decoder is already there when it arrives. */
    irs_mirror_peer_ladder(sess, new_frame);
    if (new_frame && g_timing)
        arq_timing_record_data_rx(g_timing, (int)ev->seq,
                                  (int)ev->data_bytes, sess->local_snr_x10);
    sess->last_rx_ms = time_now_ms();
    sess->peer_has_data = new_frame
                          ? (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0
                          : true;
}

/* ISS: the retained frame was delivered (explicit pattern ACK or an implicit
 * ACK carried by the peer's reverse DATA).  The frame is an immutable unit
 * (fixed seq + byte range), so clear it whole and advance tx_seq. */
static void iss_frame_delivered(arq_session_t *sess)
{
    sess->tx_frame_present = false;
    sess->tx_frame_len     = 0;
    sess->tx_frame_retx    = false;
    sess->tx_seq           = (uint8_t)(sess->tx_frame_seq + 1);
    sess->last_tx_progress_ms = time_now_ms();
    sess->tx_retries_left     = ARQ_DATA_RETRY_SLOTS;
    if (g_cbs.send_buffer_status)
        g_cbs.send_buffer_status(session_tx_backlog(sess));
}

static void fsm_dflow(arq_session_t *sess, const arq_event_t *ev)
{
    const arq_mode_timing_t *tm;

    switch (sess->dflow_state)
    {
    case ARQ_DFLOW_IDLE_ISS:
        if (ev->id == ARQ_EV_APP_DATA_READY &&
            (session_tx_backlog(sess) > 0 || sess->tx_frame_present))
        {
            if (sess->need_initial_guard)
            {
                /* First DATA after connect: apply channel guard so IRS has
                 * time to reset decoders from TX→RX before our preamble. */
                sess->need_initial_guard = false;
                dflow_enter(sess, ARQ_DFLOW_DATA_TX,
                            time_now_ms() + ARQ_ISS_POST_ACK_GUARD_MS,
                            ARQ_EV_TIMER_ACK);
            }
            else
            {
                dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
                send_data_burst(sess);
            }
        }
        else if (ev->id == ARQ_EV_RX_DATA)
        {
            /* Peer sent data while we hold the (idle) TX turn — receive it and
             * ACK.  The pattern ACK carries HAS_DATA if we too have backlog. */
            irs_receive_data(sess, ev);
            irs_arm_ack_deadline(sess, ev);
        }
        break;

    case ARQ_DFLOW_DATA_TX:
        if (ev->id == ARQ_EV_TIMER_ACK)
        {
            /* Channel guard elapsed — now safe to transmit data. */
            send_data_burst(sess);
        }
        else if (ev->id == ARQ_EV_TX_STARTED)
        {
            if (g_timing)
                arq_timing_record_tx_start(g_timing, (int)sess->tx_frame_seq,
                                           sess->payload_mode,
                                           session_tx_backlog(sess));
        }
        else if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            if (g_timing)
                arq_timing_record_tx_end(g_timing, (int)sess->tx_frame_seq);
            tm = arq_protocol_mode_timing(sess->payload_mode);
            dflow_enter(sess, ARQ_DFLOW_WAIT_ACK,
                        deadline_from_s(tm ? tm->ack_timeout_s : 9.0f),
                        ARQ_EV_TIMER_ACK);
        }
        break;

    case ARQ_DFLOW_WAIT_ACK:
        if (ev->id == ARQ_EV_RX_ACK)
        {
            /* Pattern ACK: in stop-and-wait only one frame is outstanding, so
             * an ACK unambiguously confirms it.  A stale ACK with no frame
             * present is ignored. */
            if (!sess->tx_frame_present)
            {
                HLOGD(LOG_COMP, "ACK with no outstanding frame — ignored");
                break;
            }
            bool clean = !sess->tx_frame_retx &&
                         sess->tx_retries_left == ARQ_DATA_RETRY_SLOTS;
            if (g_timing)
                arq_timing_record_ack_rx(g_timing, (int)sess->tx_frame_seq,
                                         (uint8_t)ev->ack_delay_raw,
                                         sess->local_snr_x10);
            iss_frame_delivered(sess);
            /* Only a CLEAN delivery feeds the ladder here; a dirty one already
             * stepped down once per TIMER_ACK retransmit below, so re-penalising
             * it would double-count. */
            if (clean)
                record_tx_outcome(sess, true);
            sess->peer_has_data = (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0;
            bool i_have_data = session_tx_backlog(sess) > 0 || sess->tx_frame_present;

            if (sess->peer_has_data && i_have_data)
            {
                /* Simultaneous bid (both ends have data).  Mirror of the ACK_TX
                 * tiebreak: the CALLER keeps the floor, the CALLEE yields, so
                 * exactly one side is ISS. */
                if (sess->role == ARQ_ROLE_CALLER)
                    enter_idle_iss_guarded(sess, false);
                else
                {
                    if (g_timing) arq_timing_record_turn(g_timing, false, "piggyback");
                    enter_idle_irs(sess);
                }
            }
            else if (sess->peer_has_data)
            {
                /* ACK+TURN break, we are drained: peer bid — yield to IRS. */
                if (g_timing) arq_timing_record_turn(g_timing, false, "piggyback");
                enter_idle_irs(sess);
            }
            else
            {
                enter_idle_iss_guarded(sess, false);  /* ISS retains the turn */
            }
        }
        else if (ev->id == ARQ_EV_TIMER_ACK)
        {
            if (sess->tx_retries_left > 0)
            {
                int retries_before_cap = (int)sess->tx_retries_left;
                /* Pending disconnect: cap to 1 more retry so the peer gets one
                 * last chance to ACK before teardown (final UUCP hangup frame). */
                if (sess->pending_disconnect && sess->tx_retries_left > 1)
                {
                    HLOGD(LOG_COMP,
                          "Pending DISCONNECT: capping retries to 1 for seq=%d",
                          (int)sess->tx_frame_seq);
                    sess->tx_retries_left = 1;
                }
                sess->tx_retries_left--;
                sess->tx_frame_retx = true;   /* retransmit ends "clean" */
                /* Any retry steps the ladder down one rung (delivery-driven):
                 * a fade descends quickly toward the robust floor rather than
                 * burning the whole retry budget at a mode the channel can no
                 * longer carry.  The eventual (dirty) ACK does NOT re-penalise. */
                record_tx_outcome(sess, false);
                if (g_timing)
                    arq_timing_record_retry(g_timing, (int)sess->tx_frame_seq,
                                            ARQ_DATA_RETRY_SLOTS - retries_before_cap + 1,
                                            "ack_timeout");
                dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
                send_data_burst(sess);
            }
            else
            {
                /* Retry budget exhausted — disconnect only past the no-progress
                 * budget (or when a disconnect is pending); otherwise reset the
                 * counter, step the mode down (this run of ACK timeouts is a
                 * definitive non-delivery), and keep trying. */
                uint64_t now = time_now_ms();
                uint64_t budget_ms = (uint64_t)ARQ_NO_PROGRESS_TIMEOUT_S * 1000ULL;
                bool no_progress_dead =
                    (now - sess->last_tx_progress_ms) >= budget_ms;
                if (no_progress_dead || sess->pending_disconnect)
                {
                    HLOGW(LOG_COMP,
                          "Data retry exhausted seq=%d (%s) — disconnecting",
                          (int)sess->tx_frame_seq,
                          sess->pending_disconnect ? "app disconnect pending"
                                                   : "no forward progress");
                    sess->pending_disconnect = false;
                    send_ctrl_frame(sess, ARQ_SUBTYPE_DISCONNECT);
                    sess->tx_retries_left = ARQ_DISCONNECT_RETRY_SLOTS;
                    tm = arq_protocol_mode_timing(sess->control_mode);
                    sess_enter(sess, ARQ_CONN_DISCONNECTING,
                               deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                               ARQ_EV_TIMER_RETRY);
                }
                else
                {
                    unsigned long long since_s =
                        (unsigned long long)((now - sess->last_tx_progress_ms) / 1000);
                    HLOGI(LOG_COMP,
                          "Data retry exhausted seq=%d, persisting (%llus / %ds budget)",
                          (int)sess->tx_frame_seq, since_s, ARQ_NO_PROGRESS_TIMEOUT_S);
                    if (g_timing)
                        arq_timing_record_retry(g_timing, (int)sess->tx_frame_seq,
                                                ARQ_DATA_RETRY_SLOTS,
                                                "persist_after_exhaust");
                    sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;
                    /* Ladder already stepped down once per consumed retry above;
                     * no extra penalty here. */
                    dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
                    send_data_burst(sess);
                }
            }
        }
        else if (ev->id == ARQ_EV_RX_DATA)
        {
            update_local_snr(sess, ev);
            sess->last_rx_ms = time_now_ms();

            if (ev->seq == sess->rx_expected)
            {
                /* Peer sent new DATA while we await our ACK — implicit ACK:
                 * the peer would not send new DATA unless it received ours.
                 * Consume our frame, receive theirs, and ACK (yield to IRS). */
                HLOGD(LOG_COMP,
                      "RX_DATA in WAIT_ACK (new seq=%d) — implicit ACK for tx_seq=%d",
                      (int)ev->seq, (int)sess->tx_frame_seq);
                bool clean = !sess->tx_frame_retx &&
                             sess->tx_retries_left == ARQ_DATA_RETRY_SLOTS;
                if (sess->tx_frame_present)
                {
                    iss_frame_delivered(sess);
                    record_tx_outcome(sess, clean);
                }
                irs_receive_data(sess, ev);
                irs_arm_ack_deadline(sess, ev);
            }
            else
            {
                /* Duplicate frame (our ACK was lost; peer retransmitting).
                 * Not an implicit ACK of our own frame.  Retransmit our frame
                 * so the peer can ACK it; do NOT advance our seq. */
                HLOGD(LOG_COMP,
                      "RX_DATA in WAIT_ACK (dup seq=%d expected=%d) — re-TX our seq=%d",
                      (int)ev->seq, (int)sess->rx_expected, (int)sess->tx_frame_seq);
                deliver_rx_checked(sess, ev);   /* logs dup; no delivery */
                irs_mirror_peer_ladder(sess, false);  /* peer retried → mirror step-down */
                sess->tx_frame_retx = true;
                dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
                send_data_burst(sess);
            }
        }
        break;

    case ARQ_DFLOW_IDLE_IRS:
        if (ev->id == ARQ_EV_RX_DATA)
        {
            irs_receive_data(sess, ev);
            irs_arm_ack_deadline(sess, ev);
        }
        else if (ev->id == ARQ_EV_APP_DATA_READY)
        {
            /* We hold data but are the IRS.  Turn handoff is piggyback-only:
             * we do NOT self-promote here (that would collide with an ISS about
             * to transmit — the bidirectional double-ISS deadlock).  Instead we
             * keep the floor state; our next pattern ACK carries the ACK+TURN
             * break, and if the peer never sends, the peer-backlog timer below
             * self-promotes us after a proven-silent interval.  Stamp when we
             * first got data so that silence window starts now (the peer may be
             * about to send — give it a full turn to bid). */
            if (sess->irs_data_wait_ms == 0)
                sess->irs_data_wait_ms = time_now_ms();
            break;
        }
        else if (ev->id == ARQ_EV_TIMER_PEER_BACKLOG)
        {
            /* Piggyback-only turn handoff cannot start a reverse transfer when
             * the peer is idle (no DATA to piggyback on).  When we hold data
             * and the peer has been silent for a full idle hold, self-promote
             * to ISS after the post-ACK guard: an idle peer will just receive.
             * If the peer resumes sending, our RX_DATA path handles it. */
            if (session_tx_backlog(sess) > 0 || sess->tx_frame_present)
            {
                /* Only self-promote once the peer has been silent long enough
                 * that it clearly has nothing to send.  The silence window runs
                 * from the later of the last RX and when we first queued data
                 * (a still-active peer's frames refresh last_rx_ms and reset
                 * this).  Otherwise keep waiting to piggyback — avoids the
                 * double-ISS collision at the start of a bidirectional flow. */
                uint64_t since = sess->last_rx_ms;
                if (sess->irs_data_wait_ms > since) since = sess->irs_data_wait_ms;
                if (since == 0 ||
                    time_now_ms() - since <
                        (uint64_t)ARQ_IRS_SELFPROMOTE_S * 1000ULL)
                {
                    enter_idle_irs(sess);   /* re-arm; not silent long enough yet */
                    break;
                }
                sess->irs_data_wait_ms = 0;
                sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;
                dflow_enter(sess, ARQ_DFLOW_DATA_TX,
                            time_now_ms() + ARQ_ISS_POST_ACK_GUARD_MS,
                            ARQ_EV_TIMER_ACK);
            }
            else if (sess->pending_disconnect)
            {
                sess->pending_disconnect = false;
                sess->tx_retries_left    = ARQ_DISCONNECT_RETRY_SLOTS;
                sess_enter(sess, ARQ_CONN_DISCONNECTING,
                           time_now_ms() + ARQ_CHANNEL_GUARD_MS,
                           ARQ_EV_TIMER_ACK);
            }
            else if (ev->id == ARQ_EV_TIMER_PEER_BACKLOG &&
                     sess->last_rx_ms > 0 &&
                     time_now_ms() - sess->last_rx_ms >=
                         (uint64_t)ARQ_NO_PROGRESS_TIMEOUT_S * 1000ULL)
            {
                /* IRS liveness net (replaces keepalive): the peer has gone
                 * silent past the no-progress budget and we have no data to
                 * piggyback a turn on, so the link is dead.  Tear it down
                 * instead of re-arming the idle hold forever. */
                HLOGW(LOG_COMP,
                      "IRS inactivity (%llus without RX) — disconnecting",
                      (unsigned long long)((time_now_ms() - sess->last_rx_ms) / 1000));
                sess->tx_retries_left = ARQ_DISCONNECT_RETRY_SLOTS;
                sess_enter(sess, ARQ_CONN_DISCONNECTING,
                           time_now_ms() + ARQ_CHANNEL_GUARD_MS,
                           ARQ_EV_TIMER_ACK);
            }
            else
            {
                /* Reset-on-miss: a full idle hold passed with no DATA, so our
                 * RX-mode mirror and the mode actually on the air have come
                 * apart and we are deaf until they meet again.  Only one
                 * payload decoder runs at a time, so a one-rung disagreement
                 * is total deafness — there is no partial decode to steer by.
                 *
                 * Walk DOWN a rung per hold and PARK at the floor.  Parking is
                 * right because the sender ends up at the floor too: a retry
                 * steps it down a rung at a time and, now that a frame is never
                 * read larger than a rung that has already delivered
                 * (send_data_burst), it can always follow the ladder all the
                 * way down.  Cycling back up instead was measured on the 0 dB
                 * cell and is worse: it puts the mirror on the sender's actual
                 * rung for 15 s in every 75 while the sender sits at the floor
                 * transmitting into a decoder that has wandered off. */
                if (sess->rx_speed_level > 0)
                    irs_mirror_peer_ladder(sess, false);
                enter_idle_irs(sess);   /* re-arm the idle hold */
            }
        }
        break;

    case ARQ_DFLOW_ACK_TX:
        if (ev->id == ARQ_EV_TIMER_ACK)
        {
            /* Guard elapsed — emit the pattern ACK (break if we have data).
             *
             * The post-ACCEPT connect confirmation takes this same path.  It
             * used to build a coded DATAC16 ACK, which cost 3.74 s on the air
             * to say one thing the answerer already has every other bit of:
             * "I heard your ACCEPT".  The pattern says it in 0.64 s and, being
             * a full-energy Welch-Costas correlation rather than a coded
             * frame, says it roughly 10 dB further down — so the third leg of
             * the handshake stops being the most fragile one.  It carries no
             * session id, but none is needed: the answerer is in ACCEPTING
             * with exactly one call outstanding. */
            send_ack(sess, 0);
            if (g_timing && !sess->pending_connect_confirm)
                arq_timing_record_ack_tx(g_timing, (int)sess->rx_expected - 1);
            dflow_enter(sess, ARQ_DFLOW_ACK_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        }
        else if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            if (sess->pending_connect_confirm)
            {
                sess->pending_connect_confirm = false;
                if (session_tx_backlog(sess) > 0)
                    enter_idle_iss_guarded(sess, false);
                else
                    enter_idle_iss(sess, false);
            }
            else if (sess->acktx_had_has_data && sess->peer_has_data)
            {
                /* Simultaneous bid: both ends want the floor (the peer's DATA
                 * carried HAS_DATA and our ACK was a break).  Break the tie by
                 * role so exactly one side takes ISS — the CALLER wins, the
                 * CALLEE yields.  The peer applies the mirror rule in its
                 * WAIT_ACK RX_ACK handler, so there is no double-ISS. */
                if (sess->role == ARQ_ROLE_CALLER)
                {
                    if (g_timing) arq_timing_record_turn(g_timing, true, "piggyback");
                    enter_idle_iss_guarded(sess, true);
                }
                else
                {
                    enter_idle_irs(sess);
                }
            }
            else if (sess->peer_has_data)
            {
                /* Peer is still the active ISS (it had HAS_DATA or we saw a
                 * duplicate) — stay IRS. */
                enter_idle_irs(sess);
            }
            else if (sess->acktx_had_has_data)
            {
                /* We sent an ACK+TURN break and the peer had no data — the peer
                 * yields, so it is safe to take the ISS role (piggyback turn). */
                if (g_timing) arq_timing_record_turn(g_timing, true, "piggyback");
                enter_idle_iss_guarded(sess, true);
            }
            else
            {
                enter_idle_irs(sess);
            }
        }
        else if (ev->id == ARQ_EV_RX_DATA)
        {
            /* Another frame arrived during the guard/ACK window — receive it
             * and re-arm (covers a lost ACK where the ISS retransmits). */
            irs_receive_data(sess, ev);
            irs_arm_ack_deadline(sess, ev);
        }
        break;

    default:
        break;
    }
}


/* ======================================================================
 * Top-level dispatch
 * ====================================================================== */

void arq_fsm_dispatch(arq_session_t *sess, const arq_event_t *ev)
{
    if (!sess || !ev)
        return;

    HLOGD(LOG_COMP, "state=%s dflow=%s ev=%s",
          arq_conn_state_name(sess->conn_state),
          arq_dflow_state_name(sess->dflow_state),
          arq_event_name(ev->id));

    /* Track last RX time from any received frame */
    switch (ev->id)
    {
    case ARQ_EV_RX_DATA:
    case ARQ_EV_RX_ACK:
    case ARQ_EV_RX_CALL:
    case ARQ_EV_RX_ACCEPT:
    case ARQ_EV_RX_DISCONNECT:
        sess->last_rx_ms = time_now_ms();
        /* Session ID validation: drop frames from a different session when
         * we are in CONNECTED or DISCONNECTING state (CALL/ACCEPT frames
         * are handled separately and carry session_id in their own format). */
        if ((sess->conn_state == ARQ_CONN_CONNECTED ||
             sess->conn_state == ARQ_CONN_DISCONNECTING) &&
            ev->id != ARQ_EV_RX_CALL && ev->id != ARQ_EV_RX_ACCEPT &&
            ev->session_id != 0 && ev->session_id != sess->session_id)
        {
            HLOGD(LOG_COMP, "Session ID mismatch: got %d expected %d — dropped",
                  (int)ev->session_id, (int)sess->session_id);
            return;
        }
        break;
    default:
        break;
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
}
