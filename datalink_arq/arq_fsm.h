/* HERMES Modem — ARQ FSM: state/event types and session structure
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARQ_FSM_H_
#define ARQ_FSM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arq.h"  /* CALLSIGN_MAX_SIZE, arq_action_t/type, arq_info */
#include "arq_timing.h"  /* arq_timing_ctx_t */
#include "arq_protocol.h"  /* ARQ_BURST_MAX */

/* ======================================================================
 * Level 1 — Connection FSM states
 * ====================================================================== */

typedef enum
{
    ARQ_CONN_DISCONNECTED  = 0, /* no session; idle                              */
    ARQ_CONN_LISTENING     = 1, /* waiting for incoming CALL frame               */
    ARQ_CONN_CALLING       = 2, /* outgoing CALL sent; awaiting ACCEPT           */
    ARQ_CONN_ACCEPTING     = 3, /* ACCEPT sent; awaiting first data/ACK          */
    ARQ_CONN_CONNECTED     = 4, /* data-flow sub-FSM is active                   */
    ARQ_CONN_DISCONNECTING = 5, /* DISCONNECT frame being exchanged              */
    ARQ_CONN__COUNT
} arq_conn_state_t;

/* ======================================================================
 * Level 2 — Data-flow sub-FSM states (active only in ARQ_CONN_CONNECTED)
 * ====================================================================== */

typedef enum
{
    ARQ_DFLOW_IDLE_ISS  =  0, /* ISS: no pending frame; waiting for data          */
    ARQ_DFLOW_DATA_TX   =  1, /* ISS: the one retained frame is queued/on air     */
    ARQ_DFLOW_WAIT_ACK  =  2, /* ISS: PTT-OFF; waiting for the peer's pattern ACK */
    ARQ_DFLOW_IDLE_IRS  =  3, /* IRS: waiting for the peer's data frame           */
    ARQ_DFLOW_ACK_TX    =  4, /* IRS: pattern ACK being transmitted (folds the
                               * old DATA_RX guard into this state's entry)       */
    ARQ_DFLOW__COUNT
} arq_dflow_state_t;

/* ======================================================================
 * Caller/callee role (set at connect time, stays for session lifetime)
 * ====================================================================== */

typedef enum
{
    ARQ_ROLE_NONE   = 0,
    ARQ_ROLE_CALLER = 1,  /* originated the call (starts as ISS) */
    ARQ_ROLE_CALLEE = 2   /* received the call   (starts as IRS) */
} arq_role_t;

/* ======================================================================
 * Events
 * ====================================================================== */

typedef enum
{
    /* Application events (from TCP interface via channel bus) */
    ARQ_EV_APP_LISTEN         =  0,  /* LISTEN ON received            */
    ARQ_EV_APP_STOP_LISTEN    =  1,  /* LISTEN OFF received           */
    ARQ_EV_APP_CONNECT        =  2,  /* CONNECT <dst> received        */
    ARQ_EV_APP_DISCONNECT     =  3,  /* DISCONNECT received           */
    ARQ_EV_APP_DATA_READY     =  4,  /* TX data available in buffer   */

    /* Radio RX events (from modem worker) */
    ARQ_EV_RX_CALL            =  5,  /* CALL frame decoded            */
    ARQ_EV_RX_ACCEPT          =  6,  /* ACCEPT frame decoded          */
    ARQ_EV_RX_ACK             =  7,  /* pattern ACK detected (rx_flags
                                      * HAS_DATA = ACK+TURN break)    */
    ARQ_EV_RX_DATA            =  8,  /* DATA frame decoded            */
    ARQ_EV_RX_DISCONNECT      =  9,  /* DISCONNECT frame decoded      */

    /* Timer events */
    ARQ_EV_TIMER_RETRY        = 10,  /* retry deadline expired        */
    ARQ_EV_TIMER_ACK          = 11,  /* ACK wait deadline expired     */
    ARQ_EV_TIMER_PEER_BACKLOG = 12,  /* peer-backlog hold expired     */

    /* Modem events */
    ARQ_EV_TX_STARTED         = 13,  /* PTT ON (frame on air)         */
    ARQ_EV_TX_COMPLETE        = 14,  /* PTT OFF (TX finished)         */

    ARQ_EV__COUNT
} arq_event_id_t;

