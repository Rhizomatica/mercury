/*
 * ARQ FSM Unit Tests
 *
 * Tests for datalink_arq/arq_fsm.c — state transitions, callback
 * invocations and timeout handling.
 *
 * All 9 arq_fsm_callbacks_t function pointers are faked via FFF.
 * arq_protocol_build_* and arq_timing_* are mocked to isolate FSM logic.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include <stdint.h>

#include "unity.h"
#include "fff.h"

DEFINE_FFF_GLOBALS;

#include "arq_fsm.h"
#include "arq_protocol.h"
#include "virtual_clock.h"   /* time_now_ms(), to stage a busy channel */
#include "freedv/freedv_api.h"
#include "modem_mfsk.h"   /* MERCURY_MODE_MFSK */

/* Provided by arq_test_stubs.c */
extern void mock_set_uptime_ms(uint64_t ms);

/* ---- FFF Fakes for arq_fsm_callbacks_t ---- */

FAKE_VOID_FUNC(fake_send_tx_frame, int, int, size_t, const uint8_t *, int);
FAKE_VOID_FUNC(fake_notify_connected, const char *, const char *);
FAKE_VOID_FUNC(fake_notify_pending, const char *, const char *);
FAKE_VOID_FUNC(fake_notify_cancelpending);
FAKE_VOID_FUNC(fake_notify_disconnected, bool);
FAKE_VOID_FUNC(fake_deliver_rx_data, const uint8_t *, size_t);
FAKE_VALUE_FUNC(int, fake_tx_backlog);
FAKE_VALUE_FUNC(bool, fake_channel_busy);
FAKE_VALUE_FUNC(int, fake_tx_read, uint8_t *, size_t);
FAKE_VOID_FUNC(fake_send_buffer_status, int);

static arq_fsm_callbacks_t test_callbacks = {
    .send_tx_frame       = fake_send_tx_frame,
    .notify_connected    = fake_notify_connected,
    .notify_pending      = fake_notify_pending,
    .notify_cancelpending = fake_notify_cancelpending,
    .notify_disconnected = fake_notify_disconnected,
    .deliver_rx_data     = fake_deliver_rx_data,
    .tx_backlog          = fake_tx_backlog,
    .channel_busy        = fake_channel_busy,
    .tx_read             = fake_tx_read,
    .send_buffer_status  = fake_send_buffer_status,
};

static arq_session_t sess;
static arq_timing_ctx_t timing;

/* ---- Helper: create a minimal event ---- */
static arq_event_t make_event(arq_event_id_t id)
{
    arq_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.id = id;
    return ev;
}

/* ---- setUp / tearDown ---- */

static void fixture_offsets_reset(void);

void setUp(void)
{
    fixture_offsets_reset();
    /* Reset all FFF fakes */
    RESET_FAKE(fake_send_tx_frame);
    RESET_FAKE(fake_notify_connected);
    RESET_FAKE(fake_notify_pending);
    RESET_FAKE(fake_notify_cancelpending);
    RESET_FAKE(fake_notify_disconnected);
    RESET_FAKE(fake_deliver_rx_data);
    RESET_FAKE(fake_tx_backlog);
    RESET_FAKE(fake_channel_busy);
    RESET_FAKE(fake_tx_read);
    RESET_FAKE(fake_send_buffer_status);
    FFF_RESET_HISTORY();

    /* Init session and register callbacks */
    mock_set_uptime_ms(1000);
    arq_conn.my_call_sign[0] = '\0';
    arq_timing_init(&timing);
    arq_fsm_set_timing(&timing);
    arq_fsm_set_callbacks(&test_callbacks);
    arq_fsm_init(&sess);
}

void tearDown(void) { }

/* ---- Connection lifecycle tests ---- */

/* Initial state shall be DISCONNECTED */
void test_init_state_disconnected(void)
{
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTED, sess.conn_state);
}

/* Initial modes: DATAC16 control plane, DATAC15 payload floor */
void test_init_mode_defaults(void)
{
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC16, sess.control_mode);
    /* Sessions now start at the MFSK ladder floor, not DATAC15. */
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK, sess.payload_mode);
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK, sess.peer_tx_mode);
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK, sess.initial_payload_mode);
    TEST_ASSERT_EQUAL_INT(0, sess.speed_level);
}

/* APP_LISTEN transitions to LISTENING */
void test_listen_transitions_to_listening(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_LISTENING, sess.conn_state);
}

/* APP_CONNECT transitions to CALLING */
void test_connect_transitions_to_calling(void)
{
    /* First go to LISTENING */
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    /* Then CONNECT */
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "TEST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);
    /* Remote callsign should be stored */
    TEST_ASSERT_EQUAL_STRING("TEST1", sess.remote_call);
}

/* Incoming CALL from LISTENING transitions to ACCEPTING */
void test_incoming_call_transitions_to_accepting(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x42;
    strncpy(ev.remote_call, "REMOTE1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);
    TEST_ASSERT_EQUAL_UINT8(0x42, sess.session_id);
    /* notify_pending should have been called */
    TEST_ASSERT_GREATER_THAN(0, fake_notify_pending_fake.call_count);
}

/* A station listening on a secondary SSID must attribute an incoming CALL that
 * dialed that secondary to the *dialed* callsign, not the primary — so a
 * multi-SSID station reports the SSID the caller actually reached.  The DST is
 * not carried on the wire (only its CRC16), so arq.c resolves which of our
 * callsigns matched and passes it as ev.local_call; this asserts the FSM stores
 * it and surfaces it to the host on both pending and connected.
 * Regression guard for the "-2 SSID dropped" field report. */
void test_incoming_call_records_dialed_secondary(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x21;
    strncpy(ev.remote_call, "W1ABC", CALLSIGN_MAX_SIZE);
    strncpy(ev.local_call,  "KO0OOO-2", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);

    /* Dialed SSID stored on the session and reported on the pending notice. */
    TEST_ASSERT_EQUAL_STRING("KO0OOO-2", sess.local_call);
    TEST_ASSERT_GREATER_THAN(0, fake_notify_pending_fake.call_count);
    TEST_ASSERT_EQUAL_STRING("KO0OOO-2", fake_notify_pending_fake.arg1_val);

    /* On the caller's first ACK the callee connects and reports the same SSID. */
    ev = make_event(ARQ_EV_RX_ACK);
    ev.session_id = 0x21;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_GREATER_THAN(0, fake_notify_connected_fake.call_count);
    TEST_ASSERT_EQUAL_STRING("KO0OOO-2", fake_notify_connected_fake.arg1_val);
}

/* Helper: drive LISTENING -> ACCEPTING via an incoming CALL. */
static void enter_accepting(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x42;
    strncpy(ev.remote_call, "REMOTE1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);
}

/* An IRS in ACCEPTING must give up (return to LISTENING) after the ACCEPT
 * retry budget is spent, so it does NOT linger long after the caller stops.
 * Regression guard for the field report where the IRS sat in ACCEPTING ~90 s
 * after the ISS gave up — caused by CALL/ACCEPT slots being inflated to the
 * DATA default (10) at startup; connection-setup slots stay short (4). */
void test_accepting_gives_up_after_budget(void)
{
    enter_accepting();

    /* No further RX_CALL: exhaust the ACCEPT retries. */
    for (int i = 0; i < ARQ_ACCEPT_RETRY_SLOTS + 2; i++) {
        arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
        mock_set_uptime_ms(1000 + (uint64_t)(i + 1) * 10000);
        arq_fsm_dispatch(&sess, &ev);
        if (sess.conn_state == ARQ_CONN_LISTENING)
            break;
    }
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_LISTENING, sess.conn_state);
}

/* Exhaust our ACCEPT retries: back to LISTENING through the fallback. */
static void accept_until_fallback(void)
{
    for (int i = 0; i < ARQ_ACCEPT_RETRY_SLOTS + 2; i++) {
        arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
        mock_set_uptime_ms(1000 + (uint64_t)(i + 1) * 10000);
        arq_fsm_dispatch(&sess, &ev);
        if (sess.conn_state == ARQ_CONN_LISTENING)
            break;
    }
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_LISTENING, sess.conn_state);
}

/* The safety net this guards: our ACCEPT retries ran out, but the caller did
 * hear an ACCEPT and is already sending -- its first frame completes the
 * session. */
void test_accept_fallback_completes_on_first_data(void)
{
    enter_accepting();
    accept_until_fallback();

    RESET_FAKE(fake_notify_connected);
    arq_event_t ev = make_event(ARQ_EV_RX_DATA);
    ev.session_id  = 0x42;
    ev.data_bytes  = 8;
    ev.payload_len = 8;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_EQUAL_INT(1, fake_notify_connected_fake.call_count);
}

/* A session that has ENDED is not resurrected by a late frame from its peer.
 *
 * The session id survives a teardown, and the rejoin used to key on it alone:
 * a station that gave up on retries and went back to listening, while its peer
 * never noticed and kept sending, rejoined on the peer's next DATA.  The peer
 * then treated it as one unbroken session while this side had been torn down
 * and reconnected -- its in-flight frame gone, its next bytes spliced onto the
 * peer's stream.  Measured on the two-FSM sim (bidirectional, 10 % loss /
 * NVIS): 90 bytes silently missing from a delivered stream. */
