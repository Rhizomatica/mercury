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

void setUp(void)
{
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
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC15, sess.payload_mode);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC15, sess.peer_tx_mode);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC15, sess.initial_payload_mode);
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

/* Connection-setup slots must stay short by default (not the DATA default of
 * 10) so a failed connect and its mirror ACCEPT window give up quickly. */
void test_default_call_accept_slots_are_short(void)
{
    TEST_ASSERT_EQUAL_INT(ARQ_CALL_RETRY_SLOTS_DEFAULT,   ARQ_CALL_RETRY_SLOTS);
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
 * listening in QAM16C2 must still start its mode ladder at the safest rung,
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

    /* The caller's first data burst completes the inbound connect. */
    ev = make_event(ARQ_EV_RX_DATA);
    ev.session_id = 0x42;
    ev.mode       = FREEDV_MODE_DATAC15;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC15, sess.payload_mode);
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
    /* The session climbed away from the listen mode. */
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC15, sess.peer_tx_mode);

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
    arq_event_t ev = make_event(ARQ_EV_RX_ACK);
    ev.session_id = sess.session_id;
    ev.ack_seq    = (uint8_t)(sess.tx_window[0].seq + 1);
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

/* Mode negotiation frames are session-bound too.  They were previously
 * absent from the top-level validation list, allowing another session's
 * MODE_REQ to change the active receiver mode. */
void test_connected_rejects_foreign_mode_request(void)
{
    goto_connected();
    uint64_t last_rx = sess.last_rx_ms;
    int peer_tx_mode = sess.peer_tx_mode;

    mock_set_uptime_ms(5000);
    arq_event_t ev = make_event(ARQ_EV_RX_MODE_REQ);
    ev.session_id = (uint8_t)(sess.session_id + 1);
    ev.mode = FREEDV_MODE_DATAC4;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_EQUAL_INT(peer_tx_mode, sess.peer_tx_mode);
    TEST_ASSERT_EQUAL_UINT64(last_rx, sess.last_rx_ms);
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
    ev = make_event(ARQ_EV_TIMER_KEEPALIVE);
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

/* Cumulative ACK with ack_seq == window base + 1 confirms the single
 * in-flight frame (the K=1 degenerate case of the burst window). */
/* A foreign MODE_REQ arriving in WAIT_ACK is treated as an IMPLICIT ACK: it
 * retires the whole TX window, resets the retry budget and repoints the RX
 * decoder.  That is the state where session-ID validation actually bites, and
 * it is silent DATA LOSS rather than just a mode change.
 *
 * test_connected_rejects_foreign_mode_request above cannot show this: MODE_REQ
 * is only handled inside ARQ_DFLOW_WAIT_ACK, so from an idle CONNECTED session
 * it reaches no handler and that test passes with or without the check. */
void test_wait_ack_rejects_foreign_mode_request(void)
{
    goto_connected();
    goto_wait_ack();
    TEST_ASSERT_EQUAL_INT(1, sess.tx_window_count);
    int peer_tx_mode = sess.peer_tx_mode;

    arq_event_t ev = make_event(ARQ_EV_RX_MODE_REQ);
    ev.session_id = (uint8_t)(sess.session_id + 1);   /* another session */
    ev.mode = FREEDV_MODE_DATAC4;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sess.tx_window_count,
        "a foreign MODE_REQ retired our outstanding frame as an implicit ACK");
    TEST_ASSERT_EQUAL_INT_MESSAGE(peer_tx_mode, sess.peer_tx_mode,
        "a foreign MODE_REQ repointed our payload decoder");
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);
}

void test_wait_ack_cumulative_ack_advances_window(void)
{
    goto_connected();
    goto_wait_ack();
    TEST_ASSERT_EQUAL_INT(1, sess.tx_window_count);

    arq_event_t ev = make_event(ARQ_EV_RX_ACK);
    ev.session_id = sess.session_id;
    ev.ack_seq = (uint8_t)(sess.tx_window[0].seq + 1);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(0, sess.tx_window_count);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_NOT_EQUAL(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);
}

/* A stale ACK (ack_seq == window base — peer still expects our oldest
 * frame) must confirm nothing: stay in WAIT_ACK with the window intact
 * so TIMER_ACK drives the retransmission. */
void test_wait_ack_stale_ack_keeps_window(void)
{
    goto_connected();
    goto_wait_ack();
    TEST_ASSERT_EQUAL_INT(1, sess.tx_window_count);

    arq_event_t ev = make_event(ARQ_EV_RX_ACK);
    ev.session_id = sess.session_id;
    ev.ack_seq = sess.tx_window[0].seq;   /* nothing new received */
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(1, sess.tx_window_count);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);
}

/* Turn coordination in WAIT_ACK, both halves of it.
 *
 * The ISS must not IGNORE a peer's TURN_REQ while awaiting an ACK: the pre-fix
 * bug sat out the full ack-timeout and retransmitted while the peer kept
 * re-sending TURN_REQ -- a mutual stall that hung bidirectional traffic
 * (observed: a 21-min uucp hang).  That is what 11a6a9f fixed.
 *
 * But it must not yield by KEYING either, which is what 11a6a9f did.  The peer
 * asked for the floor because it decoded our DATA, so it is already on its own
 * ARQ_CHANNEL_GUARD_MS timer to ACK that DATA.  Two equal guards from nearly
 * the same instant put the TURN_ACK on top of the ACK; both are lost, the ISS
 * never sees an ACK, retransmits, and the pair collide again every cycle --
 * an indefinite retransmit loop with zero ack_rx in 600 s (issue #278).
 *
 * So the contract is: latch the request, transmit NOTHING, and yield when the
 * ACK arrives.  This test pins the "transmit nothing" half, which is the part
 * that stops the collision. */
void test_wait_ack_turn_req_defers_the_yield_without_keying(void)
{
    goto_connected();
    goto_wait_ack();
    TEST_ASSERT_EQUAL_INT(1, sess.tx_window_count);

    RESET_FAKE(fake_send_tx_frame);

    arq_event_t ev = make_event(ARQ_EV_RX_TURN_REQ);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);

    /* The collision fix: nothing is keyed in response to the TURN_REQ, and we
     * do not leave WAIT_ACK, so the peer's ACK is still being waited for. */
    TEST_ASSERT_EQUAL_INT(0, fake_send_tx_frame_fake.call_count);
    TEST_ASSERT_NOT_EQUAL_INT(ARQ_DFLOW_TURN_ACK_TX, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);
    /* The request is not dropped -- it is recorded, so the yield is owed. */
    TEST_ASSERT_TRUE(sess.peer_turn_req_pending);
    /* Frame still in flight and still ours to retry. */
    TEST_ASSERT_EQUAL_INT(1, sess.tx_window_count);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
}

/* A TURN_REQ in WAIT_ACK usually means our burst never arrived: a peer that
 * decoded it ACKs instead.  Waiting out the whole ACK timeout then left 8-14 s
 * of silence after every lost burst on air, and the eventual retransmission
 * keyed over the peer's TURN_REQ retry.  The deadline is pulled in to about
 * one reply guard plus one control frame -- still keying nothing -- and when it
 * fires we retransmit. */
