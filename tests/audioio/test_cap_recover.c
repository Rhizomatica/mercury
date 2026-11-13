/* cap_recover: what the capture thread does about a device that stopped
 * delivering samples (audioio/cap_recover.h). */
#include "unity.h"
#include "cap_recover.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* K7EK, 2026-11-10: the USB codec dropped for 47 min.  The first stall is a
 * wedged device (one reopen fixes it); a third in a row, two reopens that
 * brought nothing, is reported once, and the device looked up by name then
 * and every CAP_RESOLVE_EVERY reopens after. */
void test_dead_after_three_dry_stalls_reported_once(void)
{
    cap_recover_t r = {0};
    TEST_ASSERT_EQUAL_INT(0, cap_recover_on_stall(&r, false));
    TEST_ASSERT_EQUAL_INT(0, cap_recover_on_stall(&r, false));
    TEST_ASSERT_EQUAL_INT(CAP_REC_DEAD | CAP_REC_RESOLVE, cap_recover_on_stall(&r, false));
    for (int i = 1; i < CAP_RESOLVE_EVERY; i++)
        TEST_ASSERT_EQUAL_INT(0, cap_recover_on_stall(&r, false));
    TEST_ASSERT_EQUAL_INT(CAP_REC_RESOLVE, cap_recover_on_stall(&r, false));
    TEST_ASSERT_TRUE(r.dead);
}

/* A stall now and then, each cured by its reopen, is never "dead". */
void test_reopen_that_brings_samples_starts_over(void)
{
    cap_recover_t r = {0};
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_EQUAL_INT(0, cap_recover_on_stall(&r, false));
        TEST_ASSERT_EQUAL_INT(0, cap_recover_on_stall(&r, false));
        TEST_ASSERT_FALSE(cap_recover_on_data(&r));
    }
}

/* A half-duplex codec delivers nothing while we transmit, and an MFSK floor
 * frame is 27 s: stalls then are not the device dying. */
void test_stalls_while_keyed_do_not_count(void)
{
    cap_recover_t r = {0};
    TEST_ASSERT_EQUAL_INT(0, cap_recover_on_stall(&r, false));
    for (int i = 0; i < 14; i++)
        TEST_ASSERT_EQUAL_INT(0, cap_recover_on_stall(&r, true));
    TEST_ASSERT_EQUAL_INT(0, cap_recover_on_stall(&r, false));
    TEST_ASSERT_FALSE(r.dead);
}

/* Samples after it was reported dead: running again, and said once. */
void test_data_after_dead_reports_recovery_once(void)
{
    cap_recover_t r = {0};
    for (int i = 0; i < CAP_DRY_REOPENS_DEAD; i++)
        cap_recover_on_stall(&r, false);
    TEST_ASSERT_TRUE(cap_recover_on_data(&r));
    TEST_ASSERT_FALSE(cap_recover_on_data(&r));
    TEST_ASSERT_EQUAL_INT(0, cap_recover_on_stall(&r, false));
}

static char ids[4][AUDIO_DEV_STR_MAX], names[4][AUDIO_DEV_STR_MAX];
static int devs(const char *const *list)
{
    int n = 0;
    for (; list[2 * n]; n++) {
        snprintf(ids[n], AUDIO_DEV_STR_MAX, "%s", list[2 * n]);
        snprintf(names[n], AUDIO_DEV_STR_MAX, "%s", list[2 * n + 1]);
    }
    return n;
}
#define PICK(cur, name, n) cap_pick_by_name(cur, name, (const char (*)[AUDIO_DEV_STR_MAX])ids, \
                                            (const char (*)[AUDIO_DEV_STR_MAX])names, n)

static const char *TS590 = "TS-590SG RECEIVE (6- USB Audio CODEC )";

/* K7EK's codec, back after the reset under a new endpoint id. */
void test_pick_finds_the_device_under_its_new_id(void)
{
    const char *l[] = { "{0.0.1.00000000}.{11111111}", "Microphone (Realtek)",
                        "{0.0.1.00000000}.{0ae73a95}", "TS-590SG RECEIVE (6- USB Audio CODEC )",
                        NULL };
    int n = devs(l);
    TEST_ASSERT_EQUAL_INT(1, PICK("{0.0.1.00000000}.{464a7b2c}", TS590, n));
}

/* Its id still listed: keep it, whatever else carries the name. */
void test_pick_keeps_an_id_still_listed(void)
{
    const char *l[] = { "{a}", "TS-590SG RECEIVE (6- USB Audio CODEC )",
                        "{b}", "TS-590SG RECEIVE (6- USB Audio CODEC )", NULL };
    int n = devs(l);
    TEST_ASSERT_EQUAL_INT(CAP_PICK_PRESENT, PICK("{b}", TS590, n));
}

/* Gone, and two devices carry its name (two of the same codec): not
 * guessing which radio. */
void test_pick_refuses_an_ambiguous_name(void)
{
    const char *l[] = { "{a}", "TS-590SG RECEIVE (6- USB Audio CODEC )",
                        "{b}", "ts-590sg receive (6- usb audio codec )", NULL };
    int n = devs(l);
    TEST_ASSERT_EQUAL_INT(CAP_PICK_AMBIGUOUS, PICK("{gone}", TS590, n));
}

/* Unplugged: nothing of that name, and a name that is only part of another
 * device's is not it. */
void test_pick_none_when_unplugged(void)
{
    const char *l[] = { "{a}", "Microphone (Realtek)",
                        "{b}", "TS-590SG RECEIVE (6- USB Audio CODEC ) #2", NULL };
    int n = devs(l);
    TEST_ASSERT_EQUAL_INT(CAP_PICK_NONE, PICK("{gone}", TS590, n));
    TEST_ASSERT_EQUAL_INT(CAP_PICK_NONE, PICK("{gone}", "", n));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dead_after_three_dry_stalls_reported_once);
    RUN_TEST(test_reopen_that_brings_samples_starts_over);
    RUN_TEST(test_stalls_while_keyed_do_not_count);
    RUN_TEST(test_data_after_dead_reports_recovery_once);
    RUN_TEST(test_pick_finds_the_device_under_its_new_id);
    RUN_TEST(test_pick_keeps_an_id_still_listed);
    RUN_TEST(test_pick_refuses_an_ambiguous_name);
    RUN_TEST(test_pick_none_when_unplugged);
    return UNITY_END();
}