void test_ended_session_is_not_resurrected_by_late_data(void)
{
    enter_accepting();
    arq_event_t ev = make_event(ARQ_EV_RX_DATA);   /* caller's first frame */
    ev.session_id  = 0x42;
    ev.data_bytes  = 8;
    ev.payload_len = 8;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);

    /* The session ends; we are back to listening. */
    ev = make_event(ARQ_EV_RX_DISCONNECT);
    ev.session_id = 0x42;
    arq_fsm_dispatch(&sess, &ev);
    for (int i = 0; i < 4 && sess.conn_state != ARQ_CONN_LISTENING; i++) {
        ev = make_event(ARQ_EV_TX_COMPLETE);
        arq_fsm_dispatch(&sess, &ev);
    }
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_LISTENING, sess.conn_state);

    /* A late frame from that session's peer. */
    RESET_FAKE(fake_notify_connected);
    ev = make_event(ARQ_EV_RX_DATA);
    ev.session_id  = 0x42;
    ev.data_bytes  = 8;
    ev.payload_len = 8;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_CONN_LISTENING, sess.conn_state,
        "an ended session was resurrected by a late DATA frame");
    TEST_ASSERT_EQUAL_INT(0, fake_notify_connected_fake.call_count);
}

/* A fresh RX_CALL while ACCEPTING re-arms the retry budget (so the window
 * stays open while the caller is still calling); the give-up is bounded by
 * the ACCEPT budget measured from the LAST heard CALL. */
void test_accepting_rx_call_rearms_budget(void)
{
    enter_accepting();

    /* Spend the budget down to (but not past) exhaustion. */
    for (int i = 0; i < ARQ_ACCEPT_RETRY_SLOTS; i++) {
        arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
        mock_set_uptime_ms(1000 + (uint64_t)(i + 1) * 10000);
        arq_fsm_dispatch(&sess, &ev);
    }
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);

    /* Caller still calling -> budget re-armed. */
    arq_event_t call = make_event(ARQ_EV_RX_CALL);
    call.session_id = 0x42;
    strncpy(call.remote_call, "REMOTE1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &call);

    /* Survives another near-full round of retries because of the re-arm. */
    for (int i = 0; i < ARQ_ACCEPT_RETRY_SLOTS - 1; i++) {
        arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
        mock_set_uptime_ms(1000 + (uint64_t)(ARQ_ACCEPT_RETRY_SLOTS + i + 2) * 10000);
        arq_fsm_dispatch(&sess, &ev);
    }
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);
}

/* The caller confirms an ACCEPT with a 0.64 s Welch-Costas pattern, and nothing
 * but the pattern correlator can decode one -- so the answerer has to run that
 * correlator or the connect never completes.  It is also expensive (measured
 * 3.5k samp/s consumed against 8k arriving), so it must NOT stay on for the
 * whole ~18 s ACCEPT window, which is what starves the decoders that handle the
 * caller's first data burst.
 *
 * The contract is therefore a bounded window: opened when our ACCEPT leaves the
 * air (TX_COMPLETE), closed on any state change.  Both halves matter -- an
 * always-off window breaks the handshake, an always-on one costs sensitivity
 * exactly where we cannot afford it. */
void test_connect_confirm_listen_window_is_bounded(void)
{
    enter_accepting();

    /* Not yet keyed off: nothing to listen for. */
    TEST_ASSERT_EQUAL_UINT64(0, sess.confirm_listen_until_ms);

    mock_set_uptime_ms(5000);
    arq_event_t done = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &done);

    /* Opened at PTT-OFF, and bounded -- emphatically shorter than the ACCEPT
     * RX window it used to be conflated with. */
    TEST_ASSERT_EQUAL_UINT64(5000 + ARQ_CONNECT_CONFIRM_LISTEN_MS,
                             sess.confirm_listen_until_ms);
    TEST_ASSERT_TRUE(ARQ_CONNECT_CONFIRM_LISTEN_MS < ARQ_ACCEPT_RX_WINDOW_MS);

    /* The confirm arrives; ACCEPTING is over and so is the correlator. */
    arq_event_t ack = make_event(ARQ_EV_RX_ACK);
    arq_fsm_dispatch(&sess, &ack);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_EQUAL_UINT64(0, sess.confirm_listen_until_ms);
}

/* The answerer's ACCEPT retries stay short by default (not the DATA default of
 * 10): it re-arms on every CALL it hears, so the caller's connect budget, not
 * this, is what keeps a connect alive. */
void test_default_accept_slots_are_short(void)
{
    TEST_ASSERT_EQUAL_INT(ARQ_ACCEPT_RETRY_SLOTS_DEFAULT, ARQ_ACCEPT_RETRY_SLOTS);
    TEST_ASSERT_TRUE(ARQ_ACCEPT_RETRY_SLOTS < ARQ_DATA_RETRY_SLOTS_DEFAULT);
}

/* RX_ACCEPT from CALLING transitions to CONNECTED */
void test_accept_transitions_to_connected(void)
{
    /* LISTEN + CONNECT */
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);

    /* Simulate ACCEPT received */
    ev = make_event(ARQ_EV_RX_ACCEPT);
    ev.session_id = sess.session_id;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_GREATER_THAN(0, fake_notify_connected_fake.call_count);
}

/* The listen mode (initial_payload_mode) is an operator setting for the IDLE
 * decoder -- which broadcast frames the station can hear while no session is
 * up.  It must not leak into a session: an inbound CALL answered on a station
 * listening in QAM16C2 must still start its mode ladder at the start rung
 * (arq_mode_ladder[ARQ_LADDER_START_LEVEL] -- DATAC15 on trunk, the MFSK floor
 * here; asserted symbolically so the test means the same on both),
 * or the first data burst of every inbound connection would be sent in a mode
 * the link may not support at all.  This pins the guarantee that makes the
 * MODE control-port command safe to accept while LISTENING. */
void test_listen_mode_does_not_leak_into_inbound_session(void)
{
    sess.initial_payload_mode = FREEDV_MODE_QAM16C2;

    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    /* Entering LISTENING puts the idle decoder on the listen mode. */
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_QAM16C2, sess.peer_tx_mode);

    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x42;
    strncpy(ev.remote_call, "REMOTE1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);

    const int start_rung = arq_mode_ladder[ARQ_LADDER_START_LEVEL];

    /* The caller's first data burst completes the inbound connect. */
    ev = make_event(ARQ_EV_RX_DATA);
    ev.session_id = 0x42;
    ev.mode       = start_rung;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_EQUAL_INT(start_rung, sess.payload_mode);
    TEST_ASSERT_NOT_EQUAL(FREEDV_MODE_QAM16C2, sess.payload_mode);
    /* The listen mode is remembered for the next idle period, not applied. */
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_QAM16C2, sess.initial_payload_mode);
}

/* ...and the converse: once the session ends, the idle decoder goes back to
 * the listen mode rather than being stranded on whatever rung the ladder
 * happened to finish on -- otherwise broadcast RX would silently stop working
 * after every ARQ call. */
void test_listen_mode_restored_after_session_ends(void)
{
    sess.initial_payload_mode = FREEDV_MODE_QAM16C2;

    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_ACCEPT);
    ev.session_id = sess.session_id;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    /* The session starts on the ladder, away from the listen mode. */
    TEST_ASSERT_EQUAL_INT(arq_mode_ladder[ARQ_LADDER_START_LEVEL], sess.peer_tx_mode);

    ev = make_event(ARQ_EV_RX_DISCONNECT);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_QAM16C2, sess.peer_tx_mode);
}

/* ---- CONNECT while the previous session is still tearing down ----
 *
 * DISCONNECTING had no APP_CONNECT case, so a call placed during teardown was
 * silently dropped -- after the control port had already answered OK.  A
 * client that redials promptly waited for a CONNECTED that could never come.
 * The call is now deferred and placed when the teardown completes. */

/* Caller with a live session, then DISCONNECT with nothing queued: the
 * DISCONNECT exchange is on the air and the FSM is in DISCONNECTING. */
static void goto_disconnecting_caller(void)
{
    /* setUp() clears our callsign, and a CALL with an empty SRC cannot be
     * encoded -- no frame would go out and the "CALL was sent" checks below
     * could not pass for any implementation. */
    snprintf(arq_conn.my_call_sign, CALLSIGN_MAX_SIZE, "%s", "AAA");
    arq_conn.bw = ARQ_BANDWIDTH_FULL_HZ;   /* arq_get_bw() stub reads this; 0 is not a bandwidth */
    fake_tx_backlog_fake.return_val = 0;
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_ACCEPT);
    ev.session_id = sess.session_id;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    ev = make_event(ARQ_EV_APP_DISCONNECT);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTING, sess.conn_state);
    RESET_FAKE(fake_send_tx_frame);
    RESET_FAKE(fake_notify_disconnected);
}

static void connect_to(const char *call)
{
    arq_event_t ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, call, CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
}