/**
 * @brief ARQ event with all possible payload fields.
 *
 * Callers fill only the fields relevant to their event type.
 * Unset numeric fields are 0; unset bool fields are false.
 */
typedef struct
{
    arq_event_id_t id;

    /* Frame-derived fields (set on RX events) */
    uint8_t  session_id;
    uint8_t  seq;
    uint8_t  ack_seq;
    uint8_t  rx_flags;        /* ARQ_FLAG_HAS_DATA (+ LEN_* on DATA frames);
                               * on a pattern ACK, HAS_DATA = ACK+TURN break   */
    int8_t   snr_encoded;     /* as received from frame header                */
    uint16_t ack_delay_raw;   /* as received (10ms units, 0=unknown)          */

    /* Mode negotiation */
    int      mode;            /* requested/applied FreeDV mode                */
    size_t   data_bytes;      /* payload byte count (DATA frames)             */

    /* Received data payload — carried through event queue so the FSM can
     * validate sequence numbers before delivering to the application. */
    uint8_t  payload[1280];           /* >= largest user payload: QAM16C2
                                       * carries 1213 - 8 = 1205 bytes      */
    size_t   payload_len;

    /* Local receive SNR at the time the frame was decoded (dB, 0 = unknown).
     * Carried in-band so the FSM can update local_snr_x10 without relying on
     * the cross-thread arq_update_link_metrics() write, which races with the
     * event queue push in modem.c. */
    float    rx_snr;

    /* Call setup */
    char     remote_call[CALLSIGN_MAX_SIZE];
    /* For an incoming CALL: which of OUR callsigns (primary or a secondary)
     * the frame's DST CRC16 matched — i.e. the SSID the caller dialed.  Empty
     * when not applicable; the callee reports this as the connection's local
     * address so a station listening on multiple SSIDs shows the dialed one. */
    char     local_call[CALLSIGN_MAX_SIZE];
} arq_event_t;

/* ======================================================================
 * Session structure — replaces arq_ctx_t
 *
 * All state is in this struct; no hidden global flags.
 * Monotonic timestamps are uint64_t milliseconds from hermes_log startup.
 * ====================================================================== */

