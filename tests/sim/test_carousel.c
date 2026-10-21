/* tests/sim/test_carousel.c -- the carousel data plane's invariants on the
 * sim's channel: every run completes, delivers exactly what was sent, and
 * never keys over the peer.
 *
 * Timing is not asserted, except that an idle station keeps hearing its peer
 * within the lost-peer watchdog's 240 s: tests/sim/README.md has the
 * throughput matrix, run with carousel_bench.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "unity.h"
#include "carousel_sim.h"
#include "carousel.h"
#include "modem_mfsk.h"
#include "freedv_api.h"

#include <stdio.h>
#include <stdlib.h>

#define LIMIT_MS (8ULL * 3600 * 1000)

void setUp(void) {}
/* Here, not at the end of a test: a failed assertion leaves the test at once,
 * and the next one would run with its sensing. */
void tearDown(void)
{
    unsetenv("CAR_CS_DECODABLE"); unsetenv("CAR_CHAT"); unsetenv("CAR_CHAT_REPLY_MS"); unsetenv("CAR_BW");
    unsetenv("CAR_SCRIPT");
}

static void check(uint64_t seed, const char *chan, bool bidir)
{
    car_sim_result_t r;
    char what[64];
    carousel_sim_run(seed, chan, bidir, LIMIT_MS, &r);
    snprintf(what, sizeof(what), "seed %llu %s %s", (unsigned long long)seed, chan, bidir ? "bidir" : "oneway");
    TEST_ASSERT_TRUE_MESSAGE(r.intact, what);
    TEST_ASSERT_FALSE_MESSAGE(r.stalled, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.collisions, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.bad_frames, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.overruns, what);
    TEST_ASSERT_EQUAL_MESSAGE(CAR_SIM_BYTES, r.a2b, what);
    TEST_ASSERT_EQUAL_MESSAGE(bidir ? CAR_SIM_BYTES : 0, r.b2a, what);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, r.done_ms, what);
}

static const char *CHANNELS[] = {
    "clean", "awgn:0.10", "awgn:0.25", "cliff:3", "cliff:10", "nvis",
    "fade:3:0.5", "fade:8:1.0", "fade:15:0.1", "fade:25:1.0",
    /* the MFSK floor, answered by patterns */
    "cliff:-9", "cliff:-11", "fade:-9:0.5",
};

static void test_carousel_bidir_completes_intact(void)
{
    for (size_t c = 0; c < sizeof(CHANNELS) / sizeof(CHANNELS[0]); c++)
        for (uint64_t seed = 1; seed <= 10; seed++)
            check(seed, CHANNELS[c], true);
}

static void test_carousel_oneway_completes_intact(void)
{
    for (size_t c = 0; c < sizeof(CHANNELS) / sizeof(CHANNELS[0]); c++)
        for (uint64_t seed = 1; seed <= 5; seed++)
            check(seed, CHANNELS[c], false);
}

/* A link one way below the control mode: its receiver's control frames go on
 * MFSK, and each end learns which way from the other's control-deaf report.
 * With carrier sense only on what the listener can sync on (as on air), the
 * control-mode-only plane keyed over itself on these -- asym:14:-10 26
 * collisions, asym:-9:3 465 and 2 of 8 unfinished, cliff:-9 740 -- and on air
 * a gateway at 2 % polled a deaf estacao2 for minutes. */
static void test_carousel_floor_control_plane(void)
{
    static const char *ASYM[] = { "asym:14:-10", "asym:-10:14", "asym:-9:3", "cliff:-9",
                                  /* a marginal probe rung the listener cannot
                                   * sync on: no quick re-poll over it */
                                  "cliff:-5", "asym:-6:14" };
    setenv("CAR_CS_DECODABLE", "1", 1);
    for (size_t c = 0; c < sizeof(ASYM) / sizeof(ASYM[0]); c++)
        for (uint64_t seed = 1; seed <= 4; seed++)
            check(seed, ASYM[c], true);
    unsetenv("CAR_CS_DECODABLE");
}

/* A poll off a floor stream, lost: the sender carries on at the floor, and a
 * receiver bound to the rung it asked for neither hears nor senses it.  Its
 * re-poll keyed into that round -- on air (car23, 3 %) 3.7 s into the
 * gateway's 27 s MFSK; here 2-7 collisions on the fade:-5 seeds below and on
 * these asym:-6:14 bidir seeds -- until it listened at the floor first.
 * (fade:-5 seeds 1, 3 and 7 keep one collision each of another kind: the
 * sender's 90 s silence nudge, after two polls lost in a fade, landing on the
 * receiver's third, which it cannot sense either.) */