void test_connect_during_teardown_is_deferred_not_dropped(void)
{
    goto_disconnecting_caller();
    uint8_t old_session = sess.session_id;

    connect_to("DST2");
    /* Not placed over the teardown, but not lost either. */
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTING, sess.conn_state);
    TEST_ASSERT_TRUE_MESSAGE(sess.pending_connect,
        "CONNECT during DISCONNECTING was dropped: the client never gets an answer");

    /* Peer acks the DISCONNECT: teardown done, the deferred call goes out. */
    arq_event_t ev = make_event(ARQ_EV_RX_DISCONNECT);
    ev.session_id = old_session;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);
    TEST_ASSERT_EQUAL_STRING("DST2", sess.remote_call);
    TEST_ASSERT_GREATER_THAN(0, fake_send_tx_frame_fake.call_count);   /* the CALL */
    TEST_ASSERT_FALSE(sess.pending_connect);
    /* The old session's end is not reported after the new CONNECT: it would
     * read as "your call failed". */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_notify_disconnected_fake.call_count,
        "old teardown notified DISCONNECTED after the client's new CONNECT");
}

/* Same, when the teardown ends by DISCONNECT retry exhaustion instead. */
void test_deferred_connect_is_placed_after_teardown_timeout(void)
{
    goto_disconnecting_caller();
    connect_to("DST2");

    sess.tx_retries_left = 0;
    arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);
    TEST_ASSERT_EQUAL_STRING("DST2", sess.remote_call);
    TEST_ASSERT_EQUAL_INT(0, fake_notify_disconnected_fake.call_count);
}

/* A DISCONNECT after the deferred CONNECT cancels it: the station goes idle,
 * and the client gets the ordinary DISCONNECTED. */
void test_disconnect_cancels_deferred_connect(void)
{
    goto_disconnecting_caller();
    uint8_t old_session = sess.session_id;
    connect_to("DST2");

    arq_event_t ev = make_event(ARQ_EV_APP_DISCONNECT);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_FALSE(sess.pending_connect);

    ev = make_event(ARQ_EV_RX_DISCONNECT);
    ev.session_id = old_session;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CALLING, sess.conn_state);
    TEST_ASSERT_EQUAL_INT(1, fake_notify_disconnected_fake.call_count);
}

/* LISTEN OFF means "release the radio", which rules out the deferred call too.
 * The client must be told, or it waits for a CONNECTED that is not coming. */
void test_listen_off_cancels_deferred_connect(void)
{
    goto_disconnecting_caller();
    connect_to("DST2");

    arq_event_t ev = make_event(ARQ_EV_APP_STOP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CALLING, sess.conn_state);
    TEST_ASSERT_FALSE(sess.pending_connect);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_notify_disconnected_fake.call_count,
        "LISTEN OFF dropped the deferred call without telling the client");
}

/* The redial that actually happens.  Right after DISCONNECT the station is
 * usually still CONNECTED, with the disconnect deferred while its last frame
 * waits for an ACK -- not yet in DISCONNECTING.  A fix covering only
 * DISCONNECTING passed the four tests above and still dropped a real redial on
 * the loopback; these pin the path it took. */
/* Defined further down with the other session helpers. */
static void goto_connected(void);
static void goto_wait_ack(void);

static void goto_connected_with_deferred_disconnect(void)
{
    goto_connected();
    goto_wait_ack();
    fake_tx_backlog_fake.return_val = 0;          /* last frame sent, ACK outstanding */
    arq_event_t ev = make_event(ARQ_EV_APP_DISCONNECT);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_TRUE(sess.pending_disconnect);
    RESET_FAKE(fake_notify_disconnected);
}

void test_redial_while_disconnect_deferred_is_placed_after_teardown(void)
{
    goto_connected_with_deferred_disconnect();
    connect_to("DST2");
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_TRUE_MESSAGE(sess.pending_connect,
        "redial during a deferred disconnect was dropped");

    /* The outstanding frame is ACKed: the deferred disconnect proceeds. */
    /* No ack_seq: an in-session ACK on this branch is a Welch-Costas pattern
     * with no header, so it carries no sequence and the FSM never reads one
     * (trunk's version of this test took the seq from its TX window). */
    arq_event_t ev = make_event(ARQ_EV_RX_ACK);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTING, sess.conn_state);
    TEST_ASSERT_TRUE(sess.pending_connect);       /* survives the state change */

    uint8_t old_session = sess.session_id;
    ev = make_event(ARQ_EV_RX_DISCONNECT);
    ev.session_id = old_session;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);
    TEST_ASSERT_EQUAL_STRING("DST2", sess.remote_call);
    TEST_ASSERT_EQUAL_INT(0, fake_notify_disconnected_fake.call_count);
}

/* The peer ends the session first.  Its DISCONNECTED notice waits for our
 * DISCONNECT ack frame to finish transmitting, and so must the deferred CALL:
 * queuing it over that frame would key over our own ack. */
void test_redial_waits_for_peer_disconnect_ack_to_finish(void)
{
    goto_connected_with_deferred_disconnect();
    connect_to("DST2");

    arq_event_t ev = make_event(ARQ_EV_RX_DISCONNECT);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(ARQ_CONN_CALLING, sess.conn_state,
        "CALL placed while our DISCONNECT ack was still on the air");
    TEST_ASSERT_TRUE(sess.pending_connect);

    ev = make_event(ARQ_EV_TX_COMPLETE);          /* the ack has left the air */
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);
    TEST_ASSERT_EQUAL_STRING("DST2", sess.remote_call);
    TEST_ASSERT_EQUAL_INT(0, fake_notify_disconnected_fake.call_count);
}

/* LISTEN OFF on a live link releases the radio: the queued call goes too, and
 * the host is told. */
void test_listen_off_while_connected_cancels_deferred_connect(void)
{
    goto_connected_with_deferred_disconnect();
    connect_to("DST2");

    arq_event_t ev = make_event(ARQ_EV_APP_STOP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CALLING, sess.conn_state);
    TEST_ASSERT_FALSE(sess.pending_connect);
    TEST_ASSERT_EQUAL_INT(1, fake_notify_disconnected_fake.call_count);
}

/* A CONNECT on a live session with no disconnect requested is a host error:
 * it must not be queued to fire when this session ends by itself. */
void test_connect_on_live_session_without_disconnect_is_not_queued(void)
{
    goto_connected();
    connect_to("DST2");
    TEST_ASSERT_FALSE(sess.pending_connect);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
}

/* APP_DISCONNECT from CONNECTED */
void test_disconnect_from_connected(void)
{
    /* Get to CONNECTED state */
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_ACCEPT);
    ev.session_id = sess.session_id;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);

    /* Reset call counts to track disconnect-specific calls */
    RESET_FAKE(fake_send_tx_frame);

    /* Disconnect */
    ev = make_event(ARQ_EV_APP_DISCONNECT);
    arq_fsm_dispatch(&sess, &ev);

    /* Should either go to DISCONNECTING or DISCONNECTED */
    TEST_ASSERT_TRUE(
        sess.conn_state == ARQ_CONN_DISCONNECTING ||
        sess.conn_state == ARQ_CONN_DISCONNECTED
    );
}

/* RX_DISCONNECT transitions to DISCONNECTED */
void test_rx_disconnect_from_connected(void)
{
    /* Get to CONNECTED state */
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_ACCEPT);
    ev.session_id = sess.session_id;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);

    /* RX disconnect */
    ev = make_event(ARQ_EV_RX_DISCONNECT);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTED, sess.conn_state);
}

/* ---- Helper: drive the session to CONNECTED ---- */
static void goto_connected(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_ACCEPT);
    ev.session_id = sess.session_id;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
}

/* Session ID zero is invalid, not a wildcard.  A zero-ID DISCONNECT used to
 * bypass the mismatch check and tear down an unrelated active session. */
void test_connected_rejects_zero_session_id(void)
{
    goto_connected();
    uint64_t last_rx = sess.last_rx_ms;

    mock_set_uptime_ms(5000);
    arq_event_t ev = make_event(ARQ_EV_RX_DISCONNECT);
    ev.session_id = 0;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_EQUAL_UINT64(last_rx, sess.last_rx_ms);
}

/* The other half of the zero-session rule, and the reason it is not a blanket
 * ban.  An in-session ACK is a bare Welch-Costas pattern: it has no header, so
 * it cannot carry a session ID and is posted with zero.  Tightening the gate to
 * reject every zero-ID event -- which is correct for framed events, as the test
 * above shows -- would discard every in-session ACK and stall the data plane
 * with no visible error.  This pins the asymmetry so the two cannot drift. */
void test_connected_accepts_zero_session_pattern_ack(void)
{
    goto_connected();
    mock_set_uptime_ms(5000);
    uint64_t before = sess.last_rx_ms;

    arq_event_t ev = make_event(ARQ_EV_RX_ACK);
    ev.session_id = 0;                 /* what a pattern ACK always carries */
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(before, sess.last_rx_ms,
        "a zero-session pattern ACK was dropped by the session-ID gate");
}