void test_wait_ack_turn_req_pulls_the_retransmission_in(void)
{
    goto_connected();
    goto_wait_ack();
    uint64_t full = sess.deadline_ms;

    RESET_FAKE(fake_send_tx_frame);
    arq_event_t ev = make_event(ARQ_EV_RX_TURN_REQ);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);

    const arq_mode_timing_t *ctm = arq_protocol_mode_timing(sess.control_mode);
    uint64_t bound = time_now_ms() + ARQ_CHANNEL_GUARD_MS +
                     (uint64_t)(ctm->frame_duration_s * 1000.0f) + ARQ_TURN_REQ_ACK_MARGIN_MS;
    TEST_ASSERT_EQUAL_INT(0, fake_send_tx_frame_fake.call_count);   /* nothing keyed */
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);
    TEST_ASSERT_TRUE_MESSAGE(sess.deadline_ms < full, "still waiting out the full ACK timeout");
    TEST_ASSERT_TRUE(sess.deadline_ms <= bound);
    TEST_ASSERT_EQUAL_INT(ARQ_EV_TIMER_ACK, sess.deadline_event);

    /* No ACK came: the timer retransmits. */
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
}

/* The ACK timeout fires on a clock.  When the burst was lost the peer is often
 * on air right then with a TURN_REQ, and on air the retransmission keyed 1-2 s
 * into it while our own decoder was synced on it -- both lost.  With the peer
 * transmitting, the retransmission waits and nothing is keyed. */
void test_wait_ack_retransmission_is_not_keyed_over_the_peer(void)
{
    goto_connected();
    goto_wait_ack();
    RESET_FAKE(fake_send_tx_frame);

    sess.last_rx_sync_ms = time_now_ms();   /* a frame is arriving right now */
    arq_event_t ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "retransmitted over the peer's transmission");
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(ARQ_EV_TIMER_ACK, sess.deadline_event);
    TEST_ASSERT_TRUE(sess.deadline_ms <= time_now_ms() + ARQ_TURN_REQ_DEFER_MS);
    TEST_ASSERT_EQUAL_INT(ARQ_DATA_RETRY_SLOTS, sess.tx_retries_left);  /* no retry spent */

    /* Channel energy without sync counts too. */
    sess.last_rx_sync_ms = 0;
    fake_channel_busy_fake.return_val = true;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(0, fake_send_tx_frame_fake.call_count);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);

    /* Quiet channel: the retransmission goes. */
    fake_channel_busy_fake.return_val = false;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
    TEST_ASSERT_GREATER_THAN(0, fake_send_tx_frame_fake.call_count);
}

/* Deferring usually lets us decode the peer's TURN_REQ.  It has only just ended
 * then, so the retransmission waits one reply guard for the peer to get back
 * on receive instead of keying on the next deferral step. */
void test_turn_req_decoded_while_deferring_waits_one_guard(void)
{
    goto_connected();
    goto_wait_ack();
    sess.last_rx_sync_ms = time_now_ms();
    arq_event_t ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);

    ev = make_event(ARQ_EV_RX_TURN_REQ);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_TRUE(sess.peer_turn_req_pending);
    TEST_ASSERT_TRUE(sess.deadline_ms >= time_now_ms() + ARQ_CHANNEL_GUARD_MS);
    TEST_ASSERT_EQUAL_INT(ARQ_EV_TIMER_ACK, sess.deadline_event);
}

/* Bounded, like the TURN_REQ deferral: a decoder stuck in false sync must not
 * stop the ISS retransmitting. */
void test_wait_ack_retransmission_deferral_is_bounded(void)
{
    goto_connected();
    goto_wait_ack();
    arq_event_t ev = make_event(ARQ_EV_TIMER_ACK);
    for (int i = 0; i < ARQ_TURN_REQ_DEFER_MAX; i++)
    {
        sess.last_rx_sync_ms = time_now_ms();
        arq_fsm_dispatch(&sess, &ev);
        TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);
    }
    RESET_FAKE(fake_send_tx_frame);
    sess.last_rx_sync_ms = time_now_ms();
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_DATA_TX, sess.dflow_state,
        "deferral never gave up: the ISS would never retransmit");
    TEST_ASSERT_GREATER_THAN(0, fake_send_tx_frame_fake.call_count);
    TEST_ASSERT_EQUAL_UINT8(0, sess.retx_defer_count);
}

/* ...and the other half: the latched request is honoured on the ACK, even when
 * HAS_DATA is clear (the peer's backlog can drain between asking and ACKing --
 * the explicit request is still its stated intent).
 *
 * Yielding only after the ACK is also what stops the second failure in #278: a
 * frame yielded out of WAIT_ACK while still un-acknowledged was neither
 * retried, nor counted as backlog for piggyback/TURN_REQ, and sat in the window
 * forever -- a station stuck with BUFFER=41 for 15 minutes.  Here the window
 * must be empty by the time we give up the floor. */
void test_wait_ack_yields_to_latched_turn_req_when_the_ack_lands(void)
{
    goto_connected();
    goto_wait_ack();
    uint8_t base = sess.tx_window[0].seq;

    arq_event_t ev = make_event(ARQ_EV_RX_TURN_REQ);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_TRUE(sess.peer_turn_req_pending);

    /* ACK confirming our frame, with NO HAS_DATA flag. */
    ev = make_event(ARQ_EV_RX_ACK);
    ev.session_id = sess.session_id;
    ev.ack_seq    = (uint8_t)(base + 1);
    ev.rx_flags   = 0;
    arq_fsm_dispatch(&sess, &ev);

    /* Floor handed over, because the peer asked for it. */
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
    /* Nothing stranded: the frame was confirmed before we yielded. */
    TEST_ASSERT_EQUAL_INT(0, sess.tx_window_count);
    /* Latch consumed, so a later ACK does not yield again spuriously. */
    TEST_ASSERT_FALSE(sess.peer_turn_req_pending);
}

/* Without a pending request and without HAS_DATA, an ACK must leave the ISS
 * holding the floor -- the latch must not make every ACK hand the turn away. */
void test_wait_ack_ack_without_request_keeps_the_turn(void)
{
    goto_connected();
    goto_wait_ack();
    uint8_t base = sess.tx_window[0].seq;

    arq_event_t ev = make_event(ARQ_EV_RX_ACK);
    ev.session_id = sess.session_id;
    ev.ack_seq    = (uint8_t)(base + 1);
    ev.rx_flags   = 0;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_NOT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
}