typedef struct
{
    /* --- State machine fields --- */
    arq_conn_state_t  conn_state;      /* Level 1 connection state             */
    arq_dflow_state_t dflow_state;     /* Level 2 data-flow state              */
    arq_role_t        role;            /* CALLER or CALLEE                     */

    /* --- Identifiers --- */
    uint8_t  session_id;               /* random byte chosen by caller         */
    char     remote_call[CALLSIGN_MAX_SIZE];
    char     local_call[CALLSIGN_MAX_SIZE];  /* our dialed callsign/SSID for an
                                              * accepted incoming CALL (empty =>
                                              * fall back to the primary)        */

    /* --- Sequence numbers --- */
    uint8_t  tx_seq;                   /* next seq we will send                */
    uint8_t  rx_expected;              /* next seq we expect from peer         */

    /* --- Mode / speed --- */
    int      payload_mode;             /* MY data TX mode (ISS) = mode_ladder
                                        * [speed_level]; per-direction, set from
                                        * delivery outcomes only (no SNR)       */
    int      control_mode;             /* always FREEDV_MODE_DATAC16           */
    int      initial_payload_mode;     /* startup payload mode (= broadcast RX
                                        * mode); restored on disconnect so the
                                        * payload decoder matches broadcast    */
    int      speed_level;              /* delivery-driven ladder index
                                        * (0 = MFSK floor .. ARQ_LADDER_LEVELS-1)*/
    int      tx_success_count;         /* consecutive clean deliveries toward a
                                        * ladder step-up                        */
    bool     fast_ramp;                /* faster initial climb: 1 rung per clean
                                        * delivery until the first retry, then
                                        * ARQ_LADDER_UP_SUCCESSES-per-step       */
    int      tx_last_good_level;       /* highest rung that has delivered: a
                                        * miss falls back here in one step
                                        * rather than walking down rung by
                                        * rung, each costing an ACK timeout  */
    int      tx_below_good_misses;     /* consecutive misses while ALREADY at
                                        * tx_last_good_level; abandoning a rung
                                        * that has delivered takes more than one
                                        * miss, or occasional loss ratchets the
                                        * session down to the floor            */
    int      peer_tx_mode;             /* my payload RX decoder mode when IRS =
                                        * arq_mode_ladder[rx_speed_level]; the
                                        * mode the peer's NEXT DATA burst uses  */
    int      call_sends_done;          /* CALL transmissions made this attempt;
                                        * counts SENDS, not remaining slots, so
                                        * ARQ_CALL_FAST_SLOTS means what it says */
    int      call_carrier;             /* carrier CHOSEN for the CALL now in
                                        * flight.  Decided once, when the frame
                                        * is built and sized; the modem keys
                                        * exactly this.  Recomputing it later
                                        * raced the send counter and put the
                                        * FIRST call on MFSK.  0 = not set.     */
    int      call_rx_mode;             /* carrier the incoming CALL decoded on,
                                        * so the ACCEPT answers on the same one.
                                        * 0 = answer on the control mode        */

    /* --- IRS-side mirror of the peer's (ISS) delivery-driven ladder --------
     * The IRS observes the same per-frame outcomes the sender climbs on (a
     * clean new frame == a clean delivery; a duplicate == a sender retry), so
     * applying the identical ladder rule keeps rx_speed_level == the sender's
     * speed_level with no on-wire mode negotiation.  Without it the decoder can
     * only ever track a mode it has ALREADY decoded, so it misses the first
     * burst of every mode the sender climbs to (stalling the transfer at the
     * MFSK floor).  A prolonged RX gap (a lost ACK left us above the sender)
     * steps this back down toward the floor so the two ends re-rendezvous. */
    int      rx_speed_level;           /* mirror of the peer's ladder index    */
    int      rx_last_good_level;       /* mirror of tx_last_good_level        */
    int      rx_success_count;         /* clean receives toward a mirror step-up*/
    bool     rx_fast_ramp;             /* mirror of the peer's fast initial ramp*/
    int      rx_below_good_misses;     /* mirror of tx_below_good_misses        */

    /* --- Retry/timeout bookkeeping --- */
    int      tx_retries_left;          /* retries remaining for current frame  */
    uint64_t state_enter_ms;          /* when current conn_state was entered   */

    /* --- Peer state observed from frames --- */
    bool     peer_has_data;            /* peer's HAS_DATA flag in last frame   */
    bool     acktx_had_has_data;       /* HAS_DATA was set in the last ACK sent */
    /* Peer-reported SNR for OUR signal, * 10.  TELEMETRY ONLY.
     *
     * This branch's data plane is delivery-driven: it does NOT adapt on SNR,
     * and must not start doing so here (that was the gear-shift oscillation
     * the rethink removed).  The peer's reading is still on the wire in every
     * frame header, though, and an operator setting TX drive needs it -- it is
     * the only number that says whether the far side can hear us.  So it is
     * kept purely so the UI can show it.
     *
     * peer_snr_valid stays false until a reading actually arrives, so the UI
     * can say "--" rather than a 0.0 dB that reads as "they hear us at zero". */
    int      peer_snr_x10;
    bool     peer_snr_valid;

    int      local_snr_x10;           /* local RX SNR EMA * 10 — host display
                                       * only (from decoded DATA frames); not
                                       * used for mode control                 */

    /* --- Data bookkeeping --- */
    int      pending_burst_frames;     /* frames accumulated for current PTT burst;
                                        * written by cb_send_tx_frame under g_sess_lock
                                        * (replaces the former function-static so that
                                        * the cmd-bridge SEND_CQ path and the FSM event
                                        * loop do not race on a shared static) */

    /* --- Connection lifecycle --- */
    bool     listen_enabled;           /* app listen intent (LISTEN ON/OFF):
                                        * selects the post-call idle status,
                                        * LISTENING vs DISCONNECTED, so an ARQ
                                        * call always returns to where it was   */

    /* --- Teardown flags --- */
    bool     deferred_listen_off;      /* LISTEN OFF received during grace period;
                                        * will be honoured once the grace expires    */
    bool     pending_disconnect_notify;/* defer notify_disconnected until TX done */
    bool     pending_disconnect;       /* APP_DISCONNECT deferred until TX buf empty */
    uint64_t disconnect_deadline_ms;   /* absolute time by which a deferred
                                        * APP_DISCONNECT must resolve into a
                                        * clean teardown. 0 = none armed. */

    /* --- Initial connect guard --- */
    bool     pending_connect_confirm;  /* caller must ACK ACCEPT when no initial
                                        * DATA is queued, otherwise callee stays
                                        * in ACCEPTING waiting for first traffic */
    uint64_t confirm_listen_until_ms;  /* answerer only: run the pattern
                                        * correlator until this instant, waiting
                                        * for the caller's connect confirm.
                                        * 0 = do not run it.  Bounded on purpose;
                                        * see ARQ_CONNECT_CONFIRM_LISTEN_MS      */
    bool     need_initial_guard;       /* ISS must apply channel guard before
                                        * first DATA after connect (prevents
                                        * transmitting before IRS resets its
                                        * decoders from TX→RX)               */

    /* --- Retransmit: one retained frame (stop-and-wait, no window/restage) ---
     * Holds the RAW USER bytes of the single outstanding frame (read once from
     * the app ring when the frame is created).  A retransmit re-frames these
     * bytes at the CURRENT payload_mode; a mode drop to a smaller payload sends
     * the head now and retains the tail via a memmove on this one buffer.
     * Sized to the largest user payload (QAM16C2: 1213 - 8 = 1205). */
    uint8_t  tx_frame[1213];
    int      tx_frame_len;             /* valid user bytes in tx_frame (0=none)*/
    uint8_t  tx_frame_seq;             /* seq assigned to the retained frame   */
    bool     tx_frame_present;         /* a frame is outstanding (awaiting ACK)*/
    bool     tx_frame_retx;            /* it was retransmitted at least once   */

    uint64_t last_rx_ms;              /* last successful frame decode time     */
    uint64_t irs_data_wait_ms;        /* when this IRS first had data queued
                                       * with the peer still active; the self-
                                       * promote silence window counts from
                                       * max(last_rx_ms, this) so a peer that is
                                       * about to send gets a full turn to bid  */
    uint64_t last_tx_progress_ms;     /* baseline for the no-progress budget:
                                       * seeded on CONNECTED entry and refreshed
                                       * whenever tx_seq advances.             */

    /* --- Timer mechanism --- */
    uint64_t       deadline_ms;       /* absolute monotonic deadline           */
    arq_event_id_t deadline_event;   /* event to fire when deadline fires     */
} arq_session_t;

