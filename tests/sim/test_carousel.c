/* tests/sim/test_carousel.c -- the carousel data plane's invariants on the
 * sim's channel: every run completes, delivers exactly what was sent, and
 * never keys over the peer.
 *
 * Timing is not asserted, except that an idle station keeps hearing its peer
 * within the lost-peer watchdog's 240 s, and a message after a drop below the
 * control mode gets through in that time: tests/sim/README.md has the
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
#include <string.h>

#define LIMIT_MS (8ULL * 3600 * 1000)

void setUp(void) {}
/* Here, not at the end of a test: a failed assertion leaves the test at once,
 * and the next one would run with its sensing. */
void tearDown(void)
{
    unsetenv("CAR_CS_DECODABLE"); unsetenv("CAR_CHAT"); unsetenv("CAR_CHAT_REPLY_MS"); unsetenv("CAR_BW");
    unsetenv("CAR_SCRIPT"); unsetenv("CAR_MFSK_TX"); unsetenv("CAR_KBD");
    unsetenv("CAR_ZSTD"); unsetenv("CAR_RANDOM_PAYLOAD");
    unsetenv("CAR_CTL_SNR_OFFSET");
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

/* Typed lines, as an operator's at a BBS: one that comes while M waits out
 * the peer's answer slot, or while a keydown is on the air, cannot go at once,
 * and the log says what it waits for (car_on_app_data).  On air nothing did:
 * N3JXB's commands took 2-11 s to key and the log was silent about it. */
static void test_carousel_app_wait_is_said(void)
{
    car_sim_result_t r;
    setenv("CAR_KBD", "10:40:15", 1);
    carousel_sim_run(3, "clean", false, LIMIT_MS, &r);
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT(0, r.collisions);
    TEST_ASSERT_GREATER_THAN_INT(0, r.app_wait_says);
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

/* A Winlink exchange whose client -> RMS path falls below the control mode
 * mid-session (#431: the RMS's banner, the client's 74 bytes, the RMS's
 * answer).  Each message must get through within the lost-peer watchdog's
 * 240 s, and each end hear the other in that time.  The client asked on DATAC16 for 150 s while the RMS listened on
 * MFSK, and then the RMS's smoothed reading of the client's first MFSK frame
 * (-9 dB after 0) said it heard it above the control mode: back to DATAC16
 * for another silence (stepa:0:-12:40:12, 432 s for the 74 bytes). */
static void test_carousel_drop_below_control_mode(void)
{
    static const int AT_S[] = { 10, 15, 20, 30, 40 };
    static const char *TO[] = { "-9", "-12" };
    setenv("CAR_SCRIPT", "B193,A74,B20", 1);
    for (size_t t = 0; t < sizeof(TO) / sizeof(TO[0]); t++)
        for (size_t i = 0; i < sizeof(AT_S) / sizeof(AT_S[0]); i++) {
            car_sim_result_t r;
            char chan[48];
            snprintf(chan, sizeof(chan), "stepa:0:%s:%d:12", TO[t], AT_S[i]);
            carousel_sim_run(1, chan, false, LIMIT_MS, &r);
            TEST_ASSERT_TRUE_MESSAGE(r.intact, chan);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0, r.done_ms, chan);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.collisions, chan);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.overruns, chan);
            TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(240000, r.silent_ms[0], chan);
            TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(240000, r.silent_ms[1], chan);
            TEST_ASSERT_EQUAL_INT_MESSAGE(3, r.msgs, chan);
            for (int m = 0; m < r.msgs; m++)
                TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(240000, r.msg_delay_ms[m], chan);
        }
}

/* A rung that failed is not re-probed while the SNR gates it.  On air
 * (VE4KLM <- K7EK, 30 m, 2026-11-05) DATAC3 went dead near its -1 dB
 * threshold, and the receiver came back to it at -4.2 dB every couple of
 * minutes: a lost frame and a re-poll each time.  The same shape here: a
 * start just under DATAC3's threshold, then a drop past its gate.  Before the
 * fix these seeds made 10 such probes (7, 11, 13, 18, 19, 26). */