/* A station waiting for a KEEPALIVE_ACK must accept incoming DATA.
 *
 * KEEPALIVE_WAIT handled RX_KEEPALIVE_ACK, RX_KEEPALIVE and TIMER_RETRY, and
 * nothing else -- so RX_DATA fell through and was DISCARDED.  The sender then
 * retransmitted into a station that would not listen, made no progress, and
 * this side counted keepalive misses until it tore the link down.
 *
 * Reproduced in the two-station sim (tests/sim) at seed 17 with only 10% frame
 * erasure: zero bytes delivered in EITHER direction, the sender cycling
 * DATA_TX -> WAIT_ACK the whole time, and "Keepalive miss limit --
 * disconnecting" at 37 s.  Fixing it took the sim's total-failure count from 2
 * to 0 over 114 bidirectional trials.
 *
 * A keepalive asks whether the peer is still there.  A DATA frame answers that
 * better than a KEEPALIVE_ACK would, so there is nothing left to wait for: take
 * the data and ACK it.
 *
 * Third instance of one class -- a state ignoring an event that can
 * legitimately arrive in it.  WAIT_ACK ignored RX_TURN_REQ (a 21-minute uucp
 * hang), TURN_REQ_WAIT ignored RX_TURN_REQ, and this one ignored RX_DATA.
 */
void test_keepalive_wait_accepts_data(void)
{
    goto_connected();

    /* ISS with nothing queued goes idle, then the keepalive timer fires. */
    fake_tx_backlog_fake.return_val = 0;
    arq_event_t ev = make_event(ARQ_EV_TIMER_KEEPALIVE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_KEEPALIVE_TX, sess.dflow_state);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_KEEPALIVE_WAIT, sess.dflow_state);

    /* The peer answers with DATA rather than a KEEPALIVE_ACK: it is alive and
     * it holds the floor. */
    const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    ev = make_event(ARQ_EV_RX_DATA);
    ev.session_id  = sess.session_id;
    ev.seq         = sess.rx_expected;
    ev.data_bytes  = sizeof(payload);
    ev.payload_len = sizeof(payload);
    memcpy(ev.payload, payload, sizeof(payload));
    unsigned before = fake_deliver_rx_data_fake.call_count;
    arq_fsm_dispatch(&sess, &ev);

    /* Delivered upward, not dropped. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1,
        fake_deliver_rx_data_fake.call_count,
        "DATA arriving in KEEPALIVE_WAIT was discarded");
    /* Keepalive abandoned in favour of ACKing what arrived. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_DATA_RX, sess.dflow_state,
        "must move to DATA_RX to ACK, not keep waiting for a KEEPALIVE_ACK");
    /* A frame is proof of life, so the miss counter must not stand. */
    TEST_ASSERT_EQUAL_INT(0, sess.keepalive_miss_count);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
}

/* Both stations ask for the turn at the same instant.
 *
 * TURN_REQ_WAIT used to have no RX_TURN_REQ case at all, so the event fell
 * through and NEITHER peer yielded.  Both retried to exhaustion, both dropped
 * to IDLE_IRS, and the link sat with no sender until one requested again --
 * immediately and unstaggered, so the requests collided and it repeated.  Both
 * halves of the field report on bidirectional B1F forwarding (long silent
 * gaps, then both stations transmitting over each other) are that one loop.
 *
 * This is the same omission already fixed for WAIT_ACK in
 * test_wait_ack_yields_on_turn_req; TURN_REQ_WAIT was the state it was missed
 * in.  A one-way transfer harness cannot reach either, because its IRS never
 * initiates data.
 *
 * The tie is broken on the retry rank -- the strcmp() of the exchanged
 * callsign pair that #217's stagger already uses -- so both ends compute the
 * same answer and exactly one yields, with no extra exchange.  These two cases
 * assert both sides of that decision from the same state, which is what proves
 * it is a tiebreak and not just "always yield" (which would deadlock the other
 * way) or "never yield" (the original bug).
 */
static void goto_turn_req_wait(void)
{
    /* Connect as the CALLEE, which starts as the receiver -- the caller sends
     * first.  (goto_connected() dials out and would make us the sender.) */
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x42;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);          /* send the ACCEPT */
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_DATA);       /* caller's first burst confirms us */
    ev.session_id = sess.session_id;
    ev.seq = sess.rx_expected;
    ev.data_bytes = 8;
    ev.payload_len = 8;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);          /* our ACK */
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);          /* -> IDLE_IRS */
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);

    fake_tx_backlog_fake.return_val = 256; /* we now have traffic of our own */
    ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);          /* TURN_REQ_TX -> TURN_REQ_WAIT */
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_REQ_WAIT, sess.dflow_state);
}

/* The two crossed requests of issue #282.
 *
 * When both ends have a backlog, the ISS takes the floor and negotiates its
 * mode while the IRS asks for the floor.  Each then sat in its own wait state
 * ignoring the other's frame -- MODE_REQ_WAIT dropped RX_TURN_REQ, TURN_REQ_WAIT
 * dropped RX_MODE_REQ -- and both retried to exhaustion, ~40 s of dead air per
 * turn.  The reporter saw it on every turn of a bidirectional transfer; normal
 * traffic, where one side is idle, never reaches either state.
 *
 * The two sides resolve it asymmetrically, by role rather than by a tiebreak:
 * the ISS already holds the floor and finishes its negotiation, so it defers
 * the peer's request to the MODE_ACK (where handing over costs no
 * transmission); the IRS concedes immediately, exactly as it already concedes
 * to RX_DATA.  Together that settles the turn in one exchange.
 */
static void goto_mode_req_wait(void)
{
    /* Reached directly.  The natural path needs OLLA hysteresis, a peer SNR
     * estimate and the startup window to have elapsed, which would make this a
     * test about mode selection rather than about the crossed requests. */
    goto_connected();
    goto_wait_ack();
    sess.dflow_state    = ARQ_DFLOW_MODE_REQ_WAIT;
    sess.pending_tx_mode = FREEDV_MODE_DATAC3;
    sess.tx_retries_left = ARQ_MODE_REQ_RETRIES;
    sess.peer_turn_req_pending = false;
}

/* ISS: a TURN_REQ crossing our MODE_REQ must be remembered, not dropped, and
 * must not be answered on the air -- the peer is sending our MODE_ACK. */
void test_mode_req_wait_latches_crossed_turn_req(void)
{
    goto_mode_req_wait();
    unsigned sends_before = fake_send_tx_frame_fake.call_count;

    arq_event_t ev = make_event(ARQ_EV_RX_TURN_REQ);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_TRUE_MESSAGE(sess.peer_turn_req_pending,
        "TURN_REQ crossing a MODE_REQ was dropped (issue #282)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_MODE_REQ_WAIT, sess.dflow_state,
        "the negotiation must continue; the yield belongs on the MODE_ACK");
    TEST_ASSERT_EQUAL_INT_MESSAGE(sends_before, fake_send_tx_frame_fake.call_count,
        "nothing may be keyed: the peer is transmitting our MODE_ACK");
}

/* ...and the MODE_ACK then hands the floor over instead of retaining it. */
void test_mode_ack_yields_to_the_latched_turn_req(void)
{
    goto_mode_req_wait();
    arq_event_t ev = make_event(ARQ_EV_RX_TURN_REQ);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);

    ev = make_event(ARQ_EV_RX_MODE_ACK);
    ev.session_id = sess.session_id;
    ev.mode       = FREEDV_MODE_DATAC3;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_IDLE_IRS, sess.dflow_state,
        "the floor must go to the peer that asked for it");
    TEST_ASSERT_FALSE_MESSAGE(sess.peer_turn_req_pending,
        "the latch must be consumed, not left to fire again");
}

