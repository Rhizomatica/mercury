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

const char *arq_dflow_state_name(arq_dflow_state_t s)
{
    static const char *names[] = {
        [ARQ_DFLOW_IDLE_ISS]       = "IDLE_ISS",
        [ARQ_DFLOW_DATA_TX]        = "DATA_TX",
        [ARQ_DFLOW_WAIT_ACK]       = "WAIT_ACK",
        [ARQ_DFLOW_IDLE_IRS]       = "IDLE_IRS",
        [ARQ_DFLOW_DATA_RX]        = "DATA_RX",
        [ARQ_DFLOW_ACK_TX]         = "ACK_TX",
        [ARQ_DFLOW_TURN_REQ_TX]    = "TURN_REQ_TX",
        [ARQ_DFLOW_TURN_REQ_WAIT]  = "TURN_REQ_WAIT",
        [ARQ_DFLOW_TURN_ACK_TX]    = "TURN_ACK_TX",
        [ARQ_DFLOW_MODE_REQ_TX]    = "MODE_REQ_TX",
        [ARQ_DFLOW_MODE_REQ_WAIT]  = "MODE_REQ_WAIT",
        [ARQ_DFLOW_MODE_ACK_TX]    = "MODE_ACK_TX",
        [ARQ_DFLOW_KEEPALIVE_TX]   = "KEEPALIVE_TX",
        [ARQ_DFLOW_KEEPALIVE_WAIT] = "KEEPALIVE_WAIT",
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
        [ARQ_EV_RX_TURN_REQ]        = "RX_TURN_REQ",
        [ARQ_EV_RX_TURN_ACK]        = "RX_TURN_ACK",
        [ARQ_EV_RX_MODE_REQ]        = "RX_MODE_REQ",
        [ARQ_EV_RX_MODE_ACK]        = "RX_MODE_ACK",
        [ARQ_EV_RX_KEEPALIVE]       = "RX_KEEPALIVE",
        [ARQ_EV_RX_KEEPALIVE_ACK]   = "RX_KEEPALIVE_ACK",
        [ARQ_EV_TIMER_RETRY]        = "TIMER_RETRY",
        [ARQ_EV_TIMER_TIMEOUT]      = "TIMER_TIMEOUT",
        [ARQ_EV_TIMER_ACK]          = "TIMER_ACK",
        [ARQ_EV_TIMER_PEER_BACKLOG] = "TIMER_PEER_BACKLOG",
        [ARQ_EV_TIMER_KEEPALIVE]    = "TIMER_KEEPALIVE",
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
    sess->control_mode        = FREEDV_MODE_DATAC13;
    sess->payload_mode        = FREEDV_MODE_DATAC4;  /* my TX mode, starts at safest level */
    sess->peer_tx_mode        = FREEDV_MODE_DATAC4;  /* RX decoder, starts at safest level */
    sess->initial_payload_mode = FREEDV_MODE_DATAC4;  /* overwritten by arq_set_initial_mode */
    sess->speed_level    = 0;
    sess->tx_success_count = 0;
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

static void sess_enter(arq_session_t *sess, arq_conn_state_t new_state,
                       uint64_t deadline_ms, arq_event_id_t deadline_event)
{
    HLOGD(LOG_COMP, "conn: %s -> %s",
          arq_conn_state_name(sess->conn_state),
          arq_conn_state_name(new_state));
    sess->conn_state     = new_state;
    sess->state_enter_ms = hermes_uptime_ms();
    sess->deadline_ms    = deadline_ms;
    sess->deadline_event = deadline_event;
    if (new_state != ARQ_CONN_CONNECTED)
    {
        sess->pending_connect_confirm = false;
        sess->need_initial_guard = false;
    }
    /* Reset data-flow and mode state when returning to idle connection states.
     * Restore peer_tx_mode to initial_payload_mode (= broadcast mode) so the
     * payload decoder can receive broadcast frames while LISTENING.  The
     * session-start paths (RX_CALL, APP_CONNECT) override this to DATAC4
     * before entering ACCEPTING/CALLING. */
    if (new_state == ARQ_CONN_DISCONNECTED || new_state == ARQ_CONN_LISTENING)
    {
        sess->dflow_state       = ARQ_DFLOW_IDLE_ISS;
        sess->peer_tx_mode      = sess->initial_payload_mode;
        sess->tx_inflight_bytes = 0;
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

static void send_frame(int ptype, int mode, size_t len, const uint8_t *frame)
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
        g_cbs.send_tx_frame(ptype, mode, slot, padded);
        return;
    }

    g_cbs.send_tx_frame(ptype, mode, len, frame);
}

static uint64_t deadline_from_s(float seconds)
{
    return hermes_uptime_ms() + (uint64_t)(seconds * 1000.0f + 0.5f);
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

/** Update peer_snr_x10 from the sender's SNR feedback carried in a received
 *  frame.  The DATA frame's snr_encoded = sender's local_snr_x10 = what the
 *  sender (current ISS) receives from us (current IRS). */
static void update_peer_snr(arq_session_t *sess, const arq_event_t *ev)
{
    if (ev->snr_encoded != 0)
        sess->peer_snr_x10 =
            (int)(arq_protocol_decode_snr((uint8_t)ev->snr_encoded) * 10.0f);
}

/** Build and send a MODE_REQ or MODE_ACK control frame. */
static void send_mode_negotiation(arq_session_t *sess, arq_subtype_t subtype, int mode)
{
    uint8_t frame[INT_BUFFER_SIZE];
    uint8_t snr_raw = 0;
    if (sess->local_snr_x10 != 0)
        snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);

    int n = -1;
    if (subtype == ARQ_SUBTYPE_MODE_REQ)
        n = arq_protocol_build_mode_req(frame, sizeof(frame),
                                        sess->session_id, snr_raw, mode);
    else
        n = arq_protocol_build_mode_ack(frame, sizeof(frame),
                                        sess->session_id, snr_raw, mode);
    if (n > 0)
        send_frame(PACKET_TYPE_ARQ_CONTROL, sess->control_mode, (size_t)n, frame);
}

/** Map a FreeDV payload mode to a comparable rank: higher rank = faster/more
 *  aggressive mode.  DATAC1 (=10) is fastest, DATAC3 (=12) is medium, and
 *  DATAC4 (=18) is slowest — the numeric constants decrease as throughput
 *  increases, so direct integer comparisons between them are misleading. */
static int mode_rank(int mode)
{
    if (mode == FREEDV_MODE_DATAC1) return 2;
    if (mode == FREEDV_MODE_DATAC3) return 1;
    return 0; /* DATAC4 or any other conservative mode */
}

static int clamp_payload_mode_to_bandwidth(int mode)
{
    if (!arq_bandwidth_allows_mode(mode) && mode == FREEDV_MODE_DATAC1)
        return FREEDV_MODE_DATAC3;

    return mode;
}

/** Record the outcome of a TX frame.  Called once per frame when its fate is
 *  known: clean=true when ACK arrived with no retries consumed, clean=false
 *  when the frame was retransmitted at least once.  Steps speed_level ladder
 *  up (slowly, after consecutive clean ACKs) or down (immediately on any
 *  non-clean outcome). */
static void record_tx_outcome(arq_session_t *sess, bool clean)
{
    if (!clean)
    {
        /* Any retry → step down immediately to improve reliability */
        if (sess->speed_level > 0)
        {
            sess->speed_level--;
            HLOGD(LOG_COMP, "Ladder step-down to %d (retry)", sess->speed_level);
        }
        sess->tx_success_count = 0;
        sess->consecutive_retries++;
    }
    else
    {
        sess->consecutive_retries = 0;
        sess->tx_success_count++;
        if (sess->tx_success_count >= ARQ_LADDER_UP_SUCCESSES &&
            sess->speed_level < ARQ_LADDER_LEVELS - 1)
        {
            sess->speed_level++;
            sess->tx_success_count = 0;
            HLOGD(LOG_COMP, "Ladder step-up to %d (%d clean ACKs)",
                  sess->speed_level, ARQ_LADDER_UP_SUCCESSES);
        }
    }
}

/** Compute desired payload mode based on peer_snr_x10 and TX backlog.
 *  Returns current payload_mode if no change is warranted.
 *
 *  Hybrid SNR + delivery-feedback mode selection.  Upgrades require SNR
 *  above the mode threshold plus a hysteresis margin.  Downgrades happen
 *  when SNR drops below the current mode's base threshold, OR when
 *  consecutive retries indicate the channel can't support the current mode
 *  (catches deep fades where SNR is stale).  After a retry-forced downgrade,
 *  a hold timer prevents re-upgrade oscillation. */
static int select_best_mode(const arq_session_t *sess, int backlog)
{
    int effective_mode = clamp_payload_mode_to_bandwidth(sess->payload_mode);

    /* Safety net: if consecutive retries hit the threshold, the channel
     * can't support the current mode — force one level down.  The hold
     * timer in maybe_upgrade_mode() prevents immediate re-upgrade. */
    if (sess->consecutive_retries >= ARQ_RETRY_DOWNGRADE_THRESHOLD &&
        mode_rank(effective_mode) > 0)
    {
        int cur = effective_mode;
        if (cur == FREEDV_MODE_DATAC1) return FREEDV_MODE_DATAC3;
        return FREEDV_MODE_DATAC4;
    }

    /* Don't upgrade if the backlog fits in a single frame at the current mode.
     * MODE_REQ/MODE_ACK airtime overhead is never worthwhile for one frame. */
    const arq_mode_timing_t *cur = arq_protocol_mode_timing(effective_mode);
    if (cur && backlog <= cur->payload_bytes - ARQ_FRAME_HDR_SIZE)
        return effective_mode;

    float peer_snr = (float)sess->peer_snr_x10 / 10.0f;
    int   cur_rank = mode_rank(effective_mode);

    /* For the current mode, stay if SNR is at or above base threshold.
     * For a higher mode, upgrade only if SNR exceeds threshold + hysteresis.
     * This asymmetry prevents rapid oscillation at mode boundaries. */
    if (arq_bandwidth_allows_mode(FREEDV_MODE_DATAC1))
    {
        float c1_thresh = (cur_rank >= mode_rank(FREEDV_MODE_DATAC1))
                          ? ARQ_SNR_MIN_DATAC1_DB
                          : ARQ_SNR_MIN_DATAC1_DB + ARQ_SNR_HYST_DB;
        if (peer_snr >= c1_thresh && backlog >= ARQ_BACKLOG_MIN_DATAC1)
            return FREEDV_MODE_DATAC1;
    }

    float c3_thresh = (cur_rank >= mode_rank(FREEDV_MODE_DATAC3))
                      ? ARQ_SNR_MIN_DATAC3_DB
                      : ARQ_SNR_MIN_DATAC3_DB + ARQ_SNR_HYST_DB;
    if (peer_snr >= c3_thresh && backlog >= ARQ_BACKLOG_MIN_DATAC3)
        return FREEDV_MODE_DATAC3;

    return FREEDV_MODE_DATAC4;
}

/** Check whether a mode upgrade/downgrade is warranted.  If yes, send
 *  MODE_REQ and enter MODE_REQ_TX.  Returns true when negotiation started. */
static bool maybe_upgrade_mode(arq_session_t *sess)
{
    /* Stay on DATAC13 during startup window. */
    if (hermes_uptime_ms() < sess->startup_deadline_ms)
        return false;

    /* Need at least one valid SNR reading from the peer before deciding. */
    if (sess->peer_snr_x10 == 0)
        return false;

    int backlog = g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0;
    int desired_mode = select_best_mode(sess, backlog);

    if (desired_mode == sess->payload_mode)
    {
        sess->mode_upgrade_count = 0;
        return false;
    }

    /* After a retry-forced downgrade, don't allow re-upgrade until the
     * hold timer expires.  This prevents oscillation when stale SNR
     * says "upgrade" but the channel can't actually support it. */
    if (mode_rank(desired_mode) > mode_rank(sess->payload_mode) &&
        hermes_uptime_ms() < sess->mode_hold_until_ms)
        return false;

    /* Hysteresis: require ARQ_MODE_SWITCH_HYST_COUNT consecutive observations. */
    sess->mode_upgrade_count++;
    if (sess->mode_upgrade_count < ARQ_MODE_SWITCH_HYST_COUNT)
        return false;

    /* If this is a retry-forced downgrade, set the hold timer. */
    if (mode_rank(desired_mode) < mode_rank(sess->payload_mode) &&
        sess->consecutive_retries >= ARQ_RETRY_DOWNGRADE_THRESHOLD)
    {
        sess->mode_hold_until_ms =
            hermes_uptime_ms() + (ARQ_MODE_HOLD_AFTER_DOWNGRADE_S * 1000ULL);
        sess->consecutive_retries = 0;
        HLOGI(LOG_COMP, "Retry-forced downgrade: hold for %ds",
              ARQ_MODE_HOLD_AFTER_DOWNGRADE_S);
    }

    sess->mode_upgrade_count = 0;
    sess->pending_tx_mode = desired_mode;
    sess->tx_retries_left = ARQ_MODE_REQ_RETRIES;

    HLOGI(LOG_COMP, "Mode negotiation: %d -> %d (peer_snr=%.1f dB, ladder=%d, backlog=%d)",
          sess->payload_mode, desired_mode,
          (float)sess->peer_snr_x10 / 10.0f, sess->speed_level, backlog);

    send_mode_negotiation(sess, ARQ_SUBTYPE_MODE_REQ, desired_mode);
    dflow_enter(sess, ARQ_DFLOW_MODE_REQ_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
    return true;
}

/** Deliver RX payload to the application only if the sequence number matches
 *  what we expect.  Returns true if data was delivered (new frame), false if
 *  it was a duplicate that was silently dropped. */
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
    const char *my_call = arq_conn.my_call_sign;
    int bw_hz = is_accept ? arq_reported_bandwidth_hz() : arq_conn.bw;
    if (is_accept)
        n = arq_protocol_build_accept(frame, sizeof(frame), sess->session_id,
                                      my_call, sess->remote_call, bw_hz);
    else
        n = arq_protocol_build_call(frame, sizeof(frame), sess->session_id,
                                    my_call, sess->remote_call, bw_hz);
    if (n > 0)
        send_frame(PACKET_TYPE_ARQ_CALL, sess->control_mode, (size_t)n, frame);
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
    case ARQ_SUBTYPE_KEEPALIVE:
        n = arq_protocol_build_keepalive(frame, sizeof(frame),
                                         sess->session_id, snr_raw); break;
    case ARQ_SUBTYPE_KEEPALIVE_ACK:
        n = arq_protocol_build_keepalive_ack(frame, sizeof(frame),
                                             sess->session_id, snr_raw); break;
    case ARQ_SUBTYPE_TURN_REQ:
        n = arq_protocol_build_turn_req(frame, sizeof(frame),
                                        sess->session_id,
                                        sess->rx_expected, snr_raw); break;
    case ARQ_SUBTYPE_TURN_ACK:
        n = arq_protocol_build_turn_ack(frame, sizeof(frame),
                                        sess->session_id, snr_raw); break;
    default:
        return;
    }
    if (n > 0)
        send_frame(PACKET_TYPE_ARQ_CONTROL, sess->control_mode, (size_t)n, frame);
}

static void send_ack(arq_session_t *sess, uint8_t ack_delay_raw)
{
    uint8_t frame[INT_BUFFER_SIZE];
    uint8_t flags   = 0;
    uint8_t snr_raw = 0;

    if (g_cbs.tx_backlog && g_cbs.tx_backlog() > 0)
        flags |= ARQ_FLAG_HAS_DATA;
    if (sess->local_snr_x10 != 0)
        snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);

    int n = arq_protocol_build_ack(frame, sizeof(frame), sess->session_id,
                                   sess->rx_expected, flags, snr_raw, ack_delay_raw);
    if (n > 0)
        send_frame(PACKET_TYPE_ARQ_CONTROL, sess->control_mode, (size_t)n, frame);
}