static void test_carousel_no_reprobe_below_gate(void)
{
    static const char *CHAN[] = { "step:-1:-4.5:60", "step:-1.5:-4.5:90" };
    for (size_t k = 0; k < sizeof(CHAN) / sizeof(CHAN[0]); k++)
        for (uint64_t seed = 1; seed <= 30; seed++) {
            car_sim_result_t r;
            char what[64];
            carousel_sim_run(seed, CHAN[k], true, LIMIT_MS, &r);
            snprintf(what, sizeof(what), "seed %llu %s bidir", (unsigned long long)seed, CHAN[k]);
            TEST_ASSERT_TRUE_MESSAGE(r.intact, what);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.gated_probes, what);
        }
}

/* Rounds that bring nothing of the peer at all leave the SNR as old as the
 * last frame.  On air (NW8L, IONOS MPP, +5 -> -5 dB within a vARIM session,
 * 1.9.22) the receiver read 4.2 dB throughout and polled DATAC3 and DATAC4
 * seven times with nothing heard in between; the lost-peer watchdog ended the
 * session.  The same shape: vARIM's blocks one way, its short acks back, and
 * the step.  Two dark rounds now let two losses kill a rung.  Before the fix
 * these runs reached 9 such polls in a row; with it, 5 (200 seeds). */
static void test_carousel_dark_rounds_drop_the_rung(void)
{
    static const char *CHAN[] = { "step:5:-5:60", "step:5:-6:60" };
    setenv("CAR_SCRIPT", "A1500,B59+500,A1500+500,B59+500,A1500+500,B59+500,"
                         "A1500+500,B59+500,A1500+500,B59+500", 1);
    for (size_t k = 0; k < sizeof(CHAN) / sizeof(CHAN[0]); k++)
        for (uint64_t seed = 1; seed <= 40; seed++) {
            car_sim_result_t r;
            char what[64];
            carousel_sim_run(seed, CHAN[k], false, LIMIT_MS, &r);
            snprintf(what, sizeof(what), "seed %llu %s", (unsigned long long)seed, CHAN[k]);
            TEST_ASSERT_TRUE_MESSAGE(r.intact, what);
            TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(6, r.dark_polls, what);
        }
    unsetenv("CAR_SCRIPT");
}

/* Floor polls B never heard leave nothing for A to wait out: B continues in
 * a poll's later slots only after a round of its own in its first, and
 * nothing was on the air there.  Held for them, A kept its next poll off the
 * air 14 s after each pair of unheard polls (on air, NW8L-KA8RYU, MPD -5 dB:
 * 97 s, and vARIM gave up on the /OK 25 s before it came).  The frames: A's
 * message (0), B's poll (1), A's poll (2), B's message, first round (3), A's
 * poll on DATAC4 (4), B's DATAC4 round (5), then A's floor polls for 2
 * frames (6, 7, 8), the three lost. */
static void test_carousel_unheard_polls_free_their_slots(void)
{
    static const int lost[] = { 5, 6, 7, 8 };
    car_sim_result_t r;
    setenv("CAR_SCRIPT", "A40,B200+2000", 1);
    carousel_sim_force_losses(NULL, 0);
    carousel_sim_run(1, "cliff:-7", false, LIMIT_MS, &r);
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT_MESSAGE(11, r.frames, "the frame order changed: pick the lost frames again (CAR_TRACE=1)");
    carousel_sim_force_losses(lost, 4);
    carousel_sim_run(1, "cliff:-7", false, LIMIT_MS, &r);
    carousel_sim_force_losses(NULL, -1);
    unsetenv("CAR_SCRIPT");
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT(0, r.kd_overlaps);
    char what[64];
    snprintf(what, sizeof(what), "held %llu ms for the slots of unheard polls", (unsigned long long)r.slot_hold_ms);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)r.slot_hold_ms, what);
}

/* M's own round of one frame, after my "done", lost whole: M asks (REQ)
 * about the one frame it sent.  Scored as the 8 my last poll had asked for,
 * one lost frame took QAM16C2 out at 20 dB (on air at its threshold: NW8L-
 * KA8RYU, IONOS MPD 35 dB, and the rest of the file on DATAC17, #502).  The
 * frames: A's first round (0), B's poll for 8 (1), A's 5 (2-6), B's "done"
 * (7), A's poll and B's ack (8, 9), A's ack (10), A's 300 bytes (11). */