/* IRS: a MODE_REQ arriving while our TURN_REQ is outstanding means the peer
 * holds the floor and is not yielding.  Accept its mode and answer. */
void test_turn_req_wait_concedes_to_mode_req(void)
{
    goto_turn_req_wait();
    int my_tx_mode = sess.payload_mode;

    arq_event_t ev = make_event(ARQ_EV_RX_MODE_REQ);
    ev.session_id = sess.session_id;
    ev.mode       = FREEDV_MODE_DATAC3;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_MODE_ACK_TX, sess.dflow_state,
        "MODE_REQ crossing a TURN_REQ was dropped (issue #282)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FREEDV_MODE_DATAC3, sess.peer_tx_mode,
        "the peer's TX mode is what we must decode next");
    TEST_ASSERT_EQUAL_INT_MESSAGE(my_tx_mode, sess.payload_mode,
        "our own TX mode is per-direction and must not change");

    /* The answer goes out and we are the receiver again, with our own backlog
     * still queued -- IDLE_IRS arms the timer that asks for the floor later. */
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
}

/* Get to IDLE_IRS as the receiver, with traffic of our own queued -- the state
 * where TIMER_PEER_BACKLOG decides whether to ask for the floor. */
static void goto_idle_irs_with_backlog(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x42;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_DATA);
    ev.session_id  = sess.session_id;
    ev.seq         = sess.rx_expected;
    ev.data_bytes  = 8;
    ev.payload_len = 8;
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
    fake_tx_backlog_fake.return_val = 256;   /* we have data to send */
}

/* The other half of #278: TIMER_PEER_BACKLOG fires on a clock, with no regard
 * for whether the peer is mid-burst.  The IRS's TURN_REQ therefore landed on
 * top of the ISS's DATA and both were lost -- the collision that STARTS the
 * retransmit loop, as the reporter diagnosed.
 *
 * With a decoder holding sync (the peer is transmitting), the request must be
 * held back and nothing keyed. */
void test_turn_req_is_not_keyed_over_the_peers_burst(void)
{
    goto_idle_irs_with_backlog();
    RESET_FAKE(fake_send_tx_frame);

    sess.last_rx_sync_ms = time_now_ms();   /* a burst is arriving right now */

    arq_event_t ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "keyed a TURN_REQ while the peer was transmitting");
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
    TEST_ASSERT_EQUAL_UINT8(1, sess.turn_req_defer_count);
}

/* The same for new application data: it can arrive mid-burst, and on air the
 * TURN_REQ it triggered keyed 3.5 s into the peer's DATAC1 frame. */
void test_turn_req_for_new_data_is_not_keyed_over_the_peers_burst(void)
{
    goto_idle_irs_with_backlog();
    RESET_FAKE(fake_send_tx_frame);

    sess.last_rx_sync_ms = time_now_ms();
    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "keyed a TURN_REQ for new data while the peer was transmitting");
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(ARQ_EV_TIMER_PEER_BACKLOG, sess.deadline_event);
    TEST_ASSERT_TRUE(sess.deadline_ms <= time_now_ms() + ARQ_TURN_REQ_DEFER_MS);

    /* Quiet channel: new data asks for the floor straight away. */
    sess.last_rx_sync_ms = 0;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_REQ_TX, sess.dflow_state);
    TEST_ASSERT_GREATER_THAN(0, fake_send_tx_frame_fake.call_count);
}

/* ...and once the channel is quiet the request goes out, so the deferral is a
 * wait and not a mute. */
void test_turn_req_is_sent_once_the_channel_is_quiet(void)
{
    goto_idle_irs_with_backlog();
    RESET_FAKE(fake_send_tx_frame);

    sess.last_rx_sync_ms = 0;               /* nothing has been heard */

    arq_event_t ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_REQ_TX, sess.dflow_state);
    TEST_ASSERT_GREATER_THAN(0, fake_send_tx_frame_fake.call_count);
}

/* The deferral must be bounded.  False sync is real -- a payload decoder can
 * latch onto a control burst -- and a decoder stuck in sync would otherwise
 * hold the turn request off forever, which trades #278's loop for a silent
 * station that never sends its backlog.  After the cap we ask anyway. */
/* The case decoder sync CANNOT see: the peer is transmitting but nothing has
 * synced on it.  That is not a corner case -- it covers the acquisition window
 * at the start of every burst, a mode neither decoder is bound to, and any
 * burst too weak to sync on, which is precisely the fringe where a collision
 * costs most.  Energy on the channel is the only signal left, so the busy
 * detector must be honoured independently of sync. */
void test_turn_req_defers_on_channel_energy_without_sync(void)
{
    goto_idle_irs_with_backlog();
    RESET_FAKE(fake_send_tx_frame);

    sess.last_rx_sync_ms = 0;                    /* nothing ever synced */
    fake_channel_busy_fake.return_val = true;    /* but the channel is occupied */

    arq_event_t ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "keyed over an un-synced transmission: sync alone is not enough");
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
}

void test_turn_req_deferral_is_bounded(void)
{
    goto_idle_irs_with_backlog();

    for (int i = 0; i < ARQ_TURN_REQ_DEFER_MAX; i++)
    {
        sess.last_rx_sync_ms = time_now_ms();   /* permanently "busy" */
        arq_event_t ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
        arq_fsm_dispatch(&sess, &ev);
        TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
    }
    TEST_ASSERT_EQUAL_UINT8(ARQ_TURN_REQ_DEFER_MAX, sess.turn_req_defer_count);

    /* Cap reached: the next fire requests the floor even though sync persists. */
    RESET_FAKE(fake_send_tx_frame);
    sess.last_rx_sync_ms = time_now_ms();
    arq_event_t ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_TURN_REQ_TX, sess.dflow_state,
        "deferral never gave up: the station would never send its backlog");
    TEST_ASSERT_GREATER_THAN(0, fake_send_tx_frame_fake.call_count);
    TEST_ASSERT_EQUAL_UINT8(0, sess.turn_req_defer_count);
}

/* The retry must listen as well.  On air (1.9.14, 7.050 MHz) the ISS never
 * heard the first TURN_REQ, retransmitted a 7.4 s DATAC17 burst, and the
 * TURN_REQ_WAIT retry -- on a fixed 10 s clock -- keyed over its last second.
 * The first request had the check; the retry did not. */
static void goto_turn_req_wait_after_first_request(void)
{
    goto_idle_irs_with_backlog();
    sess.last_rx_sync_ms = 0;
    arq_event_t ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_REQ_WAIT, sess.dflow_state);
}