/* A repeated CALL while ACCEPTING is proof our ACCEPT was lost AND that the
 * caller has not started a data burst.  The deadline set at ACCEPT TX_COMPLETE
 * holds the RX window open for that burst -- ~18 s once MFSK is the ladder
 * floor -- so waiting it out drifts our retransmitted ACCEPT into the middle of
 * the caller's next CALL, where it is deaf over its own transmission.  Answer
 * one channel guard later instead, in the gap the caller just opened. */
void test_accepting_reanchors_accept_retry_on_repeated_call(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x42;
    strncpy(ev.remote_call, "REMOTE1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);

    /* Our ACCEPT goes out; the long data-burst window is armed here. */
    mock_set_uptime_ms(1000);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    uint64_t long_window = sess.deadline_ms;
    TEST_ASSERT_GREATER_THAN_UINT64(1000 + ARQ_CHANNEL_GUARD_MS, long_window);

    /* The caller retries CALL: it never heard the ACCEPT. */
    mock_set_uptime_ms(5000);
    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x42;
    strncpy(ev.remote_call, "REMOTE1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(5000 + ARQ_CHANNEL_GUARD_MS, sess.deadline_ms,
        "a repeated CALL must re-anchor the ACCEPT retry to one channel guard "
        "from now, not leave the data-burst window running");
    TEST_ASSERT_EQUAL_INT(ARQ_EV_TIMER_RETRY, sess.deadline_event);
}

/* ---- Disconnect teardown tests (K7EK field regressions) ---- */

/* Entering CONNECTED seeds the no-progress clock so the wall-clock budget
 * always has a baseline even before the first advancing ACK. */
void test_connected_seeds_no_progress_clock(void)
{
    goto_connected();
    TEST_ASSERT_NOT_EQUAL_UINT64(0, sess.last_tx_progress_ms);
}

/* APP_DISCONNECT with unsent TX backlog is deferred (stays CONNECTED) and
 * arms the absolute drain deadline rather than tearing down immediately. */
void test_app_disconnect_defers_with_backlog(void)
{
    goto_connected();
    fake_tx_backlog_fake.return_val = 256;  /* bytes still queued */

    arq_event_t ev = make_event(ARQ_EV_APP_DISCONNECT);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_TRUE(sess.pending_disconnect);
    TEST_ASSERT_NOT_EQUAL_UINT64(0, sess.disconnect_deadline_ms);
}

/* A deferred APP_DISCONNECT that never drains must still tear down once the
 * drain deadline elapses — guarantees the rig is not keyed indefinitely
 * after the host disconnects (the "Mercury kept hanging on" report). */
void test_disconnect_drain_timeout_forces_teardown(void)
{
    goto_connected();
    fake_tx_backlog_fake.return_val = 256;

    arq_event_t ev = make_event(ARQ_EV_APP_DISCONNECT);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);

    /* Advance past the absolute drain budget and feed any CONNECTED event. */
    mock_set_uptime_ms(1000 + (uint64_t)ARQ_DISCONNECT_DRAIN_TIMEOUT_S * 1000 + 1000);
    ev = make_event(ARQ_EV_TIMER_RETRY /* was TIMER_KEEPALIVE; keepalive removed in the FSM rewrite */);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTING, sess.conn_state);
    TEST_ASSERT_FALSE(sess.pending_disconnect);
}

/* tx_read fake that always yields one small frame of data. */
static int tx_read_one_frame(uint8_t *buf, size_t n)
{
    size_t k = (n < 16) ? n : 16;
    memset(buf, 0xA5, k);
    return (int)k;
}

/* Drive one ACK-timeout cycle in WAIT_ACK: the timeout resends (DATA_TX) or,
 * once retries are exhausted, runs the exhaustion branch.  If we land back in
 * DATA_TX, complete the TX so the next call resumes from WAIT_ACK. */
static void wait_ack_timeout_cycle(void)
{
    arq_event_t ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    if (sess.conn_state == ARQ_CONN_CONNECTED &&
        sess.dflow_state == ARQ_DFLOW_DATA_TX)
    {
        ev = make_event(ARQ_EV_TX_COMPLETE);
        arq_fsm_dispatch(&sess, &ev);
    }
}

/* Drive the session into WAIT_ACK with one frame in flight.  The caller side
 * first has to clear the post-accept connect-confirmation (resolved on
 * TX_COMPLETE) before data flows, then send a DATA frame (TIMER_ACK triggers
 * the actual send) and complete it (TX_COMPLETE) to land in WAIT_ACK. */
static void goto_wait_ack(void)
{
    fake_tx_backlog_fake.return_val = 512;
    fake_tx_read_fake.custom_fake   = tx_read_one_frame;

    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);

    for (int i = 0; i < 8 && sess.dflow_state != ARQ_DFLOW_WAIT_ACK; i++)
    {
        if (sess.dflow_state == ARQ_DFLOW_DATA_TX)
        {
            ev = make_event(ARQ_EV_TIMER_ACK);    /* ensure the frame is sent */
            arq_fsm_dispatch(&sess, &ev);
            ev = make_event(ARQ_EV_TX_COMPLETE);  /* DATA_TX -> WAIT_ACK */
            arq_fsm_dispatch(&sess, &ev);
        }
        else
        {
            ev = make_event(ARQ_EV_TX_COMPLETE);  /* advance connect-confirm */
            arq_fsm_dispatch(&sess, &ev);
        }
    }
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);
}

/* The peer's DATA while we wait for our ACK means the peer holds the floor --
 * not that it received our frame.  Taking it as an implicit ACK lost our frame
 * whenever both stations were acting as sender: on the two-FSM sim a whole
 * 8 KB transfer was ACKed frame by frame and not one byte delivered.  We must
 * yield and receive theirs, KEEP ours for resend, and ask for the turn back. */
void test_peer_data_in_wait_ack_is_not_an_ack_of_ours(void)
{
    goto_connected();
    goto_wait_ack();
    TEST_ASSERT_TRUE(sess.tx_frame_present);
    uint16_t off_before = sess.tx_stream_off;

    RESET_FAKE(fake_deliver_rx_data);
    arq_event_t ev = make_event(ARQ_EV_RX_DATA);
    ev.session_id  = sess.session_id;
    ev.stream_off  = sess.rx_stream_hwm;          /* the peer's next bytes */
    ev.data_bytes  = 8;
    ev.payload_len = 8;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_TRUE_MESSAGE(sess.tx_frame_present, "our unACKed frame was retired");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(off_before, sess.tx_stream_off,
        "our stream advanced past a frame the peer never ACKed");
    TEST_ASSERT_EQUAL_INT(1, fake_deliver_rx_data_fake.call_count);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_ACK_TX, sess.dflow_state);

    /* Nothing else queued: the kept frame alone must still claim the turn. */
    fake_tx_backlog_fake.return_val = 0;
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_TRUE_MESSAGE(sess.acktx_had_has_data,
        "the ACK did not ask for the turn back for the kept frame");
}

/* A frame starting past our stream position is proof of a gap, not a
 * duplicate.  ACKing it tells the sender the gap is filled, and the loss
 * becomes permanent and silent. */
void test_data_past_our_position_is_not_acked(void)
{
    enter_accepting();
    arq_event_t ev = make_event(ARQ_EV_RX_DATA);       /* bytes 0..7 */
    ev.session_id  = 0x42;
    ev.stream_off  = 0;
    ev.data_bytes  = 8;
    ev.payload_len = 8;
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
    TEST_ASSERT_EQUAL_UINT16(8, sess.rx_stream_hwm);

    RESET_FAKE(fake_deliver_rx_data);
    ev = make_event(ARQ_EV_RX_DATA);                   /* bytes 100.., a gap */
    ev.session_id  = 0x42;
    ev.stream_off  = 100;
    ev.data_bytes  = 8;
    ev.payload_len = 8;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(0, fake_deliver_rx_data_fake.call_count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_IDLE_IRS, sess.dflow_state,
        "a frame past our stream position was ACKed");
}

/* A pending (deferred) disconnect must not drop the unACKed last frame: the
 * first ACK timeout retries it once (capped), and only the second timeout
 * completes the teardown.  Regression test for the Fix-14 zero-retry abort
 * that dropped the peer's final UUCP hangup packet. */
void test_pending_disconnect_retries_last_frame_before_teardown(void)
{
    goto_connected();
    goto_wait_ack();   /* one frame in flight, backlog still > 0 */

    arq_event_t ev = make_event(ARQ_EV_APP_DISCONNECT);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_TRUE(sess.pending_disconnect);

    /* First ACK timeout: must retransmit the unACKed frame, not abort. */
    unsigned sends_before = fake_send_tx_frame_fake.call_count;
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
    TEST_ASSERT_GREATER_THAN(sends_before, fake_send_tx_frame_fake.call_count);
    TEST_ASSERT_TRUE(sess.pending_disconnect);

    /* Retry exhausted (capped to 1): the next timeout completes the
     * deferred disconnect cleanly. */
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTING, sess.conn_state);
    TEST_ASSERT_FALSE(sess.pending_disconnect);
}