static void test_carousel_lost_probe_poll(void)
{
    static const uint64_t SEEDS[] = { 2, 4, 5, 6 };
    setenv("CAR_CS_DECODABLE", "1", 1);
    for (size_t i = 0; i < sizeof(SEEDS) / sizeof(SEEDS[0]); i++)
        check(SEEDS[i], "fade:-5:0.5", false);
    check(8, "asym:-6:14", true);
    check(11, "asym:-6:14", true);
    unsetenv("CAR_CS_DECODABLE");
}

/* Under fading a round on the air is not always synced on, so not sensed:
 * taking silence at the sense check for a lost poll, the quick re-poll keyed
 * into rounds -- 226 collisions in 20 one-way fade:3 runs, 178 on nvis --
 * where waiting for the window keys over none. */
static void test_carousel_fading_no_blind_repoll(void)
{
    static const char *FADE[] = { "fade:3:0.5", "fade:8:1.0", "nvis" };
    setenv("CAR_CS_DECODABLE", "1", 1);
    for (size_t c = 0; c < sizeof(FADE) / sizeof(FADE[0]); c++)
        for (uint64_t seed = 1; seed <= 4; seed++)
            check(seed, FADE[c], false);
    for (uint64_t seed = 1; seed <= 2; seed++) {
        check(seed, "fade:3:0.5", true);
        check(seed, "fade:8:1.0", true);
    }
    unsetenv("CAR_CS_DECODABLE");
}

/* Both ways at the deep floor, under fading: the receiver takes the turn
 * with a handover on MFSK after its turn quantum, in place of a pattern, and
 * the sender, continuing at the pattern delay, keyed into it -- 19, 12 and
 * 11 collisions on these seeds -- until it waited for the handover once the
 * quantum was near. */
static void test_carousel_floor_handover_not_keyed_over(void)
{
    setenv("CAR_CS_DECODABLE", "1", 1);
    for (uint64_t seed = 1; seed <= 3; seed++)
        check(seed, "fade:-9:0.5", true);
}

/* Both ways in a fade: the receiver, its quantum due, answers a floor round
 * with a handover that carries its own first round; the sender, missing the
 * handover, took the silence for "keep going" and keyed into that round --
 * 4-5 collisions on each of these seeds -- until it held for the peer once its
 * turn was due. */
static void test_carousel_sender_holds_for_peer_turn(void)
{
    static const uint64_t SEEDS[] = { 11, 13, 14, 17 };
    setenv("CAR_CS_DECODABLE", "1", 1);
    for (size_t i = 0; i < sizeof(SEEDS) / sizeof(SEEDS[0]); i++)
        check(SEEDS[i], "fade:-5:0.5", true);
}

/* A DISCONNECT waits for what is left to be delivered, for as long as a few
 * exchanges take on the rung in use.  A fixed 30 s dropped the final UUCP
 * reply at the floor (on air, car29), where one exchange is 40-50 s. */
/* Request/response traffic, as UUCP's: each message is written only after
 * the peer's answer to the last is delivered, so a direction goes idle and
 * comes back many times in a session.  A REQ about a round that opened a
 * block the callee never heard got "done" for an answer -- the callee had
 * not taken M's block state from it -- and the session went on for ever on
 * a rung the callee had left (these seeds, before the fix). */
static void test_carousel_request_response_completes(void)
{
    static const struct { const char *chan; uint64_t seed; } RUNS[] = {
        { "fade:5:0.5", 14 }, { "fade:0:0.5", 13 }, { "nvis", 3 }, { "nvis", 4 }, { "nvis", 8 },
        { "awgn:0.1", 1 }, { "cliff:-5", 1 },
    };
    setenv("CAR_CS_DECODABLE", "1", 1);
    setenv("CAR_CHAT", "10:200", 1);
    /* A quick application, and one slow enough that both ends wait for it
     * (APP_GRACE_MS): S's answer then starts late in its slot. */
    for (size_t i = 0; i < 2 * sizeof(RUNS) / sizeof(RUNS[0]); i++) {
        size_t k = i % (sizeof(RUNS) / sizeof(RUNS[0]));
        setenv("CAR_CHAT_REPLY_MS", i < sizeof(RUNS) / sizeof(RUNS[0]) ? "200" : "1500", 1);
        car_sim_result_t r;
        char what[64];
        carousel_sim_run(RUNS[k].seed, RUNS[k].chan, false, LIMIT_MS, &r);
        snprintf(what, sizeof(what), "seed %llu %s chat %s", (unsigned long long)RUNS[k].seed, RUNS[k].chan,
                 getenv("CAR_CHAT_REPLY_MS"));
        TEST_ASSERT_TRUE_MESSAGE(r.intact, what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.collisions, what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.overruns, what);
        TEST_ASSERT_EQUAL_MESSAGE(2000, r.a2b, what);
        TEST_ASSERT_EQUAL_MESSAGE(2000, r.b2a, what);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, r.done_ms, what);
    }
}

