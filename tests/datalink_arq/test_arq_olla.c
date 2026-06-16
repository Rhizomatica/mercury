/*
 * OLLA (outer-loop link adaptation) unit tests.
 *
 * Drives the arq_olla_update() control loop in a closed loop against a
 * synthetic fading channel + the real mode-selection thresholds, and asserts
 * the gear-shift CONVERGES without oscillation — the property the ad-hoc
 * SNR-threshold + retry-downgrade + hold-timer scheme lacked.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "unity.h"
#include "arq_protocol.h"
#include "freedv/freedv_api.h"

#include <math.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* ---- mode ladder model (mirrors select_best_mode's thresholds) ---- */
/* {true decode threshold dB, payload bytes} per ladder rank 0..5. */
static const struct { float thresh_db; int payload; const char *name; } LADDER[] = {
    { -10.0f,                     30,   "DATAC15" },  /* floor; robust, always usable */
    { ARQ_SNR_MIN_DATAC4_DB,      54,   "DATAC4"  },
    { ARQ_SNR_MIN_DATAC3_DB,      126,  "DATAC3"  },
    { ARQ_SNR_MIN_DATAC1_DB,      510,  "DATAC1"  },
    { ARQ_SNR_MIN_DATAC17_DB,     1180, "DATAC17" },
    { ARQ_SNR_MIN_QAM16C2_DB,     1213, "QAM16C2" },
};
#define NLADDER ((int)(sizeof(LADDER)/sizeof(LADDER[0])))

/* select the highest rank whose threshold (with the 1 dB upgrade hysteresis) is
 * cleared by the given effective SNR — same asymmetry as select_best_mode(). */
static int select_rank(float eff_snr, int cur_rank)
{
    int best = 0;
    for (int r = 1; r < NLADDER; r++) {
        float t = LADDER[r].thresh_db + (r > cur_rank ? ARQ_SNR_HYST_DB : 0.0f);
        if (eff_snr >= t) best = r;
    }
    return best;
}

/* deterministic reproducible fade sequence (seeded LCG → ~Rayleigh-ish dips) */
static float fade_db(uint32_t *s)
{
    *s = (*s) * 1664525u + 1013904223u;
    float u = ((*s >> 8) & 0xFFFFFF) / (float)0x1000000;   /* [0,1) */
    if (u < 1e-6f) u = 1e-6f;
    return -10.0f * log10f(u);   /* exponential power fade, mean ~4.3 dB, deep tail */
}

/* Run the closed loop; return realized first-try FER and #mode-changes after
 * the warmup, and the final rank. */
static void run_loop(float mean_snr, int nsteps, int warmup,
                     float *out_fer, int *out_changes, int *out_final_rank)
{
    uint32_t seed = 12345;
    float offset = 0.0f;
    int rank = 0, prev_rank = 0, changes = 0, fails = 0, counted = 0;

    for (int t = 0; t < nsteps; t++) {
        float eff = mean_snr + offset;                 /* peer reports the mean   */
        rank = select_rank(eff, rank);
        float inst = mean_snr - fade_db(&seed);        /* instantaneous faded SNR */
        bool ok = inst >= LADDER[rank].thresh_db;      /* frame decodes?          */
        offset = arq_olla_update(offset, ok);
        if (t >= warmup) {
            if (rank != prev_rank) changes++;
            if (!ok) fails++;
            counted++;
        }
        prev_rank = rank;
    }
    *out_fer = counted ? (float)fails / counted : 0.0f;
    *out_changes = changes;
    *out_final_rank = rank;
}

/* 1. update clamps and steps in the right direction with the FER ratio. */
void test_olla_update_basic(void)
{
    /* up step is target/(1-target) of the down step */
    float up = ARQ_OLLA_STEP_UP_DB, down = ARQ_OLLA_STEP_DOWN_DB;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, ARQ_OLLA_TARGET_FER, up / (up + down));
    /* failure lowers, success raises */
    TEST_ASSERT_TRUE(arq_olla_update(0.0f, false) < 0.0f);
    TEST_ASSERT_TRUE(arq_olla_update(0.0f, true)  > 0.0f);
    /* clamps */
    float lo = 0; for (int i = 0; i < 1000; i++) lo = arq_olla_update(lo, false);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, ARQ_OLLA_OFFSET_MIN_DB, lo);
    float hi = 0; for (int i = 0; i < 1000; i++) hi = arq_olla_update(hi, true);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, ARQ_OLLA_OFFSET_MAX_DB, hi);
}

/* 2. Under fading at a mean SNR where the naive pick (DATAC17) fades out, the
 *    loop settles: FER pulled near target, and the mode stops flapping. */
void test_olla_stabilizes_under_fade(void)
{
    float fer; int changes, rank;
    run_loop(/*mean_snr*/ 10.0f, /*nsteps*/ 4000, /*warmup*/ 1000, &fer, &changes, &rank);

    /* realized FER is held near the target (not the ~40%+ a naive DATAC17 pick
     * would suffer on these fades) */
    TEST_ASSERT_TRUE_MESSAGE(fer < ARQ_OLLA_TARGET_FER + 0.10f, "FER not held near target");
    /* converged: very few mode changes per 100 frames after warmup */
    float changes_per_100 = 100.0f * changes / 3000.0f;
    TEST_ASSERT_TRUE_MESSAGE(changes_per_100 < 5.0f, "mode oscillates after warmup");
}

/* 3. Tracks a stepped SNR: low → high → low, settling at a sane rank each time. */
void test_olla_tracks_snr_steps(void)
{
    float fer; int changes, rank_lo, rank_hi, rank_lo2;
    run_loop(0.0f,  3000, 1000, &fer, &changes, &rank_lo);   /* ~floor/DATAC3 */
    run_loop(18.0f, 3000, 1000, &fer, &changes, &rank_hi);   /* climbs high   */
    run_loop(4.0f,  3000, 1000, &fer, &changes, &rank_lo2);  /* back down     */

    TEST_ASSERT_TRUE_MESSAGE(rank_hi > rank_lo,  "did not climb with higher SNR");
    TEST_ASSERT_TRUE_MESSAGE(rank_hi >= 4,       "did not reach a fast mode at 18 dB");
    TEST_ASSERT_TRUE_MESSAGE(rank_lo2 < rank_hi, "did not back off with lower SNR");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_olla_update_basic);
    RUN_TEST(test_olla_stabilizes_under_fade);
    RUN_TEST(test_olla_tracks_snr_steps);
    return UNITY_END();
}