/* APP_DISCONNECT landing in WAIT_ACK with an empty backlog (last frame sent,
 * awaiting its ACK) must defer, not tear down immediately — otherwise the
 * unACKed final frame loses its retry protection whenever the disconnect
 * arrives after PTT-OFF instead of during DATA_TX. */
void test_app_disconnect_defers_in_wait_ack(void)
{
    goto_connected();
    goto_wait_ack();
    fake_tx_backlog_fake.return_val = 0;  /* everything sent, ACK outstanding */

    arq_event_t ev = make_event(ARQ_EV_APP_DISCONNECT);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_TRUE(sess.pending_disconnect);

    /* The capped retry still protects the in-flight frame. */
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
}





/* Retry exhaustion within the no-progress budget persists (stays CONNECTED);
 * once the budget elapses, the next exhaustion tears the link down. */
void test_retry_exhaustion_persists_then_disconnects(void)
{
    goto_connected();
    goto_wait_ack();

    /* Within budget: many ACK timeouts (several full exhaustion rounds) must
     * never disconnect — VARA-style persistence. */
    for (int i = 0; i < 3 * (ARQ_DATA_RETRY_SLOTS + 2); i++)
    {
        wait_ack_timeout_cycle();
        TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    }

    /* Past the no-progress budget: the next exhaustion must disconnect. */
    mock_set_uptime_ms(1000 + (uint64_t)ARQ_NO_PROGRESS_TIMEOUT_S * 1000 + 5000);
    int guard = 0;
    while (sess.conn_state == ARQ_CONN_CONNECTED && guard++ < ARQ_DATA_RETRY_SLOTS + 4)
        wait_ack_timeout_cycle();

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTING, sess.conn_state);
}

/* A CONNECTED baseline at uptime 0 is still valid: the no-progress budget
 * must expire relative to that baseline rather than treating 0 as "unset". */
void test_retry_exhaustion_disconnects_from_zero_uptime_baseline(void)
{
    mock_set_uptime_ms(0);
    goto_connected();
    TEST_ASSERT_EQUAL_UINT64(0, sess.last_tx_progress_ms);
    goto_wait_ack();

    for (int i = 0; i < 3 * (ARQ_DATA_RETRY_SLOTS + 2); i++)
    {
        wait_ack_timeout_cycle();
        TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    }

    mock_set_uptime_ms((uint64_t)ARQ_NO_PROGRESS_TIMEOUT_S * 1000 + 5000);
    int guard = 0;
    while (sess.conn_state == ARQ_CONN_CONNECTED && guard++ < ARQ_DATA_RETRY_SLOTS + 4)
        wait_ack_timeout_cycle();

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTING, sess.conn_state);
}

/* ---- Timeout tests ---- */

/* CALL timeout returns to the pre-call status.  The app had LISTEN enabled
 * before placing the call, so an exhausted call must fall back to LISTENING
 * (not DISCONNECTED) -- the connection status returns to where it was. */
void test_call_timeout(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);

    /* Run out the connect budget */
    for (int i = 0; i < ARQ_CONNECT_TIMEOUT_S / 10 + 2; i++) {
        ev = make_event(ARQ_EV_TIMER_RETRY);
        mock_set_uptime_ms(1000 + (uint64_t)(i + 1) * 10000);
        arq_fsm_dispatch(&sess, &ev);
        if (sess.conn_state == ARQ_CONN_LISTENING)
            break;
    }

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_LISTENING, sess.conn_state);
}

/* CALL timeout with NO listen intent falls back to DISCONNECTED. */
void test_call_timeout_no_listen(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);

    for (int i = 0; i < ARQ_CONNECT_TIMEOUT_S / 10 + 2; i++) {
        ev = make_event(ARQ_EV_TIMER_RETRY);
        mock_set_uptime_ms(1000 + (uint64_t)(i + 1) * 10000);
        arq_fsm_dispatch(&sess, &ev);
        if (sess.conn_state == ARQ_CONN_DISCONNECTED)
            break;
    }

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTED, sess.conn_state);
}

/* STOP_LISTEN returns to DISCONNECTED */
void test_stop_listen(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_LISTENING, sess.conn_state);

    ev = make_event(ARQ_EV_APP_STOP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTED, sess.conn_state);
}

/* FSM timeout_ms returns INT_MAX when idle */
void test_timeout_ms_idle(void)
{
    int ms = arq_fsm_timeout_ms(&sess, 1000);
    /* When DISCONNECTED with no deadline, should return INT_MAX or large value */
    TEST_ASSERT_GREATER_THAN(60000, ms);
}


/* ---- LISTEN OFF releases the radio (VARA semantics, host interlock) ----
 *
 * A host asserting a transmitter interlock or stopping a frequency scan sends
 * LISTEN OFF meaning "release the radio now". Honouring it only in LISTENING
 * left Mercury retrying CALL/ACCEPT on a channel another port had taken —
 * reported from the field as BPQ32 INTERLOCK being ignored, and as a scanner
 * that kept stepping frequencies while Mercury answered a connect request.
 */

void test_listen_off_drops_pending_accept(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x42;
    strncpy(ev.remote_call, "REMOTE1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);

    RESET_FAKE(fake_send_tx_frame);
    RESET_FAKE(fake_notify_cancelpending);

    /* Advance time past the LISTEN OFF grace period so the event is
     * acted on immediately rather than deferred (see ARQ_LISTEN_OFF_GRACE_MS). */
    mock_set_uptime_ms(4000);

    ev = make_event(ARQ_EV_APP_STOP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    /* Must leave ACCEPTING: staying there keeps retrying ACCEPT on the air. */
    TEST_ASSERT_NOT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);
    /* The host must learn the pending connection is gone. */
    TEST_ASSERT_GREATER_THAN(0, fake_notify_cancelpending_fake.call_count);
    /* And listen intent is cleared, so we do not fall back into LISTENING. */
    TEST_ASSERT_FALSE(sess.listen_enabled);
}

void test_listen_off_drops_outgoing_call(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);

    /* Advance time past the LISTEN OFF grace period. */
    mock_set_uptime_ms(4000);

    ev = make_event(ARQ_EV_APP_STOP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_NOT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);
}

/* LISTEN OFF received within ARQ_LISTEN_OFF_GRACE_MS of entering ACCEPTING
 * must be deferred, not acted on immediately — a scanning host (BPQ32) sends
 * LISTEN OFF at dwell expiry and needs time to process the just-sent PENDING
 * and cancel its own timer.  Once the grace expires, the deferred event is
 * honoured on the next TIMER_RETRY. */
void test_listen_off_deferred_within_grace_accepting(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    mock_set_uptime_ms(1010);
    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x42;
    strncpy(ev.remote_call, "REMOTE1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);

    /* LISTEN OFF at 1010 ms (just after state enter at 1010) — inside grace. */
    ev = make_event(ARQ_EV_APP_STOP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    /* Must still be in ACCEPTING: grace deferred the teardown. */
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);
    TEST_ASSERT_TRUE(sess.deferred_listen_off);

    /* Advance past the grace period and fire TIMER_RETRY. */
    mock_set_uptime_ms(4000);
    ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);

    /* Now the deferred LISTEN OFF should have been processed. */
    TEST_ASSERT_NOT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);
    TEST_ASSERT_FALSE(sess.deferred_listen_off);
    TEST_ASSERT_GREATER_THAN(0, fake_notify_cancelpending_fake.call_count);
}

/* CALLING has no grace period: PENDING announces an INCOMING call, so it is
 * never sent while we are the caller and there is no race to protect. The host
 * asked for the radio; it gets it at once. */
void test_listen_off_in_calling_is_immediate(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    mock_set_uptime_ms(1010);
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);

    /* Well inside what used to be the grace window. */
    ev = make_event(ARQ_EV_APP_STOP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    /* Released straight away — not deferred. */
    TEST_ASSERT_NOT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);
    TEST_ASSERT_FALSE(sess.deferred_listen_off);
}

/* The case the grace period is FOR, and the one that decides whether the
 * interlock still means anything: LISTEN OFF arrives during the grace, and the
 * handshake then SUCCEEDS. The release must survive the transition into
 * CONNECTED and be honoured there. Clearing the flag on every state change made
 * a successful answer swallow the host's request entirely — the interlock was
 * respected only when the call failed anyway. */
void test_deferred_listen_off_survives_connect(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    mock_set_uptime_ms(1010);

    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x42;
    strncpy(ev.remote_call, "REMOTE1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);

    /* Host releases the channel inside the grace window: deferred, not acted on. */
    ev = make_event(ARQ_EV_APP_STOP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);
    TEST_ASSERT_TRUE(sess.deferred_listen_off);

    RESET_FAKE(fake_notify_disconnected);

    /* The caller answers our ACCEPT and the session would come up. */
    ev = make_event(ARQ_EV_RX_ACK);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);

    /* We must NOT settle into a session on a channel we were told to release. */
    TEST_ASSERT_NOT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_FALSE(sess.deferred_listen_off);
    TEST_ASSERT_GREATER_THAN(0, fake_notify_disconnected_fake.call_count);
}

