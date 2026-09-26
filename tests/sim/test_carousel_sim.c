/* tests/sim/test_carousel_sim.c -- the carousel data plane through the real
 * ARQ FSM, on the two-FSM sim
 *
 * test_carousel runs the carousel alone; this runs it inside arq_fsm.c: the
 * CALL/ACCEPT connect (the ACCEPT is the first poll), the session CRC seed,
 * the payload decoder binding (sim_core delivers a payload frame only in the
 * mode the receiver is bound to), keydowns of separate bursts, the lost-peer
 * watchdog and the DISCONNECT.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "sim_clock.h"
#include "sim_channel.h"
#include "sim_endpoint.h"
#include "sim_core.h"

#include "arq_fsm.h"
#include "arq.h"
#include "freedv_api.h"

#include <stdio.h>
#include <string.h>

void setUp(void)    { arq_fsm_set_carousel(true); }
void tearDown(void) {}

static sim_t *make_sim(uint64_t seed, double per)
{
    sim_channel_cfg_t chan = { .seed = seed, .per = per, .guard_ms = 100 };
    sim_t *s = sim_create(&chan, "A0AAA", "B0BBB");
    TEST_ASSERT_NOT_NULL(s);
    sim_set_half_duplex(s, true);
    sim_set_carrier_sense(s, true, 400);
    return s;
}

static void connect_ab(sim_t *s)
{
    arq_event_t listen = { .id = ARQ_EV_APP_LISTEN };
    sim_inject(s, sim_b(s), &listen);
    arq_event_t conn = { .id = ARQ_EV_APP_CONNECT };
    snprintf(conn.remote_call, CALLSIGN_MAX_SIZE, "%s", "B0BBB");
    sim_inject(s, sim_a(s), &conn);
}

static void queue(sim_t *s, sim_endpoint_t *ep, const uint8_t *data, size_t len)
{
    sim_endpoint_queue_tx(ep, data, len);
    arq_event_t dready = { .id = ARQ_EV_APP_DATA_READY };
    sim_inject(s, ep, &dready);
}

static void fill(uint8_t *b, size_t n, int k)
{
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)((i * (size_t)k + 7) & 0xFF);
}

/* Delivered is an exact prefix of sent. */
static bool intact_prefix(sim_endpoint_t *dst, const uint8_t *sent, size_t sent_len, size_t *got)
{
    static uint8_t rx[64 * 1024];
    size_t n = sim_endpoint_delivered(dst, rx, sizeof(rx));
    if (got) *got = n;
    return n <= sent_len && memcmp(rx, sent, n) == 0;
}

static arq_conn_state_t state(sim_endpoint_t *ep) { return sim_endpoint_session(ep)->conn_state; }

void test_car_connect_seeds_both_ends(void)
{
    sim_t *s = make_sim(1, 0.0);
    connect_ab(s);
    sim_run_until_idle(s, 120000);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, state(sim_a(s)));
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, state(sim_b(s)));
    TEST_ASSERT_TRUE(sim_endpoint_session(sim_a(s))->car_active);
    TEST_ASSERT_TRUE(sim_endpoint_session(sim_b(s))->car_active);
    uint16_t sa = sim_endpoint_crc_seed(sim_a(s)), sb = sim_endpoint_crc_seed(sim_b(s));
    TEST_ASSERT_NOT_EQUAL(0, sa);
    TEST_ASSERT_EQUAL_UINT16(sa, sb);
    sim_destroy(s);
}

/* Data queued after the connect: the idle station takes the turn. */
void test_car_transfer_after_connect(void)
{
    static uint8_t blob[8192];
    fill(blob, sizeof(blob), 13);
    sim_t *s = make_sim(42, 0.0);
    connect_ab(s);
    sim_run_until_idle(s, 120000);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, state(sim_a(s)));
    queue(s, sim_a(s), blob, sizeof(blob));
    sim_run_until_idle(s, 1800000);
    size_t got;
    TEST_ASSERT_TRUE(intact_prefix(sim_b(s), blob, sizeof(blob), &got));
    TEST_ASSERT_EQUAL_size_t(sizeof(blob), got);
    TEST_ASSERT_EQUAL_INT(0, sim_collisions(s));
    sim_destroy(s);
}

/* Both have data before the connect: the first round, then a handover. */
void test_car_bidirectional(void)
{
    static uint8_t ab[8192], ba[8192];
    fill(ab, sizeof(ab), 13);
    fill(ba, sizeof(ba), 29);
    sim_t *s = make_sim(7, 0.02);
    sim_endpoint_queue_tx(sim_a(s), ab, sizeof(ab));
    sim_endpoint_queue_tx(sim_b(s), ba, sizeof(ba));
    connect_ab(s);
    sim_run_until_idle(s, 1800000);
    size_t got_b, got_a;
    TEST_ASSERT_TRUE(intact_prefix(sim_b(s), ab, sizeof(ab), &got_b));
    TEST_ASSERT_TRUE(intact_prefix(sim_a(s), ba, sizeof(ba), &got_a));
    TEST_ASSERT_EQUAL_size_t(sizeof(ab), got_b);
    TEST_ASSERT_EQUAL_size_t(sizeof(ba), got_a);
    TEST_ASSERT_EQUAL_INT(0, sim_collisions(s));
    sim_destroy(s);
}

