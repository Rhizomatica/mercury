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
        [ARQ_DFLOW_KEEPALIVE_ACK_TX] = "KEEPALIVE_ACK_TX",
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
    sess->control_mode        = ARQ_CONTROL_MODE;
    sess->payload_mode        = FREEDV_MODE_DATAC15;  /* my TX mode, starts at safest level */
    sess->peer_tx_mode        = FREEDV_MODE_DATAC15;  /* RX decoder, starts at safest level */
    sess->initial_payload_mode = FREEDV_MODE_DATAC15;  /* overwritten by arq_set_initial_mode */
    sess->speed_level    = 0;
    sess->tx_success_count = 0;
    sess->olla_offset_db = 0.0f;
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
    else
    {
        /* Seed the no-progress clock at connection establishment so the
         * wall-clock disconnect budget always has a baseline.  Without this,
         * a session that never lands an advancing ACK (e.g. one-way TX
         * failure while peer keepalives still arrive) would leave
         * last_tx_progress_ms at 0 and persist forever after retry
         * exhaustion.  Advancing ACKs refresh this timestamp during data
         * flow (see fsm_dflow). */
        sess->last_tx_progress_ms = hermes_uptime_ms();
    }
    /* Reset data-flow and mode state when returning to idle connection states.
     * Restore peer_tx_mode to initial_payload_mode (= broadcast mode) so the
     * payload decoder can receive broadcast frames while LISTENING.  The
     * session-start paths (RX_CALL, APP_CONNECT) override this to DATAC15
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
    {
        sess->peer_snr_x10 =
            (int)(arq_protocol_decode_snr((uint8_t)ev->snr_encoded) * 10.0f);
        sess->peer_snr_valid = true;
    }
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
        send_frame(PACKET_TYPE_ARQ_CONTROL, sess->control_mode, (size_t)n, frame, 0);
}

/** Map a FreeDV payload mode to a comparable rank: higher rank = faster/more
 *  aggressive mode (by ARQ goodput, not raw bps).  QAM16C2 is fastest, then
 *  DATAC17, DATAC1, DATAC3, DATAC4, and DATAC15 is slowest — the numeric
 *  constants do not order by throughput, so direct integer comparisons
 *  between them are misleading. */
