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
#include "arq_protocol.h"
#include "carousel.h"      /* car_t: the carousel data plane */

/* ======================================================================
 * Level 1 — Connection FSM states
 * ====================================================================== */

typedef enum
{
    ARQ_CONN_DISCONNECTED  = 0, /* no session; idle                              */
    ARQ_CONN_LISTENING     = 1, /* waiting for incoming CALL frame               */
    ARQ_CONN_CALLING       = 2, /* outgoing CALL sent; awaiting ACCEPT           */
    ARQ_CONN_ACCEPTING     = 3, /* ACCEPT sent; awaiting the caller's first frame */
    ARQ_CONN_CONNECTED     = 4, /* the carousel runs the session                 */
    ARQ_CONN_DISCONNECTING = 5, /* DISCONNECT frame being exchanged              */
    ARQ_CONN__COUNT
} arq_conn_state_t;

/* The step a teardown has reached.  Each value belongs to one connection
 * state, and every state change resets it (sess_enter): nothing of one
 * teardown is left for the next session to trip on. */
typedef enum
{
    ARQ_TD_NONE = 0,
    ARQ_TD_LISTEN_OFF,   /* ACCEPTING: a LISTEN OFF inside the grace period,
                          * honoured when it expires or once connected     */
    ARQ_TD_DRAINING,     /* CONNECTED: the application's DISCONNECT waits
                          * for delivery, until teardown_deadline_ms       */
    ARQ_TD_SENT,         /* DISCONNECTING: our DISCONNECT is on the air    */
    ARQ_TD_REPLYING,     /* DISCONNECTED: the reply to the peer's
                          * DISCONNECT is due when the reply guard expires */
    ARQ_TD_NOTIFYING,    /* DISCONNECTED: the reply is out; the host hears
                          * DISCONNECTED when it has finished (TX_COMPLETE),
                          * so data_rx_buffer_arq drains to it first       */
} arq_teardown_t;

/* ======================================================================
 * Events
 * ====================================================================== */

