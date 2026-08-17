/*
 * Delivery-driven mode-ladder tests
 *
 * Replaces the OLLA/SNR gear-shift test.  The new ladder is delivery-driven
 * (no SNR): payload_mode = arq_mode_ladder[speed_level], sessions start at the
 * MFSK floor, climb on clean deliveries (fast ramp: 1 rung per clean delivery
 * until the first retry, then ARQ_LADDER_UP_SUCCESSES-per-step), and step down
 * one rung on any retransmission.  Drives the real arq_fsm so the ladder is
 * exercised through the actual RX_ACK / TIMER_ACK transitions.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>
#include <stdint.h>

#include "unity.h"
#include <stdio.h>
#include "fff.h"

DEFINE_FFF_GLOBALS;

#include "arq_fsm.h"
#include "arq_protocol.h"
#include "freedv/freedv_api.h"
#include "modem_mfsk.h"   /* MERCURY_MODE_MFSK */

extern void mock_set_uptime_ms(uint64_t ms);

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

/* tx_read fake: always yields a small frame so a DATA frame is always sendable
 * (backlog is kept high by fake_tx_backlog). */
static int tx_read_small(uint8_t *buf, size_t n)
{
    size_t k = (n < 16) ? n : 16;
    memset(buf, 0xA5, k);
    return (int)k;
}

/* tx_read fake for the retreat tests: hands back everything asked for, the way
 * a ring with plenty of backlog does.  tx_read_small() always returns 16 bytes,
 * which fits every rung and so can never show a frame stranded above the
 * ladder — the bug these tests pin needs full-width reads. */
static int tx_read_full(uint8_t *buf, size_t n)
{
    memset(buf, 0x5A, n);
    return (int)n;
}

/* Payload mode of the last DATA frame actually handed to the modem. */
static int last_tx_mode(void)
{
    return fake_send_tx_frame_fake.arg1_val;
}

static arq_event_t make_event(arq_event_id_t id)
{
    arq_event_t ev; memset(&ev, 0, sizeof(ev)); ev.id = id; return ev;
}

void setUp(void)
{
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

    mock_set_uptime_ms(1000);
    arq_timing_init(&timing);
    arq_fsm_set_timing(&timing);
    arq_fsm_set_callbacks(&test_callbacks);
    arq_fsm_init(&sess);

    fake_tx_backlog_fake.return_val = 100000;   /* effectively unlimited */
    fake_tx_read_fake.custom_fake   = tx_read_small;
}

void tearDown(void) { }

/* Drive to CONNECTED as the caller. */
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
    /* We have backlog, so the caller entered the data path via
     * enter_idle_iss_guarded (DATA_TX after a guard). */
}

/* Push the session into WAIT_ACK with exactly one frame outstanding. */
static void goto_wait_ack(void)
{
    arq_event_t ev = make_event(ARQ_EV_APP_DATA_READY);
    arq_fsm_dispatch(&sess, &ev);
    for (int i = 0; i < 8 && sess.dflow_state != ARQ_DFLOW_WAIT_ACK; i++)
    {
        if (sess.dflow_state == ARQ_DFLOW_DATA_TX)
        {
            ev = make_event(ARQ_EV_TIMER_ACK);   /* guard elapsed -> send */
            arq_fsm_dispatch(&sess, &ev);
            ev = make_event(ARQ_EV_TX_COMPLETE);  /* DATA_TX -> WAIT_ACK */
            arq_fsm_dispatch(&sess, &ev);
        }
        else
        {
            ev = make_event(ARQ_EV_TX_COMPLETE);  /* clear connect-confirm */
            arq_fsm_dispatch(&sess, &ev);
        }
    }
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);
}

/* Deliver a clean pattern ACK; then re-enter WAIT_ACK for the next frame. */
static void clean_ack_cycle(void)
{
    arq_event_t ev = make_event(ARQ_EV_RX_ACK);   /* plain ACK, no HAS_DATA */
    arq_fsm_dispatch(&sess, &ev);
    /* ISS retains the turn (no HAS_DATA) -> enter_idle_iss_guarded -> DATA_TX
     * after a guard.  Advance to WAIT_ACK again. */
    for (int i = 0; i < 8 && sess.dflow_state != ARQ_DFLOW_WAIT_ACK; i++)
    {
        if (sess.dflow_state == ARQ_DFLOW_DATA_TX)
        {
            ev = make_event(ARQ_EV_TIMER_ACK);
            arq_fsm_dispatch(&sess, &ev);
            ev = make_event(ARQ_EV_TX_COMPLETE);
            arq_fsm_dispatch(&sess, &ev);
        }
        else break;
    }
}

