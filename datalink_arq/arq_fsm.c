/* HERMES Modem — ARQ FSM implementation
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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
    sess->payload_mode        = arq_mode_ladder[0];   /* ladder floor = MFSK */
    sess->peer_tx_mode        = arq_mode_ladder[0];   /* RX decoder starts at floor */
    sess->initial_payload_mode = arq_mode_ladder[0];  /* overwritten by arq_set_initial_mode */
    sess->speed_level    = 0;
    sess->tx_success_count = 0;
    sess->fast_ramp      = true;
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
    sess->tx_base            = 0;
    sess->tx_burst_retx      = false;
    sess->tx_burst_count     = 0;
    sess->burst_depth        = 1;
    for (int i = 0; i < ARQ_WIN_SLOTS; i++)
    {
        sess->tx_win[i].present = false;
        sess->tx_win[i].len     = 0;
        sess->tx_win[i].retx    = false;
        sess->rx_win[i].present = false;
        sess->rx_win[i].len     = 0;
    }
    sess->rx_burst_complete  = false;
    sess->rx_burst_dup       = false;
    sess->rx_burst_new       = false;
    sess->rx_burst_blocks    = 0;
    sess->tx_retries_left    = ARQ_DATA_RETRY_SLOTS;
    sess->speed_level        = 0;
    sess->tx_success_count   = 0;
    sess->fast_ramp          = true;
    sess->rx_speed_level     = 0;
    sess->rx_success_count   = 0;
    sess->rx_fast_ramp       = true;
    sess->payload_mode       = arq_mode_ladder[0];   /* MFSK floor */
    sess->peer_tx_mode       = arq_mode_ladder[0];
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
    sess->deadline_ms    = deadline_ms;
    sess->deadline_event = deadline_event;
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
        sess->tx_base          = sess->tx_seq;
        sess->tx_burst_retx    = false;
        sess->tx_burst_count   = 0;
        sess->burst_depth      = 1;
        for (int i = 0; i < ARQ_WIN_SLOTS; i++)
        {
            sess->tx_win[i].present = false;
            sess->tx_win[i].len     = 0;
            sess->tx_win[i].retx    = false;
            sess->rx_win[i].present = false;
            sess->rx_win[i].len     = 0;
        }
        sess->rx_burst_complete = false;
        sess->rx_burst_dup      = false;
        sess->rx_burst_new      = false;
        sess->rx_burst_blocks   = 0;
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
static void apply_speed_level(arq_session_t *sess)
{
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
 *  over-climb oscillation.  Evaluated once PER BURST (a whole keydown is one
 *  clean/dirty outcome), so windowing does not change the ladder dynamics.
 *  Returns the signed level change (for logging). */
static int ladder_step(int *level, int *success_count, bool *fast_ramp, bool clean)
{
    int before = *level;
    if (!clean)
    {
        *fast_ramp = false;
        if (*level > 0)
            (*level)--;
        *success_count = 0;
        return *level - before;
    }
    (*success_count)++;
    int need = *fast_ramp ? 1 : ARQ_LADDER_UP_SUCCESSES;
    if (*success_count >= need && *level < ARQ_LADDER_LEVELS - 1)
    {
        (*level)++;
        *success_count = 0;
    }
    return *level - before;
}

/** Record the outcome of the retained TX frame once its fate is known.
 *  Delivery-driven, no SNR/OLLA/reverse-hold: clean == the frame was delivered
 *  with no retransmission; else it needed at least one retry. */
static void record_tx_outcome(arq_session_t *sess, bool clean)
{
    int delta = ladder_step(&sess->speed_level, &sess->tx_success_count,
                            &sess->fast_ramp, clean);
    if (delta < 0)
        HLOGD(LOG_COMP, "Ladder step-down to %d (retry)", sess->speed_level);
    else if (delta > 0)
        HLOGD(LOG_COMP, "Ladder step-up to %d", sess->speed_level);
    apply_speed_level(sess);

    /* Adaptive burst depth (see arq_session_t.burst_depth): hold depth at 1
     * while the mode is still moving (a clean burst that climbs, or any dirty
     * burst that steps down), then grow once the mode has SETTLED (clean, no
     * rung change) up to the mode's cap.  Growth is GEOMETRIC (slow-start
     * double), not +1: a good-channel transfer is dominated by the ramp itself
     * (at QAM16C2 the additive ramp 1+2+3+4+5 = 15 frames ~= 17 kB is spent
     * before steady K=5 is ever reached), so doubling reaches the cap in
     * ceil(log2(cap)) settled bursts instead of cap-1, banking the K=5 rate far
     * sooner.  Any loss/mode-change resets to 1 (multiplicative decrease), so an
     * over-shoot self-corrects and a fade re-ramps quickly — AIMD-with-
     * slow-start, the anti-oscillation invariant preserved (depth reacts to
     * loss, mode reacts to delivery; they never co-react). */
    if (!clean || delta != 0)
    {
        sess->burst_depth = 1;                 /* loss or mode change: back to 1 */
    }
    else
    {
        const arq_mode_timing_t *tm =
            arq_protocol_mode_timing(clamp_payload_mode_to_bandwidth(
                arq_mode_ladder[sess->speed_level]));
        int cap = (tm && tm->burst_frames > 0) ? tm->burst_frames : 1;
        if (cap > ARQ_BURST_MAX) cap = ARQ_BURST_MAX;
        if (sess->burst_depth < cap)
        {
            int nd = sess->burst_depth * 2;    /* slow-start double */
            if (nd > cap) nd = cap;
            sess->burst_depth = nd;
            HLOGD(LOG_COMP, "Burst depth grow to %d (mode settled at level %d)",
                  sess->burst_depth, sess->speed_level);
        }
    }
}

/** IRS: mirror the peer's (ISS) ladder from the outcome of a received DATA
 *  frame so our payload decoder is already on the mode the peer's NEXT burst
 *  will use.  clean_new == a new in-order frame decoded first try (mirrors the
 *  sender's clean delivery); a duplicate (our ACK was lost, the sender retried
 *  and stepped down) mirrors the sender's step-down.  Keeps peer_tx_mode ==
 *  arq_mode_ladder[rx_speed_level]. */
static void irs_mirror_peer_ladder(arq_session_t *sess, bool clean_new)
{
    int delta = ladder_step(&sess->rx_speed_level, &sess->rx_success_count,
                            &sess->rx_fast_ramp, clean_new);
    sess->peer_tx_mode =
        clamp_payload_mode_to_bandwidth(arq_mode_ladder[sess->rx_speed_level]);
    if (delta != 0)
        HLOGD(LOG_COMP, "IRS RX-mode mirror %s to level %d (mode=%d)",
              delta > 0 ? "climb" : "step-down",
              sess->rx_speed_level, sess->peer_tx_mode);
}


/* IRS: arm the consolidated per-burst ACK deadline after a received DATA
 * frame.  The frame's self-described burst_remaining says how many frames of
 * this keydown are still on the air:
 *   0  -> the burst is over; ack after the channel guard (the ISS relay needs
 *         the guard to switch TX->RX before our tones/frame arrive).
 *   R>0-> R more frames coming; wait their airtime (+guard+decode margin)
 *         before acking, so ONE ack covers the whole burst.  If the tail is
 *         lost this deadline still fires and the ack (then a SACK) names the
 *         holes — the tail-loss safety net.
 * Each further frame of the burst re-arms this with its own remaining count,
 * so the deadline converges on the true end of burst. */
static void irs_arm_ack_deadline(arq_session_t *sess, const arq_event_t *ev)
{
    uint8_t rem = ev->rx_flags & ARQ_FLAG_BURST_REM_MASK;
    uint64_t dl = time_now_ms() + ARQ_CHANNEL_GUARD_MS;
    if (rem > 0)
    {
        const arq_mode_timing_t *tm = arq_protocol_mode_timing(ev->mode);
        float dur_s = tm ? tm->frame_duration_s : 5.0f;
        /* frame_duration_s is a whole single-frame keydown (preamble incl.),
         * so rem*dur over-estimates the tail airtime — safe (waits longer). */
        dl += (uint64_t)(dur_s * 1000.0f) * rem + 1500 /* decode margin */;
    }
    dflow_enter(sess, ARQ_DFLOW_ACK_TX, dl, ARQ_EV_TIMER_ACK);
}

/* Windowed receive: insert a DATA frame into the reassembly window and deliver
 * the in-order prefix.  Returns true for a NEW frame (in-order or stored out of
 * order), false for a duplicate/out-of-range one.  Tracks the per-burst flags
 * that the consolidated ACK decision and the mode mirror consume. */
/* Reassemble ONE received block (seq + data) into the IRS window, delivering
 * the in-order prefix.  Returns true if the block was new (delivered now or
 * held for later), false if duplicate/undeliverable.  Called once per block of
 * a decoded modem frame. */
static bool deliver_rx_checked(arq_session_t *sess, uint8_t seq,
                               const uint8_t *data, size_t len)
{
    uint8_t d = (uint8_t)(seq - sess->rx_expected);

    if (d >= ARQ_WIN_SLOTS)
    {
        /* Behind the window base (peer retransmitting — our ack was lost) or
         * absurdly far ahead (corrupt header): not new either way. */
        HLOGD(LOG_COMP, "Duplicate data seq=%d (expected=%d) — suppressed",
              (int)seq, (int)sess->rx_expected);
        sess->rx_burst_dup = true;
        return false;
    }

    if (d == 0)
    {
        /* In order: deliver it and drain any contiguous run it unblocks. */
        if (len > 0 && g_cbs.deliver_rx_data)
            g_cbs.deliver_rx_data(data, len);
        sess->rx_expected = (uint8_t)(seq + 1);
        for (;;)
        {
            struct arq_rxslot *sl =
                &sess->rx_win[sess->rx_expected % ARQ_WIN_SLOTS];
            if (!sl->present || sl->seq != sess->rx_expected)
                break;   /* stored-seq check: never mis-deliver an aliased slot */
            if (sl->len > 0 && g_cbs.deliver_rx_data)
                g_cbs.deliver_rx_data(sl->data, sl->len);
            sl->present = false;
            sl->len = 0;
            sess->rx_expected = (uint8_t)(sess->rx_expected + 1);
        }
        sess->rx_burst_new = true;
        return true;
    }

    /* Out of order above the base: hold it for reassembly (validated by seq). */
    struct arq_rxslot *sl = &sess->rx_win[seq % ARQ_WIN_SLOTS];
    if (sl->present && sl->seq == seq)
    {
        sess->rx_burst_dup = true;   /* duplicate of a held slot */
        return false;
    }
    if (sl->present)
        return false;   /* slot busy with a different live seq — treat as lost */
    if (len > sizeof(sl->data))
        return false;                 /* cannot hold — treated as lost */
    memcpy(sl->data, data, len);
    sl->len     = len;
    sl->seq     = seq;
    sl->present = true;
    sess->rx_burst_new = true;
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
 * (== HAS_DATA piggyback), else a plain ACK.  No coded frame, no seq: a bare
 * pattern is only ever sent when the WHOLE outstanding burst arrived, so "an
 * ACK was heard" retires it unambiguously.  ack_delay_raw is unused (kept for
 * the DATA_RX call site). */
static void send_ack(arq_session_t *sess, uint8_t ack_delay_raw)
{
    (void)ack_delay_raw;
    int kind = (session_tx_backlog(sess) > 0) ? ARQ_PATTERN_BREAK
                                               : ARQ_PATTERN_ACK;
    sess->acktx_had_has_data = (kind == ARQ_PATTERN_BREAK);
    if (g_cbs.send_pattern_ack)
        g_cbs.send_pattern_ack(sess->payload_mode, kind);
}

/* IRS: emit the consolidated per-burst acknowledgement and reset the burst
 * tracking.  Split by outcome:
 *   - CLEAN (the burst-final frame arrived and no out-of-order holes are
 *     held): the fast Welch-Costas pattern — 0.64 s, ~10 dB more robust than
 *     a coded frame, and it needs no bits ("everything you sent arrived").
 *   - HOLES (missing frames below the highest received, or the burst tail
 *     never arrived): a coded SACK on the control mode — rcv_base + a bitmap
 *     of the out-of-order seqs held above it; the ISS retransmits exactly the
 *     un-named ones.  Carries HAS_DATA for the piggyback turn bid, like the
 *     pattern break.  The mirror steps down once here (unless the dup path
 *     already did): the ISS steps its ladder down on receiving the SACK. */
static void irs_send_burst_ack(arq_session_t *sess)
{
    uint8_t bitmap[ARQ_SACK_BITMAP_BYTES] = {0};
    bool    any_bit = false;
    for (int off = 1; off <= ARQ_SACK_BITMAP_BYTES * 8; off++)
    {
        uint8_t sq = (uint8_t)(sess->rx_expected + off);
        struct arq_rxslot *sl = &sess->rx_win[sq % ARQ_WIN_SLOTS];
        if (sl->present && sl->seq == sq)
        {
            bitmap[(off - 1) / 8] |= (uint8_t)(1u << ((off - 1) % 8));
            any_bit = true;
        }
    }
    bool holes = !sess->rx_burst_complete || any_bit;

    /* A bare pattern ACK carries no sequence, so the ISS can only read it as
     * "the block I am waiting for arrived" — it retires tx_base.  That is safe
     * ONLY for a single-block burst that delivered a genuinely NEW block:
     *   - multi-block bursts -> a stale pattern would retire the whole window;
     *   - a DUPLICATE-only burst (our earlier ACK was lost, the peer retried a
     *     block we already delivered) must NOT pattern-ACK: the peer may have
     *     pipelined on to a new tx_base, and the seq-less pattern would retire
     *     that not-yet-delivered block (a selective-repeat over-retirement ->
     *     stranded hole -> stall).
     * Both cases are acked instead by a coded ACK carrying rcv_base (base-
     * driven retirement is idempotent and stale-safe). */
    bool multi = (sess->rx_burst_blocks > 1);

    if (!holes && !multi && !sess->rx_burst_dup)
    {
        send_ack(sess, 0);                 /* fast pattern: clean new 1-block */
    }
    else
    {
        uint8_t flags = 0;
        if (session_tx_backlog(sess) > 0)
            flags |= ARQ_FLAG_HAS_DATA;
        sess->acktx_had_has_data = (flags & ARQ_FLAG_HAS_DATA) != 0;

        uint8_t snr_raw = 0;
        if (sess->local_snr_x10 != 0)
            snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);

        uint8_t frame[INT_BUFFER_SIZE];
        int n = arq_protocol_build_sack(frame, sizeof(frame), sess->session_id,
                                        sess->rx_expected, flags, snr_raw,
                                        bitmap);
        if (n > 0)
            send_frame(PACKET_TYPE_ARQ_CONTROL, sess->control_mode,
                       (size_t)n, frame, 0);
        HLOGD(LOG_COMP, "SACK: base=%d bitmap=0x%02x%02x%02x%02x%02x%02x%s",
              (int)sess->rx_expected, bitmap[0], bitmap[1], bitmap[2],
              bitmap[3], bitmap[4], bitmap[5],
              sess->rx_burst_complete ? "" : " (burst tail missing)");

        /* Only a genuine-hole SACK makes the peer retransmit and step its
         * ladder down; mirror once unless a duplicate already stepped us this
         * burst.  A clean multi-block burst acked here (no holes) already had
         * its climb credited per burst in irs_receive_data — don't re-credit. */
        if (holes && !sess->rx_burst_dup)
            irs_mirror_peer_ladder(sess, false);
    }

    /* Fresh tracking for the next burst. */
    sess->rx_burst_complete = false;
    sess->rx_burst_dup      = false;
    sess->rx_burst_new      = false;
    sess->rx_burst_blocks   = 0;
}