void test_listen_off_drops_live_link_without_draining(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_ACCEPT);
    ev.session_id = sess.session_id;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);

    RESET_FAKE(fake_send_tx_frame);

    ev = make_event(ARQ_EV_APP_STOP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    /* Unlike APP_DISCONNECT this must NOT linger in a draining teardown:
     * the radio was requested back, so queued bytes buy no more airtime. */
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTED, sess.conn_state);
    TEST_ASSERT_FALSE(sess.pending_disconnect);
    /* No air-side DISCONNECT frame — that is one more keydown on a channel we
     * were just told to give up; the peer times out instead. */
    TEST_ASSERT_EQUAL_INT(0, fake_send_tx_frame_fake.call_count);
}

/* A pattern ACK (RX_ACK) confirms the single outstanding frame: stop-and-wait
 * has at most one frame in flight, so a heard ACK acks it unambiguously.  The
 * retained frame is cleared and the flow leaves WAIT_ACK. */
void test_wait_ack_pattern_ack_confirms_frame(void)
{
    goto_connected();
    goto_wait_ack();
    TEST_ASSERT_TRUE(sess.tx_frame_present);

    arq_event_t ev = make_event(ARQ_EV_RX_ACK);   /* plain pattern ACK */
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_FALSE(sess.tx_frame_present);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_NOT_EQUAL(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);
}
/* A pattern ACK+TURN (break: HAS_DATA set) confirms the frame AND, when the
 * local side has drained its own backlog, yields the floor to the peer
 * (piggyback turn) -> the ISS becomes IRS.  (With local backlog still present
 * a role tiebreak applies instead; that is covered by the sim's bidirectional
 * test.) */
void test_wait_ack_break_yields_floor(void)
{
    goto_connected();
    goto_wait_ack();
    TEST_ASSERT_TRUE(sess.tx_frame_present);

    /* Local side has no more data to send: the break must hand it the floor. */
    fake_tx_backlog_fake.return_val = 0;

    arq_event_t ev = make_event(ARQ_EV_RX_ACK);
    ev.rx_flags = ARQ_FLAG_HAS_DATA;   /* ACK+TURN break */
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_FALSE(sess.tx_frame_present);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
}
/* A stale RX_ACK with no outstanding frame is ignored (no state churn). */
void test_wait_ack_stale_ack_ignored(void)
{
    goto_connected();
    goto_wait_ack();
    /* Clear the frame with a first ACK, land in an idle ISS/DATA state. */
    arq_event_t ack = make_event(ARQ_EV_RX_ACK);
    arq_fsm_dispatch(&sess, &ack);
    /* A second, spurious ACK must not crash or advance anything odd. */
    arq_fsm_dispatch(&sess, &ack);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
}
/* LISTENING -> ACCEPTING -> CONNECTED as the answerer: IRS role, IDLE_IRS. */
static void goto_connected_irs(void)
{
    enter_accepting();
    arq_event_t ev = make_event(ARQ_EV_RX_ACK);
    ev.session_id = 0x42;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
}
/* Offsets a real sender would use: first sighting of a seq consumes the next
 * `n` bytes of the stream; a repeat of that seq replays the same offset. */
static uint16_t fixture_seq_off[256];
static bool     fixture_seq_seen[256];
static uint16_t fixture_stream_next;

static void fixture_offsets_reset(void)
{
    memset(fixture_seq_seen, 0, sizeof(fixture_seq_seen));
    fixture_stream_next = 0;
}

static uint16_t fixture_offset_for_seq(uint8_t seq, size_t n)
{
    if (!fixture_seq_seen[seq])
    {
        fixture_seq_seen[seq] = true;
        fixture_seq_off[seq]  = fixture_stream_next;
        fixture_stream_next   = (uint16_t)(fixture_stream_next + n);
    }
    return fixture_seq_off[seq];
}

/* In-order DATA frame carrying `n` payload bytes.  HAS_DATA keeps us the IRS
 * across frames (the sender has more to send).  mode is set to what the peer
 * actually sent, but the mirror deliberately ignores it (anticipation, not
 * follow-the-decoded-mode). */
static arq_event_t make_data_event(uint8_t seq, int mode, size_t n)
{
    arq_event_t ev = make_event(ARQ_EV_RX_DATA);
    ev.session_id  = 0x42;
    ev.seq         = seq;
    /* Frames are identified by stream offset now, so the fixture has to model a
     * real sender: offsets accumulate by payload length (these tests feed 90,
     * 54 and 126-byte frames, so seq*n would be wrong), and a RETRANSMISSION of
     * the same seq must carry the same offset -- which is what the duplicate
     * tests depend on. */
    ev.stream_off  = fixture_offset_for_seq(seq, n);
    ev.mode        = mode;
    ev.rx_flags    = ARQ_FLAG_HAS_DATA;
    ev.data_bytes  = n;
    ev.payload_len = n;
    for (size_t i = 0; i < n && i < sizeof(ev.payload); i++)
        ev.payload[i] = (uint8_t)(seq * 17 + i);
    return ev;
}
/* Run the ACK_TX cycle (guard timer -> pattern ACK -> TX complete) back to
 * IDLE_IRS, so the next DATA frame is received in the same state a real IRS is. */
static void complete_ack_tx(void)
{
    arq_event_t ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
}
/* Each clean in-order frame climbs the sender one rung (fast initial ramp);
 * the IRS mirror climbs in lock step so peer_tx_mode names the NEXT burst's
 * mode before it arrives. */
void test_irs_mirror_climbs_with_peer(void)
{
    goto_connected_irs();
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK, sess.peer_tx_mode);

    arq_event_t ev = make_data_event(0, MERCURY_MODE_MFSK, 90);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(arq_mode_ladder[1], sess.peer_tx_mode);
    complete_ack_tx();

    ev = make_data_event(1, FREEDV_MODE_DATAC4, 54);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(arq_mode_ladder[2], sess.peer_tx_mode);
    complete_ack_tx();

    ev = make_data_event(2, FREEDV_MODE_DATAC3, 126);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(arq_mode_ladder[3], sess.peer_tx_mode);    /* level 3 */
}
/* A duplicate frame means our ACK was lost and the sender retried, stepping ITS
 * ladder down — the mirror must step down too so we can decode the retransmit. */
void test_irs_mirror_steps_down_on_duplicate(void)
{
    goto_connected_irs();
    arq_event_t ev = make_data_event(0, MERCURY_MODE_MFSK, 90);
    arq_fsm_dispatch(&sess, &ev);
    complete_ack_tx();
    ev = make_data_event(1, FREEDV_MODE_DATAC4, 54);
    arq_fsm_dispatch(&sess, &ev);
    complete_ack_tx();
    TEST_ASSERT_EQUAL_INT(arq_mode_ladder[2], sess.peer_tx_mode);    /* climbed two rungs */

    /* Duplicate of an already-delivered seq (rx_expected has advanced past it). */
    ev = make_data_event(0, MERCURY_MODE_MFSK, 90);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(arq_mode_ladder[1], sess.peer_tx_mode);    /* stepped down one rung */
}
/* A frame the sender marks as a RETRANSMISSION must not climb the mirror.
 *
 * A lost burst is invisible on the receiving side: the retry is the first copy
 * it ever sees, so without ARQ_FLAG_RETX it scores a clean delivery and CLIMBS
 * while the sender scored a retry and DESCENDED.  Only one payload decoder runs
 * at a time, so that split is total deafness — which loses the next burst and
 * widens the split.  Measured on the 0 dB bench cell: the mirror oscillated
 * 0 -> 1 -> 0 against a sender parked at the floor and only 12 of 23 bursts
 * were ever decoded. */
void test_retx_flagged_frame_does_not_climb_the_mirror(void)
{
    goto_connected_irs();
    arq_event_t ev = make_data_event(0, MERCURY_MODE_MFSK, 90);
    arq_fsm_dispatch(&sess, &ev);
    complete_ack_tx();
    int climbed = sess.rx_speed_level;
    TEST_ASSERT_GREATER_THAN_INT(0, climbed);   /* a clean first copy climbs */

    /* The next frame is NEW to us, but the sender says it is a retransmission:
     * score it the way the sender did, so the two ladders stay in step. */
    ev = make_data_event(1, arq_mode_ladder[sess.rx_speed_level], 22);
    ev.rx_flags |= ARQ_FLAG_RETX;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(climbed, sess.rx_speed_level,
        "mirror climbed on a frame the sender had to retransmit: the sender "
        "stepped down for that frame, so the two ends are now on different "
        "rungs and only one payload decoder is running");
}

/* Listen before self-promotion.
 *
 * This FSM has no TURN_REQ: an IRS holding data takes the floor itself once the
 * peer has been silent for ARQ_IRS_SELFPROMOTE_S.  That fires on a clock, so it
 * can key DATA on top of a burst the peer has just started -- the collision
 * that starts #278's retransmit loop on trunk, arriving here by a different
 * route.  These pin the port of trunk's #280 guard onto that path.
 *
 * Helper: an IRS with backlog whose silence window has fully elapsed, so the
 * next TIMER_PEER_BACKLOG would self-promote if nothing else stopped it. */
