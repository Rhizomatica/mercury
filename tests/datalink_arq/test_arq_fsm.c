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

/* An ACCEPT a test builds by hand comes from the station it called: the
 * FSM takes an ACCEPT only with that station's CRC (protocol version 1). */
static void test_dispatch(arq_session_t *s, arq_event_t *e)
{
    /* The caller's first frame is seeded, so it can only follow our ACCEPT:
     * a test that answers with it at once sends the ACCEPT first. */
    if (e->id == ARQ_EV_RX_CAROUSEL)
    {
        if (s->conn_state == ARQ_CONN_ACCEPTING && s->crc_seed == 0)
        {
            arq_event_t t;
            memset(&t, 0, sizeof(t));
            t.id = ARQ_EV_TIMER_RETRY;
            arq_fsm_dispatch(s, &t);
        }
    }
    if (e->id == ARQ_EV_RX_ACCEPT && e->car_called_crc == 0)
        e->car_called_crc = arq_protocol_callsign_crc16(s->remote_call);
    bool calling = s->conn_state == ARQ_CONN_CALLING;
    arq_fsm_dispatch(s, e);
    /* The caller keys its first round at connect: take it as sent. */
    if (calling && e->id == ARQ_EV_RX_ACCEPT && s->conn_state == ARQ_CONN_CONNECTED)
    {
        arq_event_t done;
        memset(&done, 0, sizeof(done));
        done.id = ARQ_EV_TX_COMPLETE;
        arq_fsm_dispatch(s, &done);
    }
}
#define arq_fsm_dispatch(s, e) test_dispatch((s), (e))

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
    ev = make_event(ARQ_EV_RX_CAROUSEL);
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
void test_accept_fallback_completes_on_first_frame(void)
{
    enter_accepting();
    accept_until_fallback();

    RESET_FAKE(fake_notify_connected);
    arq_event_t ev = make_event(ARQ_EV_RX_CAROUSEL);
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
void test_ended_session_is_not_resurrected_by_a_late_frame(void)
{
    enter_accepting();
    arq_event_t ev = make_event(ARQ_EV_RX_CAROUSEL);   /* caller's first frame */
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
    ev = make_event(ARQ_EV_RX_CAROUSEL);
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
    ev = make_event(ARQ_EV_RX_CAROUSEL);
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

/* The guard expires and our DISCONNECT goes out: a DISCONNECT from the peer
 * after this answers ours (before it, the two crossed -- see
 * test_crossed_disconnect_is_answered). */
static void send_our_disconnect(void)
{
    arq_event_t ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    /* The caller's DISCONNECT waits out a reply slot it opened for the
     * callee (the turn rules): follow the FSM's own deadline to it. */
    for (int i = 0; i < 8 && !sess.disconnect_sent && sess.deadline_ms != UINT64_MAX; i++)
    {
        mock_set_uptime_ms(sess.deadline_ms);
        ev = make_event(sess.deadline_event);
        arq_fsm_dispatch(&sess, &ev);
    }
    TEST_ASSERT_TRUE_MESSAGE(sess.disconnect_sent, "our DISCONNECT did not go out");
}

/* Caller with a live session, then DISCONNECT with nothing queued: the
 * DISCONNECT exchange is on the air and the FSM is in DISCONNECTING. */
static void goto_disconnecting_caller_held(void)
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
    /* The carousel holds a DISCONNECT until its first exchange is over;
     * ABORT skips that drain, and the teardown under test is the same. */
    ev = make_event(ARQ_EV_APP_DISCONNECT);
    ev.abort = true;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTING, sess.conn_state);
    RESET_FAKE(fake_send_tx_frame);
    RESET_FAKE(fake_notify_disconnected);
}

static void goto_disconnecting_caller(void)
{
    goto_disconnecting_caller_held();
    send_our_disconnect();
    RESET_FAKE(fake_send_tx_frame);
}

/* Both ends hung up, and the peer's DISCONNECT came while ours was still held
 * back: the peer has nothing from us.  Finalizing silently (on air, car24)
 * left it retrying for 21 s and deaf to our redial.  It is answered. */
void test_crossed_disconnect_is_answered(void)
{
    goto_disconnecting_caller_held();
    TEST_ASSERT_FALSE(sess.disconnect_sent);

    arq_event_t ev = make_event(ARQ_EV_RX_DISCONNECT);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(0, fake_notify_disconnected_fake.call_count);   /* not before the answer */

    ev = make_event(ARQ_EV_TIMER_ACK);            /* one reply guard later */
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_send_tx_frame_fake.call_count,
        "the peer's DISCONNECT, crossing ours before it went out, was not answered");

    ev = make_event(ARQ_EV_TX_COMPLETE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(1, fake_notify_disconnected_fake.call_count);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_LISTENING, sess.conn_state);
}

/* Once ours is out, the peer's DISCONNECT is the answer: nothing more is sent. */
void test_disconnect_after_ours_is_the_answer(void)
{
    goto_disconnecting_caller();
    arq_event_t ev = make_event(ARQ_EV_RX_DISCONNECT);
    ev.session_id = sess.session_id;
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(0, fake_send_tx_frame_fake.call_count);
    TEST_ASSERT_EQUAL_INT(1, fake_notify_disconnected_fake.call_count);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_LISTENING, sess.conn_state);
}

/* The peer we are tearing down with calls again: it has left the old session.
 * Retrying on, the CALL was ignored (on air, car24: the gateway kept sending
 * DISCONNECT over the IC-7100's redial).  The teardown ends and the CALL is
 * answered.  A CALL from anyone else does not end it. */
