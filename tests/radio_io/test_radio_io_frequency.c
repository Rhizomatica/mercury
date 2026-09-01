/* Read-only CAT frequency telemetry concurrency contract.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <time.h>

#include "unity.h"
#include "radio_io.h"

/* Test-only seams compiled by radio_io.c under RADIO_IO_TEST. */
void radio_io_test_lock(void);
void radio_io_test_unlock(void);
void radio_io_test_seed_frequency(uint64_t frequency_hz, uint64_t read_ms);
void radio_io_test_install_backend(int set_result);
bool radio_io_test_ptt_active(void);
uint64_t radio_io_test_last_ptt_off_ms(void);

/* radio_io.c retains all production backends in this unit build.  These stubs
 * keep the telemetry contract test independent of serial and HID hardware. */
int serial_ptt_open(const ptt_config_t *config) { (void)config; return -1; }
int serial_ptt_set(bool on) { (void)on; return -1; }
void serial_ptt_close(void) { }
int mercury_cm108_open(const ptt_config_t *config) { (void)config; return -1; }
int mercury_cm108_set(bool on) { (void)on; return -1; }
void mercury_cm108_close(void) { }

void setUp(void) {}
void tearDown(void) {}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL +
           (uint64_t)ts.tv_nsec / 1000000ULL;
}

void test_busy_radio_returns_cached_frequency_without_waiting(void)
{
    uint64_t frequency_hz = 0;
    uint64_t age_ms = 0;

    radio_io_test_seed_frequency(14074000, 0);
    radio_io_test_lock();
    uint64_t started_ms = monotonic_ms();
    bool valid = radio_io_get_frequency(true, &frequency_hz, &age_ms);
    uint64_t elapsed_ms = monotonic_ms() - started_ms;
    radio_io_test_unlock();

    TEST_ASSERT_TRUE(valid);
    TEST_ASSERT_EQUAL_UINT64(14074000, frequency_hz);
    TEST_ASSERT_LESS_THAN_UINT64_MESSAGE(
        50, elapsed_ms,
        "optional CAT telemetry waited behind a timing-sensitive radio operation");
}

void test_null_outputs_are_rejected(void)
{
    uint64_t value = 0;
    TEST_ASSERT_FALSE(radio_io_get_frequency(true, NULL, &value));
    TEST_ASSERT_FALSE(radio_io_get_frequency(true, &value, NULL));
}

void test_common_backend_dispatch_tracks_ptt_and_failed_key(void)
{
    radio_io_test_install_backend(0);
    TEST_ASSERT_EQUAL_INT(0, radio_io_key_on());
    TEST_ASSERT_TRUE(radio_io_test_ptt_active());

    TEST_ASSERT_EQUAL_INT(0, radio_io_key_off());
    TEST_ASSERT_FALSE(radio_io_test_ptt_active());
    TEST_ASSERT_NOT_EQUAL(UINT64_MAX, radio_io_test_last_ptt_off_ms());

    radio_io_test_install_backend(-1);
    TEST_ASSERT_EQUAL_INT(-1, radio_io_key_on());
    TEST_ASSERT_FALSE(radio_io_test_ptt_active());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_busy_radio_returns_cached_frequency_without_waiting);
    RUN_TEST(test_null_outputs_are_rejected);
    RUN_TEST(test_common_backend_dispatch_tracks_ptt_and_failed_key);
    return UNITY_END();
}
