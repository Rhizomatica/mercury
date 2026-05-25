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

/* Provided by arq_test_stubs.c */
extern void mock_set_uptime_ms(uint64_t ms);

/* ---- FFF Fakes for arq_fsm_callbacks_t ---- */

FAKE_VOID_FUNC(fake_send_tx_frame, int, int, size_t, const uint8_t *);
FAKE_VOID_FUNC(fake_notify_connected, const char *);
FAKE_VOID_FUNC(fake_notify_pending, const char *);
FAKE_VOID_FUNC(fake_notify_cancelpending);
FAKE_VOID_FUNC(fake_notify_disconnected, bool);
FAKE_VOID_FUNC(fake_deliver_rx_data, const uint8_t *, size_t);
FAKE_VALUE_FUNC(int, fake_tx_backlog);
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

/* CALL timeout transitions to DISCONNECTED */
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

int main(void)
{
    UNITY_BEGIN();
    /* Connection lifecycle tests */
    RUN_TEST(test_init_state_disconnected);
    RUN_TEST(test_listen_transitions_to_listening);
    RUN_TEST(test_connect_transitions_to_calling);
    RUN_TEST(test_incoming_call_transitions_to_accepting);
    RUN_TEST(test_accept_transitions_to_connected);
    RUN_TEST(test_disconnect_from_connected);
    RUN_TEST(test_rx_disconnect_from_connected);
    RUN_TEST(test_connected_seeds_no_progress_clock);
    RUN_TEST(test_app_disconnect_defers_with_backlog);
    RUN_TEST(test_disconnect_drain_timeout_forces_teardown);
    RUN_TEST(test_retry_exhaustion_persists_then_disconnects);
    RUN_TEST(test_retry_exhaustion_disconnects_from_zero_uptime_baseline);
    /* Timeout tests */
    RUN_TEST(test_call_timeout);
    RUN_TEST(test_stop_listen);
    RUN_TEST(test_timeout_ms_idle);
    return UNITY_END();
}