void test_turn_req_retry_is_not_keyed_over_the_peers_burst(void)
{
    goto_turn_req_wait_after_first_request();
    uint8_t retries = sess.tx_retries_left;
    RESET_FAKE(fake_send_tx_frame);

    sess.last_rx_sync_ms = time_now_ms();   /* the peer's retransmission */
    arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "retried a TURN_REQ on top of the peer's burst");
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_REQ_WAIT, sess.dflow_state);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(retries, sess.tx_retries_left,
        "waiting for a quiet channel spent a retry");

    /* Burst over: the retry goes out and only now costs a retry. */
    sess.last_rx_sync_ms = 0;
    ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_REQ_TX, sess.dflow_state);
    TEST_ASSERT_GREATER_THAN(0, fake_send_tx_frame_fake.call_count);
    TEST_ASSERT_EQUAL_UINT8(retries - 1, sess.tx_retries_left);
    TEST_ASSERT_EQUAL_UINT8(0, sess.turn_req_defer_count);
}

/* Energy alone must hold the retry, as it holds the first request. */
void test_turn_req_retry_defers_on_channel_energy_without_sync(void)
{
    goto_turn_req_wait_after_first_request();
    RESET_FAKE(fake_send_tx_frame);

    fake_channel_busy_fake.return_val = true;
    arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(0, fake_send_tx_frame_fake.call_count);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_REQ_WAIT, sess.dflow_state);
}

/* Bounded like the first request: a stuck detector cannot hold it forever. */
void test_turn_req_retry_deferral_is_bounded(void)
{
    goto_turn_req_wait_after_first_request();

    for (int i = 0; i < ARQ_TURN_REQ_DEFER_MAX; i++)
    {
        sess.last_rx_sync_ms = time_now_ms();
        arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
        arq_fsm_dispatch(&sess, &ev);
        TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_REQ_WAIT, sess.dflow_state);
    }

    RESET_FAKE(fake_send_tx_frame);
    sess.last_rx_sync_ms = time_now_ms();
    arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_TURN_REQ_TX, sess.dflow_state,
        "retry deferral never gave up");
    TEST_ASSERT_GREATER_THAN(0, fake_send_tx_frame_fake.call_count);
}

/* A TURN_REQ that ends some other way must not shorten the next wait's
 * deferral budget: here a retry is deferred, then the peer's DATA arrives and
 * we concede, which lands back in IDLE_IRS through ACK_TX.  The next request
 * must start from a zero count (enter_idle_irs resets it). */
void test_turn_req_concede_resets_the_deferral_budget(void)
{
    goto_turn_req_wait_after_first_request();

    sess.last_rx_sync_ms = time_now_ms();   /* the peer's burst */
    arq_event_t ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_REQ_WAIT, sess.dflow_state);
    TEST_ASSERT_EQUAL_UINT8(1, sess.turn_req_defer_count);

    /* The burst decodes: DATA instead of TURN_ACK, so we concede.  It is a
     * retransmission (the peer missed our last ACK), so the peer keeps the
     * floor and we go back to IDLE_IRS after ACKing it. */
    ev = make_event(ARQ_EV_RX_DATA);
    ev.session_id  = sess.session_id;
    ev.seq         = (uint8_t)(sess.rx_expected - 1);
    ev.data_bytes  = 8;
    ev.payload_len = 8;
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, sess.turn_req_defer_count,
        "a conceded TURN_REQ left its deferrals charged to the next wait");
}

/* ---- Nothing reaches an application that has ended its session ----
 *
 * DISCONNECT is answered with DISCONNECTED at once (VARA semantics) while the
 * air side drains, so data arriving during the drain has no reader.  It used
 * to be delivered anyway, parked in the receive buffer, and handed to the NEXT
 * client: on air, a frame decoded during a drain reached a new NNCP session
 * 86 ms after its CONNECTED and killed it. */
void test_no_delivery_after_the_application_disconnects(void)
{
    goto_idle_irs_with_backlog();            /* connected callee, 256 B of its own */
    arq_event_t ev = make_event(ARQ_EV_APP_DISCONNECT);
    arq_fsm_dispatch(&sess, &ev);            /* deferred: backlog still queued */
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);

    RESET_FAKE(fake_deliver_rx_data);
    ev = make_event(ARQ_EV_RX_DATA);
    ev.session_id  = sess.session_id;
    ev.seq         = sess.rx_expected;
    ev.data_bytes  = 8;
    ev.payload_len = 8;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_deliver_rx_data_fake.call_count,
        "data delivered to an application that had already disconnected");
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_DATA_RX, sess.dflow_state,
        "no ACK pending for the frame: the peer could not finish");
}

/* ...and the next session delivers again. */
void test_new_session_delivers_again(void)
{
    sess.host_released = true;               /* left over from a previous session */
    RESET_FAKE(fake_deliver_rx_data);
    goto_idle_irs_with_backlog();            /* a fresh inbound session */
    TEST_ASSERT_FALSE(sess.host_released);
    TEST_ASSERT_GREATER_THAN(0, fake_deliver_rx_data_fake.call_count);
}

/* ---- Bench run 12: a DISCONNECT right behind a TURN_ACK ----
 *
 * The host hung up while our TURN_ACK was on the air.  The DISCONNECT keyed
 * 42 ms after the TURN_ACK ended, the peer keyed its first DATA 0.9 s later
 * (it held the TURN_ACK and never listened), and both were lost. */

/* Connected caller, confirm ACK sent, idle as ISS with nothing queued. */
static void goto_idle_iss(void)
{
    goto_connected();
    fake_tx_backlog_fake.return_val = 0;
    arq_event_t ev;
    for (int i = 0; i < 4 && sess.dflow_state != ARQ_DFLOW_IDLE_ISS; i++)
    {
        ev = make_event(ARQ_EV_TIMER_ACK);
        arq_fsm_dispatch(&sess, &ev);
        ev = make_event(ARQ_EV_TX_COMPLETE);
        arq_fsm_dispatch(&sess, &ev);
    }
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_ISS, sess.dflow_state);
}

/* The peer asks an idle ISS for the floor; we key the TURN_ACK and the host
 * hangs up while it is on the air. */
static void goto_disconnect_during_turn_ack(void)
{
    mock_set_uptime_ms(100000);      /* room to place events in the past */
    goto_idle_iss();
    arq_event_t ev = make_event(ARQ_EV_RX_TURN_REQ);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_ACK_TX, sess.dflow_state);
    ev = make_event(ARQ_EV_TIMER_ACK);             /* TURN_ACK queued */
    arq_fsm_dispatch(&sess, &ev);
    sess.last_tx_end_ms = time_now_ms() - 60000;   /* our previous frame: long
                                                     * ago, as on air (5 s) */
    ev = make_event(ARQ_EV_TX_STARTED);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_APP_DISCONNECT);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTING, sess.conn_state);
    RESET_FAKE(fake_send_tx_frame);
}