/* ---- tests ---- */

/* A rung is probed with a frame the rung BELOW can still carry.
 *
 * The retained frame is immutable: it is never re-framed smaller, so its size
 * decides, once and for all, which modes can ever transmit it.  Read it at the
 * full width of a rung the channel has not carried yet and a failed probe
 * strands it — mode_that_fits() pins every retransmission to that rung while
 * the ladder steps down beneath it, and the peer's mirror follows the ladder
 * away from the mode that is actually on the air.  Sizing the read to a rung
 * that has already delivered keeps the retreat open. */
void test_probe_frame_fits_the_rung_below(void)
{
    fake_tx_read_fake.custom_fake = tx_read_full;
    goto_connected();
    goto_wait_ack();

    /* Climb to DATAC1 (level 4).  Each clean delivery proves the rung that
     * carried it, so the frame sent while probing level 4 is sized for the
     * proven level 3. */
    for (int expect = ARQ_LADDER_START_LEVEL + 1; expect <= 4; expect++)
    {
        clean_ack_cycle();
        TEST_ASSERT_EQUAL_INT(expect, sess.speed_level);
    }
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC1, sess.payload_mode);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC1, last_tx_mode());

    const arq_mode_timing_t *below = arq_protocol_mode_timing(arq_mode_ladder[3]);
    TEST_ASSERT_NOT_NULL(below);
    TEST_ASSERT_TRUE_MESSAGE(
        sess.tx_frame_len <= (int)below->payload_bytes - ARQ_FRAME_HDR_SIZE,
        "probe frame is too big for the rung the ladder retreats to");
}

/* ...and because it fits, a failed probe actually retreats ON THE AIR.
 *
 * This is the property the 0 dB bench cell was failing: the ladder stepped
 * 4->3->2->1->0 while every retransmission went out on DATAC1 at an identical
 * 4.817 s keydown, because no lower rung could carry the frame.  The step-down
 * was pure bookkeeping; the only thing it moved was the peer's decoder, away
 * from the mode being transmitted.  Assert the transmitted mode, not the
 * ladder index — the ladder index was never the thing that was wrong. */
void test_failed_probe_retreats_on_the_air(void)
{
    fake_tx_read_fake.custom_fake = tx_read_full;
    goto_connected();
    goto_wait_ack();
    for (int expect = ARQ_LADDER_START_LEVEL + 1; expect <= 4; expect++)
    {
        clean_ack_cycle();
        TEST_ASSERT_EQUAL_INT(expect, sess.speed_level);
    }
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC1, last_tx_mode());

    /* The probe is not acknowledged: one retry steps the ladder down a rung
     * and retransmits.  The frame must follow the ladder down. */
    arq_event_t ev = make_event(ARQ_EV_TIMER_ACK);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(3, sess.speed_level);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FREEDV_MODE_DATAC3, last_tx_mode(),
        "retransmission stayed on the probed rung: the ladder stepped down "
        "but the transmitter did not");
}

void test_ladder_starts_one_rung_above_the_floor(void)
{
    /* MFSK is still the floor — it is just not where a session opens.  The
     * handshake has already carried DATAC16 both ways, and MFSK sits about
     * 10-12 dB below that, so the floor is reached by stepping DOWN when a
     * burst is actually lost rather than assumed at the start. */
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK, arq_mode_ladder[0]);
    TEST_ASSERT_EQUAL_INT(ARQ_LADDER_START_LEVEL, sess.speed_level);
    TEST_ASSERT_EQUAL_INT(arq_mode_ladder[ARQ_LADDER_START_LEVEL],
                          sess.payload_mode);
    TEST_ASSERT_TRUE(ARQ_LADDER_START_LEVEL > 0);
}

void test_ladder_table_ordered_and_sized(void)
{
    TEST_ASSERT_EQUAL_INT(7, ARQ_LADDER_LEVELS);
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK,   arq_mode_ladder[0]);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC15, arq_mode_ladder[1]);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC4,  arq_mode_ladder[2]);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC3,  arq_mode_ladder[3]);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC1,  arq_mode_ladder[4]);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC17, arq_mode_ladder[5]);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_QAM16C2, arq_mode_ladder[6]);
}

/* Fast initial ramp: one rung per clean delivery until the ladder tops out. */
void test_ladder_fast_ramp_climbs_one_per_clean(void)
{
    goto_connected();
    goto_wait_ack();
    TEST_ASSERT_EQUAL_INT(ARQ_LADDER_START_LEVEL, sess.speed_level);

    for (int expect = ARQ_LADDER_START_LEVEL + 1; expect < ARQ_LADDER_LEVELS; expect++)
    {
        clean_ack_cycle();
        TEST_ASSERT_EQUAL_INT(expect, sess.speed_level);
    }
    /* Top of the ladder holds. */
    clean_ack_cycle();
    TEST_ASSERT_EQUAL_INT(ARQ_LADDER_LEVELS - 1, sess.speed_level);
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_QAM16C2, sess.payload_mode);
}

