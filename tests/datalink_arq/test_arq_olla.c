/*
 * OLLA (outer-loop link adaptation) unit tests.
 *
 * Drives the arq_olla_update() control loop in a closed loop against a
 * synthetic fading channel + the real mode-selection thresholds, and asserts:
 *   - the gear-shift CONVERGES without oscillation, and
 *   - it delivers MORE effective goodput than the old SNR-threshold +
 *     retry-downgrade + hold-timer scheme that oscillated (the behaviour that
 *     made trunk ~2x slower than v1.9.9 on a variable-SNR link).
 *
 * Effective goodput is modelled faithfully for stop-and-wait: payload of
 * first-try-OK frames divided by total airtime, where total airtime charges
 * every frame (failed ones are retransmitted) AND every mode change a MODE_REQ
 * round-trip — the dominant cost of the oscillation.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unity.h"
#include "arq_protocol.h"
#include "freedv/freedv_api.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* ladder model: {decode threshold dB, payload bytes, frame airtime s,
 * ack_timeout s} — ack_timeout is the wait before a failed frame is
 * retransmitted in stop-and-wait (the dominant cost of a fade-failed frame). */
static const struct { float thresh_db; int payload; float frame_s; float ack_to_s; const char *name; } LADDER[] = {
    { -10.0f,                 30,   4.40f, 11.0f, "DATAC15" },  /* floor, robust  */
    { ARQ_SNR_MIN_DATAC4_DB,  54,   5.80f, 13.0f, "DATAC4"  },
    { ARQ_SNR_MIN_DATAC3_DB,  126,  3.82f, 11.0f, "DATAC3"  },
    { ARQ_SNR_MIN_DATAC1_DB,  510,  4.81f, 12.0f, "DATAC1"  },
    { ARQ_SNR_MIN_DATAC17_DB, 1180, 7.40f, 14.0f, "DATAC17" },
    { ARQ_SNR_MIN_QAM16C2_DB, 1213, 3.70f, 11.0f, "QAM16C2" },
};
#define NLADDER ((int)(sizeof(LADDER)/sizeof(LADDER[0])))
#define MODE_CHANGE_COST_S 12.0f   /* MODE_REQ+MODE_ACK round-trip incl. guards */
#define ACK_OK_S            4.0f   /* IRS ACK return + guards on a clean frame   */

/* highest rank whose threshold (+1 dB upgrade hysteresis) is cleared — same
 * asymmetry as select_best_mode(). */
static int select_rank(float eff_snr, int cur_rank)
{
    int best = 0;
    for (int r = 1; r < NLADDER; r++) {
        float t = LADDER[r].thresh_db + (r > cur_rank ? ARQ_SNR_HYST_DB : 0.0f);
        if (eff_snr >= t) best = r;
    }
    return best;
}

/* deterministic reproducible exponential power fade (mean ~4.3 dB, deep tail) */
static float fade_db(uint32_t *s)
{
    *s = (*s) * 1664525u + 1013904223u;
    float u = ((*s >> 8) & 0xFFFFFF) / (float)0x1000000;
    if (u < 1e-6f) u = 1e-6f;
    return -10.0f * log10f(u);
}

/* Run the closed loop with either the OLLA offset (use_olla=1) or the OLD
 * gear-shift (use_olla=0: raw-SNR pick + forced-downgrade after 2 consecutive
 * fails + a fixed hold that then re-climbs on the still-high mean → flap). */