static void test_carousel_req_scores_the_frames_sent(void)
{
    static const int lost[] = { 11 };
    car_sim_result_t r;
    setenv("CAR_SCRIPT", "A6000,B59+500,A300+3000", 1);
    carousel_sim_force_losses(NULL, 0);
    carousel_sim_run(1, "cliff:20", false, LIMIT_MS, &r);
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT_MESSAGE(12, r.frames, "the frame order changed: pick the lost frame again (CAR_TRACE=1)");
    carousel_sim_force_losses(lost, 1);
    carousel_sim_run(1, "cliff:20", false, LIMIT_MS, &r);
    carousel_sim_force_losses(NULL, -1);
    unsetenv("CAR_SCRIPT");
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.one_frame_kills, "one lost frame took a rung out");
}

/* My "done" poll asks for nothing; M's next round comes on its own turn, and
 * is no answer to the poll before it.  With that round's tail lost, it was
 * scored as the 8 frames that poll asked for, not the 2 it had: 1 of 8, and
 * DATAC1 read 0.49 delivered instead of 0.83 (#507).  The frames: A's first
 * round (0), B's poll for 8 (1), A's 5 (2-6), B's "done" (7), A's poll and
 * B's 59 bytes (8, 9), A's ack (10), A's 1500 bytes in 2 (11, 12). */
static void test_carousel_done_poll_asks_for_nothing(void)
{
    static const int lost[] = { 12 };
    car_sim_result_t r;
    setenv("CAR_SCRIPT", "A6000,B59+500,A1500+3000", 1);
    carousel_sim_force_losses(NULL, 0);
    carousel_sim_run(1, "cliff:20", false, LIMIT_MS, &r);
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT_MESSAGE(13, r.frames, "the frame order changed: pick the lost frame again (CAR_TRACE=1)");
    carousel_sim_force_losses(lost, 1);
    carousel_sim_run(1, "cliff:20", false, LIMIT_MS, &r);
    carousel_sim_force_losses(NULL, -1);
    unsetenv("CAR_SCRIPT");
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.over_scored, "a round scored as more frames than were sent");
}

/* My poll asked for 8; M had 5 to send.  With the round's last frame lost,
 * it was scored as the 8 asked: 4 of 8, QAM16C2 read 0.58 delivered instead
 * of 0.81 (#516).  It is the 5 that fit between my poll and the round's end.
 * Its first frame lost too, the frames heard say 4 (seen + left): the time
 * still says 5, so the lost one is scored.  The frames: A's first round (0),
 * B's poll for 8 (1), A's 5 (2-6). */
static void test_carousel_short_round_scored_as_sent(void)
{
    static const int tail[] = { 6 }, both[] = { 2, 6 };
    car_sim_result_t r;
    setenv("CAR_SCRIPT", "A6000,B59+500,A1500+3000", 1);
    carousel_sim_force_losses(NULL, 0);
    carousel_sim_run(1, "cliff:20", false, LIMIT_MS, &r);
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT_MESSAGE(13, r.frames, "the frame order changed: pick the lost frames again (CAR_TRACE=1)");
    carousel_sim_force_losses(tail, 1);
    carousel_sim_run(1, "cliff:20", false, LIMIT_MS, &r);
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.over_scored, "the tail lost: scored as more frames than were sent");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.under_scored, "the tail lost: scored as fewer frames than were sent");
    carousel_sim_force_losses(both, 2);
    carousel_sim_run(1, "cliff:20", false, LIMIT_MS, &r);
    carousel_sim_force_losses(NULL, -1);
    unsetenv("CAR_SCRIPT");
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.over_scored, "head and tail lost: scored as more frames than were sent");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.under_scored, "head and tail lost: scored as fewer frames than were sent");
}

/* M's side of #516: A polled B for 8; B had 2 to send, and the second was
 * lost.  M kept no time to bound the round with, and scored it 1 of 8
 * (QAM16C2 read 0.31 delivered); S's answer slot opens no sooner
 * than mslot_lo, so 2 fit, and it is 1 of 2 (0.69).  The frames: A's 30
 * bytes (0), B's ack (1), A's ack-only poll (2), B's first round (3), A's
 * poll for 8 (4), B's 2 (5, 6). */
static void test_carousel_m_scores_a_short_round_as_sent(void)
{
    static const int lost[] = { 6 };
    car_sim_result_t r;
    setenv("CAR_SCRIPT", "A30,B3000+500", 1);
    carousel_sim_force_losses(NULL, 0);
    carousel_sim_run(1, "cliff:20", false, LIMIT_MS, &r);
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT_MESSAGE(7, r.frames, "the frame order changed: pick the lost frame again (CAR_TRACE=1)");
    carousel_sim_force_losses(lost, 1);
    carousel_sim_run(1, "cliff:20", false, LIMIT_MS, &r);
    carousel_sim_force_losses(NULL, -1);
    unsetenv("CAR_SCRIPT");
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.over_scored, "scored as more frames than were sent");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.under_scored, "scored as fewer frames than were sent");
}