/* Build and transmit one KEYDOWN as block-packed DATA frames (windowed burst).
 *
 * Block selection (selective repeat; the window drains monotonically under
 * loss):
 *  - If un-acked blocks are outstanding (holes after a SACK, or the whole
 *    window after an ACK timeout / duplicate), retransmit THOSE, oldest first
 *    — never mixed with fresh blocks.
 *  - Else read fresh <=44-byte blocks from the app ring, each an IMMUTABLE
 *    unit (raw bytes read once, fixed seq) so a duplicate is idempotent.
 *
 * Blocks are packed into up to burst_frames modem frames at ONE mode =
 * ladder[speed_level], filling each frame to its payload BYTE BUDGET with
 * <=44-byte blocks (the last block of a frame is sized to the remaining
 * budget).  Byte-budget packing matters at the floor: a single 44-byte block
 * would waste half of MFSK's 90-byte frame (a low-SNR goodput penalty), so the
 * wide MFSK frame carries two blocks (44 + 42 = 86 user bytes) instead.  Every
 * block stays <=44 bytes, so it still fits DATAC4 (the narrowest ladder rung,
 * 46-byte payload) on a retransmit — no mode bump, and the peer decodes it at
 * whatever robust mode the fade forced.  Frames go out via send_tx_frame with
 * frame-level burst_remaining counting down to 0; the arq.c accumulator turns
 * them into ONE modem action (one preamble/keydown) and the IRS re-anchors per
 * frame + consolidates its ACK per burst. */
