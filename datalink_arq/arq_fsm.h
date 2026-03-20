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
    ARQ_DFLOW_IDLE_ISS        =  0, /* ISS: no pending frame; waiting for data   */
    ARQ_DFLOW_DATA_TX         =  1, /* ISS: frame queued/transmitting            */
    ARQ_DFLOW_WAIT_ACK        =  2, /* ISS: PTT-OFF; waiting for peer ACK        */
    ARQ_DFLOW_IDLE_IRS        =  3, /* IRS: waiting for peer data frame          */
    ARQ_DFLOW_DATA_RX         =  4, /* IRS: data frame decoded; ACK pending      */
    ARQ_DFLOW_ACK_TX          =  5, /* IRS: ACK frame being transmitted          */
    ARQ_DFLOW_TURN_REQ_TX     =  6, /* IRS→ISS: TURN_REQ being transmitted       */
    ARQ_DFLOW_TURN_REQ_WAIT   =  7, /* IRS→ISS: waiting for TURN_ACK            */
    ARQ_DFLOW_TURN_ACK_TX     =  8, /* ISS→IRS: TURN_ACK being transmitted       */
    ARQ_DFLOW_MODE_REQ_TX     =  9, /* mode upgrade: MODE_REQ being transmitted  */
    ARQ_DFLOW_MODE_REQ_WAIT   = 10, /* mode upgrade: waiting for MODE_ACK        */
    ARQ_DFLOW_MODE_ACK_TX     = 11, /* mode upgrade: MODE_ACK being transmitted  */
    ARQ_DFLOW_KEEPALIVE_TX    = 12, /* KEEPALIVE being transmitted               */
    ARQ_DFLOW_KEEPALIVE_WAIT  = 13, /* waiting for KEEPALIVE_ACK                 */
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
    ARQ_EV_RX_ACK             =  7,  /* ACK frame decoded             */
    ARQ_EV_RX_DATA            =  8,  /* DATA frame decoded            */
    ARQ_EV_RX_DISCONNECT      =  9,  /* DISCONNECT frame decoded      */
    ARQ_EV_RX_TURN_REQ        = 10,  /* TURN_REQ frame decoded        */
    ARQ_EV_RX_TURN_ACK        = 11,  /* TURN_ACK frame decoded        */
    ARQ_EV_RX_MODE_REQ        = 12,  /* MODE_REQ frame decoded        */
    ARQ_EV_RX_MODE_ACK        = 13,  /* MODE_ACK frame decoded        */
    ARQ_EV_RX_KEEPALIVE       = 14,  /* KEEPALIVE frame decoded       */
    ARQ_EV_RX_KEEPALIVE_ACK   = 15,  /* KEEPALIVE_ACK frame decoded   */

    /* Timer events */
    ARQ_EV_TIMER_RETRY        = 16,  /* retry deadline expired        */
    ARQ_EV_TIMER_TIMEOUT      = 17,  /* session/call timeout expired  */
    ARQ_EV_TIMER_ACK          = 18,  /* ACK wait deadline expired     */
    ARQ_EV_TIMER_PEER_BACKLOG = 19,  /* peer-backlog hold expired     */
    ARQ_EV_TIMER_KEEPALIVE    = 20,  /* keepalive interval expired    */

    /* Modem events */
    ARQ_EV_TX_STARTED         = 21,  /* PTT ON (frame on air)         */
    ARQ_EV_TX_COMPLETE        = 22,  /* PTT OFF (TX finished)         */

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
    uint8_t  rx_flags;        /* ARQ_FLAG_TURN_REQ / HAS_DATA / HAS_SNR bits  */
    int8_t   snr_encoded;     /* as received from frame header                */
    uint16_t ack_delay_raw;   /* as received (10ms units, 0=unknown)          */

    /* Mode negotiation */
    int      mode;            /* requested/applied FreeDV mode                */
    size_t   data_bytes;      /* payload byte count (DATA frames)             */

    /* Received data payload — carried through event queue so the FSM can
     * validate sequence numbers before delivering to the application. */
    uint8_t  payload[512];
    size_t   payload_len;

    /* Local receive SNR at the time the frame was decoded (dB, 0 = unknown).
     * Carried in-band so the FSM can update local_snr_x10 without relying on
     * the cross-thread arq_update_link_metrics() write, which races with the
     * event queue push in modem.c. */
    float    rx_snr;

    /* Call setup */
    char     remote_call[CALLSIGN_MAX_SIZE];
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

    /* --- Sequence numbers --- */
    uint8_t  tx_seq;                   /* next seq we will send                */
    uint8_t  rx_expected;              /* next seq we expect from peer         */

    /* --- Mode / speed --- */
    int      payload_mode;             /* MY data TX mode (ISS); per-direction,
                                        * independent of peer's TX mode        */
    int      control_mode;             /* always FREEDV_MODE_DATAC13           */
    int      initial_payload_mode;     /* startup payload mode (= broadcast RX
                                        * mode); restored on disconnect so the
                                        * payload decoder matches broadcast    */
    int      speed_level;              /* reliability ladder: 0=DATAC4,
                                        * 1=DATAC3, 2=DATAC1                  */
    int      tx_success_count;         /* consecutive clean ACKs (no retry)
                                        * towards ladder step-up               */
    int      mode_upgrade_count;       /* SNR hysteresis counter for upgrade   */
    int      pending_tx_mode;          /* mode requested in MODE_REQ (retry)   */
    int      peer_tx_mode;             /* mode peer last TX'd in = my payload
                                        * RX decoder mode when IRS; updated
                                        * from ev->mode on every DATA frame    */

    /* --- Retry/timeout bookkeeping --- */
    int      tx_retries_left;          /* retries remaining for current frame  */
    uint64_t state_enter_ms;          /* when current conn_state was entered   */
    uint64_t startup_deadline_ms;     /* end of DATAC13-only startup period    */

    /* --- Peer state observed from frames --- */
    bool     peer_has_data;            /* peer's HAS_DATA flag in last frame   */
    bool     acktx_had_has_data;       /* HAS_DATA was set in the last ACK sent */
    int      peer_snr_x10;            /* peer-reported SNR * 10 (integer)     */
    int      local_snr_x10;           /* local SNR EMA * 10                   */
    uint64_t peer_busy_until_ms;      /* remote TX busy guard expiry          */

    /* --- Data bookkeeping --- */
    int      tx_backlog_bytes;         /* bytes pending in TX buffer           */

    /* --- Teardown flags --- */
    bool     disconnect_to_no_client;  /* after disconnect: clear arq_info     */
    bool     pending_disconnect_notify;/* defer notify_disconnected until TX done */
    bool     pending_disconnect;       /* APP_DISCONNECT deferred until TX buf empty */

    /* --- Initial connect guard --- */
    bool     pending_connect_confirm;  /* caller must ACK ACCEPT when no initial
                                        * DATA is queued, otherwise callee stays
                                        * in ACCEPTING waiting for first traffic */
    bool     need_initial_guard;       /* ISS must apply channel guard before
                                        * first DATA after connect (prevents
                                        * transmitting before IRS resets its
                                        * decoders from TX→RX)               */

    /* --- Delivery-feedback safety net --- */
    int      consecutive_retries;      /* consecutive non-clean TX outcomes     */
    uint64_t mode_hold_until_ms;       /* after forced downgrade: don't upgrade
                                        * until this uptime (prevents oscillation
                                        * when stale SNR says "upgrade" but the
                                        * channel can't support it)            */

    /* --- Retransmit buffer --- */
    uint8_t  tx_retransmit_buf[1024];  /* last-sent data frame bytes; must be
                                       * >= max frame: 8 hdr + 502 DATAC1
                                       * payload = 510 bytes (was 256, too
                                       * small → DATAC1 retries consumed fresh
                                       * ring bytes, corrupting byte stream)  */
    int      tx_retransmit_len;       /* 0 = no saved frame                   */
    uint8_t  tx_retransmit_seq;       /* tx_seq the saved frame belongs to    */
    int      tx_inflight_bytes;      /* payload bytes in unACKed frame       */

    /* --- Keepalive tracking --- */
    int      keepalive_miss_count;
    uint64_t last_rx_ms;              /* last successful frame decode time     */

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
    void (*send_tx_frame)(int packet_type, int mode,
                          size_t frame_size, const uint8_t *frame);

    /** Notify TCP interface that a connection is established. */
    void (*notify_connected)(const char *remote_call);

    /** Notify TCP interface of an incoming call that is pending acceptance. */
    void (*notify_pending)(const char *remote_call);

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

#endif /* ARQ_FSM_H_ */