/* Any retry steps the ladder down one rung and ends the fast ramp; afterwards
 * it takes ARQ_LADDER_UP_SUCCESSES clean deliveries to climb one rung. */
void test_ladder_retry_steps_down_then_slow_ramp(void)
{
    goto_connected();
    goto_wait_ack();

    /* Climb to level 3 via the fast ramp (one rung per clean delivery from
     * the start rung). */
    while (sess.speed_level < 3)
        clean_ack_cycle();
    TEST_ASSERT_EQUAL_INT(3, sess.speed_level);

    /* A retransmission (ACK timeout) steps the mode down and ends fast ramp. */
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);
    arq_event_t ev = make_event(ARQ_EV_TIMER_ACK);   /* retransmit */
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_TX_COMPLETE);              /* DATA_TX -> WAIT_ACK */
    arq_fsm_dispatch(&sess, &ev);
    /* The retry itself does not change speed_level (only the delivery outcome
     * does, on the next ACK); the ACK after a retx is "not clean". */
    ev = make_event(ARQ_EV_RX_ACK);
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(2, sess.speed_level);   /* stepped down one rung */

    /* Now the slow ramp: need ARQ_LADDER_UP_SUCCESSES clean deliveries to
     * climb one rung (default 2). */
    for (int i = 0; i < 8 && sess.dflow_state != ARQ_DFLOW_WAIT_ACK; i++)
    {
        if (sess.dflow_state == ARQ_DFLOW_DATA_TX)
        {
            ev = make_event(ARQ_EV_TIMER_ACK); arq_fsm_dispatch(&sess, &ev);
            ev = make_event(ARQ_EV_TX_COMPLETE); arq_fsm_dispatch(&sess, &ev);
        }
        else break;
    }
    int lvl = sess.speed_level;
    clean_ack_cycle();
    /* One clean delivery is not enough under the slow ramp (LADDER_UP>=2). */
    if (ARQ_LADDER_UP_SUCCESSES >= 2)
        TEST_ASSERT_EQUAL_INT(lvl, sess.speed_level);
    for (int i = 1; i < ARQ_LADDER_UP_SUCCESSES; i++)
        clean_ack_cycle();
    TEST_ASSERT_EQUAL_INT(lvl + 1, sess.speed_level);
}

/* One "dirty" delivery: from WAIT_ACK, a retransmit (TIMER_ACK) marks the
 * frame retx, then the ACK reports a non-clean outcome (step down). */
static void dirty_ack_cycle(void)
{
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_WAIT_ACK, sess.dflow_state);
    arq_event_t ev = make_event(ARQ_EV_TIMER_ACK);   /* retransmit */
    arq_fsm_dispatch(&sess, &ev);
    TEST_ASSERT_EQUAL_INT(ARQ_DFLOW_DATA_TX, sess.dflow_state);
    ev = make_event(ARQ_EV_TX_COMPLETE);             /* DATA_TX -> WAIT_ACK */
    arq_fsm_dispatch(&sess, &ev);
    ev = make_event(ARQ_EV_RX_ACK);                  /* non-clean delivery */
    arq_fsm_dispatch(&sess, &ev);
    /* Re-enter WAIT_ACK for the next frame (ISS retains the turn). */
    for (int i = 0; i < 8 && sess.dflow_state != ARQ_DFLOW_WAIT_ACK; i++)
    {
        if (sess.dflow_state == ARQ_DFLOW_DATA_TX)
        {
            ev = make_event(ARQ_EV_TIMER_ACK); arq_fsm_dispatch(&sess, &ev);
            ev = make_event(ARQ_EV_TX_COMPLETE); arq_fsm_dispatch(&sess, &ev);
        }
        else break;
    }
}

/* Ladder never drops below the MFSK floor no matter how many retries — and a
 * session that opens above the floor still reaches it by stepping down. */
void test_ladder_floor_holds_at_mfsk(void)
{
    goto_connected();
    goto_wait_ack();
    TEST_ASSERT_EQUAL_INT(ARQ_LADDER_START_LEVEL, sess.speed_level);

    for (int i = 0; i < ARQ_LADDER_START_LEVEL; i++)
        dirty_ack_cycle();
    TEST_ASSERT_EQUAL_INT(0, sess.speed_level);
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK, sess.payload_mode);

    for (int i = 0; i < 5; i++)
    {
        dirty_ack_cycle();
        TEST_ASSERT_EQUAL_INT(0, sess.speed_level);
        TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK, sess.payload_mode);
    }
}

