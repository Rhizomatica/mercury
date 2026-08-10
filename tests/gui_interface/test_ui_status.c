/* Pins the UI status wire format.
 *
 * The embedded UI now reads ui_status_t directly while remote clients (HERMES
 * web UI, mercury-qt) keep parsing the JSON. Both render the same gathered
 * snapshot, so the risk worth guarding is the JSON drifting as fields are
 * added to the struct — a rename or a reordered field silently breaks every
 * remote client, and nothing else in the build would notice.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "unity.h"
#include "ui_status.h"

void setUp(void) {}
void tearDown(void) {}

static ui_status_t sample(void)
{
    ui_status_t st;
    memset(&st, 0, sizeof(st));
    st.bitrate_bps = 1200;
    st.snr_db      = 7.25;      /* renders at one decimal */
    snprintf(st.user_callsign, sizeof(st.user_callsign), "PU2UIT");
    snprintf(st.dest_callsign, sizeof(st.dest_callsign), "K7EK");
    st.sync                = true;
    st.transmitting        = false;
    st.client_tcp_connected = true;
    st.bytes_transmitted   = 4096;
    st.bytes_received      = 128;
    st.tx_gain_db          = -15.0f;
    st.tx_peak_dbfs        = -3.5f;
    st.waterfall_enabled   = true;
    st.audio_ok            = true;
    st.audio_error[0]      = '\0';
    return st;
}

/* The exact bytes a remote client receives. */
void test_status_json_is_byte_exact(void)
{
    ui_status_t st = sample();
    char buf[512];
    int n = ui_status_to_json(&st, buf, sizeof(buf));

    const char *expect =
        "{\"type\":\"status\","
        "\"bitrate\":1200,"
        "\"snr\":7.2,"
        "\"user_callsign\":\"PU2UIT\","
        "\"dest_callsign\":\"K7EK\","
        "\"sync\":true,"
        "\"direction\":\"rx\","
        "\"client_tcp_connected\":true,"
        "\"bytes_transmitted\":4096,"
        "\"bytes_received\":128,"
        "\"tx_gain_db\":-15.0,"
        "\"tx_peak_dbfs\":-3.5,"
        "\"waterfall\":true,"
        "\"audio_ok\":true,"
        "\"audio_error\":\"\"}";

    TEST_ASSERT_EQUAL_STRING(expect, buf);
    TEST_ASSERT_EQUAL_INT((int)strlen(expect), n);
}

/* direction is derived from the transmit flag, not carried separately. */
void test_direction_follows_ptt(void)
{
    ui_status_t st = sample();
    char buf[512];

    st.transmitting = true;
    TEST_ASSERT_TRUE(ui_status_to_json(&st, buf, sizeof(buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"direction\":\"tx\""));

    st.transmitting = false;
    TEST_ASSERT_TRUE(ui_status_to_json(&st, buf, sizeof(buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"direction\":\"rx\""));
}

/* Booleans must render as JSON literals, not 0/1 — clients test them as bools. */
void test_booleans_render_as_json_literals(void)
{
    ui_status_t st = sample();
    st.sync                 = false;
    st.client_tcp_connected = false;
    st.waterfall_enabled    = false;

    char buf[512];
    TEST_ASSERT_TRUE(ui_status_to_json(&st, buf, sizeof(buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"sync\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"client_tcp_connected\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"waterfall\":false"));
}

/* A short buffer must fail rather than emit truncated JSON: half a status
 * object would leave a remote client parsing garbage. */
void test_truncation_is_reported_not_emitted(void)
{
    ui_status_t st = sample();
    char small[32];
    TEST_ASSERT_EQUAL_INT(-1, ui_status_to_json(&st, small, sizeof(small)));
}

void test_null_arguments_are_rejected(void)
{
    ui_status_t st = sample();
    char buf[512];
    TEST_ASSERT_EQUAL_INT(-1, ui_status_to_json(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-1, ui_status_to_json(&st, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-1, ui_status_to_json(&st, buf, 0));
}

/* A failed sound card must reach the operator.  Mercury deliberately stays up
 * when a device cannot be opened or negotiates a rate the modem cannot use --
 * the operator needs it running to pick another card -- so this status is the
 * only channel by which they learn anything is wrong.  If audio_ok were
 * dropped from the payload, the UI would show a healthy station that hears
 * nothing, which is exactly the bug this field exists to prevent. */
void test_audio_failure_is_reported(void)
{
    ui_status_t st = sample();
    char buf[512];

    st.audio_ok = false;
    snprintf(st.audio_error, sizeof(st.audio_error),
             "capture: device negotiated 44100 Hz, not a multiple of 8000 Hz");

    TEST_ASSERT_TRUE(ui_status_to_json(&st, buf, sizeof(buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"audio_ok\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "44100 Hz"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_status_json_is_byte_exact);
    RUN_TEST(test_audio_failure_is_reported);
    RUN_TEST(test_direction_follows_ptt);
    RUN_TEST(test_booleans_render_as_json_literals);
    RUN_TEST(test_truncation_is_reported_not_emitted);
    RUN_TEST(test_null_arguments_are_rejected);
    return UNITY_END();
}