static void send_data_frame(arq_session_t *sess)
{
    if (!g_cbs.tx_read || !g_cbs.tx_backlog)
        return;

    const arq_mode_timing_t *tm = arq_protocol_mode_timing(sess->payload_mode);
    if (!tm)
        return;

    if ((int)tm->payload_bytes <= ARQ_FRAME_HDR_SIZE)
        return;
    size_t user_bytes = (size_t)tm->payload_bytes - ARQ_FRAME_HDR_SIZE;

    uint8_t frame[INT_BUFFER_SIZE];
    uint8_t payload[INT_BUFFER_SIZE];

    /* Retransmit without consuming ring-buffer bytes.  Checked BEFORE tx_read
     * so that retries always replay the saved frame and never corrupt the byte
     * stream by sending fresh (out-of-order) data. */
    if (sess->tx_retransmit_len > 0 &&
        sess->tx_retransmit_seq == sess->tx_seq)
    {
        send_frame(PACKET_TYPE_ARQ_DATA, sess->payload_mode,
                   (size_t)sess->tx_retransmit_len, sess->tx_retransmit_buf);
        if (g_timing)
            arq_timing_record_tx_queue(g_timing, (int)sess->tx_seq,
                                       sess->payload_mode,
                                       g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0,
                                       0);  /* retransmit — no new bytes consumed */
        return;
    }

    memset(payload, 0, user_bytes);
    int payload_len = g_cbs.tx_read(payload, user_bytes);
    if (payload_len <= 0)
        return;  /* no data and no saved frame */

    /* 0 = full frame; else = exact valid byte count (receiver trims).
     * payload_len can exceed 255 for DATAC1 (up to 502 bytes), so we
     * cannot fit it in uint8_t directly.  Carry bit 8 of the count in
     * ARQ_FLAG_LEN_HI (bit 5 of the flags byte); bits [7:0] go in the
     * payload_valid (ack_delay_raw) byte.  This allows lengths up to 511. */
    uint8_t payload_valid;
    uint8_t data_flags = 0;
    if ((size_t)payload_len == user_bytes)
    {
        payload_valid = ARQ_DATA_LEN_FULL;
    }
    else
    {
        payload_valid = (uint8_t)(payload_len & 0xFF);
        if (payload_len > 0xFF)
            data_flags = ARQ_FLAG_LEN_HI;
    }

    uint8_t snr_raw = 0;
    if (sess->local_snr_x10 != 0)
        snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);

    int n = arq_protocol_build_data(frame, sizeof(frame),
                                    sess->session_id, sess->tx_seq,
                                    sess->rx_expected, data_flags, snr_raw,
                                    payload_valid,
                                    payload, user_bytes);
    if (n <= 0)
        return;

    sess->tx_inflight_bytes = payload_len;

    /* Save for potential retransmission if ACK is lost. */
    if ((size_t)n <= sizeof(sess->tx_retransmit_buf))
    {
        memcpy(sess->tx_retransmit_buf, frame, (size_t)n);
        sess->tx_retransmit_len = n;
        sess->tx_retransmit_seq = sess->tx_seq;
    }
    else
    {
        /* Frame too large for retransmit buffer — retries would consume fresh
         * ring bytes and corrupt the stream.  This must not happen; it means
         * tx_retransmit_buf needs to be enlarged. */
        HLOGE(LOG_COMP, "FATAL: retransmit buf too small (%zu < %d) for seq=%d mode=%d",
              sizeof(sess->tx_retransmit_buf), n,
              (int)sess->tx_seq, sess->payload_mode);
    }

    send_frame(PACKET_TYPE_ARQ_DATA, sess->payload_mode, (size_t)n, frame);
    if (g_timing)
        arq_timing_record_tx_queue(g_timing, (int)sess->tx_seq,
                                   sess->payload_mode,
                                   g_cbs.tx_backlog(),
                                   payload_len);  /* new data bytes consumed */
}