static void goto_irs_due_to_selfpromote(void)
{
    goto_connected_irs();
    fake_tx_backlog_fake.return_val = 64;
    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);   /* starts the silence window */
    arq_fsm_dispatch(&sess, &ev);
    mock_set_uptime_ms(time_now_ms() + 120000);           /* well past ARQ_IRS_SELFPROMOTE_S */
    RESET_FAKE(fake_send_tx_frame);
}

/* Control: with a quiet channel the promotion happens.  Without this the three
 * tests below could pass merely because the silence window never elapsed. */
void test_selfpromote_proceeds_on_a_quiet_channel(void)
{
    goto_irs_due_to_selfpromote();
    sess.last_rx_sync_ms = 0;
    arq_event_t ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
    TEST_ASSERT_EQUAL_UINT8(0, sess.turn_req_defer_count);
}

void test_selfpromote_is_not_keyed_over_the_peers_burst(void)
{
    goto_irs_due_to_selfpromote();
    sess.last_rx_sync_ms = time_now_ms();        /* a decoder holds sync right now */
    arq_event_t ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_IDLE_IRS, sess.dflow_state,
        "self-promoted into a burst the peer was transmitting");
    TEST_ASSERT_EQUAL_UINT8(1, sess.turn_req_defer_count);
}

/* Sync cannot see the acquisition window, a mode no decoder is bound to, or a
 * burst too weak to sync on; channel energy can. */
void test_selfpromote_defers_on_channel_energy_without_sync(void)
{
    goto_irs_due_to_selfpromote();
    sess.last_rx_sync_ms = 0;
    fake_channel_busy_fake.return_val = true;
    arq_event_t ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_IDLE_IRS, sess.dflow_state,
        "self-promoted over an un-synced transmission: sync alone is not enough");
}

/* A stuck detector must not mute the station: after the cap it keys anyway. */
void test_selfpromote_deferral_is_bounded(void)
{
    goto_irs_due_to_selfpromote();
    for (int i = 0; i < ARQ_TURN_REQ_DEFER_MAX; i++)
    {
        sess.last_rx_sync_ms = time_now_ms();
        arq_event_t ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
        arq_fsm_dispatch(&sess, &ev);
        TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
    }
    sess.last_rx_sync_ms = time_now_ms();
    arq_event_t ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_DATA_TX, sess.dflow_state,
        "deferral never gave up: the station would never send its backlog");
    TEST_ASSERT_EQUAL_UINT8(0, sess.turn_req_defer_count);
}

/* Reset-on-miss: a full idle hold with no DATA (a lost ACK left us climbed above
 * the sender) steps the mirror down toward the floor so the two ends re-sync. */
void test_irs_mirror_resets_toward_floor_on_silence(void)
{
    goto_connected_irs();
    arq_event_t ev = make_data_event(0, MERCURY_MODE_MFSK, 90);
    arq_fsm_dispatch(&sess, &ev);
    complete_ack_tx();
    TEST_ASSERT_EQUAL_INT(arq_mode_ladder[1], sess.peer_tx_mode);    /* climbed to level 1 */

    /* Idle-hold fires with no reverse backlog and a recent RX (not dead yet). */
    fake_tx_backlog_fake.return_val = 0;
    ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK, sess.peer_tx_mode);     /* stepped back to the floor */
}

/* The CALL retry clock must start when the burst leaves the air, not when it
 * was queued.  A CALL spends ~3.8 s modulating, so anchoring at enqueue spent
 * that airtime out of the 8 s retry interval and fired the retransmission at
 * t~8.0 s — while the peer's ACCEPT was still arriving at t~8.1 s.  That cost
 * a fourth transmission on a channel that had lost nothing (measured: 4.0
 * frames for a 3-frame handshake on a clean link). */
void test_calling_reanchors_retry_on_tx_complete(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);

    mock_set_uptime_ms(1000);
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);
    uint64_t deadline_at_enqueue = sess.deadline_ms;

    /* The burst occupies the channel, then PTT drops. */
    mock_set_uptime_ms(1000 + 3840);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);
    TEST_ASSERT_TRUE_MESSAGE(sess.deadline_ms > deadline_at_enqueue,
        "CALL retry still anchored at enqueue: it will collide with the ACCEPT");
    /* A full retry interval measured from PTT-OFF. */
    TEST_ASSERT_UINT64_WITHIN(50,
        (uint64_t)(1000 + 3840) +
            (uint64_t)(arq_protocol_call_interval_s() * 1000.0f),
        sess.deadline_ms);
}

/* The CALL retry interval must be measured from PTT-OFF, not from the moment
 * the CALL was queued.
 *
 * The CALL occupies ~3.7 s of DATAC16 airtime and the peer cannot start its
 * ACCEPT until our transmitter releases, so anchoring the retry at enqueue
 * spends most of the interval on our own transmission and fires the retry into
 * the arriving ACCEPT.  fsm_accepting already anchors on TX_COMPLETE; this pins
 * the same behaviour for CALLING.
 *
 * Asserts the deadline strictly ADVANCES on TX_COMPLETE -- that is the whole
 * property, and it fails if the handler is absent. */
void test_call_retry_deadline_anchored_to_ptt_off(void)
{
    /* Deterministic stagger ranks us against "TEST1" (we rank first), so the
     * deadline is exact — no random draw. */
    snprintf(arq_conn.my_call_sign, CALLSIGN_MAX_SIZE, "%s", "AAA");

    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "TEST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);

    uint64_t at_enqueue = sess.deadline_ms;

    /* Time passes while the CALL is actually on the air, then PTT drops. */
    mock_set_uptime_ms(1000 + 3700);
    arq_event_t done = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &done);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CALLING, sess.conn_state);
    TEST_ASSERT_TRUE_MESSAGE(sess.deadline_ms > at_enqueue,
        "CALL retry deadline must be re-anchored to PTT-OFF");
    /* And it must be a full interval from PTT-OFF, not a partial remainder. */
    uint64_t ptt_off_ms = (uint64_t)(1000 + 3700);
    uint64_t base_deadline_ms = ptt_off_ms +
                               (uint64_t)arq_protocol_call_interval_s() * 1000ULL;
    TEST_ASSERT_EQUAL_UINT64(base_deadline_ms, sess.deadline_ms);
}

/* The DATA retransmit (ack_timeout_s) is the load-bearing timer for the #217
 * deadlock: two connected stations that submit data simultaneously both
 * transmit, both land in WAIT_ACK, and would retransmit on the same fixed
 * schedule without a stagger.  ack_timeout_s is a lower bound (must cover the
 * peer's ACK), so a deterministic positive stagger only ever delays it. */
void test_wait_ack_deadline_stagger_is_exact(void)
{
    snprintf(arq_conn.my_call_sign, CALLSIGN_MAX_SIZE, "%s", "AAA");

    goto_connected();
    goto_wait_ack();
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);

    const arq_mode_timing_t *tm = arq_protocol_mode_timing(sess.payload_mode);
    uint64_t base = 1000 + (uint64_t)(tm->ack_timeout_s * 1000.0f + 0.5f);
    TEST_ASSERT_EQUAL_UINT64(base, sess.deadline_ms);
}

/* An ACCEPT is only correct one channel guard after a CALL we actually heard:
 * that is the only moment we know the caller has dropped PTT and is listening.
 *
 * The RX window armed at TX_COMPLETE is a LISTENING window, not a retry timer.
 * Firing an ACCEPT when it expires has no phase relationship to the caller and
 * lands inside its next CALL, where a half-duplex radio is deaf.  This is a
 * protocol invariant — the caller cannot hear anything over its own
 * transmission — independent of any measured connect outcome.
 *
 * tx_retries_left is spent whether we transmit or merely wait, so the frame
 * counter is what distinguishes the two. */
void test_accept_is_only_sent_in_answer_to_a_heard_call(void)
{
    enter_accepting();

    /* The CALL that put us here earns one ACCEPT. */
    int sent = (int)fake_send_tx_frame_fake.call_count;
    arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(sent + 1, (int)fake_send_tx_frame_fake.call_count);

    /* Our ACCEPT finishes: the deadline now guards the caller's reply. */
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_FALSE(sess.accept_tx_pending);
    TEST_ASSERT_EQUAL_UINT64(1000 + ARQ_ACCEPT_RX_WINDOW_MS, sess.deadline_ms);

    /* That window expiring means the caller never came back.  Stay off the air
     * -- but keep spending the budget, so a pending call stays bounded. */
    sent = (int)fake_send_tx_frame_fake.call_count;
    int budget = (int)sess.tx_retries_left;
    ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(sent, (int)fake_send_tx_frame_fake.call_count);
    TEST_ASSERT_EQUAL_INT(budget - 1, (int)sess.tx_retries_left);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);

    /* A fresh CALL re-arms both the budget and the right to transmit, and
     * re-anchors the ACCEPT to the gap the caller just opened — one channel
     * guard after NOW, not a leftover RX-window deadline. */
    arq_event_t call = make_event(ARQ_EV_RX_CALL);
    call.session_id = 0x42;
    strncpy(call.remote_call, "REMOTE1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &call);
    TEST_ASSERT_TRUE(sess.accept_tx_pending);
    TEST_ASSERT_EQUAL_UINT64(1000 + ARQ_CHANNEL_GUARD_MS, sess.deadline_ms);

    ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(sent + 1, (int)fake_send_tx_frame_fake.call_count);
}