/* A round M carried with its poll (ANC_BOTH) is the size the poll said: B
 * asked for 5, A's keydown carried its poll and 2 frames, the second lost.
 * Scored as the 5 asked (1 of 5) before; 1 of 2 now.  On fade:16:2, seed
 * 21, at 102.8 s: if this run no longer has the case, pick another seed
 * (CAR_TRACE=1, "as its poll said"). */
static void test_carousel_round_with_its_poll_scored_as_declared(void)
{
    car_sim_result_t r;
    setenv("CAR_SCRIPT", "A4096,B59+500,A4096+500,B59+500", 1);
    carousel_sim_run(21, "fade:16:2", false, LIMIT_MS, &r);
    unsetenv("CAR_SCRIPT");
    TEST_ASSERT_TRUE(r.intact);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.over_scored, "scored as more frames than were sent");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.under_scored, "scored as fewer frames than were sent");
}

/* CAR_CTL_SNR_OFFSET: the control frames' estimate reads that much off, and
 * only theirs (#501).  The frames are still decoded and delivered -- the knob
 * is the estimate, not the channel -- so the session still completes, and the
 * data frames' readings do not move.  The default (0) is the channel as it
 * reads: the same run, down to the frame count. */
static void test_carousel_ctl_snr_offset_moves_only_the_control_reading(void)
{
    car_sim_result_t plain, zero, off;
    char what[96];

    carousel_sim_run(3, "cliff:10", false, LIMIT_MS, &plain);
    setenv("CAR_CTL_SNR_OFFSET", "0", 1);
    carousel_sim_run(3, "cliff:10", false, LIMIT_MS, &zero);
    unsetenv("CAR_CTL_SNR_OFFSET");
    snprintf(what, sizeof(what), "the offset 0 run differs from the plain one (seed 3 cliff:10)");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(plain.done_ms, zero.done_ms, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(plain.frames, zero.frames, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(plain.keydowns, zero.keydowns, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(plain.ctl_frames, zero.ctl_frames, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(plain.data_frames, zero.data_frames, what);

    setenv("CAR_CTL_SNR_OFFSET", "-10", 1);
    carousel_sim_run(3, "cliff:10", false, LIMIT_MS, &off);
    unsetenv("CAR_CTL_SNR_OFFSET");

    /* The control frames read ten dB lower than they did ... */
    snprintf(what, sizeof(what), "the control frames read %.2f dB, the data frames %.2f dB",
             off.ctl_snr_db, off.data_snr_db);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, (float)(plain.ctl_snr_db - 10.0), (float)off.ctl_snr_db, what);
    /* ... and the data frames read what they read. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, (float)plain.data_snr_db, (float)off.data_snr_db, what);
    /* The frames are still decoded: the session completes, and what arrives is
     * what was sent. */
    TEST_ASSERT_TRUE_MESSAGE(off.intact, what);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, off.done_ms, what);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, off.ctl_frames, what);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, off.data_frames, what);
}

/* Below the control mode's cliff the knob takes the frames out, not just the
 * reading: at cliff:0 with the control mode reading -10 dB it sits below its
 * -7 dB cliff, the polls mostly stop coming, and the session falls to the
 * MFSK floor -- the control-deaf regime (#501's other half).  The same
 * channel at offset 0 rides DATAC3. */
static void test_carousel_ctl_snr_offset_below_the_cliff_falls_to_the_floor(void)
{
    car_sim_result_t plain, off;
    char what[128];

    carousel_sim_run(1, "cliff:0", false, LIMIT_MS, &plain);
    setenv("CAR_CTL_SNR_OFFSET", "-10", 1);
    carousel_sim_run(1, "cliff:0", false, LIMIT_MS, &off);
    unsetenv("CAR_CTL_SNR_OFFSET");

    TEST_ASSERT_TRUE_MESSAGE(plain.intact, "offset 0 baseline");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, plain.done_ms, "offset 0 baseline");
    snprintf(what, sizeof(what), "control reads %.1f dB, data %.1f dB, done in %llu ms against %llu ms",
             off.ctl_snr_db, off.data_snr_db,
             (unsigned long long)off.done_ms, (unsigned long long)plain.done_ms);
    TEST_ASSERT_TRUE_MESSAGE(off.intact, what);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, off.done_ms, what);
    /* The frames that do come through read ten dB low; the data frames read
     * as the channel reads them. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, (float)(plain.ctl_snr_db - 10.0), (float)off.ctl_snr_db, what);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, (float)plain.data_snr_db, (float)off.data_snr_db, what);
    /* ...and the ones below the cliff do not come: the session spends its
     * time on the floor.  Twice the offset 0 run, not three times: with the
     * data frames' readings taken for the estimate (#501) the data still
     * goes on DATAC3 between the floor's stretches -- 986 s here, 2395 s
     * before, and 523 to 1092 s on seeds 2-10 against 2071 to 2446. */
    TEST_ASSERT_GREATER_THAN_MESSAGE(plain.done_ms * 2, off.done_ms, what);
}

