/* play_recover: what the playback thread does about a device that stopped
 * taking audio (audioio/play_recover.h). */
#include "unity.h"
#include "play_recover.h"

void setUp(void) {}
void tearDown(void) {}

/* A full buffer on a non-blocking stream takes nothing for a period or two:
 * not a stall. */
void test_short_gap_is_not_a_stall(void)
{
    play_recover_t r = {0};
    TEST_ASSERT_FALSE(play_recover_on_stuck(&r, 5000));
    TEST_ASSERT_FALSE(play_recover_on_stuck(&r, 5000 + PLAY_STALL_MS - 1));
    TEST_ASSERT_EQUAL_INT(0, play_recover_on_write(&r));
    /* The window starts again after the device takes something. */
    TEST_ASSERT_FALSE(play_recover_on_stuck(&r, 5000 + PLAY_STALL_MS + 10));
}

/* Issue #591: a stream that fails every write is reopened after the window,
 * not written to for ever. */
void test_stuck_past_window_reopens(void)
{
    play_recover_t r = {0};
    TEST_ASSERT_FALSE(play_recover_on_stuck(&r, 100));
    TEST_ASSERT_TRUE(play_recover_on_stuck(&r, 100 + PLAY_STALL_MS));
}

/* A reopen gives the device a whole window again. */
void test_reopen_restarts_window(void)
{
    play_recover_t r = {0};
    play_recover_on_stuck(&r, 0);
    TEST_ASSERT_TRUE(play_recover_on_stuck(&r, PLAY_STALL_MS));
    TEST_ASSERT_FALSE(play_recover_on_reopen(&r));
    TEST_ASSERT_FALSE(play_recover_on_stuck(&r, PLAY_STALL_MS + 1));
    TEST_ASSERT_FALSE(play_recover_on_stuck(&r, 2 * PLAY_STALL_MS));
    TEST_ASSERT_TRUE(play_recover_on_stuck(&r, 2 * PLAY_STALL_MS + 1));
}

/* A reopen that cures it is said once and never called dead. */
void test_cured_reopen_reports_back_not_alive(void)
{
    play_recover_t r = {0};
    TEST_ASSERT_FALSE(play_recover_on_reopen(&r));
    TEST_ASSERT_EQUAL_INT(PLAY_REC_BACK, play_recover_on_write(&r));
    TEST_ASSERT_EQUAL_INT(0, play_recover_on_write(&r));
}

/* Reopens that bring nothing: dead at the third, reported once; the first
 * audio after that is running again. */
void test_dead_after_dry_reopens_reported_once(void)
{
    play_recover_t r = {0};
    for (int i = 1; i < PLAY_DRY_REOPENS_DEAD; i++)
        TEST_ASSERT_FALSE(play_recover_on_reopen(&r));
    TEST_ASSERT_TRUE(play_recover_on_reopen(&r));
    for (int i = 0; i < 20; i++)
        TEST_ASSERT_FALSE(play_recover_on_reopen(&r));
    TEST_ASSERT_EQUAL_INT(PLAY_REC_BACK | PLAY_REC_ALIVE, play_recover_on_write(&r));
    TEST_ASSERT_FALSE(r.dead);
    /* ...and a later failure counts from zero. */
    for (int i = 1; i < PLAY_DRY_REOPENS_DEAD; i++)
        TEST_ASSERT_FALSE(play_recover_on_reopen(&r));
    TEST_ASSERT_TRUE(play_recover_on_reopen(&r));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_gap_is_not_a_stall);
    RUN_TEST(test_stuck_past_window_reopens);
    RUN_TEST(test_reopen_restarts_window);
    RUN_TEST(test_cured_reopen_reports_back_not_alive);
    RUN_TEST(test_dead_after_dry_reopens_reported_once);
    return UNITY_END();
}