/* The silent-wait give-up path: after the RX window armed at TX_COMPLETE
 * closes without another CALL, we must hold the pending call open for the
 * remaining budget WITHOUT transmitting again, then return to LISTENING. */
void test_accepting_silent_wait_gives_up_after_budget(void)
{
    enter_accepting();

    /* First (and only) ACCEPT, anchored to the CALL that put us here. */
    arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);
    int sent = (int)fake_send_tx_frame_fake.call_count;
    TEST_ASSERT_EQUAL_INT(1, sent);

    /* ACCEPT finishes; the caller never comes back. */
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_FALSE(sess.accept_tx_pending);

    /* Exhaust the remaining budget through silent waits: every slot must be
     * spent off the air, and the give-up must still arrive. */
    for (int i = 0; i < ARQ_ACCEPT_RETRY_SLOTS + 2; i++) {
        ev = make_event(ARQ_EV_TIMER_RETRY);
        mock_set_uptime_ms(1000 + (uint64_t)(i + 1) * 10000);
        arq_fsm_dispatch(&sess, &ev);
        TEST_ASSERT_EQUAL_INT(sent, (int)fake_send_tx_frame_fake.call_count);
        if (sess.conn_state == ARQ_CONN_LISTENING)
            break;
    }
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_LISTENING, sess.conn_state);
}


/* ---- Connect budget ------------------------------------------------------
 *
 * A caller keeps CALLing for ARQ_CONNECT_TIMEOUT_S.  It used to stop after 5
 * CALLs (~59 s); at -8.8 dB SNR3k under fading that routinely ended inside one
 * long fade, with the answerer decoding a late CALL and ACCEPTing just as the
 * caller hung up.
 */

static void call_and_step(uint64_t *t_ms, uint64_t step_ms)
{
    *t_ms += step_ms;
    mock_set_uptime_ms(*t_ms);
    arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);
}

/* Still calling well past the old ~59 s limit, sending a CALL each interval,
 * right up to the end of the budget. */
void test_call_keeps_trying_for_the_whole_connect_budget(void)
{
    snprintf(arq_conn.my_call_sign, CALLSIGN_MAX_SIZE, "%s", "AAA");
    arq_conn.bw = ARQ_BANDWIDTH_FULL_HZ;
    uint64_t t = 1000;
    mock_set_uptime_ms(t);
    arq_event_t ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);                       /* CALL #1 */

    const uint64_t step = 12000;                        /* ~one DATAC16 CALL cycle */
    const uint64_t end  = 1000 + ARQ_CONNECT_TIMEOUT_S * 1000ULL;
    while (t + step < end)
    {
        call_and_step(&t, step);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_CONN_CALLING, sess.conn_state,
            "gave up before the connect budget was spent");
    }
    TEST_ASSERT_TRUE(t > 60000);                       /* past the old limit */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1 + (int)((end - 1000 - 1) / step), fake_send_tx_frame_fake.call_count,
        "a CALL must go out on every retry while the budget lasts");
}

/* Once the budget is spent the next retry reports the failure instead of
 * keying another CALL. */
void test_call_gives_up_once_the_connect_budget_is_spent(void)
{
    snprintf(arq_conn.my_call_sign, CALLSIGN_MAX_SIZE, "%s", "AAA");
    arq_conn.bw = ARQ_BANDWIDTH_FULL_HZ;
    arq_event_t ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    unsigned sent = fake_send_tx_frame_fake.call_count;

    uint64_t t = 1000 + ARQ_CONNECT_TIMEOUT_S * 1000ULL;   /* budget just spent */
    call_and_step(&t, 0);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTED, sess.conn_state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(sent, fake_send_tx_frame_fake.call_count,
        "a CALL was keyed after the connect budget ran out");
    TEST_ASSERT_EQUAL_INT(1, fake_notify_disconnected_fake.call_count);
}

int main(void)
{
    UNITY_BEGIN();
    /* Connection lifecycle tests */
    RUN_TEST(test_init_state_disconnected);
    RUN_TEST(test_init_mode_defaults);
    RUN_TEST(test_listen_transitions_to_listening);
    RUN_TEST(test_connect_transitions_to_calling);
    RUN_TEST(test_call_retry_deadline_anchored_to_ptt_off);
    RUN_TEST(test_wait_ack_deadline_stagger_is_exact);
    RUN_TEST(test_incoming_call_transitions_to_accepting);
    RUN_TEST(test_accepting_reanchors_accept_retry_on_repeated_call);
    RUN_TEST(test_incoming_call_records_dialed_secondary);
    RUN_TEST(test_accept_transitions_to_connected);
    RUN_TEST(test_listen_mode_does_not_leak_into_inbound_session);
    RUN_TEST(test_listen_mode_restored_after_session_ends);
    RUN_TEST(test_disconnect_from_connected);
    RUN_TEST(test_connect_during_teardown_is_deferred_not_dropped);
    RUN_TEST(test_deferred_connect_is_placed_after_teardown_timeout);
    RUN_TEST(test_disconnect_cancels_deferred_connect);
    RUN_TEST(test_listen_off_cancels_deferred_connect);
    RUN_TEST(test_redial_while_disconnect_deferred_is_placed_after_teardown);
    RUN_TEST(test_redial_waits_for_peer_disconnect_ack_to_finish);
    RUN_TEST(test_listen_off_while_connected_cancels_deferred_connect);
    RUN_TEST(test_connect_on_live_session_without_disconnect_is_not_queued);
    RUN_TEST(test_listen_off_drops_pending_accept);
    RUN_TEST(test_listen_off_drops_outgoing_call);
    RUN_TEST(test_listen_off_deferred_within_grace_accepting);
    RUN_TEST(test_listen_off_in_calling_is_immediate);
    RUN_TEST(test_deferred_listen_off_survives_connect);
    RUN_TEST(test_listen_off_drops_live_link_without_draining);
    RUN_TEST(test_rx_disconnect_from_connected);
    RUN_TEST(test_connected_rejects_zero_session_id);
    RUN_TEST(test_connected_accepts_zero_session_pattern_ack);
    RUN_TEST(test_connected_seeds_no_progress_clock);
    RUN_TEST(test_app_disconnect_defers_with_backlog);
    RUN_TEST(test_peer_data_in_wait_ack_is_not_an_ack_of_ours);
    RUN_TEST(test_data_past_our_position_is_not_acked);
    RUN_TEST(test_pending_disconnect_retries_last_frame_before_teardown);
    RUN_TEST(test_app_disconnect_defers_in_wait_ack);
    RUN_TEST(test_disconnect_drain_timeout_forces_teardown);
    RUN_TEST(test_retry_exhaustion_persists_then_disconnects);
    RUN_TEST(test_retry_exhaustion_disconnects_from_zero_uptime_baseline);
    /* Timeout tests */
    RUN_TEST(test_call_timeout);
    RUN_TEST(test_call_timeout_no_listen);
    RUN_TEST(test_call_keeps_trying_for_the_whole_connect_budget);
    RUN_TEST(test_call_gives_up_once_the_connect_budget_is_spent);
    RUN_TEST(test_accepting_gives_up_after_budget);
    RUN_TEST(test_accept_fallback_completes_on_first_data);
    RUN_TEST(test_ended_session_is_not_resurrected_by_late_data);
    RUN_TEST(test_accepting_rx_call_rearms_budget);
    RUN_TEST(test_connect_confirm_listen_window_is_bounded);
    RUN_TEST(test_default_accept_slots_are_short);
    RUN_TEST(test_stop_listen);
    RUN_TEST(test_timeout_ms_idle);
    RUN_TEST(test_wait_ack_pattern_ack_confirms_frame);
    RUN_TEST(test_wait_ack_stale_ack_ignored);
    RUN_TEST(test_wait_ack_break_yields_floor);
    RUN_TEST(test_irs_mirror_climbs_with_peer);
    RUN_TEST(test_irs_mirror_steps_down_on_duplicate);
    RUN_TEST(test_retx_flagged_frame_does_not_climb_the_mirror);
    RUN_TEST(test_selfpromote_proceeds_on_a_quiet_channel);
    RUN_TEST(test_selfpromote_is_not_keyed_over_the_peers_burst);
    RUN_TEST(test_selfpromote_defers_on_channel_energy_without_sync);
    RUN_TEST(test_selfpromote_deferral_is_bounded);
    RUN_TEST(test_irs_mirror_resets_toward_floor_on_silence);
    RUN_TEST(test_calling_reanchors_retry_on_tx_complete);
    RUN_TEST(test_accept_is_only_sent_in_answer_to_a_heard_call);
    RUN_TEST(test_accepting_silent_wait_gives_up_after_budget);
    return UNITY_END();
}
