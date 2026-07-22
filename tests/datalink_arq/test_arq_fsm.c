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
#include "freedv/freedv_api.h"
#include "modem_mfsk.h"   /* MERCURY_MODE_MFSK */

/* Provided by arq_test_stubs.c */
extern void mock_set_uptime_ms(uint64_t ms);

/* ---- FFF Fakes for arq_fsm_callbacks_t ---- */

FAKE_VOID_FUNC(fake_send_tx_frame, int, int, size_t, const uint8_t *, int);
FAKE_VOID_FUNC(fake_send_pattern_ack, int, int);
FAKE_VOID_FUNC(fake_notify_connected, const char *, const char *);
FAKE_VOID_FUNC(fake_notify_pending, const char *, const char *);
FAKE_VOID_FUNC(fake_notify_cancelpending);
FAKE_VOID_FUNC(fake_notify_disconnected, bool);
FAKE_VOID_FUNC(fake_deliver_rx_data, const uint8_t *, size_t);
FAKE_VALUE_FUNC(int, fake_tx_backlog);
FAKE_VALUE_FUNC(int, fake_tx_read, uint8_t *, size_t);
FAKE_VOID_FUNC(fake_send_buffer_status, int);

static arq_fsm_callbacks_t test_callbacks = {
    .send_tx_frame       = fake_send_tx_frame,
    .send_pattern_ack    = fake_send_pattern_ack,
    .notify_connected    = fake_notify_connected,
    .notify_pending      = fake_notify_pending,
    .notify_cancelpending = fake_notify_cancelpending,
    .notify_disconnected = fake_notify_disconnected,
    .deliver_rx_data     = fake_deliver_rx_data,
    .tx_backlog          = fake_tx_backlog,
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
    RESET_FAKE(fake_send_pattern_ack);
    RESET_FAKE(fake_notify_connected);
    RESET_FAKE(fake_notify_pending);
    RESET_FAKE(fake_notify_cancelpending);
    RESET_FAKE(fake_notify_disconnected);
    RESET_FAKE(fake_deliver_rx_data);
    RESET_FAKE(fake_tx_backlog);
    RESET_FAKE(fake_tx_read);
    RESET_FAKE(fake_send_buffer_status);
    FFF_RESET_HISTORY();

    /* Init session and register callbacks */
    mock_set_uptime_ms(1000);
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

/* Initial modes: DATAC16 control plane, MFSK payload floor (ladder rank 0) */
void test_init_mode_defaults(void)
{
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC16, sess.control_mode);
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

    /* Advance past the absolute drain budget and feed any CONNECTED event.
     * (No keepalive timer any more — the peer-backlog timer is a benign
     * CONNECTED event that triggers the drain-timeout fallback check.) */
    mock_set_uptime_ms(1000 + (uint64_t)ARQ_DISCONNECT_DRAIN_TIMEOUT_S * 1000 + 1000);
    ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
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

/* ---- IRS payload-mode mirror: follow the peer's delivery-driven ladder ----
 *
 * The IRS's payload decoder must be on the mode the peer's NEXT burst will use,
 * NOT the last one it decoded — otherwise it misses the first burst of every
 * mode the sender climbs to and the transfer stalls at the MFSK floor (the
 * -x sock regression).  Since the IRS observes the same per-frame outcomes the
 * sender climbs on, it mirrors the same ladder. */

/* LISTENING -> ACCEPTING -> CONNECTED as the answerer: IRS role, IDLE_IRS. */
static void goto_connected_irs(void)
{
    enter_accepting();
    arq_event_t ev = make_event(ARQ_EV_RX_ACK);
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
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_IDLE_IRS, sess.dflow_state);
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
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC15, sess.peer_tx_mode);   /* level 1 */
    complete_ack_tx();

    ev = make_data_event(1, FREEDV_MODE_DATAC15, 30);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC4, sess.peer_tx_mode);    /* level 2 */
    complete_ack_tx();

    ev = make_data_event(2, FREEDV_MODE_DATAC4, 54);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC3, sess.peer_tx_mode);    /* level 3 */
}

/* A duplicate frame means our ACK was lost and the sender retried, stepping ITS
 * ladder down — the mirror must step down too so we can decode the retransmit. */