static void run_loop(float mean_snr, int nsteps, int warmup, bool use_olla,
                     float *out_fer, int *out_changes,
                     float *out_goodput_bps, int *out_final_rank)
{
    uint32_t seed = 12345;
    float offset = 0.0f;
    int rank = 0, prev_rank = 0, changes = 0, fails = 0, counted = 0;
    long delivered = 0;
    double airtime = 0.0;
    int consec_fail = 0, hold_cap = NLADDER - 1, hold_until = 0;
    const int HOLD = 4;

    for (int t = 0; t < nsteps; t++) {
        int sel;
        if (use_olla) {
            sel = select_rank(mean_snr + offset, rank);
        } else {
            sel = select_rank(mean_snr, rank);
            if (t >= hold_until) hold_cap = NLADDER - 1;
            if (sel > hold_cap) sel = hold_cap;
        }
        rank = sel;

        float inst = mean_snr - fade_db(&seed);
        bool ok = inst >= LADDER[rank].thresh_db;

        if (use_olla) {
            offset = arq_olla_update(offset, ok);
        } else if (!ok) {
            if (++consec_fail >= ARQ_RETRY_DOWNGRADE_THRESHOLD && rank > 0) {
                hold_cap = rank - 1; hold_until = t + HOLD; consec_fail = 0;
            }
        } else consec_fail = 0;

        if (t >= warmup) {
            airtime += LADDER[rank].frame_s;
            airtime += ok ? ACK_OK_S : LADDER[rank].ack_to_s; /* stop-and-wait cost */
            if (rank != prev_rank) { changes++; airtime += MODE_CHANGE_COST_S; }
            if (!ok) fails++; else delivered += LADDER[rank].payload;
            counted++;
        }
        prev_rank = rank;
    }
    *out_fer = counted ? (float)fails / counted : 0.0f;
    *out_changes = changes;
    *out_goodput_bps = airtime > 0 ? (float)(delivered / airtime) : 0.0f;
    *out_final_rank = rank;
}

/* 1. update clamps and steps in the right direction with the FER ratio. */
void test_olla_update_basic(void)
{
    float up = ARQ_OLLA_STEP_UP_DB, down = ARQ_OLLA_STEP_DOWN_DB;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, ARQ_OLLA_TARGET_FER, up / (up + down));
    TEST_ASSERT_TRUE(arq_olla_update(0.0f, false) < 0.0f);
    TEST_ASSERT_TRUE(arq_olla_update(0.0f, true)  > 0.0f);
    float lo = 0; for (int i = 0; i < 1000; i++) lo = arq_olla_update(lo, false);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, ARQ_OLLA_OFFSET_MIN_DB, lo);
    float hi = 0; for (int i = 0; i < 1000; i++) hi = arq_olla_update(hi, true);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, ARQ_OLLA_OFFSET_MAX_DB, hi);
}

/* 2. Under fading the loop settles: FER near target, mode stops flapping. */
void test_olla_stabilizes_under_fade(void)
{
    float fer, gp; int changes, rank;
    run_loop(10.0f, 4000, 1000, true, &fer, &changes, &gp, &rank);
    TEST_ASSERT_TRUE_MESSAGE(fer < ARQ_OLLA_TARGET_FER + 0.10f, "FER not held near target");
    TEST_ASSERT_TRUE_MESSAGE(100.0f * changes / 3000.0f < 5.0f, "mode oscillates after warmup");
}

/* 3. Tracks a stepped SNR: low → high → low, settling at a sane rank each time. */
void test_olla_tracks_snr_steps(void)
{
    float fer, gp; int changes, rk_lo, rk_hi, rk_lo2;
    run_loop(0.0f,  3000, 1000, true, &fer, &changes, &gp, &rk_lo);
    run_loop(18.0f, 3000, 1000, true, &fer, &changes, &gp, &rk_hi);
    run_loop(4.0f,  3000, 1000, true, &fer, &changes, &gp, &rk_lo2);
    TEST_ASSERT_TRUE_MESSAGE(rk_hi > rk_lo,  "did not climb with higher SNR");
    TEST_ASSERT_TRUE_MESSAGE(rk_hi >= 4,     "did not reach a fast mode at 18 dB");
    TEST_ASSERT_TRUE_MESSAGE(rk_lo2 < rk_hi, "did not back off with lower SNR");
}

/* Faithful model of the REAL adaptive loop: OLLA offset + the legacy
 * consecutive-retries forced-downgrade + post-downgrade hold that coexist in
 * select_best_mode()/maybe_upgrade_mode().  Mirrors arq_fsm.c so the test sees
 * what ships, not an idealized OLLA.  Returns time-averaged rank too, to detect
 * a pathological collapse to the robust floor. */