/* Idle on a lossy link, with patterns (the floor's continuation slots): each
 * station keeps hearing the other within the 240 s after which the FSM's
 * lost-peer watchdog ends the session.  An unanswered idle poll was asked
 * again only at the backoff (60 s), and these seeds went 240 s without a
 * frame: cliff:10:0.2 13 of 20, cliff:-5:0.1 and cliff:-9:0.1 9 of 20.  A
 * try takes longer at the floor (37 s with MFSK control), so some runs still
 * do there: of seeds 1-100, cliff:-9:0.1 1 (seed 98), cliff:-5:0.2 7 and
 * cliff:-9:0.2 9. */
static void test_carousel_idle_keeps_hearing_the_peer(void)
{
    static const char *CHANS[] = { "cliff:10:0.2", "cliff:-5:0.1", "cliff:-9:0.1" };
    setenv("CAR_CS_DECODABLE", "1", 1);
    setenv("CAR_SCRIPT", "A30,B30+1800000", 1);   /* 30 min idle between two lines */
    for (size_t c = 0; c < sizeof(CHANS) / sizeof(CHANS[0]); c++)
        for (uint64_t seed = 1; seed <= 20; seed++) {
            car_sim_result_t r;
            char what[64];
            carousel_sim_run(seed, CHANS[c], false, LIMIT_MS, &r);
            snprintf(what, sizeof(what), "seed %llu %s", (unsigned long long)seed, CHANS[c]);
            TEST_ASSERT_TRUE_MESSAGE(r.intact, what);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0, r.done_ms, what);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.collisions, what);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.overruns, what);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.kd_overlaps, what);
            TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(240000, r.silent_ms[0], what);
            TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(240000, r.silent_ms[1], what);
        }
}

static void test_carousel_drain_budget_fits_the_floor(void)
{
    car_t *c = calloc(1, sizeof(*c));
    car_io_t io = {0};
    car_init(c, &io, 0, 0);
    c->tx_level = 0; c->poll_level = 0;
    char what[64];
    snprintf(what, sizeof(what), "floor drain budget %llu ms", (unsigned long long)car_drain_budget_ms(c));
    TEST_ASSERT_TRUE_MESSAGE(car_drain_budget_ms(c) >= 90000, what);
    free(c);
}

/* Both ends cut the session into the same pieces: the caller from the rung
 * it read in the ACCEPT, the callee from the rung it put there.  23 bytes
 * only for a floor start (4 to an MFSK frame, not 3), 24 above. */
static void test_carousel_piece_size_agrees(void)
{
    car_t *a = calloc(1, sizeof(*a)), *b = calloc(1, sizeof(*b));
    car_io_t io = {0};
    for (int lv = 0; lv < CAR_NLEVELS; lv++) {
        car_init(a, &io, 3, lv);   car_start_sender(a, 0);
        car_init(b, &io, lv, -1);  car_start_receiver(b, 0);
        TEST_ASSERT_EQUAL_INT(lv == 0 ? CAR_PIECE_FLOOR : CAR_PIECE, car_piece(a));
        TEST_ASSERT_EQUAL_INT(car_piece(a), car_piece(b));
        TEST_ASSERT_EQUAL_INT(car_piece_for_start(lv), car_piece(a));
    }
    free(a); free(b);
}