/* The IRS holds its RX window open after ACCEPT waiting for the ISS's first
 * data frame.  If that window is shorter than the burst, the IRS gives up
 * mid-burst and retransmits ACCEPT on top of it -- and while it transmits it is
 * deaf, so it destroys the very frame that would have told it to stop, then
 * does it again to the retransmission.  Measured on loopsim before the fix: a
 * 9000 ms window against a 13530 ms MFSK burst, retrying every 12.7 s, 0 bytes
 * delivered.  The window must outlast the LONGEST rung on the ladder. */
/* Everything that has to survive an in-flight burst must be sized from the
 * ladder's slowest rung, never from whichever mode is selected at the moment
 * the question is asked -- the payload mode moves during a session, and a
 * guard derived from it shrinks with it.  That is what chopped a 13.5 s MFSK
 * burst: the RX backlog cap briefly recomputed itself for a fast mode (~7 s)
 * and flushed a burst that was still arriving. */
void test_longest_ladder_burst_is_the_slowest_rung(void)
{
    float longest = arq_protocol_longest_burst_s();
    float slowest = 0.0f;
    for (int i = 0; i < ARQ_LADDER_LEVELS; i++)
    {
        const arq_mode_timing_t *tm = arq_protocol_mode_timing(arq_mode_ladder[i]);
        TEST_ASSERT_NOT_NULL(tm);
        if (tm->frame_duration_s > slowest)
            slowest = tm->frame_duration_s;
    }
    printf("  slowest ladder rung: %.2f s; longest_burst_s reports %.2f s\n",
           (double)slowest, (double)longest);
    TEST_ASSERT_TRUE(longest >= slowest);

    /* And it must not collapse to a fast rung's duration just because some
     * other mode is momentarily active. */
    for (int i = 0; i < ARQ_LADDER_LEVELS; i++)
    {
        const arq_mode_timing_t *tm = arq_protocol_mode_timing(arq_mode_ladder[i]);
        TEST_ASSERT_TRUE_MESSAGE(longest >= tm->frame_duration_s,
            "longest burst is shorter than a rung the session can select");
    }
}

void test_accept_rx_window_outlasts_longest_ladder_burst(void)
{
    float longest_s = 0.0f;
    int   longest_mode = -1;
    for (int i = 0; i < ARQ_LADDER_LEVELS; i++)
    {
        const arq_mode_timing_t *tm = arq_protocol_mode_timing(arq_mode_ladder[i]);
        TEST_ASSERT_NOT_NULL(tm);
        if (tm->frame_duration_s > longest_s)
        {
            longest_s   = tm->frame_duration_s;
            longest_mode = arq_mode_ladder[i];
        }
    }
    TEST_ASSERT_TRUE(longest_s > 0.0f);

    uint32_t window_ms = arq_protocol_accept_rx_window_ms();
    uint32_t burst_ms  = (uint32_t)(longest_s * 1000.0f);

    printf("  longest ladder burst: mode %d, %.2f s; ACCEPT RX window %.2f s\n",
           longest_mode, (double)longest_s, window_ms / 1000.0);

    /* Strictly longer, with room for the ISS's turnaround before it keys. */
    TEST_ASSERT_TRUE_MESSAGE(window_ms > burst_ms,
        "ACCEPT RX window is shorter than the slowest ladder burst: the IRS "
        "will key up inside the ISS's data and go deaf for its own transmission");
    TEST_ASSERT_TRUE_MESSAGE(window_ms >= burst_ms + (uint32_t)ARQ_ISS_POST_ACK_GUARD_MS,
        "ACCEPT RX window leaves no room for the ISS post-ACK guard");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_longest_ladder_burst_is_the_slowest_rung);
    RUN_TEST(test_accept_rx_window_outlasts_longest_ladder_burst);
    RUN_TEST(test_probe_frame_fits_the_rung_below);
    RUN_TEST(test_failed_probe_retreats_on_the_air);
    RUN_TEST(test_ladder_starts_one_rung_above_the_floor);
    RUN_TEST(test_ladder_table_ordered_and_sized);
    RUN_TEST(test_ladder_fast_ramp_climbs_one_per_clean);
    RUN_TEST(test_ladder_retry_steps_down_then_slow_ramp);
    RUN_TEST(test_ladder_floor_holds_at_mfsk);
    UNITY_END();
    return 0;
}