static void run_loop_real(float mean_snr, int nsteps, int warmup, bool safetynet,
                          float *out_goodput_bps, float *out_avg_rank,
                          int *out_changes)
{
    uint32_t seed = 12345;
    float offset = 0.0f;
    int rank = 0, prev_rank = 0, changes = 0, counted = 0;
    int consec_fail = 0, hold_until = 0;
    long delivered = 0; double airtime = 0.0, rank_sum = 0.0;
    const int HOLD = 4;       /* ~ ARQ_MODE_HOLD_AFTER_DOWNGRADE_S worth of frames */
    const int HARD_LOSS = 8;  /* OLLA-only total-link-loss net (rare) */

    for (int t = 0; t < nsteps; t++) {
        int olla_pick = select_rank(mean_snr + offset, rank);
        int sel;
        if (safetynet) {
            /* CURRENT ship behaviour: FER-blind forced downgrade + hold that
             * coexists with OLLA (arq_fsm.c select_best_mode/maybe_upgrade_mode). */
            if (consec_fail >= ARQ_RETRY_DOWNGRADE_THRESHOLD && rank > 0) {
                sel = rank - 1; hold_until = t + HOLD; consec_fail = 0;
            } else {
                sel = olla_pick;
                if (sel > rank && t < hold_until) sel = rank;
            }
        } else {
            /* PROPOSED: OLLA is the sole adaptation controller; only a rare hard
             * net drops to the floor on a sustained fail run (total link loss). */
            sel = olla_pick;
            if (consec_fail >= HARD_LOSS) { sel = 0; hold_until = t + HOLD; }
            else if (sel > rank && t < hold_until) sel = rank;
        }
        rank = sel;

        float inst = mean_snr - fade_db(&seed);
        bool ok = inst >= LADDER[rank].thresh_db;

        offset = arq_olla_update(offset, ok);
        if (!ok) consec_fail++; else consec_fail = 0;

        if (t >= warmup) {
            airtime += LADDER[rank].frame_s + (ok ? ACK_OK_S : LADDER[rank].ack_to_s);
            if (rank != prev_rank) { changes++; airtime += MODE_CHANGE_COST_S; }
            if (ok) delivered += LADDER[rank].payload;
            rank_sum += rank; counted++;
        }
        prev_rank = rank;
    }
    *out_goodput_bps = airtime > 0 ? (float)(delivered / airtime) : 0.0f;
    *out_avg_rank    = counted ? (float)(rank_sum / counted) : 0.0f;
    *out_changes     = changes;
}

/* Goodput of statically pinning a single rank on the same fading channel. */
static float goodput_fixed(float mean_snr, int fixed_rank, int nsteps, int warmup)
{
    uint32_t seed = 12345;
    long delivered = 0; double airtime = 0.0;
    for (int t = 0; t < nsteps; t++) {
        float inst = mean_snr - fade_db(&seed);
        bool ok = inst >= LADDER[fixed_rank].thresh_db;
        if (t >= warmup) {
            airtime += LADDER[fixed_rank].frame_s + (ok ? ACK_OK_S : LADDER[fixed_rank].ack_to_s);
            if (ok) delivered += LADDER[fixed_rank].payload;
        }
    }
    return airtime > 0 ? (float)(delivered / airtime) : 0.0f;
}

/* 4b. LOW / VARIABLE SNR: the real adaptive loop (OLLA + legacy forced-downgrade)
 *     must NOT collapse to the robust floor when a faster mode is the goodput
 *     optimum.  Reproduces the marginal-link concern seen on the dummy-load
 *     bench (mode ratcheting toward the floor at ~6-8 dB). At 6 dB the floor
 *     (DATAC15) is far from optimal: DATAC4/DATAC3 thresholds (-6/-1 dB) clear
 *     easily, so adaptation must beat both the floor and approach best-static. */