void test_disconnect_waits_for_the_reply_its_turn_ack_invited(void)
{
    goto_disconnect_during_turn_ack();
    sess.last_rx_sync_ms = 0;
    arq_event_t timer = make_event(ARQ_EV_TIMER_ACK);

    /* Guard fires while the TURN_ACK is still on the air: nothing queued
     * behind it. */
    arq_fsm_dispatch(&sess, &timer);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "DISCONNECT queued behind our own TURN_ACK");

    /* TURN_ACK over.  The peer's DATA has not started yet, so there is nothing
     * to hear -- the reply window must hold the DISCONNECT anyway. */
    arq_event_t done = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &done);
    arq_fsm_dispatch(&sess, &timer);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "DISCONNECT keyed inside the reply window of our TURN_ACK");
    TEST_ASSERT_TRUE(sess.deadline_ms > time_now_ms() + ARQ_ISS_POST_ACK_GUARD_MS);

    /* The invited DATA arrives: still nothing keyed. */
    sess.last_tx_end_ms  = time_now_ms() - 60000;
    sess.last_rx_sync_ms = time_now_ms();
    arq_fsm_dispatch(&sess, &timer);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "DISCONNECT keyed over the peer's DATA");

    /* The DATA ended one reply guard ago: now the DISCONNECT goes. */
    sess.last_rx_sync_ms = time_now_ms() - ARQ_CHANNEL_GUARD_MS - 1;
    arq_fsm_dispatch(&sess, &timer);
    TEST_ASSERT_EQUAL_INT(1, fake_send_tx_frame_fake.call_count);
    TEST_ASSERT_EQUAL_INT(ARQ_EV_TIMER_RETRY, sess.deadline_event);
}

/* The retries listen too: in run 12 the retry clipped the tail of the DATA. */
void test_disconnect_retry_is_not_keyed_over_the_peer(void)
{
    goto_disconnect_during_turn_ack();
    arq_event_t done = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &done);
    sess.last_tx_end_ms  = time_now_ms() - 60000;
    sess.last_rx_sync_ms = 0;
    arq_event_t ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);                   /* initial DISCONNECT */
    TEST_ASSERT_EQUAL_INT(1, fake_send_tx_frame_fake.call_count);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    uint8_t retries = sess.tx_retries_left;

    sess.last_tx_end_ms  = time_now_ms() - 60000;
    sess.last_rx_sync_ms = time_now_ms();           /* peer on the air */
    ev = make_event(ARQ_EV_TIMER_RETRY);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_send_tx_frame_fake.call_count,
        "DISCONNECT retry keyed over the peer");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(retries, sess.tx_retries_left,
        "waiting for the channel spent a DISCONNECT retry");
}

/* Bounded: a decoder stuck in false sync must not stop the teardown. */
void test_disconnect_deferral_is_bounded(void)
{
    goto_disconnect_during_turn_ack();
    arq_event_t done = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &done);
    arq_event_t timer = make_event(ARQ_EV_TIMER_ACK);
    for (int i = 0; i < ARQ_TURN_REQ_DEFER_MAX; i++)
    {
        sess.last_rx_sync_ms = time_now_ms();
        arq_fsm_dispatch(&sess, &timer);
    }
    TEST_ASSERT_EQUAL_INT(0, fake_send_tx_frame_fake.call_count);
    sess.last_rx_sync_ms = time_now_ms();
    arq_fsm_dispatch(&sess, &timer);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_send_tx_frame_fake.call_count,
        "the DISCONNECT deferral never gave up");
}

/* Bench run 14: the peer's DISCONNECT decoded 186 ms before its TX_COMPLETE
 * (a frame decodes before its tail has played out), our reply keyed 40 ms
 * later and clipped it.  The reply waits one reply guard, like every other
 * answer. */
void test_disconnect_reply_waits_one_reply_guard(void)
{
    goto_idle_iss();
    RESET_FAKE(fake_send_tx_frame);
    RESET_FAKE(fake_notify_disconnected);
    arq_event_t ev = make_event(ARQ_EV_RX_DISCONNECT);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "replied to the DISCONNECT without a guard");
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTED, sess.conn_state);
    TEST_ASSERT_EQUAL_INT(ARQ_EV_TIMER_ACK, sess.deadline_event);
    TEST_ASSERT_TRUE(sess.deadline_ms >= time_now_ms() + ARQ_CHANNEL_GUARD_MS);

    /* LISTEN ON inside the guard must not drop the owed reply. */
    ev = make_event(ARQ_EV_APP_LISTEN);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_EV_TIMER_ACK, sess.deadline_event);

    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(1, fake_send_tx_frame_fake.call_count);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_GREATER_THAN(0, fake_notify_disconnected_fake.call_count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_CONN_LISTENING, sess.conn_state,
        "the listen intent given during the guard was lost");
}

/* The other half of run 12: holding the TURN_ACK, the new ISS keyed its first
 * DATA without listening, 0.83 s into the peer's DISCONNECT. */
void test_first_data_after_turn_ack_listens(void)
{
    goto_turn_req_wait_after_first_request();
    fake_tx_read_fake.custom_fake = tx_read_one_frame;
    arq_event_t ev = make_event(ARQ_EV_RX_TURN_ACK);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
    RESET_FAKE(fake_send_tx_frame);

    sess.last_rx_sync_ms = time_now_ms();           /* the peer is on the air */
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "first DATA keyed over the peer");
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
    TEST_ASSERT_TRUE(sess.deadline_ms <= time_now_ms() + ARQ_TURN_REQ_DEFER_MS);

    sess.last_rx_sync_ms = 0;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_GREATER_THAN(0, fake_send_tx_frame_fake.call_count);
}

/* New application data while an idle ISS hears the peer: listen first. */
void test_new_data_at_idle_iss_listens(void)
{
    goto_idle_iss();
    sess.need_initial_guard = false;
    fake_tx_backlog_fake.return_val = 512;
    fake_tx_read_fake.custom_fake   = tx_read_one_frame;
    RESET_FAKE(fake_send_tx_frame);

    sess.last_rx_sync_ms = time_now_ms();
    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "new data keyed over the peer");
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(ARQ_EV_TIMER_ACK, sess.deadline_event);
}

/* ---- "More is queued": HAS_DATA on DATA frames (fix 2) ----
 *
 * The last collision listen-before-talk cannot prevent is two stations
 * deciding within the ~0.4 s a decoder needs to lock on: on air (bench run 8)
 * an IRS TURN_REQ retry and an ISS ACK-timeout retransmission, 0.33 s apart.
 * The ISS now says on each DATA whether more is queued, so the IRS asks for
 * the floor in its ACK instead of competing.  Only toward peers that set
 * ARQ_FLAG_CAP_MORE: 1.9.x would read HAS_DATA on DATA as "the ISS keeps the
 * floor" and idle after asking for the turn. */

static uint8_t cap_subtype, cap_flags;
static void capture_frame(int ptype, int mode, size_t len, const uint8_t *f, int rem)
{
    (void)ptype; (void)mode; (void)rem;
    if (len > ARQ_HDR_FLAGS_IDX)
    {
        cap_subtype = f[ARQ_HDR_SUBTYPE_IDX];
        cap_flags   = f[ARQ_HDR_FLAGS_IDX];
    }
}