void test_irs_mirror_steps_down_on_duplicate(void)
{
    goto_connected_irs();
    arq_event_t ev = make_data_event(0, MERCURY_MODE_MFSK, 90);
    arq_fsm_dispatch(&sess, &ev);
    complete_ack_tx();
    ev = make_data_event(1, FREEDV_MODE_DATAC15, 30);
    arq_fsm_dispatch(&sess, &ev);
    complete_ack_tx();
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC4, sess.peer_tx_mode);    /* climbed to level 2 */

    /* Duplicate of an already-delivered seq (rx_expected has advanced past it). */
    ev = make_data_event(0, MERCURY_MODE_MFSK, 90);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC15, sess.peer_tx_mode);   /* stepped down to level 1 */
}

/* Reset-on-miss: a full idle hold with no DATA (a lost ACK left us climbed above
 * the sender) steps the mirror down toward the floor so the two ends re-sync. */
void test_irs_mirror_resets_toward_floor_on_silence(void)
{
    goto_connected_irs();
    arq_event_t ev = make_data_event(0, MERCURY_MODE_MFSK, 90);
    arq_fsm_dispatch(&sess, &ev);
    complete_ack_tx();
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC15, sess.peer_tx_mode);   /* climbed to level 1 */

    /* Idle-hold fires with no reverse backlog and a recent RX (not dead yet). */
    fake_tx_backlog_fake.return_val = 0;
    ev = make_event(ARQ_EV_TIMER_PEER_BACKLOG);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK, sess.peer_tx_mode);     /* stepped back to the floor */
}

int main(void)
{
    UNITY_BEGIN();
    /* Connection lifecycle tests */
    RUN_TEST(test_init_state_disconnected);
    RUN_TEST(test_init_mode_defaults);
    RUN_TEST(test_listen_transitions_to_listening);
    RUN_TEST(test_connect_transitions_to_calling);
    RUN_TEST(test_incoming_call_transitions_to_accepting);
    RUN_TEST(test_incoming_call_records_dialed_secondary);
    RUN_TEST(test_accept_transitions_to_connected);
    RUN_TEST(test_disconnect_from_connected);
    RUN_TEST(test_listen_off_drops_pending_accept);
    RUN_TEST(test_listen_off_drops_outgoing_call);
    RUN_TEST(test_listen_off_deferred_within_grace_accepting);
    RUN_TEST(test_listen_off_in_calling_is_immediate);
    RUN_TEST(test_deferred_listen_off_survives_connect);
    RUN_TEST(test_listen_off_drops_live_link_without_draining);
    RUN_TEST(test_rx_disconnect_from_connected);
    RUN_TEST(test_connected_seeds_no_progress_clock);
    RUN_TEST(test_app_disconnect_defers_with_backlog);
    RUN_TEST(test_pending_disconnect_retries_last_frame_before_teardown);
    RUN_TEST(test_app_disconnect_defers_in_wait_ack);
    RUN_TEST(test_wait_ack_pattern_ack_confirms_frame);
    RUN_TEST(test_wait_ack_break_yields_floor);
    RUN_TEST(test_wait_ack_stale_ack_ignored);
    RUN_TEST(test_disconnect_drain_timeout_forces_teardown);
    RUN_TEST(test_retry_exhaustion_persists_then_disconnects);
    RUN_TEST(test_retry_exhaustion_disconnects_from_zero_uptime_baseline);
    /* Timeout tests */
    RUN_TEST(test_call_timeout);
    RUN_TEST(test_call_timeout_no_listen);
    RUN_TEST(test_accepting_gives_up_after_budget);
    RUN_TEST(test_accepting_rx_call_rearms_budget);
    RUN_TEST(test_default_call_accept_slots_are_short);
    RUN_TEST(test_stop_listen);
    RUN_TEST(test_timeout_ms_idle);
    RUN_TEST(test_irs_mirror_climbs_with_peer);
    RUN_TEST(test_irs_mirror_steps_down_on_duplicate);
    RUN_TEST(test_irs_mirror_resets_toward_floor_on_silence);
    return UNITY_END();
}