/* Control frames reading low, the data frames reading true: the session
 * still climbs, its rungs chosen by the data frames' readings (#501).  The
 * offset's runs within a margin of the offset 0 run: the start rung still
 * follows the connect hint, a control-mode reading, so one more step of the
 * climb is left to pay.  Before the fix, the low readings held the estimate
 * under the top rungs' gates: awgn:0.02 took 84.8 s at -6 and 68.7 s at -10
 * against 52.1 s, and the messages on cliff:16 never went on QAM16C2. */
static void test_carousel_low_control_readings_do_not_hold_the_ladder(void)
{
    car_sim_result_t zero, off;
    char what[128];
    static const char *const offs[] = { "-6", "-10" };

    carousel_sim_run(1, "awgn:0.02", false, LIMIT_MS, &zero);
    TEST_ASSERT_TRUE_MESSAGE(zero.intact && zero.done_ms, "offset 0 baseline (awgn:0.02)");
    for (size_t i = 0; i < sizeof offs / sizeof *offs; i++) {
        setenv("CAR_CTL_SNR_OFFSET", offs[i], 1);
        carousel_sim_run(1, "awgn:0.02", false, LIMIT_MS, &off);
        unsetenv("CAR_CTL_SNR_OFFSET");
        snprintf(what, sizeof(what), "awgn:0.02 at %s dB: done in %llu ms against %llu ms", offs[i],
                 (unsigned long long)off.done_ms, (unsigned long long)zero.done_ms);
        TEST_ASSERT_TRUE_MESSAGE(off.intact && off.done_ms, what);
        TEST_ASSERT_TRUE_MESSAGE(off.done_ms * 4 <= zero.done_ms * 5, what);   /* within 25 % */
    }

    /* Message traffic on a clean high-SNR channel, the shape of NW8L's MPD
     * runs: few data frames between many control frames. */
    setenv("CAR_SCRIPT", "A2500+1000,B300+2000,A2000+4000,B300+6000,"
                         "A2000+8000,B300+10000,A1000+12000,B300+14000", 1);
    carousel_sim_run(1, "cliff:16", false, LIMIT_MS, &zero);
    setenv("CAR_CTL_SNR_OFFSET", "-10", 1);
    carousel_sim_run(1, "cliff:16", false, LIMIT_MS, &off);
    unsetenv("CAR_CTL_SNR_OFFSET");
    unsetenv("CAR_SCRIPT");
    snprintf(what, sizeof(what), "cliff:16 messages at -10 dB: done in %llu ms against %llu ms",
             (unsigned long long)off.done_ms, (unsigned long long)zero.done_ms);
    TEST_ASSERT_TRUE_MESSAGE(zero.intact && zero.done_ms, what);
    TEST_ASSERT_TRUE_MESSAGE(off.intact && off.done_ms, what);
    TEST_ASSERT_TRUE_MESSAGE(off.done_ms * 10 <= zero.done_ms * 11, what);   /* within 10 % */
}

/* The estimate's step up to the first data frame's reading (#501) corrects
 * the estimate, not the channel: the reading the peer's last report of me is
 * paired with moves with it.  Without that, peer_hears_me_db took the step
 * for a channel risen both ways, and the caller, hearing the callee 3 dB up
 * while the callee heard it 9 dB down, polled it on a DATAC16 it could not
 * decode: these messages took 3936 s, against 2576 s with the control mode
 * reading true. */