/* IRS receives one DATA with these flags; its own backlog when it ACKs. */
static void irs_receive_data(uint8_t flags, int backlog)
{
    fake_tx_backlog_fake.return_val = backlog;
    arq_event_t ev = make_event(ARQ_EV_RX_DATA);
    ev.session_id  = sess.session_id;
    ev.seq         = sess.rx_expected;
    ev.data_bytes  = 8;
    ev.payload_len = 8;
    ev.rx_flags    = flags;
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
}

void test_data_announces_more_only_to_a_capable_peer(void)
{
    goto_connected();
    goto_wait_ack();                                /* first DATA sent */
    fake_send_tx_frame_fake.custom_fake = capture_frame;

    /* The peer's ACK does not advertise the capability: no HAS_DATA. */
    fake_tx_backlog_fake.return_val = 512;
    arq_event_t ev = make_event(ARQ_EV_RX_ACK);
    ev.session_id = sess.session_id;
    ev.ack_seq    = sess.tx_seq;
    arq_fsm_dispatch(&sess, &ev);
    for (int i = 0; i < 4 && sess.dflow_state != ARQ_DFLOW_WAIT_ACK; i++)
    {
        ev = make_event(ARQ_EV_TIMER_ACK);  arq_fsm_dispatch(&sess, &ev);
        ev = make_event(ARQ_EV_TX_COMPLETE); arq_fsm_dispatch(&sess, &ev);
    }
    TEST_ASSERT_EQUAL_UINT8(ARQ_SUBTYPE_DATA, cap_subtype);
    TEST_ASSERT_TRUE(cap_flags & ARQ_FLAG_CAP_MORE);
    TEST_ASSERT_FALSE_MESSAGE(cap_flags & ARQ_FLAG_HAS_DATA,
        "HAS_DATA on DATA sent to a peer that never said it understands it");

    /* It does now, and more is queued: HAS_DATA. */
    ev = make_event(ARQ_EV_RX_ACK);
    ev.session_id = sess.session_id;
    ev.ack_seq    = sess.tx_seq;
    ev.rx_flags   = ARQ_FLAG_CAP_MORE;
    arq_fsm_dispatch(&sess, &ev);
    for (int i = 0; i < 4 && sess.dflow_state != ARQ_DFLOW_WAIT_ACK; i++)
    {
        ev = make_event(ARQ_EV_TIMER_ACK);  arq_fsm_dispatch(&sess, &ev);
        ev = make_event(ARQ_EV_TX_COMPLETE); arq_fsm_dispatch(&sess, &ev);
    }
    TEST_ASSERT_TRUE(sess.peer_cap_more);
    TEST_ASSERT_TRUE_MESSAGE(cap_flags & ARQ_FLAG_HAS_DATA, "more queued, not announced");

    /* Last frame: nothing behind it, no HAS_DATA. */
    fake_tx_backlog_fake.return_val = 0;
    ev = make_event(ARQ_EV_TIMER_ACK);             /* ACK timeout: retransmit */
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_UINT8(ARQ_SUBTYPE_DATA, cap_subtype);
    TEST_ASSERT_FALSE_MESSAGE(cap_flags & ARQ_FLAG_HAS_DATA,
        "announced more with nothing queued");
}

void test_ack_advertises_the_capability(void)
{
    goto_idle_irs_with_backlog();
    fake_send_tx_frame_fake.custom_fake = capture_frame;
    irs_receive_data(0, 0);
    TEST_ASSERT_EQUAL_UINT8(ARQ_SUBTYPE_ACK, cap_subtype);
    TEST_ASSERT_TRUE(cap_flags & ARQ_FLAG_CAP_MORE);
}

/* The IRS does not compete with an ISS that said more is coming... */
void test_irs_holds_turn_req_while_the_iss_announced_more(void)
{
    goto_idle_irs_with_backlog();
    irs_receive_data(ARQ_FLAG_CAP_MORE | ARQ_FLAG_HAS_DATA, 0);  /* nothing of ours yet */
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
    fake_tx_backlog_fake.return_val = 256;
    RESET_FAKE(fake_send_tx_frame);

    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "TURN_REQ raced an ISS that announced more data");
    ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(0, fake_send_tx_frame_fake.call_count);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
    TEST_ASSERT_EQUAL_INT(ARQ_EV_TIMER_PEER_BACKLOG, sess.deadline_event);

    /* ...but not forever: once a retransmission would have arrived, ask. */
    mock_set_uptime_ms(sess.deadline_ms + 1);
    sess.last_rx_ms = time_now_ms();                /* not an inactivity probe */
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_TURN_REQ_TX, sess.dflow_state,
        "a stale announcement muted the IRS");
}

/* HAS_DATA on DATA from a peer that did not set the capability means nothing. */
void test_has_data_without_capability_is_ignored(void)
{
    goto_idle_irs_with_backlog();
    irs_receive_data(ARQ_FLAG_HAS_DATA, 0);
    fake_tx_backlog_fake.return_val = 256;
    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_TURN_REQ_TX, sess.dflow_state);
}

/* The ISS yields to our HAS_DATA ACK whatever its own backlog, so its "more
 * is queued" must not send us idle after that ACK: both would sit in IDLE_IRS. */
void test_irs_takes_the_turn_it_asked_for_despite_announced_more(void)
{
    goto_idle_irs_with_backlog();
    fake_tx_read_fake.custom_fake = tx_read_one_frame;
    irs_receive_data(ARQ_FLAG_CAP_MORE | ARQ_FLAG_HAS_DATA, 256);  /* our ACK: HAS_DATA */
    TEST_ASSERT_TRUE(sess.acktx_had_has_data);
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(ARQ_DFLOW_IDLE_IRS, sess.dflow_state,
        "asked for the turn in the ACK, then sat idle");
}

/* Bench run 16: right after a handover the new sender is about to key its
 * first DATA, and a TURN_REQ for freshly written data keyed 250 ms into that
 * burst -- too early for any decoder to have synced.  A handover arms the same
 * hold as an announcement. */
void test_no_turn_req_right_after_yielding_to_a_has_data_ack(void)
{
    goto_connected();
    goto_wait_ack();
    arq_event_t ev = make_event(ARQ_EV_RX_ACK);   /* the peer wants the floor */
    ev.session_id = sess.session_id;
    ev.ack_seq    = sess.tx_seq;
    ev.rx_flags   = ARQ_FLAG_HAS_DATA;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);

    fake_tx_backlog_fake.return_val = 256;        /* our host writes more */
    RESET_FAKE(fake_send_tx_frame);
    ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "TURN_REQ keyed into the first burst of the sender we just yielded to");
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
}

void test_no_turn_req_right_after_granting_the_turn(void)
{
    goto_idle_iss();
    arq_event_t ev = make_event(ARQ_EV_RX_TURN_REQ);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TIMER_ACK);               /* TURN_ACK */
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);

    fake_tx_backlog_fake.return_val = 256;
    RESET_FAKE(fake_send_tx_frame);
    ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_send_tx_frame_fake.call_count,
        "TURN_REQ keyed into the first burst of the sender we just granted");
}