/* ======================================================================
 * FSM action callbacks
 *
 * Registered once at startup via arq_fsm_set_callbacks().
 * All callbacks are called from the ARQ event-loop thread.
 * ====================================================================== */

typedef struct
{
    /** Enqueue a complete TX frame to the modem action queue. */
    /* burst_remaining: DATA frames still to follow in the same PTT burst
     * (0 = last/only frame — the bridge enqueues the modem action then).
     * Control frames always pass 0. */
    void (*send_tx_frame)(int packet_type, int mode,
                          size_t frame_size, const uint8_t *frame,
                          int burst_remaining);

    /** Emit a Welch-Costas MFSK pattern ACK (no coded frame).
     *  @param mode         payload FreeDV mode to emit the pattern under
     *  @param pattern_kind arq_pattern_kind_t: ACK or ACK+TURN (break). */
    void (*send_pattern_ack)(int mode, int pattern_kind);

    /** Notify TCP interface that a connection is established.
     *  @param remote_call  the peer (caller) callsign
     *  @param local_call   our dialed callsign/SSID (empty => use primary) */
    void (*notify_connected)(const char *remote_call, const char *local_call);

    /** Notify TCP interface of an incoming call that is pending acceptance.
     *  @param remote_call  the peer (caller) callsign
     *  @param local_call   our dialed callsign/SSID (empty => use primary) */
    void (*notify_pending)(const char *remote_call, const char *local_call);

    /** Notify TCP interface that a pending incoming call did not complete. */
    void (*notify_cancelpending)(void);

    /** Notify TCP interface of disconnection.
     *  @param to_no_client  true = client disconnected too; clear arq_conn. */
    void (*notify_disconnected)(bool to_no_client);

    /** Deliver received data bytes to the TCP data stream. */
    void (*deliver_rx_data)(const uint8_t *data, size_t len);

    /** Return bytes available in the TX buffer. */
    int  (*tx_backlog)(void);

    /** Read up to len bytes from the TX buffer; returns bytes actually read. */
    int  (*tx_read)(uint8_t *buf, size_t len);

    /** Send BUFFER status (bytes remaining) to TCP interface. */
    void (*send_buffer_status)(int backlog_bytes);
} arq_fsm_callbacks_t;