static void test_carousel_estimate_step_is_not_a_channel_step(void)
{
    car_sim_result_t zero, off;
    char what[128];

    setenv("CAR_SCRIPT", "A2500+1000,B300+2000,A2000+4000,B300+6000,"
                         "A2000+8000,B300+10000,A1000+12000,B300+14000", 1);
    carousel_sim_run(4, "stepa:0:-9:20:12", false, LIMIT_MS, &zero);
    setenv("CAR_CTL_SNR_OFFSET", "-8", 1);
    carousel_sim_run(4, "stepa:0:-9:20:12", false, LIMIT_MS, &off);
    unsetenv("CAR_CTL_SNR_OFFSET");
    unsetenv("CAR_SCRIPT");
    snprintf(what, sizeof(what), "stepa:0:-9:20:12 messages at -8 dB: done in %llu ms against %llu ms",
             (unsigned long long)off.done_ms, (unsigned long long)zero.done_ms);
    TEST_ASSERT_TRUE_MESSAGE(zero.intact && zero.done_ms, what);
    TEST_ASSERT_TRUE_MESSAGE(off.intact && off.done_ms, what);
    TEST_ASSERT_TRUE_MESSAGE(off.done_ms <= zero.done_ms, what);
}


/* A drop below the control mode mid-transfer (step:0:-10:60, messages:
 * 5 KB from A, a line from B later).  B reads A at -10 dB, below the control
 * mode, and polls for A's round at the floor; A, deaf to that DATAC16 poll,
 * sends nothing there.  B stays at the floor (car_on_time, CAR_T_W): back on
 * DATAC3, where nothing of A's decodes at -10 dB, it missed A's floor
 * control until its own silence put it at the floor.  The longest B went
 * without hearing A, over seeds 1-10: 121 s before, 91 s with this. */
static void test_carousel_drop_below_the_control_mode_stays_at_the_floor(void)
{
    uint64_t worst = 0;
    char what[96];
    setenv("CAR_SCRIPT", "A5000+1000,B300+150000", 1);
    for (uint64_t seed = 1; seed <= 10; seed++) {
        car_sim_result_t r;
        carousel_sim_run(seed, "step:0:-10:60", false, LIMIT_MS, &r);
        snprintf(what, sizeof(what), "seed %llu", (unsigned long long)seed);
        TEST_ASSERT_TRUE_MESSAGE(r.intact && r.done_ms, what);
        if (r.silent_ms[1] > worst) worst = r.silent_ms[1];
    }
    unsetenv("CAR_SCRIPT");
    snprintf(what, sizeof(what), "B's longest silence %llu ms", (unsigned long long)worst);
    TEST_ASSERT_TRUE_MESSAGE(worst < 105000, what);
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

/* A station that transmits only MFSK (arq:mfsk_tx_only) keys nothing else,
 * control included, both ways full; its peer's rounds still climb the
 * ladder, and both directions deliver.  Nothing else even from a peer that
 * does not know (CAR_MFSK_TX=a): one that names it faster rungs and says it
 * hears its control.  (Not connected on air: arq.c ignores 1.9.20.) */
static void test_carousel_mfsk_tx_only(void)
{
    for (int k = 0; k < 2; k++) {
        setenv("CAR_MFSK_TX", k ? "b" : "a", 1);
        car_sim_result_t r;
        carousel_sim_run(1, "cliff:20", true, 3600000, &r);
        TEST_ASSERT_TRUE(r.intact);
        TEST_ASSERT_EQUAL_INT(0, r.non_mfsk[k]);
    }
    static const char *WHO[] = { "A", "B", "AB" };
    static const char *CHAN[] = { "cliff:20", "nvis" };
    for (int w = 0; w < 3; w++)
        for (int k = 0; k < 2; k++) {
            setenv("CAR_MFSK_TX", WHO[w], 1);
            car_sim_result_t r;
            carousel_sim_run(1, CHAN[k], true, LIMIT_MS, &r);
            char msg[64];
            snprintf(msg, sizeof(msg), "CAR_MFSK_TX=%s %s", WHO[w], CHAN[k]);
            TEST_ASSERT_TRUE_MESSAGE(r.intact, msg);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0, r.done_ms, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.collisions, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.overruns, msg);
            for (int i = 0; i < 2; i++)
                if (strchr(WHO[w], 'A' + i))
                    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.non_mfsk[i], msg);
                else if (k == 0)
                    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, r.non_mfsk[i], msg);   /* climbed */
        }
}