/* The bandwidth table, and the cap clamping the start rungs both ways. */
static void test_carousel_bandwidth_max_level(void)
{
    TEST_ASSERT_EQUAL_INT(3, car_max_level_for_bw(500));          /* DATAC3 */
    TEST_ASSERT_EQUAL_INT(CAR_NLEVELS - 1, car_max_level_for_bw(2300));
    TEST_ASSERT_EQUAL_INT(CAR_NLEVELS - 1, car_max_level_for_bw(2750));
    TEST_ASSERT_EQUAL_INT(CAR_NLEVELS - 1, car_max_level_for_bw(0));
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC3, car_level_mode(car_max_level_for_bw(500)));

    car_t *c = calloc(1, sizeof(*c));
    car_io_t io = {0};
    car_init(c, &io, 6, 5);
    car_set_max_level(c, car_max_level_for_bw(500));
    car_start_sender(c, 0);
    TEST_ASSERT_EQUAL_INT(3, c->snr_level);
    TEST_ASSERT_EQUAL_INT(3, c->peer_snr_level);
    TEST_ASSERT_EQUAL_INT(3, c->tx_level);
    TEST_ASSERT_EQUAL_INT(3, c->poll_level);
    /* rung 0 is the session's floor; the rest of the ladder is everyone's */
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK, car_session_level_mode(c, 0));
    car_set_floor_mode(c, MERCURY_MODE_MFSK_N32);
    TEST_ASSERT_EQUAL_INT(MERCURY_MODE_MFSK_N32, car_session_level_mode(c, 0));
    TEST_ASSERT_EQUAL_INT(FREEDV_MODE_DATAC15, car_session_level_mode(c, 1));
    free(c);
}

/* A BW500 session on its own floor, the 500 Hz MFSK (26 s frames, one a
 * keydown): where the 1 kHz floor no longer decodes (-14 dB), on fades, and
 * with one direction at the floor and the other well above it, every byte
 * arrives intact and no keydown meets another. */
static void test_carousel_bw500_floor(void)
{
    static const char *CH[] = { "cliff:-14", "fade:-9:0.5", "asym:-14:5", "step:5:-14:300" };
    setenv("CAR_BW", "500", 1);
    setenv("CAR_CS_DECODABLE", "1", 1);
    for (size_t c = 0; c < sizeof(CH) / sizeof(CH[0]); c++)
        for (uint64_t seed = 1; seed <= 2; seed++) {
            check(seed, CH[c], false);
            check(seed, CH[c], true);
        }
    unsetenv("CAR_BW");
}

/* The TX-MFSK-only capability floors only the affected direction: mine caps
 * my transmit rung, the peer's caps the rung I choose for what it sends me. */
static void test_carousel_tx_mfsk_caps(void)
{
    car_t *c = calloc(1, sizeof(*c));
    car_io_t io = {0};
    car_init(c, &io, 4, 3);
    car_set_max_level(c, CAR_NLEVELS - 1);

    /* I can only transmit MFSK: my TX direction floored, my RX not. */
    car_set_tx_mfsk(c, true, false);
    TEST_ASSERT_EQUAL_INT(0, c->tx_max);
    TEST_ASSERT_EQUAL_INT(CAR_NLEVELS - 1, c->rx_max);
    TEST_ASSERT_EQUAL_INT(4, c->snr_level);       /* RX direction untouched */
    TEST_ASSERT_EQUAL_INT(0, c->peer_snr_level);  /* TX direction floored */

    /* The peer can only transmit MFSK: its TX direction (my RX) floored. */
    c->snr_level = 4; c->peer_snr_level = 3;
    car_set_tx_mfsk(c, false, true);
    TEST_ASSERT_EQUAL_INT(CAR_NLEVELS - 1, c->tx_max);
    TEST_ASSERT_EQUAL_INT(0, c->rx_max);
    TEST_ASSERT_EQUAL_INT(0, c->snr_level);
    TEST_ASSERT_EQUAL_INT(3, c->peer_snr_level);

    /* Both: everything at the floor. */
    c->snr_level = 4; c->peer_snr_level = 3;
    car_set_tx_mfsk(c, true, true);
    TEST_ASSERT_EQUAL_INT(0, c->tx_max);
    TEST_ASSERT_EQUAL_INT(0, c->rx_max);
    TEST_ASSERT_EQUAL_INT(0, c->snr_level);
    TEST_ASSERT_EQUAL_INT(0, c->peer_snr_level);
    free(c);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_carousel_bidir_completes_intact);
    RUN_TEST(test_carousel_oneway_completes_intact);
    RUN_TEST(test_carousel_floor_control_plane);
    RUN_TEST(test_carousel_lost_probe_poll);
    RUN_TEST(test_carousel_fading_no_blind_repoll);
    RUN_TEST(test_carousel_floor_handover_not_keyed_over);
    RUN_TEST(test_carousel_sender_holds_for_peer_turn);
    RUN_TEST(test_carousel_drain_budget_fits_the_floor);
    RUN_TEST(test_carousel_piece_size_agrees);
    RUN_TEST(test_carousel_bandwidth_max_level);
    RUN_TEST(test_carousel_bw500_floor);
    RUN_TEST(test_carousel_tx_mfsk_caps);
    RUN_TEST(test_carousel_request_response_completes);
    RUN_TEST(test_carousel_idle_keeps_hearing_the_peer);
    return UNITY_END();
}
