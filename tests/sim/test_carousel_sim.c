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
#include "modem_mfsk.h"
#include "sim_clock.h"
#include "sim_channel.h"
#include "sim_endpoint.h"
#include "sim_core.h"

#include "arq_fsm.h"
#include "arq.h"
#include "arq_timing.h"
#include "freedv_api.h"

#include <stdio.h>
#include <string.h>

extern int arq_test_session_bw_hz;   /* arq_test_stubs.c: the negotiated BW */

void setUp(void)    { }
void tearDown(void) { arq_test_session_bw_hz = ARQ_BANDWIDTH_FULL_HZ; }

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
    /* The session's byte counters (the UI's bytes transmitted / received)
     * follow the carousel too; they stayed at 0 on air (Pedro, 2026-10-02). */
    const arq_timing_ctx_t *tm = sim_timing(s);
    TEST_ASSERT_EQUAL_UINT64(sizeof(blob), tm->tx_bytes);
    TEST_ASSERT_EQUAL_UINT64(sizeof(blob), tm->rx_bytes);
    TEST_ASSERT_TRUE(tm->frames_tx > 0 && tm->frames_rx > 0);
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

/* Run for ms of virtual time; the keydowns ep began in it. */
static int keydowns_in(sim_t *s, sim_endpoint_t *ep, uint64_t ms)
{
    int n = 0;
    bool was = sim_keyed(s, ep);
    for (uint64_t t = 0; t < ms; t += 100) {
        sim_run_until_idle(s, 100);
        bool k = sim_keyed(s, ep);
        if (k && !was) n++;
        was = k;
    }
    return n;
}

/* Connected both ends, nothing queued either way, at snr_db. */
static sim_t *idle_session(uint64_t seed, double snr_db)
{
    sim_t *s = make_sim(seed, 0.0);
    sim_set_snr(s, snr_db);
    connect_ab(s);
    for (int i = 0; i < 1200 && !(state(sim_a(s)) == ARQ_CONN_CONNECTED &&
                                  state(sim_b(s)) == ARQ_CONN_CONNECTED); i++)
        sim_run_until_idle(s, 100);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, state(sim_a(s)));
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, state(sim_b(s)));
    return s;
}

/* #384: the peer leaves an idle session.  The lost-peer watchdog ran only
 * with data in flight, and the caller polled the vanished peer for 10 min,
 * until the operator aborted. */
void test_car_idle_peer_loss_disconnects(void)
{
    sim_t *s = idle_session(9, 10.0);
    sim_set_per(s, 1.0);
    sim_run_until_idle(s, 300000);
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CONNECTED, state(sim_a(s)));
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CONNECTED, state(sim_b(s)));
    sim_destroy(s);
}

/* #384: DISCONNECT after the peer has gone quiet: one try, not three. */
void test_car_disconnect_quiet_peer_one_try(void)
{
    sim_t *s = idle_session(9, 10.0);
    sim_set_per(s, 1.0);
    sim_run_until_idle(s, 150000);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, state(sim_a(s)));
    arq_event_t disc = { .id = ARQ_EV_APP_DISCONNECT };
    sim_inject(s, sim_a(s), &disc);
    int n = keydowns_in(s, sim_a(s), 120000);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CONNECTED, state(sim_a(s)));
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_DISCONNECTING, state(sim_a(s)));
    sim_destroy(s);
}

/* #384: Winlink's tail -- the client sends FF, the RMS answers FQ, and the
 * client hangs up as FQ arrives.  Everything was delivered, but the caller's
 * DISCONNECT waited for its carousel to go idle, after an ack and an idle
 * poll: 41 s, past the B2 client's 30 s before it forces the disconnect. */
void test_car_disconnect_after_reply_is_prompt(void)
{
    sim_t *s = idle_session(1, 10.0);
    uint8_t ff[40], fq[8];
    memset(ff, 'F', sizeof(ff)); memset(fq, 'Q', sizeof(fq));
    static uint8_t rx[256];
    queue(s, sim_a(s), ff, sizeof(ff));
    for (int i = 0; i < 6000 && sim_endpoint_delivered(sim_b(s), rx, sizeof(rx)) < sizeof(ff); i++)
        sim_run_until_idle(s, 100);
    queue(s, sim_b(s), fq, sizeof(fq));
    for (int i = 0; i < 6000 && sim_endpoint_delivered(sim_a(s), rx, sizeof(rx)) < sizeof(fq); i++)
        sim_run_until_idle(s, 100);
    TEST_ASSERT_EQUAL_size_t(sizeof(fq), sim_endpoint_delivered(sim_a(s), rx, sizeof(rx)));
    arq_event_t disc = { .id = ARQ_EV_APP_DISCONNECT };
    sim_inject(s, sim_a(s), &disc);
    sim_run_until_idle(s, 20000);
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CONNECTED, state(sim_a(s)));
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_DISCONNECTING, state(sim_a(s)));
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CONNECTED, state(sim_b(s)));
    TEST_ASSERT_EQUAL_INT(0, sim_collisions(s));
    sim_destroy(s);
}