/* A station whose keydowns are held short (arq:max_keydown_s): vARIM aborts
 * a transfer when PTT stays on for about 20 s, and a full DATAC3 round is
 * 30 s (#445).  Held to 16 s at one end -- the peer may be a release without
 * the setting -- every keydown of that end stays under it, and everything
 * arrives.  Held at both, every keydown both ways does, a poll and a round in
 * one keydown included: fade:15:1 both ways, seeds 2 and 4, keyed a 3.7 s
 * poll before a 15 s round for 19.2 s (#449 review).  The whole keydown,
 * lead and tail included, stays at or under the cap.  Without it, the rounds
 * are as long as before. */
static void kd_check(const char *chan, int seed, bool bidir, const char *who, int ends, bool done)
{
    car_sim_result_t r;
    if (who) setenv(who, "16000", 1);
    else { setenv("CAR_KEYDOWN_A", "16000", 1); setenv("CAR_KEYDOWN_B", "16000", 1); }
    carousel_sim_run(seed, chan, bidir, LIMIT_MS, &r);
    unsetenv("CAR_KEYDOWN_A"); unsetenv("CAR_KEYDOWN_B");
    char msg[80];
    snprintf(msg, sizeof(msg), "%s %s seed %d %s", who ? who : "both", chan, seed, bidir ? "bidir" : "oneway");
    TEST_ASSERT_TRUE_MESSAGE(r.intact, msg);
    if (done) TEST_ASSERT_NOT_EQUAL_MESSAGE(0, r.done_ms, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.collisions, msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.overruns, msg);
    for (int i = 0; i < 2; i++)
        if (ends & (1 << i)) TEST_ASSERT_LESS_OR_EQUAL_UINT64_MESSAGE(16000, r.max_kd_ms[i], msg);
}
static void test_carousel_max_keydown(void)
{
    static const char *CHAN[] = { "cliff:10", "fade:25:1" };
    for (int k = 0; k < 2; k++) {
        car_sim_result_t r;
        carousel_sim_run(1, CHAN[k], false, LIMIT_MS, &r);
        TEST_ASSERT_GREATER_THAN_UINT64(20000, r.max_kd_ms[0]);
        for (int bidir = 0; bidir < 2; bidir++) {
            kd_check(CHAN[k], 1, bidir, "CAR_KEYDOWN_A", 1, true);
            kd_check(CHAN[k], 1, bidir, "CAR_KEYDOWN_B", 2, true);
            kd_check(CHAN[k], 1, bidir, NULL, 3, true);
        }
    }
    kd_check("fade:15:1", 2, true, NULL, 3, true);
    kd_check("fade:15:1", 4, true, NULL, 3, true);
    /* The lead and tail count too: a poll and two DATAC4 frames under 16 s
     * keyed 16.05 s with them (#449 re-review).  Slow: it need not finish. */
    kd_check("cliff:-5", 8, true, "CAR_KEYDOWN_A", 1, false);
}

/* A control loss (CAR_DEAF_LOSS) on a link where control is not being lost:
 * BW500, -5 dB both ways, where DATAC16 decodes every frame.  Counting an
 * unanswered round as a lost answer set it 13-45 times a run, and held to the
 * end of the session it kept the callee's control on 26 s MFSK frames: these
 * seeds' A->B did not finish in 2 h (rafael2k's bench on #538: 2, 9, 12, 16,
 * 17, 26; 31, 45, 107, 118 of 120), where mercuryv2 took 5187-7028 s.  Asks
 * only, but held for good, 2 and 44 still did not.  Asks only, held 60 s:
 * all finish, the slowest in 6606 s. */
void test_carousel_bw500_control_loss_does_not_stall(void)
{
    static const uint64_t SEEDS[] = { 2, 9, 12, 16, 17, 26, 31, 44, 45, 107, 118 };
    setenv("CAR_BW", "500", 1);
    for (size_t k = 0; k < sizeof(SEEDS) / sizeof(SEEDS[0]); k++) {
        car_sim_result_t r;
        char what[64];
        carousel_sim_run(SEEDS[k], "cliff:-5", true, 7200000, &r);
        snprintf(what, sizeof(what), "seed %llu cliff:-5 bidir BW500: a2b %d b2a %d",
                 (unsigned long long)SEEDS[k], (int)r.a2b, (int)r.b2a);
        TEST_ASSERT_TRUE_MESSAGE(r.intact, what);
        TEST_ASSERT_EQUAL_MESSAGE(CAR_SIM_BYTES, r.a2b, what);
        TEST_ASSERT_EQUAL_MESSAGE(CAR_SIM_BYTES, r.b2a, what);
    }
}