void test_olla_low_snr_no_collapse(void)
{
    for (float snr = 5.0f; snr <= 8.0f; snr += 1.5f) {
        float gp_net, gp_olla, ar_net, ar_olla; int ch_net, ch_olla;
        run_loop_real(snr, 4000, 1000, true,  &gp_net,  &ar_net,  &ch_net);   /* ship */
        run_loop_real(snr, 4000, 1000, false, &gp_olla, &ar_olla, &ch_olla);  /* fix  */
        float floor_gp = goodput_fixed(snr, 0, 4000, 1000);

        printf("  @%.1fdB: forced-dgr %.1f B/s (rank %.2f, %d chg) | OLLA-only %.1f B/s (rank %.2f, %d chg) | floor %.1f\n",
               snr, gp_net, ar_net, ch_net, gp_olla, ar_olla, ch_olla, floor_gp);

        char msg[220];
        /* (1) no pathological collapse to the robust floor */
        snprintf(msg, sizeof msg, "@%.1fdB OLLA-only collapsed to floor: %.1f <= floor %.1f", snr, gp_olla, floor_gp);
        TEST_ASSERT_TRUE_MESSAGE(gp_olla > floor_gp, msg);
        TEST_ASSERT_TRUE_MESSAGE(ar_olla >= 1.0f, "OLLA-only stuck at/near floor rank");
        /* (2) dropping the FER-blind forced-downgrade is strictly better at low SNR:
         *     more goodput AND far less mode churn (the dummy-load symptom). */
        snprintf(msg, sizeof msg, "@%.1fdB OLLA-only goodput %.1f < forced-dgr %.1f", snr, gp_olla, gp_net);
        TEST_ASSERT_TRUE_MESSAGE(gp_olla >= gp_net, msg);
        snprintf(msg, sizeof msg, "@%.1fdB OLLA-only churn %d not below forced-dgr %d", snr, ch_olla, ch_net);
        TEST_ASSERT_TRUE_MESSAGE(ch_olla < ch_net, msg);
    }
}

/* 4c. VERY LOW SNR (-10..-12 dB): below the data-mode floor.  The adapter must
 *     give up GRACEFULLY — settle pinned at the robust floor (DATAC15) without
 *     thrashing — rather than oscillate.  (Delivery down here is HARQ's job, not
 *     OLLA's; see docs/HARQ-FINDINGS.md.) */
void test_olla_floors_at_very_low_snr(void)
{
    for (float snr = -12.0f; snr <= -10.0f; snr += 2.0f) {
        float gp, avg_rank; int chg;
        run_loop_real(snr, 4000, 1000, false, &gp, &avg_rank, &chg);
        printf("  @%.0fdB: OLLA-only %.1f B/s (avg_rank %.2f, %d chg)\n",
               snr, gp, avg_rank, chg);
        char msg[180];
        snprintf(msg, sizeof msg, "@%.0fdB did not settle at floor (avg_rank %.2f)", snr, avg_rank);
        TEST_ASSERT_TRUE_MESSAGE(avg_rank < 1.0f, msg);   /* pinned at floor      */
        snprintf(msg, sizeof msg, "@%.0fdB thrashes at floor (%d mode changes)", snr, chg);
        TEST_ASSERT_TRUE_MESSAGE(chg < 30, msg);          /* stable, not flapping */
    }
}

/* 4. THE PROOF: on the same fading channel, OLLA delivers more effective
 *    goodput than the old oscillating gear-shift, with far less mode churn. */
void test_olla_beats_oscillating_baseline(void)
{
    for (float snr = 8.0f; snr <= 14.0f; snr += 2.0f) {
        float fer_o, fer_n, gp_o, gp_n; int ch_o, ch_n, rk_o, rk_n;
        run_loop(snr, 4000, 1000, false, &fer_o, &ch_o, &gp_o, &rk_o); /* old  */
        run_loop(snr, 4000, 1000, true,  &fer_n, &ch_n, &gp_n, &rk_n); /* OLLA */

        printf("  @%.0fdB: OLLA %.1f B/s (%d chg) vs old %.1f B/s (%d chg)\n",
               snr, gp_n, ch_n, gp_o, ch_o);
        char msg[160];
        snprintf(msg, sizeof msg, "@%.0fdB OLLA goodput %.1f <= old %.1f B/s", snr, gp_n, gp_o);
        TEST_ASSERT_TRUE_MESSAGE(gp_n > gp_o, msg);          /* faster everywhere */
        TEST_ASSERT_TRUE_MESSAGE(ch_n <= ch_o, "OLLA mode churn above old");
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_olla_update_basic);
    RUN_TEST(test_olla_stabilizes_under_fade);
    RUN_TEST(test_olla_tracks_snr_steps);
    RUN_TEST(test_olla_low_snr_no_collapse);
    RUN_TEST(test_olla_floors_at_very_low_snr);
    RUN_TEST(test_olla_beats_oscillating_baseline);
    return UNITY_END();
}