void test_simultaneous_turn_req_one_side_yields(void)
{
    /* Local "SRC1" vs remote "DST1": strcmp > 0 is rank 1, which yields. */
    goto_turn_req_wait();
    strncpy(sess.local_call, "SRC1", CALLSIGN_MAX_SIZE);
    strncpy(sess.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    TEST_ASSERT_TRUE(strcmp(sess.local_call, sess.remote_call) > 0);

    arq_event_t ev = make_event(ARQ_EV_RX_TURN_REQ);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_TURN_ACK_TX, sess.dflow_state,
        "the higher-ranked station must grant the turn, not ignore the request");
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
}

void test_simultaneous_turn_req_other_side_holds(void)
{
    /* Local "AAA1" vs remote "DST1": strcmp < 0 is rank 0, which holds. */
    goto_turn_req_wait();
    strncpy(sess.local_call, "AAA1", CALLSIGN_MAX_SIZE);
    strncpy(sess.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    TEST_ASSERT_TRUE(strcmp(sess.local_call, sess.remote_call) < 0);

    arq_event_t ev = make_event(ARQ_EV_RX_TURN_REQ);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);

    /* Holds its request and stays silent: the peer is about to answer it, and
     * transmitting now would key on top of that TURN_ACK. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_DFLOW_TURN_REQ_WAIT, sess.dflow_state,
        "the lower-ranked station must keep its request, not also yield");
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
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

    /* Exhaust retries */
    for (int i = 0; i < ARQ_CALL_RETRY_SLOTS_DEFAULT + 2; i++) {
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

    for (int i = 0; i < ARQ_CALL_RETRY_SLOTS_DEFAULT + 2; i++) {
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
    RUN_TEST(test_mode_req_wait_latches_crossed_turn_req);
    RUN_TEST(test_mode_ack_yields_to_the_latched_turn_req);
    RUN_TEST(test_turn_req_wait_concedes_to_mode_req);
    RUN_TEST(test_connect_on_live_session_without_disconnect_is_not_queued);
    RUN_TEST(test_listen_off_drops_pending_accept);
    RUN_TEST(test_listen_off_drops_outgoing_call);
    RUN_TEST(test_listen_off_deferred_within_grace_accepting);
    RUN_TEST(test_listen_off_in_calling_is_immediate);
    RUN_TEST(test_deferred_listen_off_survives_connect);
    RUN_TEST(test_listen_off_drops_live_link_without_draining);
    RUN_TEST(test_rx_disconnect_from_connected);
    RUN_TEST(test_connected_rejects_zero_session_id);
    RUN_TEST(test_connected_rejects_foreign_mode_request);
    RUN_TEST(test_wait_ack_rejects_foreign_mode_request);
    RUN_TEST(test_connected_seeds_no_progress_clock);
    RUN_TEST(test_app_disconnect_defers_with_backlog);
    RUN_TEST(test_pending_disconnect_retries_last_frame_before_teardown);
    RUN_TEST(test_app_disconnect_defers_in_wait_ack);
    RUN_TEST(test_wait_ack_cumulative_ack_advances_window);
    RUN_TEST(test_wait_ack_stale_ack_keeps_window);
    RUN_TEST(test_wait_ack_turn_req_defers_the_yield_without_keying);
    RUN_TEST(test_wait_ack_turn_req_pulls_the_retransmission_in);
    RUN_TEST(test_wait_ack_retransmission_is_not_keyed_over_the_peer);
    RUN_TEST(test_turn_req_decoded_while_deferring_waits_one_guard);
    RUN_TEST(test_wait_ack_retransmission_deferral_is_bounded);
    RUN_TEST(test_wait_ack_yields_to_latched_turn_req_when_the_ack_lands);
    RUN_TEST(test_wait_ack_ack_without_request_keeps_the_turn);
    RUN_TEST(test_keepalive_wait_accepts_data);
    RUN_TEST(test_turn_req_is_not_keyed_over_the_peers_burst);
    RUN_TEST(test_turn_req_for_new_data_is_not_keyed_over_the_peers_burst);
    RUN_TEST(test_turn_req_is_sent_once_the_channel_is_quiet);
    RUN_TEST(test_turn_req_defers_on_channel_energy_without_sync);
    RUN_TEST(test_turn_req_deferral_is_bounded);
    RUN_TEST(test_turn_req_retry_is_not_keyed_over_the_peers_burst);
    RUN_TEST(test_turn_req_retry_defers_on_channel_energy_without_sync);
    RUN_TEST(test_turn_req_retry_deferral_is_bounded);
    RUN_TEST(test_turn_req_concede_resets_the_deferral_budget);
    RUN_TEST(test_no_delivery_after_the_application_disconnects);
    RUN_TEST(test_new_session_delivers_again);
    RUN_TEST(test_disconnect_waits_for_the_reply_its_turn_ack_invited);
    RUN_TEST(test_disconnect_retry_is_not_keyed_over_the_peer);
    RUN_TEST(test_disconnect_deferral_is_bounded);
    RUN_TEST(test_disconnect_reply_waits_one_reply_guard);
    RUN_TEST(test_first_data_after_turn_ack_listens);
    RUN_TEST(test_new_data_at_idle_iss_listens);
    RUN_TEST(test_data_announces_more_only_to_a_capable_peer);
    RUN_TEST(test_ack_advertises_the_capability);
    RUN_TEST(test_irs_holds_turn_req_while_the_iss_announced_more);
    RUN_TEST(test_has_data_without_capability_is_ignored);
    RUN_TEST(test_irs_takes_the_turn_it_asked_for_despite_announced_more);
    RUN_TEST(test_no_turn_req_right_after_yielding_to_a_has_data_ack);
    RUN_TEST(test_no_turn_req_right_after_granting_the_turn);
    RUN_TEST(test_simultaneous_turn_req_one_side_yields);
    RUN_TEST(test_simultaneous_turn_req_other_side_holds);
    RUN_TEST(test_disconnect_drain_timeout_forces_teardown);
    RUN_TEST(test_retry_exhaustion_persists_then_disconnects);
    RUN_TEST(test_retry_exhaustion_disconnects_from_zero_uptime_baseline);
    /* Timeout tests */
    RUN_TEST(test_call_timeout);
    RUN_TEST(test_call_timeout_no_listen);
    RUN_TEST(test_accepting_gives_up_after_budget);
    RUN_TEST(test_accept_fallback_completes_on_first_data);
    RUN_TEST(test_ended_session_is_not_resurrected_by_late_data);
    RUN_TEST(test_accepting_rx_call_rearms_budget);
    RUN_TEST(test_default_call_accept_slots_are_short);
    RUN_TEST(test_stop_listen);
    RUN_TEST(test_timeout_ms_idle);
    RUN_TEST(test_accept_is_only_sent_in_answer_to_a_heard_call);
    RUN_TEST(test_accepting_silent_wait_gives_up_after_budget);
    return UNITY_END();
}