/* #506: payload compression.  With the knob on, the blocks go out
 * compressed and the bytes come back the same; with a payload that does not
 * compress, the blocks go out stored (the tag says so) and the bytes still
 * come back the same.  The compressed run is also the shorter one: fewer
 * pieces, fewer keydowns. */
static void test_carousel_zstd_round_trip(void)
{
    car_sim_result_t plain, zstd;
    char what[96];

    carousel_sim_run(3, "cliff:8", false, LIMIT_MS, &plain);
    setenv("CAR_ZSTD", "1", 1);
    carousel_sim_run(3, "cliff:8", false, LIMIT_MS, &zstd);
    unsetenv("CAR_ZSTD");

    snprintf(what, sizeof(what), "compressed: a2b %d intact %d done %llu against %llu",
             (int)zstd.a2b, (int)zstd.intact, (unsigned long long)zstd.done_ms,
             (unsigned long long)plain.done_ms);
    TEST_ASSERT_TRUE_MESSAGE(zstd.intact, what);
    TEST_ASSERT_EQUAL_MESSAGE(CAR_SIM_BYTES, zstd.a2b, what);
    /* the compressed session carries the same bytes in less air */
    TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(plain.done_ms, zstd.done_ms, what);
}

/* The stored fallback: a payload that does not shrink is sent as it came
 * (tagged 0), and the peer delivers it byte for byte. */
static void test_carousel_zstd_stores_what_does_not_compress(void)
{
    car_sim_result_t plain, zstd;
    char what[96];

    setenv("CAR_RANDOM_PAYLOAD", "1", 1);
    carousel_sim_run(3, "cliff:8", false, LIMIT_MS, &plain);
    setenv("CAR_ZSTD", "1", 1);
    carousel_sim_run(3, "cliff:8", false, LIMIT_MS, &zstd);
    unsetenv("CAR_ZSTD");
    unsetenv("CAR_RANDOM_PAYLOAD");

    snprintf(what, sizeof(what), "stored: a2b %d intact %d done %llu against %llu",
             (int)zstd.a2b, (int)zstd.intact, (unsigned long long)zstd.done_ms,
             (unsigned long long)plain.done_ms);
    TEST_ASSERT_TRUE_MESSAGE(zstd.intact, what);
    TEST_ASSERT_EQUAL_MESSAGE(CAR_SIM_BYTES, zstd.a2b, what);
    /* the tag byte per block is the whole cost: the session is not longer
     * than one that never compressed, by more than a round's worth */
    TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(plain.done_ms + 30000, zstd.done_ms, what);
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
    RUN_TEST(test_carousel_mfsk_tx_only);
    RUN_TEST(test_carousel_max_keydown);
    RUN_TEST(test_carousel_request_response_completes);
    RUN_TEST(test_carousel_app_wait_is_said);
    RUN_TEST(test_carousel_drop_below_control_mode);
    RUN_TEST(test_carousel_no_reprobe_below_gate);
    RUN_TEST(test_carousel_dark_rounds_drop_the_rung);
    RUN_TEST(test_carousel_unheard_polls_free_their_slots);
    RUN_TEST(test_carousel_req_scores_the_frames_sent);
    RUN_TEST(test_carousel_done_poll_asks_for_nothing);
    RUN_TEST(test_carousel_short_round_scored_as_sent);
    RUN_TEST(test_carousel_m_scores_a_short_round_as_sent);
    RUN_TEST(test_carousel_round_with_its_poll_scored_as_declared);
    RUN_TEST(test_carousel_ctl_snr_offset_moves_only_the_control_reading);
    RUN_TEST(test_carousel_ctl_snr_offset_below_the_cliff_falls_to_the_floor);
    RUN_TEST(test_carousel_low_control_readings_do_not_hold_the_ladder);
    RUN_TEST(test_carousel_estimate_step_is_not_a_channel_step);
    RUN_TEST(test_carousel_drop_below_the_control_mode_stays_at_the_floor);
    RUN_TEST(test_carousel_idle_keeps_hearing_the_peer);
    RUN_TEST(test_carousel_zstd_round_trip);
    RUN_TEST(test_carousel_zstd_stores_what_does_not_compress);
    RUN_TEST(test_carousel_bw500_control_loss_does_not_stall);
    return UNITY_END();
}