typedef enum
{
    /* Application events (from TCP interface via channel bus) */
    ARQ_EV_APP_LISTEN = 0,          /* LISTEN ON received            */
    ARQ_EV_APP_STOP_LISTEN,         /* LISTEN OFF received           */
    ARQ_EV_APP_CONNECT,             /* CONNECT <dst> received        */
    ARQ_EV_APP_DISCONNECT,          /* DISCONNECT received           */
    ARQ_EV_APP_DATA_READY,          /* TX data available in buffer   */

    /* Radio RX events (from modem worker) */
    ARQ_EV_RX_CALL,                 /* CALL frame decoded            */
    ARQ_EV_RX_ACCEPT,               /* ACCEPT frame decoded          */
    ARQ_EV_RX_DISCONNECT,           /* DISCONNECT frame decoded      */

    /* Timer events */
    ARQ_EV_TIMER_RETRY,             /* CALL/ACCEPT retry, teardown waits */
    ARQ_EV_TIMER_ACK,               /* a reply guard, a DISCONNECT's wait */

    /* Modem events */
    ARQ_EV_TX_STARTED,              /* PTT ON (frame on air)         */
    ARQ_EV_TX_COMPLETE,             /* PTT OFF (TX finished)         */

    /* Carousel data plane */
    ARQ_EV_RX_CAROUSEL,             /* a frame with the session's CRC seed */
    ARQ_EV_TIMER_CAROUSEL,          /* the carousel's next deadline  */
    ARQ_EV_RX_PATTERN,              /* a pattern heard (pattern_break) */

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

    /* RX events: the session id, the carrier the frame came on, and the
     * frame itself (RX_CAROUSEL). */
    uint8_t  session_id;
    int      mode;
    uint8_t  payload[1280];           /* >= largest user payload: QAM16C2
                                       * carries 1213 - 8 = 1205 bytes      */
    size_t   payload_len;

    /* RX_CAROUSEL: the control decoder (DATAC16) produced it; otherwise the
     * payload decoder, in `mode`.  RX_ACCEPT: the rung the callee starts our
     * rounds on (0..6), whether it hears us below the control mode, the SNR
     * it read our CALL at (0: unknown), its session nonce, and the CRC16 of
     * the callsign we called, which the FSM checks. */
    bool     from_control;
    int      bw_hz;               /* RX_CALL / RX_ACCEPT: the bandwidth it advertises */
    int      car_level;
    bool     car_ctl_deaf;
    float    car_call_snr;
    uint16_t car_nonce;
    uint16_t car_called_crc;

    /* RX_CALL / RX_ACCEPT: the peer can only transmit MFSK (its CALL/ACCEPT
     * carried the TX-MFSK-only capability bit). */
    bool     tx_mfsk;

    /* Local receive SNR at the time the frame was decoded (dB, 0 = unknown):
     * a CALL's sets the start rung the ACCEPT names. */
    float    rx_snr;
    /* RX_CAROUSEL: how long ago the frame ended when it was decoded. */
    uint32_t rx_age_ms;

    /* RX_PATTERN: the BREAK kind, else the session ACK. */
    bool     pattern_break;

    /* APP_DISCONNECT from the host's ABORT: drop what is still in flight and
     * go at once, rather than drain it first as DISCONNECT does (#218). */
    bool     abort;

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
    arq_conn_state_t  conn_state;      /* connection state                     */

    /* --- Identifiers --- */
    uint8_t  session_id;               /* random byte chosen by caller         */
    int      call_sends_done;          /* CALLs sent this attempt (escalation) */
    int      call_carrier;             /* the carrier the last CALL went out on */
    int      call_rx_mode;             /* the carrier the CALL we answer came on */
    char     remote_call[CALLSIGN_MAX_SIZE];
    char     local_call[CALLSIGN_MAX_SIZE];  /* our dialed callsign/SSID for an
                                              * accepted incoming CALL (empty =>
                                              * fall back to the primary)        */

    /* --- Mode / speed --- */
    int      payload_mode;             /* MY data TX mode (ISS); per-direction,
                                        * independent of peer's TX mode        */
    int      control_mode;             /* always FREEDV_MODE_DATAC16           */
    int      initial_payload_mode;     /* startup payload mode (= broadcast RX
                                        * mode); restored on disconnect so the
                                        * payload decoder matches broadcast    */
    int      peer_tx_mode;             /* my payload RX decoder's mode: the
                                        * carousel binds it to the peer's
                                        * rung, idle restores the listen mode */

    /* --- Retry/timeout bookkeeping --- */
    int      tx_retries_left;          /* retries remaining for current frame.
                                        * In ACCEPTING this counts TIMER_RETRY
                                        * slots, not transmissions: a slot may be
                                        * spent silently when the RX window closes
                                        * without a CALL (see fsm_accepting).    */
    uint64_t state_enter_ms;          /* when current conn_state was entered   */

    /* When a decoder was last holding sync on an incoming burst, i.e. when the
     * peer was last observed transmitting.  Written by the modem RX thread via
     * arq_update_link_metrics() under g_sess_lock, read by the FSM; used to
     * avoid keying into the peer's frame. */
    uint64_t last_rx_sync_ms;
    /* A decoder caught a frame's preamble: the frame is on the air until
     * this, whatever its sync does meanwhile (arq_note_rx_preamble). */
    uint64_t rx_frame_busy_until_ms;
    uint64_t nav_busy_until_ms;     /* a NAV header heard: the peer's keydown runs until then */
    uint8_t  defer_count;              /* consecutive busy-channel deferrals
                                        * of our DISCONNECT, or our CALL    */
    bool     tx_active;                /* we are keyed (TX_STARTED seen, no
                                        * TX_COMPLETE yet)                   */
    uint64_t last_tx_end_ms;           /* when our last transmission ended  */
    bool     host_released;            /* the application ended this session
                                        * (DISCONNECT/ABORT): nothing more is
                                        * delivered to it until a new one.
                                        * Only APP_DISCONNECT (DISCONNECT,
                                        * ABORT, control client gone) sets
                                        * it: it is the one teardown that stays
                                        * CONNECTED (draining the backlog)
                                        * after the host is told DISCONNECTED.
                                        * Peer DISCONNECT, timeouts and LISTEN
                                        * OFF leave CONNECTED at once, so
                                        * nothing is delivered after them. */
    int      local_snr_x10;           /* local SNR EMA * 10                   */

    /* --- Data bookkeeping --- */
    int      tx_inflight_bytes;        /* bytes the carousel holds unconfirmed */
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

    /* --- Connect handshake --- */
    bool     accept_fallback;          /* LISTENING was entered because our ACCEPT
                                        * retries ran out: the caller may still
                                        * have heard it, so its first carousel
                                        * frame may complete this session.  Only
                                        * then. */
    bool     accept_tx_pending;        /* the pending TIMER_RETRY is an ACCEPT
                                        * answering a CALL we actually heard,
                                        * not the RX-window timer.  Only the
                                        * former may key the transmitter -- see
                                        * fsm_accepting's TIMER_RETRY.        */

    /* --- Teardown --- */
    arq_teardown_t teardown;           /* the step reached (arq_teardown_t)   */
    uint64_t teardown_deadline_ms;     /* when that step gives up: a held
                                        * DISCONNECT tears down regardless
                                        * (CONNECTED), the callee ending stops
                                        * waiting for an answer (DISCONNECTING) */
    char     pending_connect_call[CALLSIGN_MAX_SIZE];
                                       /* a CONNECT that came during the
                                        * teardown, placed once it completes;
                                        * empty = none                       */

    uint64_t last_rx_ms;              /* last frame decoded from the peer      */

    /* --- Carousel data plane (carousel.h) --- *
     * car_active while CONNECTED (and through a callee's ending); deadline_ms
     * then mirrors the carousel's next deadline (ARQ_EV_TIMER_CAROUSEL).  crc_seed rides every carousel frame's CRC16. */
    car_t   *car;                     /* allocated by arq_fsm_init             */
    bool     car_active;
    bool     car_session;           /* this session ran the carousel (until idle) */
    uint16_t crc_seed;
    uint16_t car_nonce;             /* the callee's, in the ACCEPT: part of crc_seed */
    int      car_rx_level;            /* start rung for the peer's direction:
                                       * the SNR measured here                 */
    int      car_tx_level;            /* ...for ours, from the peer (-1: none) */
    bool     car_ctl_deaf;            /* connect: I heard the peer below the control mode */
    bool     car_peer_ctl_deaf;       /* connect: ...and it, me (the CALL came on MFSK) */
    float    car_call_snr;            /* connect: the SNR the callee read the CALL at
                                       * (the callee: its own; the caller: from the
                                       * ACCEPT), 0 unknown */
    float    car_my_snr;              /* connect: the SNR I read the peer's connect
                                       * frame at (the CALL, the ACCEPT), 0 unknown */
    bool     car_peer_tx_mfsk;        /* connect: the peer can only transmit MFSK */
    bool     ctl_floor_at_stop;       /* the carousel's peer_ctl_deaf when it stopped,
                                       * for the DISCONNECT exchange after it */
    int      floor_mode;              /* the session's MFSK floor (arq_floor_mode
                                       * of its bandwidth), from its seed on */
    uint64_t car_last_rx_ms;          /* last carousel frame from the peer     */

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

    /** Is there energy on the channel right now?  Optional (NULL when the
     *  station has no busy detector, which is the default) -- used to avoid
     *  keying over a transmission we cannot decode, which decoder sync alone
     *  cannot see.  See peer_is_transmitting() in arq_fsm.c. */
    bool (*channel_busy)(void);

    /** Carousel: key the radio for a keydown of separate bursts (copied by
     *  the callee), and set the CRC seed the modem's decoders accept (0: plain
     *  frames only, outside a carousel session). */
    void (*send_keydown)(const arq_keydown_t *kd);
    /** Key a pattern (arq_pattern_kind_t) on the air.  Optional. */
    void (*send_pattern)(int pattern_kind);
    void (*set_crc_seed)(uint16_t seed);
    /** The session's MFSK floor (arq_floor_mode), set with its seed: the
     *  geometry patterns are keyed and detected at.  Optional. */
    void (*set_floor_mode)(int mode);
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

/* Free what arq_fsm_init allocated. */
void arq_fsm_release(arq_session_t *sess);

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

/** The carrier the next CALL goes out on: the control mode for the first
 *  ARQ_CALL_FAST_SLOTS, then MFSK and the control mode alternately. */
int arq_call_carrier(const arq_session_t *sess);
/** Whether a pattern can arrive now (the modem's detector runs only then). */
bool arq_fsm_expect_pattern(const arq_session_t *sess, uint64_t now);

/**
 * @brief Human-readable name for a connection state (for log output).
 */
const char *arq_conn_state_name(arq_conn_state_t s);

/**
 * @brief Human-readable name for an event (for log output).
 */
const char *arq_event_name(arq_event_id_t ev);

#endif /* ARQ_FSM_H_ */