static void send_data_burst(arq_session_t *sess)
{
    if (!g_cbs.tx_read || !g_cbs.tx_backlog)
        return;

    int tx_mode = clamp_payload_mode_to_bandwidth(arq_mode_ladder[sess->speed_level]);
    const arq_mode_timing_t *tm = arq_protocol_mode_timing(tx_mode);
    if (!tm || (int)tm->payload_bytes <= ARQ_FRAME_HDR_SIZE)
        return;

    int cap = (int)tm->payload_bytes - ARQ_FRAME_HDR_SIZE;   /* block-area bytes/frame */
    /* Adaptive depth: the mode's burst_frames is only a CAP; the live depth
     * (grown from 1 as the ladder settles, see record_tx_outcome) is what
     * bounds this keydown so the climb stays fast. */
    int depth = sess->burst_depth;
    if (depth < 1) depth = 1;
    int max_frames = tm->burst_frames;
    if (max_frames > depth) max_frames = depth;
    if (max_frames < 1) max_frames = 1;
    if (max_frames > ARQ_BURST_MAX) max_frames = ARQ_BURST_MAX;

    /* Floor (rung 0) sends EXACTLY ONE block per keydown, sized to fill the
     * MFSK frame (<=88 user bytes).  Structurally 1 block => the fringe keeps
     * the fast Welch-Costas pattern ACK (a multi-block burst forces the coded
     * DATAC16 ACK, whose ~-7 dB cliff cannot close the loop at the -13 dB
     * floor) AND the floor keeps its full goodput.  Every other rung packs
     * <=44-byte blocks (so a retransmit fits DATAC4) up to the frame's byte
     * budget.  Structural safety for the >44 floor block: it is only ever
     * created and retransmitted at rung 0 — we climb only after retiring it —
     * so an oversize block never reaches a narrow rung. */
    bool is_floor    = (sess->speed_level == 0);
    int  blk_max     = is_floor ? ARQ_BLOCK_DATA_FLOOR : ARQ_BLOCK_DATA_MAX;
    int  max_blk_pf  = is_floor ? 1 : ARQ_MAX_BLOCKS_PER_FRAME;

    bool retx_burst = arq_win_nonempty(sess);

    /* Selection fills per-frame block lists, each frame packed to <= cap bytes
     * with <=44-byte blocks (byte-budget packing, so a wide floor frame is not
     * wasted by a single small block).  A block always fits an empty frame: the
     * narrowest rung (DATAC4) has cap 46 >= 2 + 44. */
    uint8_t fseq[ARQ_BURST_MAX][ARQ_MAX_BLOCKS_PER_FRAME];
    int     fcnt[ARQ_BURST_MAX] = {0};
    int     total   = 0;   /* total blocks selected this keydown */
    int     nframes = 0;

    if (retx_burst)
    {
        /* Holes only, oldest first, greedily packed by byte budget. */
        int d = 0;
        for (int f = 0; f < max_frames && total < ARQ_WIN_SLOTS; f++)
        {
            int used = 0;
            for (; d < ARQ_WIN_SLOTS && total < ARQ_WIN_SLOTS &&
                   fcnt[f] < max_blk_pf; d++)
            {
                uint8_t s = (uint8_t)(sess->tx_base + d);
                struct arq_txslot *sl = &sess->tx_win[s % ARQ_WIN_SLOTS];
                if (!(sl->present && sl->seq == s))
                    continue;
                if (fcnt[f] > 0 &&
                    used + ARQ_BLOCK_HDR_SIZE + sl->len > cap)
                    break;   /* doesn't fit this frame — leave for the next one */
                sl->retx = true;
                fseq[f][fcnt[f]++] = s;
                used += ARQ_BLOCK_HDR_SIZE + sl->len;
                total++;
            }
            if (fcnt[f] == 0)
                break;                 /* no more holes to place */
            nframes = f + 1;
        }
    }
    else
    {
        sess->tx_base = sess->tx_seq;   /* window empty: base tracks next new */
        for (int f = 0; f < max_frames && total < ARQ_WIN_SLOTS; f++)
        {
            int used = 0;
            while (total < ARQ_WIN_SLOTS &&
                   fcnt[f] < max_blk_pf &&
                   cap - used >= ARQ_BLOCK_HDR_SIZE + 1)
            {
                int want = blk_max;
                int room = cap - used - ARQ_BLOCK_HDR_SIZE;
                if (want > room) want = room;
                struct arq_txslot *sl =
                    &sess->tx_win[sess->tx_seq % ARQ_WIN_SLOTS];
                int got = g_cbs.tx_read(sl->data, (size_t)want);
                if (got <= 0)
                    break;             /* backlog drained */
                sl->len     = got;
                sl->seq     = sess->tx_seq;
                sl->present = true;
                sl->retx    = false;
                fseq[f][fcnt[f]++] = sess->tx_seq;
                used += ARQ_BLOCK_HDR_SIZE + got;
                total++;
                sess->tx_seq = (uint8_t)(sess->tx_seq + 1);
            }
            if (fcnt[f] == 0)
                break;                 /* backlog drained */
            nframes = f + 1;
        }
    }
    if (total == 0)
        return;
    sess->tx_burst_retx  = retx_burst;
    sess->tx_burst_count = total;   /* blocks in this keydown (ladder credit) */

    uint8_t snr_raw = 0;
    if (sess->local_snr_x10 != 0)
        snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);

    /* --- Emit each frame (one action, one preamble for the whole keydown) --- */
    for (int f = 0; f < nframes; f++)
    {
        arq_block_t blk[ARQ_MAX_BLOCKS_PER_FRAME];
        int frame_bytes = 0;
        for (int j = 0; j < fcnt[f]; j++)
        {
            struct arq_txslot *sl = &sess->tx_win[fseq[f][j] % ARQ_WIN_SLOTS];
            blk[j].seq  = fseq[f][j];
            blk[j].len  = (uint16_t)sl->len;
            blk[j].data = sl->data;
            frame_bytes += sl->len;
        }

        uint8_t data_flags = 0;
        if (session_tx_backlog(sess) > 0)
            data_flags |= ARQ_FLAG_HAS_DATA;
        /* Self-describing burst: modem frames still to come in THIS keydown. */
        uint8_t burst_remaining = (uint8_t)(nframes - 1 - f);
        data_flags |= (burst_remaining & ARQ_FLAG_BURST_REM_MASK);

        uint8_t frame[INT_BUFFER_SIZE];
        int n = arq_protocol_build_data_blocks(frame, sizeof(frame),
                                               sess->session_id,
                                               sess->rx_expected, data_flags,
                                               snr_raw, blk, fcnt[f]);
        if (n <= 0)
            return;

        /* Zero-pad to the mode's full modem-frame payload: codec2 raw-data
         * modes carry a fixed byte count per frame, and a constant frame_size
         * keeps the RX mode-inference (by frame_size) unambiguous.  block_count
         * bounds the parse, so the padding is ignored on RX. */
        int full = (int)tm->payload_bytes;
        if (n < full && full <= (int)sizeof(frame))
        {
            memset(frame + n, 0, (size_t)(full - n));
            n = full;
        }

        send_frame(PACKET_TYPE_ARQ_DATA, tx_mode, (size_t)n, frame,
                   (int)burst_remaining);
        if (g_timing)
            arq_timing_record_tx_queue(g_timing, (int)blk[0].seq, tx_mode,
                                       session_tx_backlog(sess), frame_bytes);
    }
}