/**
 * @brief Register FSM action callbacks (call once before first dispatch).
 */
void arq_fsm_set_callbacks(const arq_fsm_callbacks_t *cbs);

/**
 * @brief Register timing context for recording (call once at init).
 * @param timing Pointer to the arq_timing_ctx_t to record into.
 */
void arq_fsm_set_timing(arq_timing_ctx_t *timing);

/* ======================================================================
 * FSM public API (implemented in arq_fsm.c)
 * ====================================================================== */

/**
 * @brief Initialise a session structure to DISCONNECTED state.
 * @param sess Session to initialise.
 */
void arq_fsm_init(arq_session_t *sess);

/**
 * @brief Dispatch an event through both FSM levels.
 *
 * Runs transition logic, calls action callbacks, and updates deadlines.
 * Must be called from the single ARQ event-loop thread (no locking inside).
 *
 * @param sess  Active session.
 * @param event Event to process.
 */
void arq_fsm_dispatch(arq_session_t *sess, const arq_event_t *event);

/**
 * @brief Return milliseconds until the next deadline, or INT_MAX if idle.
 *
 * Used by the event loop's blocking wait to set a poll timeout.
 *
 * @param sess Active session.
 * @param now  Current monotonic time in milliseconds.
 * @return Milliseconds to wait (0 = fire immediately, INT_MAX = no deadline).
 */
int arq_fsm_timeout_ms(const arq_session_t *sess, uint64_t now);

/**
 * @brief Human-readable name for a connection state (for log output).
 */
const char *arq_conn_state_name(arq_conn_state_t s);

/**
 * @brief Human-readable name for a data-flow state (for log output).
 */
const char *arq_dflow_state_name(arq_dflow_state_t s);

/**
 * @brief Human-readable name for an event (for log output).
 */
const char *arq_event_name(arq_event_id_t ev);

/* Which carrier the next CALL should key.
 *
 * ONE definition, because this predicate decides two things that must agree:
 * how send_frame() SIZES/pads the frame, and which carrier
 * arq_modem_preferred_tx_mode() actually keys.  When they disagreed, the
 * escalation logged itself and still transmitted a 3.71 s DATAC16 burst -- the
 * frame was merely sized for MFSK.  Keep this the only copy.
 *
 * Counts sends rather than remaining retry slots: with the old
 * `tx_retries_left < RETRY_SLOTS - FAST_SLOTS` form, FAST_SLOTS=1 actually
 * produced TWO DATAC16 bursts, because the first CALL goes out before any
 * retry slot is consumed.
 */
int arq_call_carrier(const arq_session_t *sess);

#endif /* ARQ_FSM_H_ */