/* The callee's ACCEPTs never reach the caller: it falls back to LISTENING
 * still seeded, to catch the caller's first round in case the caller did
 * connect.  The caller never comes, and the seed must not outlive the wait:
 * a seeded station drops every plain frame, broadcast included. */
void test_car_accept_fallback_seed_expires(void)
{
    sim_t *s = make_sim(3, 0.0);
    connect_ab(s);
    for (int i = 0; i < 200 && state(sim_b(s)) != ARQ_CONN_ACCEPTING; i++)
        sim_run_until_idle(s, 100);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_ACCEPTING, state(sim_b(s)));
    sim_set_per(s, 1.0);
    sim_run_until_idle(s, 900000);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_LISTENING, state(sim_b(s)));
    TEST_ASSERT_EQUAL_UINT16(0, sim_endpoint_crc_seed(sim_b(s)));
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

/* ABORT mid-transfer (#218): the session goes at once, without delivering
 * the rest first as DISCONNECT does -- and the peer is told, not left to
 * time out. */
void test_car_abort_skips_the_drain(void)
{
    static uint8_t blob[16384];
    fill(blob, sizeof(blob), 7);
    sim_t *s = make_sim(5, 0.0);
    sim_endpoint_queue_tx(sim_a(s), blob, sizeof(blob));
    connect_ab(s);
    sim_run_until_idle(s, 30000);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, state(sim_a(s)));
    arq_event_t ab = { .id = ARQ_EV_APP_DISCONNECT, .abort = true };
    sim_inject(s, sim_a(s), &ab);
    sim_run_until_idle(s, 60000);
    size_t got;
    TEST_ASSERT_TRUE(intact_prefix(sim_b(s), blob, sizeof(blob), &got));
    TEST_ASSERT_TRUE_MESSAGE(got < sizeof(blob), "ABORT drained the whole transfer first");
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CONNECTED, state(sim_a(s)));
    TEST_ASSERT_NOT_EQUAL(ARQ_CONN_CONNECTED, state(sim_b(s)));
    sim_destroy(s);
}

/* Request and reply, as UUCP talks: A's request arrives, B's application
 * answers while B's last poll (nothing left either way) is on the air.  The
 * reply came too late for that poll and B was not idle yet, so it waited for
 * a turn that never came: both ends sat connected and silent (on air,
 * 2026-09-28, until uucico's 120 s timeout). */
void test_car_reply_during_final_poll(void)
{
    static uint8_t req[17], rep[20];
    fill(req, sizeof(req), 3);
    fill(rep, sizeof(rep), 9);
    sim_t *s = make_sim(11, 0.0);
    connect_ab(s);
    sim_run_until_idle(s, 120000);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, state(sim_a(s)));
    queue(s, sim_a(s), req, sizeof(req));

    bool replied = false;
    for (int step = 0; step < 20000 && !replied; step++) {
        if (sim_run_until_idle(s, 10) == 0) break;          /* nothing left to happen */
        size_t got;
        if (intact_prefix(sim_b(s), req, sizeof(req), &got) && got == sizeof(req) &&
            sim_keyed(s, sim_b(s))) {
            queue(s, sim_b(s), rep, sizeof(rep));
            replied = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(replied, "B never keyed after the request");
    sim_run_until_idle(s, 600000);
    size_t got_a;
    TEST_ASSERT_TRUE(intact_prefix(sim_a(s), rep, sizeof(rep), &got_a));
    TEST_ASSERT_EQUAL_size_t(sizeof(rep), got_a);
    TEST_ASSERT_EQUAL_INT(0, sim_collisions(s));
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

/* The session bandwidth caps the rungs.  On a clean 20 dB channel a full
 * band session climbs onto the wideband modes; a BW500 one must stay at or
 * below DATAC3 both ways -- MFSK..DATAC3 -- and still deliver, and BW2750
 * has BW2300's ladder (no wider rung yet).  The full-band run shows the
 * channel lets the climb happen, so a pass at 500 is the cap, not the
 * channel. */
static int wideband_frames(sim_t *s)
{
    return sim_mode_frames(s, FREEDV_MODE_DATAC1) + sim_mode_frames(s, FREEDV_MODE_DATAC17) +
           sim_mode_frames(s, FREEDV_MODE_QAM16C2);
}

static sim_t *bw_transfer(int bw_hz, const uint8_t *blob, size_t len)
{
    arq_test_session_bw_hz = bw_hz;
    sim_t *s = make_sim(7, 0.0);
    sim_set_snr(s, 20.0);
    connect_ab(s);
    sim_run_until_idle(s, 120000);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, state(sim_a(s)));
    queue(s, sim_a(s), blob, len);
    queue(s, sim_b(s), blob, len);
    sim_run_until_idle(s, 3600000);
    size_t got;
    TEST_ASSERT_TRUE(intact_prefix(sim_b(s), blob, len, &got));
    TEST_ASSERT_EQUAL_size_t(len, got);
    TEST_ASSERT_TRUE(intact_prefix(sim_a(s), blob, len, &got));
    TEST_ASSERT_EQUAL_size_t(len, got);
    return s;
}

void test_car_bandwidth_caps_the_rungs(void)
{
    static uint8_t blob[8192];
    fill(blob, sizeof(blob), 29);

    sim_t *s = bw_transfer(ARQ_BANDWIDTH_FULL_HZ, blob, sizeof(blob));
    TEST_ASSERT_TRUE_MESSAGE(wideband_frames(s) > 0, "BW2300 never climbed past DATAC3");
    sim_destroy(s);

    s = bw_transfer(ARQ_BANDWIDTH_TACTICAL_HZ, blob, sizeof(blob));
    TEST_ASSERT_TRUE_MESSAGE(wideband_frames(s) > 0, "BW2750 never climbed past DATAC3");
    sim_destroy(s);

    s = bw_transfer(ARQ_BANDWIDTH_NARROW_HZ, blob, sizeof(blob));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, wideband_frames(s), "BW500 keyed a wideband mode");
    TEST_ASSERT_TRUE_MESSAGE(sim_mode_frames(s, FREEDV_MODE_DATAC3) > 0, "BW500 never reached DATAC3");
    sim_destroy(s);
}