/* ======================================================================
 * Level 1 FSM per-state handlers
 * ====================================================================== */

static void fsm_dflow(arq_session_t *sess, const arq_event_t *ev);

static void enter_idle_iss(arq_session_t *sess, bool gained_turn)
{
    (void)gained_turn;  /* per-direction mode: my TX mode evolves independently */
    sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;  /* fresh counter on ISS role entry */
    if (session_tx_backlog(sess) > 0 || arq_win_nonempty(sess))
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
    if (session_tx_backlog(sess) > 0 || arq_win_nonempty(sess))
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
            sess->startup_deadline_ms = time_now_ms() + (ARQ_STARTUP_MAX_S * 1000ULL);
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
            sess->startup_deadline_ms =
                time_now_ms() + (ARQ_STARTUP_MAX_S * 1000ULL);
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

    case ARQ_EV_TIMER_RETRY:
        if (sess->tx_retries_left > 0)
        {
            sess->tx_retries_left--;
            send_call_accept(sess, false);
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
        reset_session_data_state(sess);  /* discard stale retransmit buf; MFSK-start */
        sess->startup_deadline_ms =
            time_now_ms() + (ARQ_STARTUP_MAX_S * 1000ULL);
        if (g_cbs.notify_connected)
            g_cbs.notify_connected(sess->remote_call, sess->local_call);
        if (g_timing)
            arq_timing_record_connect(g_timing, sess->control_mode);
        sess_enter(sess, ARQ_CONN_CONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        enter_idle_irs(sess);       /* callee receives first; process incoming data */
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
        break;

    case ARQ_EV_TIMER_RETRY:
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

    case ARQ_EV_APP_STOP_LISTEN:  /* host wants the radio back — stop answering */
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
            arq_win_nonempty(sess) ||
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
    bool was_dup   = sess->rx_burst_dup;

    /* One decoded modem frame carries nblocks blocks; reassemble each into the
     * IRS window.  A single-block frame (nblocks<=0 legacy) falls back to the
     * whole-payload path. */
    bool any_new = false;
    int  nb = ev->nblocks > 0 ? ev->nblocks : 1;
    for (int i = 0; i < nb; i++)
    {
        uint8_t        bseq;
        const uint8_t *bdata;
        size_t         blen;
        if (ev->nblocks > 0)
        {
            bseq  = ev->blocks[i].seq;
            bdata = ev->payload + ev->blocks[i].off;
            blen  = ev->blocks[i].len;
        }
        else
        {
            bseq  = ev->seq;
            bdata = ev->payload;
            blen  = ev->payload_len;
        }
        sess->rx_burst_blocks++;           /* blocks seen in the current burst */
        if (deliver_rx_checked(sess, bseq, bdata, blen))
            any_new = true;
    }

    uint8_t rem = ev->rx_flags & ARQ_FLAG_BURST_REM_MASK;
    if (rem == 0)
        sess->rx_burst_complete = true;

    /* Mode mirror, PER BURST (matches the sender's per-burst ladder):
     *  - the first duplicate of a burst = the peer is retransmitting (it
     *    stepped down at its ACK timeout / SACK) -> mirror one step down;
     *  - the final frame (remaining==0) of an all-clean burst (no dup, no
     *    out-of-order holes held) = a clean delivery on the sender's side ->
     *    mirror the climb.  Mid-burst frames never touch the mirror. */
    if (sess->rx_burst_dup && !was_dup)
        irs_mirror_peer_ladder(sess, false);
    else if (rem == 0 && any_new && !sess->rx_burst_dup)
    {
        bool holes = false;
        for (int off = 1; off <= ARQ_SACK_BITMAP_BYTES * 8; off++)
        {
            uint8_t sq = (uint8_t)(sess->rx_expected + off);
            struct arq_rxslot *sl = &sess->rx_win[sq % ARQ_WIN_SLOTS];
            if (sl->present && sl->seq == sq)
                holes = true;
        }
        if (!holes)
            /* Clean complete burst: mirror the sender's climb. */
            irs_mirror_peer_ladder(sess, true);
    }

    if (any_new && g_timing)
        arq_timing_record_data_rx(g_timing, (int)ev->seq,
                                  (int)ev->data_bytes, sess->local_snr_x10);
    sess->last_rx_ms = time_now_ms();
    sess->peer_has_data = any_new
                          ? (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0
                          : true;
}

/* ISS: a bare pattern ACK carries no sequence, so it can only mean "the block
 * my last keydown just sent arrived".  A pattern is only sent for a 1-block
 * keydown (the MFSK floor) — the oldest outstanding block, tx_base.  Retire
 * THAT ONE and advance tx_base to the next present slot; never the whole
 * window (older holes still being drained one per floor keydown must survive).
 * Returns true if the window is now empty. */
static bool iss_retire_one(arq_session_t *sess)
{
    struct arq_txslot *sl = &sess->tx_win[sess->tx_base % ARQ_WIN_SLOTS];
    if (sl->present && sl->seq == sess->tx_base)
    {
        sl->present = false;
        sl->len     = 0;
        sl->retx    = false;
    }
    /* Advance base to the next still-present seq (or tx_seq if drained). */
    while (sess->tx_base != sess->tx_seq &&
           !sess->tx_win[sess->tx_base % ARQ_WIN_SLOTS].present)
        sess->tx_base = (uint8_t)(sess->tx_base + 1);

    sess->last_tx_progress_ms = time_now_ms();
    if (!arq_win_nonempty(sess))
    {
        sess->tx_base         = sess->tx_seq;
        sess->tx_burst_retx   = false;
        sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;
    }
    if (g_cbs.send_buffer_status)
        g_cbs.send_buffer_status(session_tx_backlog(sess));
    return !arq_win_nonempty(sess);
}

/* ISS: the WHOLE outstanding burst was delivered (an implicit ACK carried by
 * the peer's reverse DATA — the peer would not send new data before acking
 * ours).  Every frame is an immutable unit, so clear all slots and advance. */
static void iss_retire_all(arq_session_t *sess)
{
    for (int i = 0; i < ARQ_WIN_SLOTS; i++)
    {
        sess->tx_win[i].present = false;
        sess->tx_win[i].len     = 0;
        sess->tx_win[i].retx    = false;
    }
    sess->tx_base             = sess->tx_seq;
    sess->tx_burst_retx       = false;
    sess->last_tx_progress_ms = time_now_ms();
    sess->tx_retries_left     = ARQ_DATA_RETRY_SLOTS;
    if (g_cbs.send_buffer_status)
        g_cbs.send_buffer_status(session_tx_backlog(sess));
}

/* ISS: apply a selective ACK.  Seqs behind rcv_base and seqs named in the
 * bitmap are delivered; the rest stay present (the holes).  Advances tx_base
 * to the lowest still-outstanding seq.  Returns the hole count. */
static int iss_apply_sack(arq_session_t *sess, uint8_t base,
                          const uint8_t bitmap[ARQ_SACK_BITMAP_BYTES])
{
    int     holes = 0;
    bool    have_hole = false;
    uint8_t lowest_hole = 0;
    bool    progressed = false;

    /* Stale/aliased-base guard.  rcv_base MUST lie within the live window
     * [tx_base, tx_seq]: a peer can only ack seqs the ISS actually sent, and
     * its rx_expected never runs ahead of tx_seq.  A delayed or echoed ACK
     * from an EARLIER mod-256 generation aliases to a base far outside the
     * window; the signed "rel < 0 => delivered" rule below would then classify
     * live blocks as "before base" and falsely retire them.  Reject it (-1):
     * the caller ignores this ACK and keeps waiting for a live one.  A
     * legitimate ACK is never rejected (0 <= base-tx_base <= tx_seq-tx_base). */
    uint8_t span = (uint8_t)(sess->tx_seq - sess->tx_base);
    if ((uint8_t)(base - sess->tx_base) > span)
        return -1;

    for (int i = 0; i < ARQ_WIN_SLOTS; i++)
    {
        struct arq_txslot *sl = &sess->tx_win[i];
        if (!sl->present)
            continue;

        int8_t rel = (int8_t)(sl->seq - base);   /* signed distance from base */
        bool delivered;
        if (rel < 0)
            delivered = true;                    /* strictly before base      */
        else if (rel == 0)
            delivered = false;                   /* base itself = the hole    */
        else if (rel <= ARQ_SACK_BITMAP_BYTES * 8)
            delivered = (bitmap[(rel - 1) / 8] >> ((rel - 1) % 8)) & 1;
        else
            delivered = false;                   /* ahead of the bitmap: hole */

        if (delivered)
        {
            sl->present = false;
            sl->len     = 0;
            sl->retx    = false;
            progressed  = true;
        }
        else
        {
            holes++;
            if (!have_hole || (int8_t)(sl->seq - lowest_hole) < 0)
            {
                lowest_hole = sl->seq;
                have_hole   = true;
            }
        }
    }

    sess->tx_base = have_hole ? lowest_hole : sess->tx_seq;
    if (progressed)
        sess->last_tx_progress_ms = time_now_ms();
    if (!have_hole)
    {
        sess->tx_burst_retx   = false;
        sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;
    }
    if (g_cbs.send_buffer_status)
        g_cbs.send_buffer_status(session_tx_backlog(sess));
    return holes;
}

static void fsm_dflow(arq_session_t *sess, const arq_event_t *ev)
{
    const arq_mode_timing_t *tm;

    switch (sess->dflow_state)
    {
    case ARQ_DFLOW_IDLE_ISS:
        if (ev->id == ARQ_EV_APP_DATA_READY &&
            (session_tx_backlog(sess) > 0 || arq_win_nonempty(sess)))
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
                arq_timing_record_tx_start(g_timing, (int)sess->tx_base,
                                           sess->payload_mode,
                                           session_tx_backlog(sess));
        }
        else if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            if (g_timing)
                arq_timing_record_tx_end(g_timing, (int)sess->tx_base);
            tm = arq_protocol_mode_timing(sess->payload_mode);
            dflow_enter(sess, ARQ_DFLOW_WAIT_ACK,
                        deadline_from_s(tm ? tm->ack_timeout_s : 9.0f),
                        ARQ_EV_TIMER_ACK);
        }
        break;

    case ARQ_DFLOW_WAIT_ACK:
        if (ev->id == ARQ_EV_RX_ACK)
        {
            /* Consolidated per-burst ACK.  A bare pattern ACK is only sent by
             * the IRS when the WHOLE burst arrived, so it retires every
             * outstanding slot; a SACK names the delivered seqs and the rest
             * are holes to retransmit NOW (ACK-driven, no timer wait).  A
             * stale ACK with nothing outstanding is ignored. */
            if (!arq_win_nonempty(sess))
            {
                HLOGD(LOG_COMP, "ACK with no outstanding frame — ignored");
                break;
            }

            if (ev->sack_present)
            {
                int holes = iss_apply_sack(sess, ev->ack_seq, ev->sack_bitmap);
                if (holes < 0)
                {
                    /* Stale/aliased ACK base outside the live window — ignore
                     * and keep waiting for a live ACK (or the retry timer),
                     * rather than falsely retiring the window. */
                    HLOGD(LOG_COMP, "Stale SACK base=%d (win %d..%d) — ignored",
                          (int)ev->ack_seq, (int)sess->tx_base, (int)sess->tx_seq);
                    break;
                }
                sess->peer_has_data = (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0;
                if (holes > 0)
                {
                    /* The burst lost frames: one ladder step down per SACK
                     * (delivery-driven, same rule as an ACK-timeout retry),
                     * one retry slot consumed, and the holes go straight back
                     * out after the post-ACK guard (the IRS just keyed). */
                    HLOGD(LOG_COMP, "SACK: %d hole(s), retransmitting", holes);
                    record_tx_outcome(sess, false);
                    if (sess->tx_retries_left > 0)
                        sess->tx_retries_left--;
                    if (g_timing)
                        arq_timing_record_retry(g_timing, (int)sess->tx_base,
                                                ARQ_DATA_RETRY_SLOTS -
                                                    (int)sess->tx_retries_left,
                                                "sack_holes");
                    dflow_enter(sess, ARQ_DFLOW_DATA_TX,
                                time_now_ms() + ARQ_ISS_POST_ACK_GUARD_MS,
                                ARQ_EV_TIMER_ACK);
                    break;
                }
                /* SACK retired everything (stale/racy tail) — fall through to
                 * the delivered path below with a non-clean outcome. */
            }

            /* A clean delivery == the last keydown's frames all arrived first
             * try: no holes on this ack (a coded ACK that retired everything,
             * or a pattern that emptied the window) AND the burst carried no
             * retransmission AND the full retry budget is intact.  A dirty
             * outcome already stepped the ladder down (SACK holes / TIMER_ACK
             * retransmit), so it must not be re-penalised here. */
            bool clean = !sess->tx_burst_retx &&
                         sess->tx_retries_left == ARQ_DATA_RETRY_SLOTS;
            if (g_timing)
                arq_timing_record_ack_rx(g_timing, (int)sess->tx_base,
                                         (uint8_t)ev->ack_delay_raw,
                                         sess->local_snr_x10);

            /* Bare pattern ACK: retires ONLY the single frame the last keydown
             * sent (tx_base); a SACK that fell through here retired via its
             * base+bitmap already.  If holes remain (older seqs still
             * outstanding after a 1-at-a-time floor retransmit), retransmit
             * them instead of yielding the turn — no ladder credit yet. */
            if (!ev->sack_present)
                iss_retire_one(sess);
            if (arq_win_nonempty(sess))
            {
                dflow_enter(sess, ARQ_DFLOW_DATA_TX,
                            time_now_ms() + ARQ_ISS_POST_ACK_GUARD_MS,
                            ARQ_EV_TIMER_ACK);
                break;
            }

            /* Window fully drained — credit the clean burst (K deliveries). */
            if (clean)
                record_tx_outcome(sess, true);
            sess->peer_has_data = (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0;
            bool i_have_data = session_tx_backlog(sess) > 0 || arq_win_nonempty(sess);

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
                          (int)sess->tx_base);
                    sess->tx_retries_left = 1;
                }
                sess->tx_retries_left--;
                sess->tx_burst_retx = true;   /* retransmit ends "clean" */
                /* Any retry steps the ladder down one rung (delivery-driven):
                 * a fade descends quickly toward the robust floor rather than
                 * burning the whole retry budget at a mode the channel can no
                 * longer carry.  The eventual (dirty) ACK does NOT re-penalise. */
                record_tx_outcome(sess, false);
                if (g_timing)
                    arq_timing_record_retry(g_timing, (int)sess->tx_base,
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
                          (int)sess->tx_base,
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
                          (int)sess->tx_base, since_s, ARQ_NO_PROGRESS_TIMEOUT_S);
                    if (g_timing)
                        arq_timing_record_retry(g_timing, (int)sess->tx_base,
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
            /* In-window classification: any NEW frame (in order or an
             * out-of-order member of the peer's burst whose earlier frames
             * were lost) means the peer moved on to sending fresh data — it
             * would not do that before acknowledging ours, so it is an
             * implicit ACK of the whole outstanding burst.  A frame BEHIND
             * the window base is a duplicate (our ack was lost; the peer is
             * retransmitting): re-send our burst so it can ack it. */
            uint8_t d = (uint8_t)(ev->seq - sess->rx_expected);
            if (d < ARQ_WIN_SLOTS)
            {
                HLOGD(LOG_COMP,
                      "RX_DATA in WAIT_ACK (new seq=%d) — implicit ACK for base=%d",
                      (int)ev->seq, (int)sess->tx_base);
                bool clean = !sess->tx_burst_retx &&
                             sess->tx_retries_left == ARQ_DATA_RETRY_SLOTS;
                if (arq_win_nonempty(sess))
                {
                    iss_retire_all(sess);
                    record_tx_outcome(sess, clean);
                }
                irs_receive_data(sess, ev);
                irs_arm_ack_deadline(sess, ev);
            }
            else
            {
                HLOGD(LOG_COMP,
                      "RX_DATA in WAIT_ACK (dup seq=%d expected=%d) — re-TX base=%d",
                      (int)ev->seq, (int)sess->rx_expected, (int)sess->tx_base);
                irs_receive_data(sess, ev);   /* records dup + mirror step   */
                sess->tx_burst_retx = true;
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
            if (session_tx_backlog(sess) > 0 || arq_win_nonempty(sess))
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
                /* Reset-on-miss: a full idle hold passed with no DATA.  If a
                 * lost ACK left our RX-mode mirror climbed ABOVE the sender
                 * (which stepped down on its retry), we can no longer decode
                 * its bursts — silently stalling.  Step the mirror down one
                 * rung toward the floor so the two ends re-rendezvous; the
                 * MFSK floor is the guaranteed common ground.  At faster modes
                 * frames arrive well within the hold, so this only fires on a
                 * genuine stall (at the floor a step-down is a harmless no-op). */
                if (sess->rx_speed_level > 0)
                    irs_mirror_peer_ladder(sess, false);
                enter_idle_irs(sess);   /* re-arm the idle hold */
            }
        }
        break;

    case ARQ_DFLOW_ACK_TX:
        if (ev->id == ARQ_EV_TIMER_ACK)
        {
            if (sess->pending_connect_confirm)
            {
                /* Post-ACCEPT connect confirmation still rides the coded ACK. */
                uint8_t frame[INT_BUFFER_SIZE];
                uint8_t snr_raw = 0;
                if (sess->local_snr_x10 != 0)
                    snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);
                int n = arq_protocol_build_ack(frame, sizeof(frame),
                                               sess->session_id, sess->rx_expected,
                                               0, snr_raw, 0);
                if (n > 0)
                    send_frame(PACKET_TYPE_ARQ_CONTROL, sess->control_mode,
                               (size_t)n, frame, 0);
            }
            else
            {
                /* Guard/burst-tail deadline elapsed — emit the consolidated
                 * per-burst ack: fast pattern when the burst arrived clean, a
                 * coded SACK naming the holes otherwise. */
                irs_send_burst_ack(sess);
                if (g_timing)
                    arq_timing_record_ack_tx(g_timing, (int)sess->rx_expected - 1);
            }
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