/* ======================================================================
 * Level 1 FSM per-state handlers
 * ====================================================================== */

static void fsm_dflow(arq_session_t *sess, const arq_event_t *ev);

static void enter_idle_iss(arq_session_t *sess, bool gained_turn)
{
    (void)gained_turn;  /* per-direction mode: my TX mode evolves independently */
    sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;  /* fresh counter on ISS role entry */
    if (g_cbs.tx_backlog && g_cbs.tx_backlog() > 0)
    {
        dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        send_data_frame(sess);
    }
    else if (sess->pending_disconnect)
    {
        /* TX buffer is empty and last ACK received — fire the deferred DISCONNECT. */
        HLOGD(LOG_COMP, "Deferred DISCONNECT: TX buffer drained — disconnecting now");
        sess->pending_disconnect      = false;
        sess->tx_retries_left         = ARQ_DISCONNECT_RETRY_SLOTS;
        sess->disconnect_to_no_client = false;
        sess_enter(sess, ARQ_CONN_DISCONNECTING,
                   hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
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
    if (g_cbs.tx_backlog && g_cbs.tx_backlog() > 0)
    {
        /* Attempt mode negotiation when startup window has passed and we
         * have a valid peer SNR estimate.  If maybe_upgrade_mode() fires
         * it handles the state transition itself; just return. */
        if (maybe_upgrade_mode(sess))
            return;

        dflow_enter(sess, ARQ_DFLOW_DATA_TX,
                    hermes_uptime_ms() + ARQ_ISS_POST_ACK_GUARD_MS,
                    ARQ_EV_TIMER_ACK);
    }
    else if (sess->pending_disconnect)
    {
        /* TX buffer is empty — honour a DISCONNECT that was deferred while
         * a frame was in flight.  Same path as enter_idle_iss(). */
        HLOGD(LOG_COMP, "Deferred DISCONNECT: TX buffer drained — disconnecting now");
        sess->pending_disconnect      = false;
        sess->tx_retries_left         = ARQ_DISCONNECT_RETRY_SLOTS;
        sess->disconnect_to_no_client = false;
        sess_enter(sess, ARQ_CONN_DISCONNECTING,
                   hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                   ARQ_EV_TIMER_ACK);
    }
    else
        dflow_enter(sess, ARQ_DFLOW_IDLE_ISS, UINT64_MAX, ARQ_EV_TIMER_RETRY);
}

static void enter_idle_irs(arq_session_t *sess)
{
    dflow_enter(sess, ARQ_DFLOW_IDLE_IRS,
                deadline_from_s(ARQ_PEER_PAYLOAD_HOLD_S),
                ARQ_EV_TIMER_PEER_BACKLOG);
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
        }
        break;

    case ARQ_EV_APP_LISTEN:
        sess_enter(sess, ARQ_CONN_LISTENING, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        break;

    case ARQ_EV_APP_CONNECT:
        snprintf(sess->remote_call, CALLSIGN_MAX_SIZE, "%s", ev->remote_call);
        sess->session_id      = (uint8_t)(hermes_uptime_ms() & 0x7F) | 0x01;
        sess->tx_retries_left = ARQ_CALL_RETRY_SLOTS;
        sess->pending_disconnect = false;  /* clear stale deferred disconnect from prior session */
        /* Reset mode state for new session */
        sess->payload_mode       = FREEDV_MODE_DATAC4;
        sess->peer_tx_mode       = FREEDV_MODE_DATAC4;
        sess->speed_level        = 0;
        sess->tx_success_count   = 0;
        sess->consecutive_retries = 0;
        sess->mode_hold_until_ms = 0;
        send_call_accept(sess, false);
        {
            const arq_mode_timing_t *tm =
                arq_protocol_mode_timing(sess->control_mode);
            float interval = tm ? tm->retry_interval_s : 7.0f;
            sess_enter(sess, ARQ_CONN_CALLING,
                       deadline_from_s(interval), ARQ_EV_TIMER_RETRY);
        }
        break;

    default:
        break;
    }
}

static void arm_connect_confirm(arq_session_t *sess)
{
    sess->pending_connect_confirm = true;
    dflow_enter(sess, ARQ_DFLOW_ACK_TX,
                hermes_uptime_ms() + ARQ_ISS_POST_ACK_GUARD_MS,
                ARQ_EV_TIMER_ACK);
}

static void fsm_listening(arq_session_t *sess, const arq_event_t *ev)
{
    switch (ev->id)
    {
    case ARQ_EV_RX_CALL:
        snprintf(sess->remote_call, CALLSIGN_MAX_SIZE, "%s", ev->remote_call);
        sess->session_id      = ev->session_id;
        sess->tx_retries_left = ARQ_ACCEPT_RETRY_SLOTS;
        /* Reset mode state so the payload decoder matches the new caller's
         * initial DATAC4.  This must happen here (not in sess_enter for
         * DISCONNECTED/LISTENING) because LISTENING needs peer_tx_mode to
         * stay at the broadcast mode for receiving broadcast frames. */
        sess->payload_mode       = FREEDV_MODE_DATAC4;
        sess->peer_tx_mode       = FREEDV_MODE_DATAC4;
        sess->speed_level        = 0;
        sess->tx_success_count   = 0;
        sess->consecutive_retries = 0;
        sess->mode_hold_until_ms = 0;
        /* Do NOT send ACCEPT immediately: the caller's PTT-OFF may not have
         * happened yet when we decode the last samples of their CALL frame.
         * Wait ARQ_CHANNEL_GUARD_MS so their relay is in RX before we TX. */
        sess_enter(sess, ARQ_CONN_ACCEPTING,
                   hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                   ARQ_EV_TIMER_RETRY);
        if (g_cbs.notify_pending)
            g_cbs.notify_pending(ev->remote_call);
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
            sess->tx_seq      = 0;
            sess->rx_expected = 0;
            sess->tx_retransmit_len = 0;
            sess->tx_inflight_bytes = 0;
            sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;
            sess->payload_mode       = FREEDV_MODE_DATAC4;
            sess->peer_tx_mode       = FREEDV_MODE_DATAC4;
            sess->pending_tx_mode    = 0;
            sess->mode_upgrade_count = 0;
            sess->speed_level        = 0;
            sess->tx_success_count   = 0;
            sess->pending_disconnect = false;  /* clear stale deferred disconnect */
            sess->startup_deadline_ms = hermes_uptime_ms() + (ARQ_STARTUP_MAX_S * 1000ULL);
            if (g_cbs.notify_connected)
                g_cbs.notify_connected(sess->remote_call);
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
    const arq_mode_timing_t *tm;

    switch (ev->id)
    {
    case ARQ_EV_RX_ACCEPT:
        if (ev->session_id == sess->session_id)
        {
            bool has_tx_backlog = g_cbs.tx_backlog && g_cbs.tx_backlog() > 0;
            sess->role        = ARQ_ROLE_CALLER;
            sess->tx_seq      = 0;
            sess->rx_expected = 0;
            sess->tx_retransmit_len = 0;  /* discard any stale retransmit buf from prior session */
            sess->tx_inflight_bytes = 0;
            sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;
            sess->payload_mode       = FREEDV_MODE_DATAC4;   /* reset mode state from prior session */
            sess->peer_tx_mode       = FREEDV_MODE_DATAC4;
            sess->pending_tx_mode    = 0;
            sess->mode_upgrade_count = 0;
            sess->speed_level        = 0;
            sess->tx_success_count   = 0;
            sess->pending_disconnect = false;  /* clear stale deferred disconnect */
            sess->startup_deadline_ms =
                hermes_uptime_ms() + (ARQ_STARTUP_MAX_S * 1000ULL);
            if (g_cbs.notify_connected)
                g_cbs.notify_connected(sess->remote_call);
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
            tm = arq_protocol_mode_timing(sess->control_mode);
            sess->deadline_ms = deadline_from_s(tm ? tm->retry_interval_s : 7.0f);
        }
        else
        {
            if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
            sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        }
        break;

    case ARQ_EV_APP_DISCONNECT:
        if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        break;

    default:
        break;
    }
}

static void fsm_accepting(arq_session_t *sess, const arq_event_t *ev)
{
    const arq_mode_timing_t *tm;

    switch (ev->id)
    {
    case ARQ_EV_RX_DATA:
    case ARQ_EV_RX_ACK:
        sess->role        = ARQ_ROLE_CALLEE;
        sess->tx_seq      = 0;
        sess->rx_expected = 0;
        sess->tx_retransmit_len = 0;  /* discard any stale retransmit buf from prior session */
        sess->tx_inflight_bytes = 0;
        sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;
        sess->payload_mode       = FREEDV_MODE_DATAC4;   /* reset mode state from prior session */
        sess->peer_tx_mode       = FREEDV_MODE_DATAC4;
        sess->pending_tx_mode    = 0;
        sess->mode_upgrade_count = 0;
        sess->speed_level        = 0;
        sess->tx_success_count   = 0;
        sess->startup_deadline_ms =
            hermes_uptime_ms() + (ARQ_STARTUP_MAX_S * 1000ULL);
        if (g_cbs.notify_connected)
            g_cbs.notify_connected(sess->remote_call);
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
         * start its first DATA frame (DATAC4, ~5800 ms) almost immediately
         * after our PTT drops.  The deadline that was set in TIMER_RETRY was
         * relative to when TIMER_RETRY fired — not to TX_COMPLETE — so it
         * only left ~4400 ms of RX window after PTT-OFF, which is shorter
         * than one DATAC4 frame.  Reset the deadline here so we always have
         * a full ARQ_ACCEPT_RX_WINDOW_MS window (guard + DATAC4 frame +
         * margin) measured from the moment our TX actually ends. */
        sess->deadline_ms = hermes_uptime_ms() + ARQ_ACCEPT_RX_WINDOW_MS;
        break;

    case ARQ_EV_TIMER_RETRY:
        if (sess->tx_retries_left > 0)
        {
            sess->tx_retries_left--;
            send_call_accept(sess, true);
            /* deadline is now managed via TX_COMPLETE above; set a generous
             * fallback here in case TX_COMPLETE is missed for any reason */
            tm = arq_protocol_mode_timing(sess->control_mode);
            sess->deadline_ms = deadline_from_s(tm ? tm->retry_interval_s : 7.0f);
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

    case ARQ_EV_APP_DISCONNECT:
        if (g_cbs.notify_cancelpending)
            g_cbs.notify_cancelpending();
        if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        break;

    default:
        break;
    }
}

static void fsm_disconnecting(arq_session_t *sess, const arq_event_t *ev)
{
    bool to_no_client = sess->disconnect_to_no_client;
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
        if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(to_no_client);
        if (g_timing) arq_timing_record_disconnect(g_timing, "peer_ack");
        sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
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
            if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(to_no_client);
            if (g_timing) arq_timing_record_disconnect(g_timing, "timeout");
            sess_enter(sess, ARQ_CONN_DISCONNECTED, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        }
        break;

    default:
        break;
    }
}

static void fsm_connected(arq_session_t *sess, const arq_event_t *ev)
{
    const arq_mode_timing_t *tm;

    switch (ev->id)
    {
    case ARQ_EV_APP_DISCONNECT:
        /* Defer DISCONNECT only while a frame is physically being transmitted
         * (PTT on) or the TX buffer still has unsent bytes.  We must NOT defer
         * when in WAIT_ACK: the frame is already sent (PTT off) and the peer
         * may never ACK it; waiting up to 10×12 s before honouring the
         * application's explicit disconnect request is unacceptable. */
        if ((g_cbs.tx_backlog && g_cbs.tx_backlog() > 0) ||
            sess->dflow_state == ARQ_DFLOW_DATA_TX)
        {
            HLOGD(LOG_COMP,
                  "APP_DISCONNECT deferred — backlog=%d dflow=%s",
                  g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0,
                  arq_dflow_state_name(sess->dflow_state));
            sess->pending_disconnect = true;
            return;
        }
        sess->pending_disconnect      = false;
        sess->tx_retries_left         = ARQ_DISCONNECT_RETRY_SLOTS;
        sess->disconnect_to_no_client = false;
        sess_enter(sess, ARQ_CONN_DISCONNECTING,
                   hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
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

    case ARQ_EV_TIMER_KEEPALIVE:
        send_ctrl_frame(sess, ARQ_SUBTYPE_KEEPALIVE);
        tm = arq_protocol_mode_timing(sess->control_mode);
        dflow_enter(sess, ARQ_DFLOW_KEEPALIVE_TX,
                    deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                    ARQ_EV_TIMER_RETRY);
        return;

    default:
        break;
    }

    fsm_dflow(sess, ev);
}

/* ======================================================================
 * Level 2 data-flow sub-FSM
 * ====================================================================== */

static void fsm_dflow(arq_session_t *sess, const arq_event_t *ev)
{
    const arq_mode_timing_t *tm;

    switch (sess->dflow_state)
    {
    case ARQ_DFLOW_IDLE_ISS:
        if (ev->id == ARQ_EV_APP_DATA_READY && g_cbs.tx_backlog &&
            g_cbs.tx_backlog() > 0)
        {
            if (sess->need_initial_guard)
            {
                /* First DATA after connect: apply channel guard so IRS has
                 * time to reset decoders from TX→RX before our preamble. */
                sess->need_initial_guard = false;
                dflow_enter(sess, ARQ_DFLOW_DATA_TX,
                            hermes_uptime_ms() + ARQ_ISS_POST_ACK_GUARD_MS,
                            ARQ_EV_TIMER_ACK);
            }
            else
            {
                dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
                send_data_frame(sess);
            }
        }
        else if (ev->id == ARQ_EV_RX_DATA)
        {
            /* Peer sent data while we hold the TX turn — receive it and ACK. */
            update_local_snr(sess, ev);
            update_peer_snr(sess, ev);
            sess->peer_tx_mode = ev->mode;   /* track peer's actual TX mode */
            bool new_frame = deliver_rx_checked(sess, ev);
            if (new_frame && g_timing)
                arq_timing_record_data_rx(g_timing, (int)ev->seq,
                                          (int)ev->data_bytes,
                                          sess->local_snr_x10);
            sess->last_rx_ms    = hermes_uptime_ms();
            /* A duplicate (new_frame=false) means the sender is still the
             * active ISS — it is retransmitting because it hasn't received
             * our ACK yet.  Force peer_has_data=true so ACK_TX→TX_COMPLETE
             * calls enter_idle_irs() instead of taking a spurious piggyback
             * turn that would place both sides into ISS simultaneously. */
            sess->peer_has_data = new_frame
                                  ? (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0
                                  : true;
            dflow_enter(sess, ARQ_DFLOW_DATA_RX,
                        hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                        ARQ_EV_TIMER_ACK);
        }
        else if (ev->id == ARQ_EV_RX_TURN_REQ)
        {
            /* Yield TX turn — guard ARQ_CHANNEL_GUARD_MS before sending
             * TURN_ACK so we don't collide with the remote's final TX audio
             * (FreeDV decoder fires ~150ms before remote PTT-OFF). */
            if (g_timing) arq_timing_record_turn(g_timing, false, "turn_req");
            dflow_enter(sess, ARQ_DFLOW_TURN_ACK_TX,
                        hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                        ARQ_EV_TIMER_ACK);
        }
        break;

    case ARQ_DFLOW_DATA_TX:
        if (ev->id == ARQ_EV_TIMER_ACK)
        {
            /* Channel guard elapsed — now safe to transmit data. */
            send_data_frame(sess);
        }
        else if (ev->id == ARQ_EV_TX_STARTED)
        {
            if (g_timing)
                arq_timing_record_tx_start(g_timing, (int)sess->tx_seq,
                                           sess->payload_mode,
                                           g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0);
        }
        else if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            if (g_timing)
                arq_timing_record_tx_end(g_timing, (int)sess->tx_seq);
            tm = arq_protocol_mode_timing(sess->payload_mode);
            dflow_enter(sess, ARQ_DFLOW_WAIT_ACK,
                        deadline_from_s(tm ? tm->ack_timeout_s : 9.0f),
                        ARQ_EV_TIMER_ACK);
        }
        else if (ev->id == ARQ_EV_RX_TURN_REQ)
        {
            /* TURN_REQ arrived while we are in the guard phase (TIMER_ACK
             * deadline not yet fired — frame not queued yet).  Yield cleanly:
             * we have not started TX so no retransmit state is touched. */
            if (sess->deadline_event == ARQ_EV_TIMER_ACK &&
                sess->deadline_ms != UINT64_MAX)
            {
                if (g_timing) arq_timing_record_turn(g_timing, false, "turn_req");
                dflow_enter(sess, ARQ_DFLOW_TURN_ACK_TX,
                            hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                            ARQ_EV_TIMER_ACK);
            }
            /* else: TX is already queued/in-progress; ignore here and let
             * WAIT_ACK handle the next TURN_REQ once TX_COMPLETE fires. */
        }
        break;

    case ARQ_DFLOW_WAIT_ACK:
        if (ev->id == ARQ_EV_RX_ACK)
        {
            /* peer_snr_x10 = IRS's local SNR = quality of IRS receiving our data */
            update_peer_snr(sess, ev);
            record_tx_outcome(sess, sess->tx_retries_left == ARQ_DATA_RETRY_SLOTS);
            if (g_timing)
                arq_timing_record_ack_rx(g_timing, (int)sess->tx_seq,
                                         (uint8_t)ev->ack_delay_raw,
                                         sess->peer_snr_x10);
            sess->tx_seq++;
            sess->tx_retransmit_len = 0;  /* ACKed — discard retransmit buffer */
            sess->tx_inflight_bytes = 0;  /* payload confirmed by peer */
            sess->tx_retries_left   = ARQ_DATA_RETRY_SLOTS;  /* fresh counter for next seq */
            sess->peer_has_data = (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0;
            if (g_cbs.send_buffer_status)
                g_cbs.send_buffer_status(g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0);

            if (sess->peer_has_data)
            {
                if (g_timing) arq_timing_record_turn(g_timing, false, "piggyback");
                enter_idle_irs(sess);
            }
            else
            {
                enter_idle_iss_guarded(sess, false);  /* ISS retaining turn */
            }
        }
        else if (ev->id == ARQ_EV_TIMER_ACK)
        {
            /* Honour a disconnect that was deferred while a frame was in
             * flight (DATA_TX).  The PTT is now off; stop retrying. */
            if (sess->pending_disconnect)
            {
                HLOGI(LOG_COMP,
                      "Pending DISCONNECT: aborting retry seq=%d",
                      (int)sess->tx_seq);
                sess->pending_disconnect      = false;
                sess->tx_retries_left         = ARQ_DISCONNECT_RETRY_SLOTS;
                sess->disconnect_to_no_client = false;
                sess_enter(sess, ARQ_CONN_DISCONNECTING,
                           hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                           ARQ_EV_TIMER_ACK);
                return;
            }
            if (sess->tx_retries_left > 0)
            {
                sess->tx_retries_left--;
                /* Ladder step-down happens once per frame in the RX_ACK /
                 * implicit-ACK handler via record_tx_outcome(), NOT here.
                 * Calling it on every retry would cause double/triple penalty
                 * when the ACK handler also calls it. */
                if (g_timing)
                    arq_timing_record_retry(g_timing, (int)sess->tx_seq,
                                            ARQ_DATA_RETRY_SLOTS - sess->tx_retries_left,
                                            "ack_timeout");
                dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
                send_data_frame(sess);
            }
            else
            {
                HLOGW(LOG_COMP, "Data retry exhausted seq=%d — disconnecting",
                      (int)sess->tx_seq);
                send_ctrl_frame(sess, ARQ_SUBTYPE_DISCONNECT);
                sess->tx_retries_left = ARQ_DISCONNECT_RETRY_SLOTS;
                tm = arq_protocol_mode_timing(sess->control_mode);
                sess_enter(sess, ARQ_CONN_DISCONNECTING,
                           deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                           ARQ_EV_TIMER_RETRY);
            }
        }
        else if (ev->id == ARQ_EV_RX_DATA)
        {
            update_local_snr(sess, ev);
            update_peer_snr(sess, ev);
            sess->peer_tx_mode = ev->mode;
            sess->last_rx_ms = hermes_uptime_ms();

            if (ev->seq == sess->rx_expected)
            {
                /* Peer sent new DATA while we await ACK for our frame —
                 * implicit ACK: peer received our frame (it wouldn't send
                 * new DATA otherwise).  Advance tx_seq and accept the data. */
                HLOGD(LOG_COMP,
                      "RX_DATA in WAIT_ACK (new seq=%d) — implicit ACK for tx_seq=%d",
                      (int)ev->seq, (int)sess->tx_seq);
                record_tx_outcome(sess, sess->tx_retries_left == ARQ_DATA_RETRY_SLOTS);
                sess->tx_seq++;
                sess->tx_retransmit_len = 0;
                sess->tx_inflight_bytes = 0;
                sess->tx_retries_left   = ARQ_DATA_RETRY_SLOTS;
                sess->peer_has_data = (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0;
                if (g_cbs.send_buffer_status)
                    g_cbs.send_buffer_status(g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0);
                if (deliver_rx_checked(sess, ev) && g_timing)
                    arq_timing_record_data_rx(g_timing, (int)ev->seq,
                                              (int)ev->data_bytes,
                                              sess->local_snr_x10);
                dflow_enter(sess, ARQ_DFLOW_DATA_RX,
                            hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                            ARQ_EV_TIMER_ACK);
            }
            else
            {
                /* Duplicate frame (our ACK was lost; peer is retransmitting).
                 * This is NOT an implicit ACK of our own pending tx_seq.
                 * Retransmit our data immediately so the peer can ACK it;
                 * DATA_TX→TX_COMPLETE returns to WAIT_ACK via the normal path.
                 * Do NOT advance tx_seq — our frame is still unacknowledged. */
                HLOGD(LOG_COMP,
                      "RX_DATA in WAIT_ACK (dup seq=%d expected=%d) — re-TX our seq=%d",
                      (int)ev->seq, (int)sess->rx_expected, (int)sess->tx_seq);
                deliver_rx_checked(sess, ev);  /* logs dup; no delivery */
                dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
                send_data_frame(sess);
            }
        }
        /* TURN_REQ is intentionally ignored in WAIT_ACK: the ISS must not
         * give up its role while a data frame is still unacknowledged.
         * The peer's TURN_REQ will be honoured after the ACK arrives and
         * the ISS enters IDLE_ISS (or after retries are exhausted). */
        else if (ev->id == ARQ_EV_RX_MODE_REQ)
        {
            if (arq_protocol_mode_timing(ev->mode) != NULL &&
                ev->mode != FREEDV_MODE_DATAC13)
            {
                /* Peer has taken ISS and is requesting a mode switch.  The peer
                 * only enters ISS after receiving our pending frame (via DATA_RX
                 * → ACK_TX → piggyback or TURN_REQ), so this is an implicit ACK.
                 * Advance tx_seq, update RX decoder mode (not our TX mode), send
                 * MODE_ACK, then hand turn to peer (MODE_ACK_TX → IDLE_IRS). */
                HLOGI(LOG_COMP,
                      "MODE_REQ in WAIT_ACK (implicit ACK) tx_seq=%d peer_tx_mode %d->%d (my TX %d unchanged)",
                      (int)sess->tx_seq, sess->peer_tx_mode, ev->mode, sess->payload_mode);
                record_tx_outcome(sess, sess->tx_retries_left == ARQ_DATA_RETRY_SLOTS);
                sess->tx_seq++;
                sess->tx_retransmit_len = 0;
                sess->tx_inflight_bytes = 0;
                sess->tx_retries_left   = ARQ_DATA_RETRY_SLOTS;
                if (g_cbs.send_buffer_status)
                    g_cbs.send_buffer_status(g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0);
                sess->peer_tx_mode = ev->mode;  /* update RX decoder; our TX mode unchanged */
                /* Guard: allow ARQ_CHANNEL_GUARD_MS for the ISS to drop PTT
                 * before our MODE_ACK preamble arrives (same guard used by
                 * DATA_RX and TURN_ACK_TX to avoid TX collisions). */
                dflow_enter(sess, ARQ_DFLOW_MODE_ACK_TX,
                            hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                            ARQ_EV_TIMER_ACK);
            }
        }
        break;

    case ARQ_DFLOW_IDLE_IRS:
        if (ev->id == ARQ_EV_RX_DATA)
        {
            /* Deliver payload with duplicate check; only count in timing if
             * actually delivered (retransmits must not inflate rx_total). */
            update_local_snr(sess, ev);
            update_peer_snr(sess, ev);
            sess->peer_tx_mode = ev->mode;   /* track peer's actual TX mode */
            bool new_frame = deliver_rx_checked(sess, ev);
            if (new_frame && g_timing)
                arq_timing_record_data_rx(g_timing, (int)ev->seq,
                                          (int)ev->data_bytes,
                                          sess->local_snr_x10);
            sess->last_rx_ms    = hermes_uptime_ms();
            /* A duplicate (new_frame=false) means the sender is still the
             * active ISS — it is retransmitting because it hasn't received
             * our ACK yet.  Force peer_has_data=true so ACK_TX→TX_COMPLETE
             * calls enter_idle_irs() instead of taking a spurious piggyback
             * turn that would place both sides into ISS simultaneously. */
            sess->peer_has_data = new_frame
                                  ? (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0
                                  : true;

            /* Guard: allow ARQ_CHANNEL_GUARD_MS for the ISS relay to switch
             * back to RX before our ACK preamble arrives.  ACK is sent
             * when TIMER_ACK fires in DATA_RX. */
            dflow_enter(sess, ARQ_DFLOW_DATA_RX,
                        hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                        ARQ_EV_TIMER_ACK);
        }
        else if (ev->id == ARQ_EV_TIMER_PEER_BACKLOG)
        {
            /* Inactivity check FIRST - must fire regardless of local
             * tx_backlog, otherwise TURN_REQ retries loop forever when
             * the peer disappears while we have pending data. */
            if (sess->last_rx_ms > 0 &&
                hermes_uptime_ms() - sess->last_rx_ms >=
                    (uint64_t)ARQ_IRS_INACTIVITY_S * 1000)
            {
                /* Peer has been silent too long - probe with keepalive.
                 * If the peer responds, keepalive_miss_count resets and
                 * we return to IDLE_IRS.  If not, the keepalive retry
                 * loop disconnects after ARQ_KEEPALIVE_MISS_LIMIT misses. */
                HLOGW(LOG_COMP, "IRS inactivity (%ds without RX) - sending keepalive",
                      (int)((hermes_uptime_ms() - sess->last_rx_ms) / 1000));
                sess->keepalive_miss_count = 0;
                send_ctrl_frame(sess, ARQ_SUBTYPE_KEEPALIVE);
                tm = arq_protocol_mode_timing(sess->control_mode);
                dflow_enter(sess, ARQ_DFLOW_KEEPALIVE_TX,
                            deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                            ARQ_EV_TIMER_RETRY);
            }
            else if (g_cbs.tx_backlog && g_cbs.tx_backlog() > 0)
            {
                send_ctrl_frame(sess, ARQ_SUBTYPE_TURN_REQ);
                sess->tx_retries_left = ARQ_TURN_REQ_RETRIES;
                tm = arq_protocol_mode_timing(sess->control_mode);
                dflow_enter(sess, ARQ_DFLOW_TURN_REQ_TX,
                            deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                            ARQ_EV_TIMER_RETRY);
            }
            else
            {
                enter_idle_irs(sess);
            }
        }
        else if (ev->id == ARQ_EV_APP_DATA_READY)
        {
            send_ctrl_frame(sess, ARQ_SUBTYPE_TURN_REQ);
            sess->tx_retries_left = ARQ_TURN_REQ_RETRIES;
            tm = arq_protocol_mode_timing(sess->control_mode);
            dflow_enter(sess, ARQ_DFLOW_TURN_REQ_TX,
                        deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                        ARQ_EV_TIMER_RETRY);
        }
        else if (ev->id == ARQ_EV_RX_TURN_REQ)
        {
            /* Remote missed our TURN_ACK — re-send with channel guard. */
            dflow_enter(sess, ARQ_DFLOW_TURN_ACK_TX,
                        hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                        ARQ_EV_TIMER_ACK);
        }
        else if (ev->id == ARQ_EV_RX_MODE_REQ)
        {
            /* ISS requests a mode change.  Accept if it is a valid payload mode.
             * Per-direction: only update our RX decoder (peer_tx_mode), not our
             * own TX mode (payload_mode), which is managed independently. */
            if (arq_protocol_mode_timing(ev->mode) != NULL &&
                ev->mode != FREEDV_MODE_DATAC13)
            {
                HLOGI(LOG_COMP, "MODE_REQ: peer TX mode %d -> %d (my TX mode %d unchanged)",
                      sess->peer_tx_mode, ev->mode, sess->payload_mode);
                sess->peer_tx_mode = ev->mode;
                /* Guard: allow ARQ_CHANNEL_GUARD_MS for the ISS to drop PTT
                 * before our MODE_ACK preamble arrives (same guard used by
                 * DATA_RX and TURN_ACK_TX to avoid TX collisions). */
                dflow_enter(sess, ARQ_DFLOW_MODE_ACK_TX,
                            hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                            ARQ_EV_TIMER_ACK);
            }
        }
        break;

    case ARQ_DFLOW_DATA_RX:
        if (ev->id == ARQ_EV_TIMER_ACK)
        {
            /* Channel guard elapsed — now safe to transmit ACK.
             * Capture HAS_DATA state before sending so ACK_TX → TX_COMPLETE
             * knows whether a piggyback turn is valid. */
            uint32_t delay_ms = (uint32_t)(hermes_uptime_ms() - sess->last_rx_ms);
            sess->acktx_had_has_data = g_cbs.tx_backlog && g_cbs.tx_backlog() > 0;
            send_ack(sess, arq_protocol_encode_ack_delay(delay_ms));
            if (g_timing)
                arq_timing_record_ack_tx(g_timing, (int)sess->rx_expected - 1);
            dflow_enter(sess, ARQ_DFLOW_ACK_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        }
        else if (ev->id == ARQ_EV_RX_DATA)
        {
            /* Another frame arrived during guard window; deliver with seq check */
            update_local_snr(sess, ev);
            update_peer_snr(sess, ev);
            sess->peer_tx_mode = ev->mode;   /* track peer's actual TX mode */
            bool new_frame = deliver_rx_checked(sess, ev);
            if (new_frame && g_timing)
                arq_timing_record_data_rx(g_timing, (int)ev->seq,
                                          (int)ev->data_bytes,
                                          sess->local_snr_x10);
            sess->last_rx_ms    = hermes_uptime_ms();
            /* A duplicate (new_frame=false) means the sender is still the
             * active ISS — it is retransmitting because it hasn't received
             * our ACK yet.  Force peer_has_data=true so ACK_TX→TX_COMPLETE
             * calls enter_idle_irs() instead of taking a spurious piggyback
             * turn that would place both sides into ISS simultaneously. */
            sess->peer_has_data = new_frame
                                  ? (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0
                                  : true;
        }
        break;

    case ARQ_DFLOW_ACK_TX:
        if (ev->id == ARQ_EV_TIMER_ACK && sess->pending_connect_confirm)
        {
            send_ack(sess, 0);
            dflow_enter(sess, ARQ_DFLOW_ACK_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
        }
        else if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            if (sess->pending_connect_confirm)
            {
                sess->pending_connect_confirm = false;
                if (g_cbs.tx_backlog && g_cbs.tx_backlog() > 0)
                    enter_idle_iss_guarded(sess, false);
                else
                    enter_idle_iss(sess, false);
            }
            else if (sess->peer_has_data)
                enter_idle_irs(sess);
            else if (sess->acktx_had_has_data)
            {
                /* Piggyback turn: HAS_DATA was set in the ACK so the remote
                 * already knows we will transmit — safe to take the ISS role. */
                if (g_timing) arq_timing_record_turn(g_timing, true, "piggyback");
                enter_idle_iss_guarded(sess, true);  /* IRS gaining turn — use peer's observed mode */
            }
            else if (g_cbs.tx_backlog && g_cbs.tx_backlog() > 0)
            {
                /* Data arrived during ACK TX (APP_DATA_READY ignored, HAS_DATA
                 * not set).  The ISS may start a new DATA_TX within ~150ms of
                 * our PTT-OFF.  Wait ARQ_TURN_WAIT_AFTER_ACK_MS so that ISS's
                 * frame arrives (cancelling this timer via RX_DATA) if it has
                 * more data.  If no frame by then, ISS has nothing to send and
                 * it is safe to request the turn without colliding. */
                dflow_enter(sess, ARQ_DFLOW_IDLE_IRS,
                            hermes_uptime_ms() + ARQ_TURN_WAIT_AFTER_ACK_MS,
                            ARQ_EV_TIMER_PEER_BACKLOG);
            }
            else
                enter_idle_irs(sess);
        }
        break;

    case ARQ_DFLOW_TURN_REQ_TX:
        if (ev->id == ARQ_EV_TIMER_ACK)
        {
            /* Deferred send — guard elapsed after completing previous TX. */
            send_ctrl_frame(sess, ARQ_SUBTYPE_TURN_REQ);
        }
        else if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            tm = arq_protocol_mode_timing(sess->control_mode);
            dflow_enter(sess, ARQ_DFLOW_TURN_REQ_WAIT,
                        deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                        ARQ_EV_TIMER_RETRY);
        }
        break;

    case ARQ_DFLOW_TURN_REQ_WAIT:
        if (ev->id == ARQ_EV_RX_TURN_ACK)
        {
            if (g_timing) arq_timing_record_turn(g_timing, true, "turn_ack");
            enter_idle_iss_guarded(sess, true);  /* IRS gaining turn */
        }
        else if (ev->id == ARQ_EV_RX_DATA)
        {
            /* Peer sent DATA instead of TURN_ACK — they have priority and
             * will not yield.  Abandon our turn request, receive the data,
             * and send an ACK.  The turn can be requested again later from
             * IDLE_IRS once the peer finishes its burst. */
            update_local_snr(sess, ev);
            update_peer_snr(sess, ev);
            sess->peer_tx_mode = ev->mode;
            bool new_frame = deliver_rx_checked(sess, ev);
            if (new_frame && g_timing)
                arq_timing_record_data_rx(g_timing, (int)ev->seq,
                                          (int)ev->data_bytes,
                                          sess->local_snr_x10);
            sess->last_rx_ms = hermes_uptime_ms();
            /* A duplicate (new_frame=false) means the sender is still the
             * active ISS — it is retransmitting because it hasn't received
             * our ACK yet.  Force peer_has_data=true so ACK_TX→TX_COMPLETE
             * calls enter_idle_irs() instead of taking a spurious piggyback
             * turn that would place both sides into ISS simultaneously.
             * NOTE: deliver_rx_checked() increments rx_expected on success,
             * so (ev->seq == sess->rx_expected) is never true after the call;
             * the return value is the only correct new/dup discriminator. */
            sess->peer_has_data = new_frame
                                  ? (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0
                                  : true;
            dflow_enter(sess, ARQ_DFLOW_DATA_RX,
                        hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                        ARQ_EV_TIMER_ACK);
        }
        else if (ev->id == ARQ_EV_TIMER_RETRY)
        {
            if (sess->tx_retries_left > 0)
            {
                sess->tx_retries_left--;
                send_ctrl_frame(sess, ARQ_SUBTYPE_TURN_REQ);
                tm = arq_protocol_mode_timing(sess->control_mode);
                dflow_enter(sess, ARQ_DFLOW_TURN_REQ_TX,
                            deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                            ARQ_EV_TIMER_RETRY);
            }
            else
            {
                enter_idle_irs(sess);
            }
        }
        break;

    case ARQ_DFLOW_TURN_ACK_TX:
        if (ev->id == ARQ_EV_TIMER_ACK)
            send_ctrl_frame(sess, ARQ_SUBTYPE_TURN_ACK);
        else if (ev->id == ARQ_EV_TX_COMPLETE)
            enter_idle_irs(sess);
        break;

    case ARQ_DFLOW_KEEPALIVE_TX:
        if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            tm = arq_protocol_mode_timing(sess->control_mode);
            dflow_enter(sess, ARQ_DFLOW_KEEPALIVE_WAIT,
                        deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                        ARQ_EV_TIMER_RETRY);
        }
        break;

    case ARQ_DFLOW_KEEPALIVE_WAIT:
        if (ev->id == ARQ_EV_RX_KEEPALIVE_ACK)
        {
            sess->keepalive_miss_count = 0;
            if (sess->role == ARQ_ROLE_CALLER)
                enter_idle_irs(sess);
            else
                enter_idle_iss_guarded(sess, false);
        }
        else if (ev->id == ARQ_EV_RX_KEEPALIVE)
        {
            send_ctrl_frame(sess, ARQ_SUBTYPE_KEEPALIVE_ACK);
            sess->keepalive_miss_count = 0;
            if (sess->role == ARQ_ROLE_CALLER)
                enter_idle_irs(sess);
            else
                enter_idle_iss_guarded(sess, false);
        }
        else if (ev->id == ARQ_EV_TIMER_RETRY)
        {
            sess->keepalive_miss_count++;
            if (sess->keepalive_miss_count >= ARQ_KEEPALIVE_MISS_LIMIT)
            {
                HLOGW(LOG_COMP, "Keepalive miss limit — disconnecting");
                send_ctrl_frame(sess, ARQ_SUBTYPE_DISCONNECT);
                sess->tx_retries_left = ARQ_DISCONNECT_RETRY_SLOTS;
                tm = arq_protocol_mode_timing(sess->control_mode);
                sess_enter(sess, ARQ_CONN_DISCONNECTING,
                           deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                           ARQ_EV_TIMER_RETRY);
            }
            else
            {
                send_ctrl_frame(sess, ARQ_SUBTYPE_KEEPALIVE);
                tm = arq_protocol_mode_timing(sess->control_mode);
                dflow_enter(sess, ARQ_DFLOW_KEEPALIVE_TX,
                            deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                            ARQ_EV_TIMER_RETRY);
            }
        }
        break;

    case ARQ_DFLOW_MODE_REQ_TX:
        /* ISS: MODE_REQ sent, waiting for modem TX to complete. */
        if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            tm = arq_protocol_mode_timing(sess->control_mode);
            dflow_enter(sess, ARQ_DFLOW_MODE_REQ_WAIT,
                        deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                        ARQ_EV_TIMER_RETRY);
        }
        break;

    case ARQ_DFLOW_MODE_REQ_WAIT:
        /* ISS: waiting for MODE_ACK from IRS. */
        if (ev->id == ARQ_EV_RX_MODE_ACK)
        {
            if (arq_protocol_mode_timing(ev->mode) != NULL &&
                ev->mode != FREEDV_MODE_DATAC13)
            {
                /* If this MODE_ACK confirms a downgrade, start the hold
                 * timer NOW (not when MODE_REQ was sent).  The MODE_REQ/ACK
                 * round trip can exceed the hold duration, so the timer set
                 * at send time may already have expired. */
                if (mode_rank(ev->mode) < mode_rank(sess->payload_mode) &&
                    sess->mode_hold_until_ms != 0)
                {
                    sess->mode_hold_until_ms =
                        hermes_uptime_ms() + (ARQ_MODE_HOLD_AFTER_DOWNGRADE_S * 1000ULL);
                }
                HLOGI(LOG_COMP, "MODE_ACK: payload mode %d -> %d",
                      sess->payload_mode, ev->mode);
                sess->payload_mode = ev->mode;
            }
            enter_idle_iss_guarded(sess, false);  /* ISS confirmed mode — retain turn */
        }
        else if (ev->id == ARQ_EV_TIMER_RETRY)
        {
            if (sess->tx_retries_left > 0)
            {
                sess->tx_retries_left--;
                send_mode_negotiation(sess, ARQ_SUBTYPE_MODE_REQ,
                                      sess->pending_tx_mode);
                dflow_enter(sess, ARQ_DFLOW_MODE_REQ_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
            }
            else
            {
                HLOGW(LOG_COMP, "MODE_REQ timeout — staying at mode %d",
                      sess->payload_mode);
                enter_idle_iss(sess, false);  /* ISS stays, no turn change */
            }
        }
        break;

    case ARQ_DFLOW_MODE_ACK_TX:
        /* IRS: channel guard elapsed — now safe to send MODE_ACK.
         * Confirm the RX decoder mode we accepted, not our TX mode. */
        if (ev->id == ARQ_EV_TIMER_ACK)
            send_mode_negotiation(sess, ARQ_SUBTYPE_MODE_ACK, sess->peer_tx_mode);
        /* IRS: MODE_ACK transmission finished. */
        else if (ev->id == ARQ_EV_TX_COMPLETE)
            enter_idle_irs(sess);
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
    case ARQ_EV_RX_TURN_REQ:
    case ARQ_EV_RX_TURN_ACK:
    case ARQ_EV_RX_KEEPALIVE:
    case ARQ_EV_RX_KEEPALIVE_ACK:
        sess->last_rx_ms = hermes_uptime_ms();
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