void test_peer_calling_again_ends_teardown(void)
{
    goto_disconnecting_caller();
    arq_event_t ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x33;
    strncpy(ev.remote_call, "DST9", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARQ_CONN_DISCONNECTING, sess.conn_state,
        "a stranger's CALL ended our teardown");

    ev = make_event(ARQ_EV_RX_CALL);
    ev.session_id = 0x33;
    strncpy(ev.remote_call, "DST1", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, sess.conn_state);
    TEST_ASSERT_EQUAL_UINT8(0x33, sess.session_id);
    TEST_ASSERT_EQUAL_INT(1, fake_notify_disconnected_fake.call_count);   /* the old session's end */
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

    /* Disconnect, right after connecting: the caller's first round is still
     * waiting for its answer, so the carousel holds the DISCONNECT until
     * that exchange is over -- bounded by a drain deadline. */
    ev = make_event(ARQ_EV_APP_DISCONNECT);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, sess.conn_state);
    TEST_ASSERT_TRUE(sess.pending_disconnect);
    TEST_ASSERT_NOT_EQUAL_UINT64(0, sess.disconnect_deadline_ms);

    /* A CONNECT while it is held is queued, not dropped: it is placed once
     * the session has ended (arq_fsm_dispatch). */
    ev = make_event(ARQ_EV_APP_CONNECT);
    strncpy(ev.remote_call, "DST2", CALLSIGN_MAX_SIZE);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_TRUE(sess.pending_connect);
    TEST_ASSERT_EQUAL_STRING("DST2", sess.pending_connect_call);
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

    /* Advance past the drain budget (the configured one, or the carousel's
     * for its rung where an exchange is slower) and feed any event. */
    TEST_ASSERT_TRUE(sess.pending_disconnect);
    TEST_ASSERT_TRUE(sess.disconnect_deadline_ms >= 1000 + (uint64_t)ARQ_DISCONNECT_DRAIN_TIMEOUT_S * 1000);
    mock_set_uptime_ms(sess.disconnect_deadline_ms + 1000);
    ev = make_event(ARQ_EV_TIMER_CAROUSEL);
    arq_fsm_dispatch(&sess, &ev);

    TEST_ASSERT_EQUAL_INT(ARQ_CONN_DISCONNECTING, sess.conn_state);
    TEST_ASSERT_FALSE(sess.pending_disconnect);
}


































/* ---- Bench run 12: a DISCONNECT right behind a TURN_ACK ----
 *
 * The host hung up while our TURN_ACK was on the air.  The DISCONNECT keyed
 * 42 ms after the TURN_ACK ended, the peer keyed its first DATA 0.9 s later
 * (it held the TURN_ACK and never listened), and both were lost. */






/* Bench run 14: the peer's DISCONNECT decoded 186 ms before its TX_COMPLETE
 * (a frame decodes before its tail has played out), our reply keyed 40 ms
 * later and clipped it.  The reply waits one reply guard, like every other
 * answer. */
void test_disconnect_reply_waits_one_reply_guard(void)
{
    goto_connected();
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
    ev = make_event(ARQ_EV_RX_CAROUSEL);
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
    RUN_TEST(test_incoming_call_transitions_to_accepting);
    RUN_TEST(test_incoming_call_records_dialed_secondary);
    RUN_TEST(test_accept_transitions_to_connected);
    RUN_TEST(test_listen_mode_does_not_leak_into_inbound_session);
    RUN_TEST(test_listen_mode_restored_after_session_ends);
    RUN_TEST(test_disconnect_from_connected);
    RUN_TEST(test_connect_during_teardown_is_deferred_not_dropped);
    RUN_TEST(test_crossed_disconnect_is_answered);
    RUN_TEST(test_disconnect_after_ours_is_the_answer);
    RUN_TEST(test_peer_calling_again_ends_teardown);
    RUN_TEST(test_deferred_connect_is_placed_after_teardown_timeout);
    RUN_TEST(test_disconnect_cancels_deferred_connect);
    RUN_TEST(test_listen_off_cancels_deferred_connect);
    RUN_TEST(test_connect_on_live_session_without_disconnect_is_not_queued);
    RUN_TEST(test_listen_off_drops_pending_accept);
    RUN_TEST(test_listen_off_drops_outgoing_call);
    RUN_TEST(test_listen_off_deferred_within_grace_accepting);
    RUN_TEST(test_listen_off_in_calling_is_immediate);
    RUN_TEST(test_deferred_listen_off_survives_connect);
    RUN_TEST(test_listen_off_drops_live_link_without_draining);
    RUN_TEST(test_rx_disconnect_from_connected);
    RUN_TEST(test_connected_rejects_zero_session_id);
    RUN_TEST(test_connected_seeds_no_progress_clock);
    RUN_TEST(test_app_disconnect_defers_with_backlog);
    RUN_TEST(test_disconnect_reply_waits_one_reply_guard);
    RUN_TEST(test_disconnect_drain_timeout_forces_teardown);
    /* Timeout tests */
    RUN_TEST(test_call_timeout);
    RUN_TEST(test_call_timeout_no_listen);
    RUN_TEST(test_accepting_gives_up_after_budget);
    RUN_TEST(test_accept_fallback_completes_on_first_frame);
    RUN_TEST(test_ended_session_is_not_resurrected_by_a_late_frame);
    RUN_TEST(test_accepting_rx_call_rearms_budget);
    RUN_TEST(test_default_call_accept_slots_are_short);
    RUN_TEST(test_stop_listen);
    RUN_TEST(test_timeout_ms_idle);
    RUN_TEST(test_accept_is_only_sent_in_answer_to_a_heard_call);
    RUN_TEST(test_accepting_silent_wait_gives_up_after_budget);
    return UNITY_END();
}