void test_car_lossy(void)
{
    static uint8_t blob[4096];
    fill(blob, sizeof(blob), 5);
    sim_t *s = make_sim(3, 0.25);
    sim_endpoint_queue_tx(sim_a(s), blob, sizeof(blob));
    connect_ab(s);
    sim_run_until_idle(s, 3600000);
    size_t got;
    TEST_ASSERT_TRUE(intact_prefix(sim_b(s), blob, sizeof(blob), &got));
    TEST_ASSERT_EQUAL_size_t(sizeof(blob), got);
    sim_destroy(s);
}

/* The peer vanishes mid-transfer: the session ends, it does not hang. */
void test_car_peer_loss_disconnects(void)
{
    static uint8_t blob[16384];
    fill(blob, sizeof(blob), 3);
    sim_t *s = make_sim(9, 0.0);
    sim_endpoint_queue_tx(sim_a(s), blob, sizeof(blob));
    connect_ab(s);
    sim_run_until_idle(s, 20000);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, state(sim_a(s)));
    sim_set_per(s, 1.0);
    sim_run_until_idle(s, 900000);
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CONNECTED, state(sim_a(s)));
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CONNECTED, state(sim_b(s)));
    sim_destroy(s);
}

/* DISCONNECT after the data: both ends leave the session, the seed is gone. */
void test_car_disconnect_after_transfer(void)
{
    static uint8_t blob[2048];
    fill(blob, sizeof(blob), 11);
    sim_t *s = make_sim(5, 0.0);
    sim_endpoint_queue_tx(sim_a(s), blob, sizeof(blob));
    connect_ab(s);
    sim_run_until_idle(s, 600000);
    size_t got;
    TEST_ASSERT_TRUE(intact_prefix(sim_b(s), blob, sizeof(blob), &got));
    TEST_ASSERT_EQUAL_size_t(sizeof(blob), got);
    arq_event_t disc = { .id = ARQ_EV_APP_DISCONNECT };
    sim_inject(s, sim_a(s), &disc);
    sim_run_until_idle(s, 300000);
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CONNECTED, state(sim_a(s)));
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CONNECTED, state(sim_b(s)));
    TEST_ASSERT_EQUAL_UINT16(0, sim_endpoint_crc_seed(sim_a(s)));
    TEST_ASSERT_EQUAL_UINT16(0, sim_endpoint_crc_seed(sim_b(s)));
    sim_destroy(s);
}

/* Across channels and seeds: never corrupt, never stuck.  A run either
 * delivers everything or the session ends; it never sits connected with data
 * undelivered and nothing scheduled. */
void test_car_fuzz(void)
{
    static uint8_t ab[3000], ba[3000];
    fill(ab, sizeof(ab), 13);
    fill(ba, sizeof(ba), 29);
    static const struct { const char *name; double per; double snr; int nvis; double fade; } CH[] = {
        { "clean", 0.0, 0, 0, 0 }, { "10%", 0.10, 0, 0, 0 }, { "25%", 0.25, 0, 0, 0 },
        { "cliff 3", 0.02, 3, 0, 0 }, { "cliff 10", 0.02, 10, 0, 0 }, { "nvis", 0.02, 0, 1, 0 },
        { "fade 8", 0.02, 0, 0, 8 }, { "fade 15", 0.02, 0, 0, 15 },
    };
    static const sim_mode_per_t NVIS[] = {
        { FREEDV_MODE_DATAC15, 0.20 }, { FREEDV_MODE_DATAC16, 0.20 },
        { FREEDV_MODE_DATAC4,  0.45 }, { FREEDV_MODE_DATAC3,  0.67 },
        { FREEDV_MODE_DATAC1,  0.89 }, { FREEDV_MODE_DATAC17, 0.93 },
        { FREEDV_MODE_QAM16C2, 0.95 },
    };
    for (size_t c = 0; c < sizeof(CH) / sizeof(CH[0]); c++)
        for (uint64_t seed = 1; seed <= 8; seed++)
        {
            char what[64];
            snprintf(what, sizeof(what), "%s seed %llu", CH[c].name, (unsigned long long)seed);
            sim_t *s = make_sim(seed, CH[c].per);
            if (CH[c].snr) sim_set_snr(s, CH[c].snr);
            if (CH[c].nvis) sim_set_mode_per(s, NVIS, (int)(sizeof(NVIS) / sizeof(NVIS[0])), 10.0f);
            if (CH[c].fade) sim_set_fading(s, CH[c].fade, 1.0);
            sim_endpoint_queue_tx(sim_a(s), ab, sizeof(ab));
            sim_endpoint_queue_tx(sim_b(s), ba, sizeof(ba));
            connect_ab(s);
            sim_run_until_idle(s, 4 * 3600000ULL);
            size_t got_b, got_a;
            TEST_ASSERT_TRUE_MESSAGE(intact_prefix(sim_b(s), ab, sizeof(ab), &got_b), what);
            TEST_ASSERT_TRUE_MESSAGE(intact_prefix(sim_a(s), ba, sizeof(ba), &got_a), what);
            bool complete = got_b == sizeof(ab) && got_a == sizeof(ba);
            bool ended = state(sim_a(s)) != ARQ_CONN_CONNECTED || state(sim_b(s)) != ARQ_CONN_CONNECTED;
            TEST_ASSERT_TRUE_MESSAGE(complete || ended, what);
            sim_destroy(s);
        }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_car_connect_seeds_both_ends);
    RUN_TEST(test_car_transfer_after_connect);
    RUN_TEST(test_car_bidirectional);
    RUN_TEST(test_car_lossy);
    RUN_TEST(test_car_peer_loss_disconnects);
    RUN_TEST(test_car_disconnect_after_transfer);
    RUN_TEST(test_car_fuzz);
    return UNITY_END();
}
