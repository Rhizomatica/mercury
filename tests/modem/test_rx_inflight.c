/* The age of the frame a decoder is locked onto (modem/rx_inflight.h). */
#include "unity.h"
#include "rx_inflight.h"

void setUp(void) {}
void tearDown(void) {}

/* An MFSK decoder reports on every second pass of the RX loop. */
void test_a_pass_with_no_report_keeps_the_lock(void)
{
    uint64_t since = 0, heard = 0;
    for (uint64_t now = 1000; now <= 15000; now += 20)
    {
        bool reported = (now / 20) % 2 == 0;
        since = rx_inflight_next(since, heard, reported, reported, false, now);
        if (reported)
            heard = now;
    }
    TEST_ASSERT_EQUAL_UINT64(1000, since);
}

void test_a_report_of_no_sync_ends_it(void)
{
    TEST_ASSERT_EQUAL_UINT64(0, rx_inflight_next(1000, 1980, true, false, false, 2000));
}

void test_a_decoded_frame_ends_it(void)
{
    TEST_ASSERT_EQUAL_UINT64(0, rx_inflight_next(1000, 1980, true, true, true, 2000));
}

void test_a_silent_decoder_lets_go_after_the_stale_bound(void)
{
    TEST_ASSERT_EQUAL_UINT64(1000, rx_inflight_next(1000, 2000, false, false, false, 2749));
    TEST_ASSERT_EQUAL_UINT64(0, rx_inflight_next(1000, 2000, false, false, false, 2750));
}

void test_no_lock_stays_none_without_a_report(void)
{
    TEST_ASSERT_EQUAL_UINT64(0, rx_inflight_next(0, 0, false, false, false, 5000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_pass_with_no_report_keeps_the_lock);
    RUN_TEST(test_a_report_of_no_sync_ends_it);
    RUN_TEST(test_a_decoded_frame_ends_it);
    RUN_TEST(test_a_silent_decoder_lets_go_after_the_stale_bound);
    RUN_TEST(test_no_lock_stays_none_without_a_report);
    return UNITY_END();
}