static int mode_rank(int mode)
{
    if (mode == FREEDV_MODE_QAM16C2) return 5;
    if (mode == FREEDV_MODE_DATAC17) return 4;
    if (mode == FREEDV_MODE_DATAC1)  return 3;
    if (mode == FREEDV_MODE_DATAC3)  return 2;
    if (mode == FREEDV_MODE_DATAC4)  return 1;
    return 0; /* DATAC15 or any other conservative mode */
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

/* Forward-link SNR margin (dB) above a mode's base threshold at which the
 * forward (data) link is judged to comfortably support the current mode — so a
 * retransmission is more likely a lost ACK on the (independently-faded) reverse
 * path than a forward decode failure. */
#define ARQ_REVERSE_LOSS_MARGIN_DB   2.0f

/* Base SNR (dB) the forward link needs to sustain a payload mode (no hyst). */
static float mode_snr_floor_db(int mode)
{
    switch (mode)
    {
    case FREEDV_MODE_QAM16C2: return ARQ_SNR_MIN_QAM16C2_DB;
    case FREEDV_MODE_DATAC17: return ARQ_SNR_MIN_DATAC17_DB;
    case FREEDV_MODE_DATAC1:  return ARQ_SNR_MIN_DATAC1_DB;
    case FREEDV_MODE_DATAC3:  return ARQ_SNR_MIN_DATAC3_DB;
    case FREEDV_MODE_DATAC4:  return ARQ_SNR_MIN_DATAC4_DB;
    default:                  return -100.0f;  /* DATAC15 floor / unknown */
    }
}

/* True when the peer-reported SNR (the FORWARD link — what the IRS sees of our
 * data) comfortably supports the given payload mode.  Lets us tell a forward
 * decode failure (step the mode down) apart from a reverse-path ACK loss (hold
 * the mode).  Conservative when no peer reading exists yet. */
static bool peer_snr_supports_mode(const arq_session_t *sess, int mode)
{
    if (!sess->peer_snr_valid)
        return false;
    float peer_snr = (float)sess->peer_snr_x10 / 10.0f;
    return peer_snr >= mode_snr_floor_db(mode) + ARQ_REVERSE_LOSS_MARGIN_DB;
}

/** Record the outcome of a TX frame.  Called once per frame when its fate is
 *  known: clean=true when ACK arrived with no retries consumed, clean=false
 *  when the frame was retransmitted at least once.  Steps speed_level ladder
 *  up (slowly, after consecutive clean ACKs) or down (immediately on any
 *  non-clean outcome). */
static void record_tx_outcome(arq_session_t *sess, bool clean)
{
    /* Asymmetric-link awareness: a non-clean outcome (frame retransmitted) is
     * often a LOST ACK on the reverse path, not a forward decode failure — HF
     * links are typically asymmetric.  peer_snr reads the forward link directly;
     * if it comfortably supports the current mode, treat the retry as a
     * reverse-path loss and HOLD the forward mode + OLLA offset (penalizing them
     * would needlessly under-run a healthy forward link).  Genuine forward /
     * propagation degradation drops peer_snr and falls through to the normal
     * step-down below. */
    if (!clean && peer_snr_supports_mode(sess, sess->payload_mode))
    {
        HLOGD(LOG_COMP,
              "Retry but peer_snr=%.1f dB supports mode %d — likely reverse-path "
              "ACK loss; holding forward level %d (no step-down)",
              (float)sess->peer_snr_x10 / 10.0f, sess->payload_mode,
              sess->speed_level);
        return;
    }

    /* OLLA: drive the per-link SNR offset toward the target first-try FER.
     * This is the primary anti-oscillation: a mode that keeps failing pushes
     * the offset down (via select_best_mode's effective SNR) and holds it
     * there until clean delivery at a lower mode raises it again. */
    sess->olla_offset_db = arq_olla_update(sess->olla_offset_db, clean);

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

    /* Hard total-link-loss net.  OLLA (below) is the primary controller: its
     * SNR offset already drives the effective SNR — and thus the selected mode —
     * down on sustained first-try failures, smoothly and without oscillation.
     * The old per-2-retry forced downgrade + hold ran ALONGSIDE OLLA and fought
     * it: on a marginal/fading link it ratcheted the mode toward the floor on
     * every fade cluster and churned MODE_REQ round-trips (measured 4-8x more
     * mode changes and ~20% less goodput at 5-8 dB — test_olla_low_snr_no_collapse).
     * Keep only a hard net: after a long unbroken fail run (genuine link loss,
     * not a normal fade dip) drop straight to the robust floor; OLLA climbs back. */
    if (sess->consecutive_retries >= ARQ_HARD_LOSS_THRESHOLD &&
        mode_rank(effective_mode) > 0)
        return FREEDV_MODE_DATAC15;

    /* Don't upgrade if the backlog fits in a single frame at the current mode.
     * MODE_REQ/MODE_ACK airtime overhead is never worthwhile for one frame. */
    const arq_mode_timing_t *cur = arq_protocol_mode_timing(effective_mode);
    if (cur && backlog <= cur->payload_bytes - ARQ_FRAME_HDR_SIZE)
        return effective_mode;

    /* OLLA: threshold on the delivery-corrected SNR, not the raw peer report.
     * The offset (negative after failures) keeps a fade-failing mode from being
     * re-selected until clean delivery at the lower mode raises it back. */
    float peer_snr = (float)sess->peer_snr_x10 / 10.0f + sess->olla_offset_db;
    int   cur_rank = mode_rank(effective_mode);

    /* For the current mode, stay if SNR is at or above base threshold.
     * For a higher mode, upgrade only if SNR exceeds threshold + hysteresis.
     * This asymmetry prevents rapid oscillation at mode boundaries. */
    if (arq_bandwidth_allows_mode(FREEDV_MODE_QAM16C2))
    {
        float qc2_thresh = (cur_rank >= mode_rank(FREEDV_MODE_QAM16C2))
                           ? ARQ_SNR_MIN_QAM16C2_DB
                           : ARQ_SNR_MIN_QAM16C2_DB + ARQ_SNR_HYST_DB;
        if (peer_snr >= qc2_thresh && backlog >= ARQ_BACKLOG_MIN_QAM16C2)
            return FREEDV_MODE_QAM16C2;
    }

    if (arq_bandwidth_allows_mode(FREEDV_MODE_DATAC17))
    {
        float c17_thresh = (cur_rank >= mode_rank(FREEDV_MODE_DATAC17))
                           ? ARQ_SNR_MIN_DATAC17_DB
                           : ARQ_SNR_MIN_DATAC17_DB + ARQ_SNR_HYST_DB;
        if (peer_snr >= c17_thresh && backlog >= ARQ_BACKLOG_MIN_DATAC17)
            return FREEDV_MODE_DATAC17;
    }

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

    float c4_thresh = (cur_rank >= mode_rank(FREEDV_MODE_DATAC4))
                      ? ARQ_SNR_MIN_DATAC4_DB
                      : ARQ_SNR_MIN_DATAC4_DB + ARQ_SNR_HYST_DB;
    if (peer_snr >= c4_thresh && backlog >= ARQ_BACKLOG_MIN_DATAC4)
        return FREEDV_MODE_DATAC4;

    return FREEDV_MODE_DATAC15;
}

/** Check whether a mode upgrade/downgrade is warranted.  If yes, send
 *  MODE_REQ and enter MODE_REQ_TX.  Returns true when negotiation started. */
static bool maybe_upgrade_mode(arq_session_t *sess)
{
    /* --- Phase-A diagnostic: throttled OLLA state, to confirm on-air climbing
     * (peer_snr should now be non-zero and the body should ride a fast mode). */
    {
        static uint64_t s_olla_log_ms = 0;
        uint64_t now_ms = hermes_uptime_ms();
        if (now_ms - s_olla_log_ms >= 4000)
        {
            s_olla_log_ms = now_ms;
            int bk = g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0;
            HLOGI(LOG_COMP,
                  "OLLA-state: payload_mode=%d peer_snr=%.1f local_snr=%.1f olla=%+.1f retries=%d backlog=%d startup_rem=%lldms",
                  sess->payload_mode,
                  (float)sess->peer_snr_x10 / 10.0f,
                  (float)sess->local_snr_x10 / 10.0f,
                  sess->olla_offset_db, sess->consecutive_retries, bk,
                  (long long)((int64_t)sess->startup_deadline_ms - (int64_t)now_ms));
        }
    }

    /* Hold the initial DATAC15 payload mode during the startup window. */
    if (hermes_uptime_ms() < sess->startup_deadline_ms)
        return false;

    /* Never change the payload mode while unACKed frames are in flight.  Those
     * go-back-N window frames were built for the current mode; the per-frame
     * size and the FULL-length sentinel (payload_valid==0 means "all slot bytes
     * valid") are mode-RELATIVE, so retransmitting them after a mode change
     * makes the receiver — now decoding at the new mode's geometry — deliver the
     * wrong byte count (a FULL DATAC15 frame read at DATAC3's larger slot
     * over-delivers: the 118-vs-112 bidirectional regression).  Defer the change
     * until the window drains; it reaches 0 after every fully-ACKed burst, so
     * mode adaptation still happens at burst boundaries. */
    if (sess->tx_window_count > 0)
        return false;

    /* Normally we need at least one valid peer SNR reading before deciding.
     * Exception: a retry-forced downgrade is driven purely by delivery
     * failure (consecutive_retries), not SNR — select_best_mode's safety net
     * steps the mode down regardless of SNR.  Allow it through even with no
     * SNR estimate, otherwise a link that never lands an advancing ACK (so
     * no reading ever arrives) would keep retransmitting at a too-fast mode
     * forever, defeating the hard-loss drop to the floor.  Gate on the
     * validity flag, not peer_snr_x10==0, so a genuine 0 dB report (snr_raw=128,
     * common at the OTA fade cliff) is honoured instead of mistaken for "no
     * reading" and stalling the climb. */
    if (!sess->peer_snr_valid &&
        sess->consecutive_retries < ARQ_HARD_LOSS_THRESHOLD)
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

    /* After a hard-loss drop to the floor, hold briefly so OLLA re-climbs on
     * fresh delivery evidence rather than stale SNR. */
    if (mode_rank(desired_mode) < mode_rank(sess->payload_mode) &&
        sess->consecutive_retries >= ARQ_HARD_LOSS_THRESHOLD)
    {
        sess->mode_hold_until_ms =
            hermes_uptime_ms() + (ARQ_MODE_HOLD_AFTER_DOWNGRADE_S * 1000ULL);
        sess->consecutive_retries = 0;
        HLOGI(LOG_COMP, "Hard-loss downgrade to floor: hold for %ds",
              ARQ_MODE_HOLD_AFTER_DOWNGRADE_S);
    }

    sess->mode_upgrade_count = 0;
    sess->pending_tx_mode = desired_mode;
    sess->tx_retries_left = ARQ_MODE_REQ_RETRIES;

    HLOGI(LOG_COMP, "Mode negotiation: %d -> %d (peer_snr=%.1f olla=%+.1f eff=%.1f dB, backlog=%d)",
          sess->payload_mode, desired_mode,
          (float)sess->peer_snr_x10 / 10.0f, sess->olla_offset_db,
          (float)sess->peer_snr_x10 / 10.0f + sess->olla_offset_db, backlog);

    send_mode_negotiation(sess, ARQ_SUBTYPE_MODE_REQ, desired_mode);
    dflow_enter(sess, ARQ_DFLOW_MODE_REQ_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
    return true;
}

/** Deliver RX payload to the application only if the sequence number matches
 *  what we expect.  Returns true if data was delivered (new frame), false if
 *  it was a duplicate that was silently dropped. */
/* IRS: arm the ACK deadline after a received DATA frame.  Burst-aware:
 * a frame without ARQ_FLAG_BURST_END announces more frames in the same PTT
 * burst, so wait ~1.5x the peer's frame duration for the next one (this
 * also covers a lost BURST_END frame — the fallback fires and we ACK what
 * we have).  The final frame of a burst arms the normal channel guard.
 * With burst_frames=1 every frame carries BURST_END, which is exactly the
 * pre-burst behaviour. */
static void irs_arm_ack_deadline(arq_session_t *sess, const arq_event_t *ev)
{
    uint64_t wait_ms = ARQ_CHANNEL_GUARD_MS;
    if (!(ev->rx_flags & ARQ_FLAG_BURST_END))
    {
        const arq_mode_timing_t *tm = arq_protocol_mode_timing(sess->peer_tx_mode);
        float fd = tm ? tm->frame_duration_s : 5.0f;
        wait_ms = (uint64_t)(fd * 1500.0f);
    }
    dflow_enter(sess, ARQ_DFLOW_DATA_RX,
                hermes_uptime_ms() + wait_ms, ARQ_EV_TIMER_ACK);
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
    const char *my_call = arq_conn.my_call_sign;
    int bw_hz = is_accept ? arq_reported_bandwidth_hz() : arq_conn.bw;
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
        send_frame(PACKET_TYPE_ARQ_CONTROL, sess->control_mode, (size_t)n, frame, 0);
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
        send_frame(PACKET_TYPE_ARQ_CONTROL, sess->control_mode, (size_t)n, frame, 0);
}

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

    int burst_max = tm->burst_frames;
    if (burst_max < 1) burst_max = 1;
    if (burst_max > ARQ_BURST_MAX) burst_max = ARQ_BURST_MAX;
    /* Keep the very first exchanges single-frame: the startup window is
     * about proving the link before spending long PTT bursts on it. */
    if (hermes_uptime_ms() < sess->startup_deadline_ms)
        burst_max = 1;

    uint8_t payload[INT_BUFFER_SIZE];

    /* Top up the go-back-N window with fresh frames.  Existing entries are
     * unACKed frames queued for retransmission — they go out again first
     * (same bytes, same seq), never re-read from the ring. */
    while (sess->tx_window_count < burst_max)
    {
        int slot_idx = sess->tx_window_count;

        memset(payload, 0, user_bytes);
        int payload_len = g_cbs.tx_read(payload, user_bytes);
        if (payload_len <= 0)
            break;  /* backlog drained */

        /* 0 = full frame; else exact valid byte count (receiver trims).
         * Bits [7:0] travel in the payload_valid byte, bits 8-10 in the
         * flags (LEN_HI/LEN_B9/LEN_B10) — counts up to 2047. */
        uint16_t payload_valid;
        uint8_t  data_flags = 0;
        if ((size_t)payload_len == user_bytes)
        {
            payload_valid = ARQ_DATA_LEN_FULL;
        }
        else
        {
            payload_valid = (uint16_t)payload_len;
            if (payload_len & 0x100) data_flags |= ARQ_FLAG_LEN_HI;
            if (payload_len & 0x200) data_flags |= ARQ_FLAG_LEN_B9;
            if (payload_len & 0x400) data_flags |= ARQ_FLAG_LEN_B10;
        }

        uint8_t snr_raw = 0;
        if (sess->local_snr_x10 != 0)
            snr_raw = arq_protocol_encode_snr((float)sess->local_snr_x10 / 10.0f);

        int n = arq_protocol_build_data(sess->tx_window[slot_idx].buf,
                                        sizeof(sess->tx_window[slot_idx].buf),
                                        sess->session_id, sess->tx_seq,
                                        sess->rx_expected, data_flags, snr_raw,
                                        payload_valid,
                                        payload, user_bytes);
        if (n <= 0)
            break;

        sess->tx_window[slot_idx].len         = n;
        sess->tx_window[slot_idx].payload_len = payload_len;
        sess->tx_window[slot_idx].seq         = sess->tx_seq;
        sess->tx_window_count++;
        sess->tx_seq++;
        sess->tx_inflight_bytes += payload_len;
    }

    if (sess->tx_window_count == 0)
        return;  /* nothing to (re)send */

    /* Transmit the whole window as one PTT burst.  BURST_END is set only on
     * the last frame so the IRS sends a single cumulative ACK. */
    for (int i = 0; i < sess->tx_window_count; i++)
    {
        uint8_t *f = sess->tx_window[i].buf;
        if (i == sess->tx_window_count - 1)
            f[ARQ_HDR_FLAGS_IDX] |= ARQ_FLAG_BURST_END;
        else
            f[ARQ_HDR_FLAGS_IDX] &= (uint8_t)~ARQ_FLAG_BURST_END;

        send_frame(PACKET_TYPE_ARQ_DATA, sess->payload_mode,
                   (size_t)sess->tx_window[i].len, f,
                   sess->tx_window_count - 1 - i);
        if (g_timing)
            arq_timing_record_tx_queue(g_timing, (int)sess->tx_window[i].seq,
                                       sess->payload_mode,
                                       g_cbs.tx_backlog(),
                                       sess->tx_window[i].payload_len);
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
    if (g_cbs.tx_backlog && g_cbs.tx_backlog() > 0)
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
        sess->session_id      = (uint8_t)(hermes_uptime_ms() & 0x7F) | 0x01;
        sess->tx_retries_left = ARQ_CALL_RETRY_SLOTS;
        sess->pending_disconnect = false;  /* clear stale deferred disconnect from prior session */
        sess->disconnect_deadline_ms = 0;
        /* Reset mode state for new session */
        sess->payload_mode       = FREEDV_MODE_DATAC15;
        sess->peer_tx_mode       = FREEDV_MODE_DATAC15;
        sess->speed_level        = 0;
        sess->tx_success_count   = 0;
        sess->olla_offset_db     = 0.0f;
        sess->consecutive_retries = 0;
        sess->mode_hold_until_ms = 0;
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
         * initial DATAC15.  This must happen here (not in sess_enter for
         * DISCONNECTED/LISTENING) because LISTENING needs peer_tx_mode to
         * stay at the broadcast mode for receiving broadcast frames. */
        sess->payload_mode       = FREEDV_MODE_DATAC15;
        sess->peer_tx_mode       = FREEDV_MODE_DATAC15;
        sess->speed_level        = 0;
        sess->tx_success_count   = 0;
        sess->olla_offset_db     = 0.0f;
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
            sess->tx_window_count = 0;
            sess->tx_window_retx  = false;
            sess->tx_inflight_bytes = 0;
            sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;
            sess->payload_mode       = FREEDV_MODE_DATAC15;
            sess->peer_tx_mode       = FREEDV_MODE_DATAC15;
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
    switch (ev->id)
    {
    case ARQ_EV_RX_ACCEPT:
        if (ev->session_id == sess->session_id)
        {
            bool has_tx_backlog = g_cbs.tx_backlog && g_cbs.tx_backlog() > 0;
            sess->role        = ARQ_ROLE_CALLER;
            sess->tx_seq      = 0;
            sess->rx_expected = 0;
            sess->tx_window_count = 0;  /* discard any stale retransmit buf from prior session */
            sess->tx_window_retx  = false;
            sess->tx_inflight_bytes = 0;
            sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;
            sess->payload_mode       = FREEDV_MODE_DATAC15;  /* reset mode state from prior session */
            sess->peer_tx_mode       = FREEDV_MODE_DATAC15;
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
            sess->deadline_ms = deadline_from_s(arq_protocol_call_interval_s());
        }
        else
        {
            if (g_cbs.notify_disconnected) g_cbs.notify_disconnected(false);
            enter_idle_after_call(sess);
        }
        break;

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
        sess->tx_seq      = 0;
        sess->rx_expected = 0;
        sess->tx_window_count = 0;  /* discard any stale retransmit buf from prior session */
        sess->tx_window_retx  = false;
        sess->tx_inflight_bytes = 0;
        sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;
        sess->payload_mode       = FREEDV_MODE_DATAC15;  /* reset mode state from prior session */
        sess->peer_tx_mode       = FREEDV_MODE_DATAC15;
        sess->pending_tx_mode    = 0;
        sess->mode_upgrade_count = 0;
        sess->speed_level        = 0;
        sess->tx_success_count   = 0;
        sess->olla_offset_db     = 0.0f;
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
         * start its first DATA frame (DATAC15, ~4400 ms) almost immediately
         * after our PTT drops.  The deadline that was set in TIMER_RETRY was
         * relative to when TIMER_RETRY fired — not to TX_COMPLETE — so it
         * only left ~4400 ms of RX window after PTT-OFF, which is barely
         * one DATAC15 frame.  Reset the deadline here so we always have
         * a full ARQ_ACCEPT_RX_WINDOW_MS window (guard + DATAC15 frame +
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
    const arq_mode_timing_t *tm;

    /* Fallback for a deferred APP_DISCONNECT that the normal fire points
     * (idle-ISS entry, WAIT_ACK ack-timer, retry exhaustion) never reach —
     * e.g. a session stuck ping-ponging keepalives or pinned as IRS.  Once
     * the drain deadline elapses, force a clean air-side teardown regardless
     * of role or backlog so the rig is never keyed indefinitely after the
     * host has disconnected (K7EK "Mercury kept hanging on"). */
    if (sess->pending_disconnect && sess->disconnect_deadline_ms != 0 &&
        hermes_uptime_ms() >= sess->disconnect_deadline_ms)
    {
        HLOGW(LOG_COMP,
              "Deferred DISCONNECT drain timeout (%ds) — forcing teardown",
              ARQ_DISCONNECT_DRAIN_TIMEOUT_S);
        sess->pending_disconnect      = false;
        sess->disconnect_deadline_ms  = 0;
        sess->tx_retries_left         = ARQ_DISCONNECT_RETRY_SLOTS;
        sess_enter(sess, ARQ_CONN_DISCONNECTING,
                   hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                   ARQ_EV_TIMER_ACK);
        return;
    }

    switch (ev->id)
    {
    case ARQ_EV_APP_DISCONNECT:
        /* Defer DISCONNECT while a frame is physically being transmitted
         * (PTT on, DATA_TX), still awaiting its ACK (WAIT_ACK), or the TX
         * buffer has unsent bytes, so the last application bytes get
         * delivered before teardown.  Without the WAIT_ACK case the final
         * frame loses its retry protection whenever the app's disconnect
         * lands after PTT-OFF but before the ACK — the same last-frame loss
         * the WAIT_ACK capped-retry fix addresses, just in a different
         * timing window.  The deferral is bounded three ways and can never
         * hang: (1) retry exhaustion with a pending disconnect tears down
         * immediately, (2) the absolute disconnect_drain_timeout_s deadline
         * armed below forces teardown regardless of FSM state, and (3) a
         * drained buffer fires the deferred disconnect at the next idle-ISS
         * entry (an ACKed last frame with empty backlog disconnects there
         * without extra delay). */
        if ((g_cbs.tx_backlog && g_cbs.tx_backlog() > 0) ||
            sess->dflow_state == ARQ_DFLOW_DATA_TX ||
            sess->dflow_state == ARQ_DFLOW_WAIT_ACK)
        {
            HLOGD(LOG_COMP,
                  "APP_DISCONNECT deferred — backlog=%d dflow=%s",
                  g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0,
                  arq_dflow_state_name(sess->dflow_state));
            sess->pending_disconnect = true;
            sess->disconnect_deadline_ms =
                hermes_uptime_ms() +
                (uint64_t)ARQ_DISCONNECT_DRAIN_TIMEOUT_S * 1000ULL;
            return;
        }
        sess->pending_disconnect      = false;
        sess->tx_retries_left         = ARQ_DISCONNECT_RETRY_SLOTS;
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
        sess->keepalive_from_irs = false;  /* ISS-originated keepalive */
        send_ctrl_frame(sess, ARQ_SUBTYPE_KEEPALIVE);
        tm = arq_protocol_mode_timing(sess->control_mode);
        dflow_enter(sess, ARQ_DFLOW_KEEPALIVE_TX,
                    deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                    ARQ_EV_TIMER_RETRY);
        return;

    case ARQ_EV_RX_KEEPALIVE:
        /* Handle keepalive probe from ANY data-flow state so the peer
         * never sees a timeout just because we are busy (e.g. WAIT_ACK
         * retrying a data frame whose ACK is lost).  KEEPALIVE_TX and
         * KEEPALIVE_WAIT manage their own RX_KEEPALIVE paths in fsm_dflow.
         *
         * When we are in an idle state (IDLE_ISS or IDLE_IRS), defer the
         * KEEPALIVE_ACK by ARQ_CHANNEL_GUARD_MS so the peer has time to
         * finish its KEEPALIVE TX and switch back to RX.  Without this
         * guard the OFDM decoder fires ~200ms before the peer's PTT-OFF
         * and our KEEPALIVE_ACK is transmitted too early, colliding with
         * the peer's still-active TX (issue #70). */
        if (sess->dflow_state != ARQ_DFLOW_KEEPALIVE_TX &&
            sess->dflow_state != ARQ_DFLOW_KEEPALIVE_WAIT &&
            sess->dflow_state != ARQ_DFLOW_KEEPALIVE_ACK_TX)
        {
            HLOGI(LOG_COMP, "RX_KEEPALIVE in dflow=%s — sending KEEPALIVE_ACK",
                  arq_dflow_state_name(sess->dflow_state));
            sess->keepalive_miss_count = 0;
            if (sess->dflow_state == ARQ_DFLOW_IDLE_IRS ||
                sess->dflow_state == ARQ_DFLOW_IDLE_ISS)
            {
                /* Guarded response: defer TX so the peer finishes its
                 * KEEPALIVE and returns to RX before our ACK arrives. */
                sess->keepalive_ack_from_irs =
                    (sess->dflow_state == ARQ_DFLOW_IDLE_IRS);
                dflow_enter(sess, ARQ_DFLOW_KEEPALIVE_ACK_TX,
                            hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                            ARQ_EV_TIMER_ACK);
            }
            else
            {
                /* Busy state (DATA_TX, WAIT_ACK, etc.): immediate ACK
                 * so we don't perturb the active data-flow timeline. */
                send_ctrl_frame(sess, ARQ_SUBTYPE_KEEPALIVE_ACK);
            }
            return;
        }
        break;

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
                send_data_burst(sess);
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
            irs_arm_ack_deadline(sess, ev);
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
            send_data_burst(sess);
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
            /* Cumulative ACK: ack_seq is the peer's rx_expected (next seq it
             * wants), so frames with seq < ack_seq are confirmed.  With
             * burst_frames=1 this degenerates to the classic single-frame
             * accept (ack_seq == base+1 -> n_acked == 1). */
            int n_acked = 0;
            if (sess->tx_window_count > 0)
            {
                uint8_t base = sess->tx_window[0].seq;
                uint8_t dist = (uint8_t)(ev->ack_seq - base);
                if (dist >= 1 && dist <= (uint8_t)sess->tx_window_count)
                    n_acked = (int)dist;
            }

            /* peer_snr_x10 = IRS's local SNR = quality of IRS receiving our data */
            update_peer_snr(sess, ev);

            if (n_acked == 0)
            {
                /* Stale/duplicate ACK that confirms nothing new (e.g. the
                 * whole burst was lost and the IRS re-ACKed its old
                 * rx_expected).  Keep waiting; TIMER_ACK drives the
                 * retransmission. */
                HLOGD(LOG_COMP, "ACK with no progress (ack_seq=%d window=%d)",
                      (int)ev->ack_seq, sess->tx_window_count);
                break;
            }

            if (g_timing)
                arq_timing_record_ack_rx(g_timing,
                                         (int)sess->tx_window[n_acked - 1].seq,
                                         (uint8_t)ev->ack_delay_raw,
                                         sess->peer_snr_x10);

            /* "clean" = this window never needed a retransmission and no
             * retry slots were consumed — captured before the reset below. */
            bool window_clean = !sess->tx_window_retx &&
                                sess->tx_retries_left == ARQ_DATA_RETRY_SLOTS;

            /* Slide the window past the confirmed frames. */
            for (int i = 0; i < n_acked; i++)
                sess->tx_inflight_bytes -= sess->tx_window[i].payload_len;
            if (sess->tx_inflight_bytes < 0)
                sess->tx_inflight_bytes = 0;
            for (int i = n_acked; i < sess->tx_window_count; i++)
                sess->tx_window[i - n_acked] = sess->tx_window[i];
            sess->tx_window_count -= n_acked;

            sess->tx_retries_left     = ARQ_DATA_RETRY_SLOTS;
            sess->last_tx_progress_ms = hermes_uptime_ms();  /* forward progress */
            sess->peer_has_data = (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0;
            if (g_cbs.send_buffer_status)
                g_cbs.send_buffer_status(g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0);

            if (sess->tx_window_count > 0)
            {
                /* Partial ACK: a fade clipped the tail of the burst.  The
                 * remainder is retransmitted (go-back-N) in the next burst
                 * after the post-ACK guard. */
                sess->tx_window_retx = true;
                record_tx_outcome(sess, false);
                dflow_enter(sess, ARQ_DFLOW_DATA_TX,
                            hermes_uptime_ms() + ARQ_ISS_POST_ACK_GUARD_MS,
                            ARQ_EV_TIMER_ACK);
                break;
            }

            record_tx_outcome(sess, window_clean);
            sess->tx_window_retx = false;

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
        else if (ev->id == ARQ_EV_RX_TURN_REQ)
        {
            /* The peer has reverse data and explicitly requested the floor
             * while we are awaiting the ACK of our last burst.  Yield instead
             * of ignoring it: otherwise we sit out the full ack-timeout and
             * retransmit, while the peer keeps re-sending TURN_REQ — a mutual
             * stall that hangs bidirectional traffic (e.g. the uucp handshake).
             * Our unacked window is retransmitted (go-back-N) once we regain
             * the turn, so no data is lost. */
            if (g_timing) arq_timing_record_turn(g_timing, false, "turn_req");
            dflow_enter(sess, ARQ_DFLOW_TURN_ACK_TX,
                        hermes_uptime_ms() + ARQ_CHANNEL_GUARD_MS,
                        ARQ_EV_TIMER_ACK);
        }
        else if (ev->id == ARQ_EV_TIMER_ACK)
        {
            if (sess->tx_retries_left > 0)
            {
                /* Save the pre-cap value so the attempt number reported to
                 * timing instrumentation reflects the true retry count rather
                 * than the artificially capped one. */
                int retries_before_cap = (int)sess->tx_retries_left;

                /* When a disconnect is pending (deferred from DATA_TX), cap
                 * remaining retries to 1 so the peer gets one more chance to
                 * ACK our last frame before we give up.  Aborting with zero
                 * retries (old Fix-14 behaviour) drops the final UUCP hangup
                 * frame, causing "Got termination signal" on the remote side. */
                if (sess->pending_disconnect && sess->tx_retries_left > 1)
                {
                    HLOGD(LOG_COMP,
                          "Pending DISCONNECT: capping retries to 1 for seq=%d",
                          (int)sess->tx_seq);
                    sess->tx_retries_left = 1;
                }
                sess->tx_retries_left--;
                sess->tx_window_retx = true;  /* whole window goes again */
                /* Ladder step-down happens once per frame in the RX_ACK /
                 * implicit-ACK handler via record_tx_outcome(), NOT here.
                 * Calling it on every retry would cause double/triple penalty
                 * when the ACK handler also calls it. */
                if (g_timing)
                    arq_timing_record_retry(g_timing,
                                            sess->tx_window_count > 0
                                                ? (int)sess->tx_window[0].seq
                                                : (int)sess->tx_seq,
                                            ARQ_DATA_RETRY_SLOTS - retries_before_cap + 1,
                                            "ack_timeout");
                dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
                send_data_burst(sess);
            }
            else
            {
                /* Retry budget exhausted — disconnect only if we're past the
                 * absolute no-progress wall-clock budget.  Otherwise reset the
                 * retry counter and keep banging at the channel: VARA-like
                 * persistence beats Pactor-like give-up.  Bumping
                 * consecutive_retries lets select_best_mode pull us down to a
                 * more robust mode the next time a decision point runs. */
                uint64_t now = hermes_uptime_ms();
                uint64_t budget_ms = (uint64_t)ARQ_NO_PROGRESS_TIMEOUT_S * 1000ULL;
                bool no_progress_dead =
                    (now - sess->last_tx_progress_ms) >= budget_ms;
                /* The application has already asked to disconnect and we are
                 * only draining its final bytes.  Persistence is for data the
                 * app still wants delivered; once it has said "disconnect",
                 * the contract is bounded effort then a CLEAN air-side
                 * teardown.  Hammering the channel for the full no-progress
                 * budget here leaves BPQ32 thinking it disconnected cleanly
                 * while Mercury keys the rig for minutes and the peer times
                 * out — the "dirty disconnect" Gary/K7EK reported.  So treat a
                 * pending disconnect like the dead-channel case: stop draining
                 * and complete the DISCONNECT handshake now. */
                if (no_progress_dead || sess->pending_disconnect)
                {
                    HLOGW(LOG_COMP,
                          "Data retry exhausted seq=%d (%s) — disconnecting",
                          (int)sess->tx_seq,
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
                          (int)sess->tx_seq, since_s, ARQ_NO_PROGRESS_TIMEOUT_S);
                    if (g_timing)
                        arq_timing_record_retry(g_timing, (int)sess->tx_seq,
                                                ARQ_DATA_RETRY_SLOTS,
                                                "persist_after_exhaust");
                    sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;

                    /* Channel couldn't deliver a frame in ARQ_DATA_RETRY_SLOTS
                     * attempts — force the delivery-feedback safety net to
                     * threshold so select_best_mode pulls payload_mode one
                     * step lower.  maybe_upgrade_mode handles the MODE_REQ /
                     * MODE_ACK negotiation; on success the FSM is now in
                     * MODE_REQ_TX state and will resume DATA_TX at the lower
                     * mode after MODE_ACK.  On failure (no peer SNR yet,
                     * MODE_REQ retries exhaust, already at slowest mode)
                     * fall through and retransmit at the current mode. */
                    if (sess->consecutive_retries < ARQ_RETRY_DOWNGRADE_THRESHOLD)
                        sess->consecutive_retries = ARQ_RETRY_DOWNGRADE_THRESHOLD;
                    if (maybe_upgrade_mode(sess))
                        return;

                    dflow_enter(sess, ARQ_DFLOW_DATA_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
                    send_data_burst(sess);
                }
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
                record_tx_outcome(sess, !sess->tx_window_retx &&
                                        sess->tx_retries_left == ARQ_DATA_RETRY_SLOTS);
                sess->tx_window_count   = 0;   /* implicit ACK covers the window */
                sess->tx_window_retx    = false;
                sess->tx_inflight_bytes = 0;
                sess->tx_retries_left   = ARQ_DATA_RETRY_SLOTS;
                sess->last_tx_progress_ms = hermes_uptime_ms();
                sess->peer_has_data = (ev->rx_flags & ARQ_FLAG_HAS_DATA) != 0;
                if (g_cbs.send_buffer_status)
                    g_cbs.send_buffer_status(g_cbs.tx_backlog ? g_cbs.tx_backlog() : 0);
                if (deliver_rx_checked(sess, ev) && g_timing)
                    arq_timing_record_data_rx(g_timing, (int)ev->seq,
                                              (int)ev->data_bytes,
                                              sess->local_snr_x10);
                irs_arm_ack_deadline(sess, ev);
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
                send_data_burst(sess);
            }
        }
        /* TURN_REQ is intentionally ignored in WAIT_ACK: the ISS must not
         * give up its role while a data frame is still unacknowledged.
         * The peer's TURN_REQ will be honoured after the ACK arrives and
         * the ISS enters IDLE_ISS (or after retries are exhausted). */
        else if (ev->id == ARQ_EV_RX_MODE_REQ)
        {
            if (arq_protocol_mode_timing(ev->mode) != NULL &&
                ev->mode != ARQ_CONTROL_MODE)
            {
                /* Peer has taken ISS and is requesting a mode switch.  The peer
                 * only enters ISS after receiving our pending frame (via DATA_RX
                 * → ACK_TX → piggyback or TURN_REQ), so this is an implicit ACK.
                 * Advance tx_seq, update RX decoder mode (not our TX mode), send
                 * MODE_ACK, then hand turn to peer (MODE_ACK_TX → IDLE_IRS). */
                HLOGI(LOG_COMP,
                      "MODE_REQ in WAIT_ACK (implicit ACK) tx_seq=%d peer_tx_mode %d->%d (my TX %d unchanged)",
                      (int)sess->tx_seq, sess->peer_tx_mode, ev->mode, sess->payload_mode);
                record_tx_outcome(sess, !sess->tx_window_retx &&
                                        sess->tx_retries_left == ARQ_DATA_RETRY_SLOTS);
                sess->tx_window_count   = 0;   /* implicit ACK covers the window */
                sess->tx_window_retx    = false;
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
            irs_arm_ack_deadline(sess, ev);
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
                sess->keepalive_from_irs = true;  /* IRS-originated keepalive */
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
                ev->mode != ARQ_CONTROL_MODE)
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
            /* Re-arm per frame: mid-burst frames push the ACK deadline out
             * past the next expected frame; the BURST_END frame collapses
             * it to the channel guard. */
            irs_arm_ack_deadline(sess, ev);
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
            irs_arm_ack_deadline(sess, ev);
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
            if (sess->keepalive_from_irs)
                enter_idle_irs(sess);
            else
                enter_idle_iss_guarded(sess, false);
        }
        else if (ev->id == ARQ_EV_RX_KEEPALIVE)
        {
            send_ctrl_frame(sess, ARQ_SUBTYPE_KEEPALIVE_ACK);
            sess->keepalive_miss_count = 0;
            if (sess->keepalive_from_irs)
                enter_idle_irs(sess);
            else
                enter_idle_iss_guarded(sess, false);
        }
        else if (ev->id == ARQ_EV_TIMER_RETRY)
        {
            sess->keepalive_miss_count++;
            HLOGD(LOG_COMP, "Keepalive miss %d/%d",
                  sess->keepalive_miss_count, ARQ_KEEPALIVE_MISS_LIMIT);
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

    case ARQ_DFLOW_KEEPALIVE_ACK_TX:
        /* Guarded KEEPALIVE_ACK transmitter: TIMER_ACK fires after
         * ARQ_CHANNEL_GUARD_MS, giving the peer time to finish its
         * KEEPALIVE TX and switch its radio back to RX (issue #70). */
        if (ev->id == ARQ_EV_TIMER_ACK)
            send_ctrl_frame(sess, ARQ_SUBTYPE_KEEPALIVE_ACK);
        else if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            if (sess->keepalive_ack_from_irs)
                enter_idle_irs(sess);
            else
                enter_idle_iss_guarded(sess, false);
        }
        break;

    case ARQ_DFLOW_MODE_REQ_TX:
        /* ISS: MODE_REQ sent, waiting for modem TX to complete. */
        if (ev->id == ARQ_EV_TX_COMPLETE)
        {
            tm = arq_protocol_mode_timing(sess->control_mode);
            if (sess->pending_tx_mode == 0)
            {
                /* Revert notification delivered.  Guard for a full DATAC16
                 * round-trip (peer guard + MODE_ACK TX + channel clear ≈ 5s;
                 * ack_timeout_s=7s provides adequate margin) so the peer's
                 * payload decoder is reset before our next DATA preamble.
                 * Skip mode-upgrade check here — we just aborted one.
                 *
                 * Reset tx_retries_left: it was decremented to 0 by the
                 * MODE_REQ retry loop and must not be inherited by the next
                 * DATA_TX cycle.  Also lets enter_idle_iss() honour a
                 * pending_disconnect whose drain condition was met here. */
                sess->tx_retries_left = ARQ_DATA_RETRY_SLOTS;
                uint64_t guard_ms = tm ? (uint64_t)(tm->ack_timeout_s * 1000.0f) : 6000ULL;
                if (g_cbs.tx_backlog && g_cbs.tx_backlog() > 0)
                    dflow_enter(sess, ARQ_DFLOW_DATA_TX,
                                hermes_uptime_ms() + guard_ms,
                                ARQ_EV_TIMER_ACK);
                else
                    enter_idle_iss(sess, false);
            }
            else
            {
                dflow_enter(sess, ARQ_DFLOW_MODE_REQ_WAIT,
                            deadline_from_s(tm ? tm->retry_interval_s : 7.0f),
                            ARQ_EV_TIMER_RETRY);
            }
        }
        break;

    case ARQ_DFLOW_MODE_REQ_WAIT:
        /* ISS: waiting for MODE_ACK from IRS. */
        if (ev->id == ARQ_EV_RX_MODE_ACK)
        {
            if (arq_protocol_mode_timing(ev->mode) != NULL &&
                ev->mode != ARQ_CONTROL_MODE)
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
                HLOGW(LOG_COMP, "MODE_REQ timeout — staying at mode %d; "
                      "sending revert notification to peer",
                      sess->payload_mode);
                /* Fire-and-forget: tell the peer to revert its payload RX
                 * decoder to our actual (unchanged) TX mode.  Without this,
                 * the peer's decoder stays locked on the negotiated mode and
                 * cannot receive our DATA frames.  pending_tx_mode=0 signals
                 * MODE_REQ_TX that no MODE_ACK wait is needed after this TX. */
                sess->pending_tx_mode = 0;
                send_mode_negotiation(sess, ARQ_SUBTYPE_MODE_REQ, sess->payload_mode);
                dflow_enter(sess, ARQ_DFLOW_MODE_REQ_TX, UINT64_MAX, ARQ_EV_TIMER_RETRY);
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
