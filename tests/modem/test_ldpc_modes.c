/* LDPC decoder policy as seen through the FreeDV API: which algorithm a mode
 * runs under the AUTO default, and how the iteration cap and the per-decode
 * budget interact.  Review items on PR #359: voice/2020 modes must keep a
 * legacy-equivalent result; on __EMBEDDED__ the 10-iteration cap must stay
 * unless a deadline replaces it. */
#include "unity.h"
#include "freedv_api.h"
#include "freedv_api_internal.h"
#include "mpdecode_core.h"
#include "ldpc_ctx.h"

void setUp(void) {}
void tearDown(void) {}

/* Voice modes are bounded by a quarter of their own modem frame period on
 * every build (700D 160 ms -> 40 ms, 700E 80 ms -> 20 ms), run to the code
 * table's iteration limit under that deadline, and their codes are in the
 * measured policy table (table sum-product). */
void test_voice_modes_are_bounded_by_their_frame_period(void)
{
    struct freedv *f = freedv_open(FREEDV_MODE_700D);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 40.0f, f->ldpc_budget_ms);
    TEST_ASSERT_EQUAL_INT(LDPC_MAX_ITER_DEFAULT, f->ldpc->max_iter);
    TEST_ASSERT_EQUAL_INT(LDPC_ALG_SPT, ldpc_ctx_policy_alg(f->ldpc->name));
    freedv_close(f);
    f = freedv_open(FREEDV_MODE_700E);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, f->ldpc_budget_ms);
    TEST_ASSERT_EQUAL_INT(LDPC_MAX_ITER_DEFAULT, f->ldpc->max_iter);
    freedv_close(f);
}

/* DATAC1 (H_4096_8192_3d) is a data mode: no budget until the host sets one
 * from its ARQ timing; the policy table decides the algorithm. */
void test_data_mode_follows_the_policy_table(void)
{
    struct freedv *f = freedv_open(FREEDV_MODE_DATAC1);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, f->ldpc_budget_ms);
    TEST_ASSERT_EQUAL_INT(LDPC_ALG_NMS16, ldpc_ctx_policy_alg(f->ldpc->name));
    freedv_close(f);
}

/* For a DATA mode the deadline replaces the cap only while a budget is
 * configured.  On a desktop build the open-time cap is the code table's
 * limit already; on __EMBEDDED__ it is 10, and clearing the budget restores
 * it. */
void test_budget_raises_the_cap_and_clearing_it_restores_the_open_time_cap(void)
{
    struct freedv *f = freedv_open(FREEDV_MODE_DATAC1);
    TEST_ASSERT_NOT_NULL(f);
    int cap0 = f->ldpc->max_iter;
    TEST_ASSERT_EQUAL_INT(cap0, f->ldpc_cap_nobudget);
#ifdef __EMBEDDED__
    TEST_ASSERT_EQUAL_INT(10, cap0);
#else
    TEST_ASSERT_EQUAL_INT(LDPC_MAX_ITER_DEFAULT, cap0);
#endif
    freedv_set_ldpc_budget_ms(f, 350.0f);
    TEST_ASSERT_EQUAL_INT(LDPC_MAX_ITER_DEFAULT, f->ldpc->max_iter);
    freedv_set_ldpc_budget_ms(f, 0.0f);
    TEST_ASSERT_EQUAL_INT(cap0, f->ldpc->max_iter);
    freedv_close(f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_voice_modes_are_bounded_by_their_frame_period);
    RUN_TEST(test_data_mode_follows_the_policy_table);
    RUN_TEST(test_budget_raises_the_cap_and_clearing_it_restores_the_open_time_cap);
    return UNITY_END();
}