/* A BW500 session's floor is the 500 Hz MFSK.  At -5 dB the CALL connects
 * on DATAC16 and both directions start on the floor (below DATAC15's start
 * at -3): both ends take the narrow floor from the negotiated bandwidth, and
 * nothing at all goes out on the 1 kHz one. */
void test_car_bw500_runs_on_the_narrow_floor(void)
{
    static uint8_t blob[400];
    fill(blob, sizeof(blob), 31);
    int bw = arq_conn.bw;
    arq_conn.bw = ARQ_BANDWIDTH_NARROW_HZ;        /* both ends' own (arq_get_bw) */
    arq_test_session_bw_hz = ARQ_BANDWIDTH_NARROW_HZ;
    sim_t *s = make_sim(3, 0.0);
    sim_set_snr(s, -5.0);
    connect_ab(s);
    sim_run_until_idle(s, 120000);
    TEST_ASSERT_EQUAL_INT(ARQ_CONN_CONNECTED, state(sim_a(s)));
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK_N32, sim_endpoint_session(sim_a(s))->floor_mode);
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK_N32, sim_endpoint_session(sim_b(s))->floor_mode);
    queue(s, sim_a(s), blob, sizeof(blob));
    queue(s, sim_b(s), blob, sizeof(blob));
    sim_run_until_idle(s, 4 * 3600000);
    size_t got;
    TEST_ASSERT_TRUE(intact_prefix(sim_b(s), blob, sizeof(blob), &got));
    TEST_ASSERT_EQUAL_size_t(sizeof(blob), got);
    TEST_ASSERT_TRUE(intact_prefix(sim_a(s), blob, sizeof(blob), &got));
    TEST_ASSERT_EQUAL_size_t(sizeof(blob), got);
    TEST_ASSERT_TRUE(sim_mode_frames(s, MERCURY_MODE_MFSK_N32) > 0);
    TEST_ASSERT_EQUAL_INT(0, sim_mode_frames(s, MERCURY_MODE_MFSK));
    TEST_ASSERT_EQUAL_INT(0, sim_collisions(s));
    sim_destroy(s);
    arq_conn.bw = bw;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_car_connect_seeds_both_ends);
    RUN_TEST(test_car_transfer_after_connect);
    RUN_TEST(test_car_bidirectional);
    RUN_TEST(test_car_lossy);
    RUN_TEST(test_car_peer_loss_disconnects);
    RUN_TEST(test_car_accept_fallback_seed_expires);
    RUN_TEST(test_car_idle_peer_loss_disconnects);
    RUN_TEST(test_car_disconnect_quiet_peer_one_try);
    RUN_TEST(test_car_disconnect_after_reply_is_prompt);
    RUN_TEST(test_car_disconnect_after_transfer);
    RUN_TEST(test_car_abort_skips_the_drain);
    RUN_TEST(test_car_reply_during_final_poll);
    RUN_TEST(test_car_fuzz);
    RUN_TEST(test_car_bandwidth_caps_the_rungs);
    RUN_TEST(test_car_bw500_runs_on_the_narrow_floor);
    return UNITY_END();
}
