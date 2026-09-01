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


/* ---- long audio device ids (issue #185) --------------------------------- */

/* The reporter's IC-7300 as PulseAudio names it.  66 and 67 characters: at the
 * old 64-byte width these were cut to 63, which both wrote a broken device
 * into mercury.ini and stopped the id matching a real device on the next open,
 * so the modem silently fell back to the default card. */
#define IC7300_CAPTURE  "alsa_input.usb-Burr-Brown_from_TI_USB_Audio_CODEC-00.analog-stereo"
#define IC7300_PLAYBACK "alsa_output.usb-Burr-Brown_from_TI_USB_Audio_CODEC-00.analog-stereo"

void test_pulseaudio_device_id_survives_the_ui_struct(void)
{
    /* The struct has to hold the name AND its NUL, or the round trip through
     * the UI silently shortens what the operator picked. */
    TEST_ASSERT_TRUE_MESSAGE(sizeof(((ui_device_t *)0)->id) > strlen(IC7300_CAPTURE),
                             "ui_device_t.id too small for a plain USB codec's PulseAudio node");
    TEST_ASSERT_TRUE_MESSAGE(sizeof(((ui_device_t *)0)->id) > strlen(IC7300_PLAYBACK),
                             "ui_device_t.id too small for a plain USB codec's PulseAudio node");

    ui_device_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.id, sizeof(d.id), "%s", IC7300_CAPTURE);
    TEST_ASSERT_EQUAL_STRING(IC7300_CAPTURE, d.id);
}

/* The id has to come back out of the JSON intact too -- that is the path the
 * web UI and the Fyne UI both read the device list through. */
void test_long_device_id_round_trips_through_json(void)
{
    ui_device_t devs[2];
    memset(devs, 0, sizeof(devs));
    snprintf(devs[0].id,   sizeof(devs[0].id),   "%s", IC7300_CAPTURE);
    snprintf(devs[0].name, sizeof(devs[0].name), "%s", "USB Audio CODEC Analog Stereo");
    snprintf(devs[1].id,   sizeof(devs[1].id),   "%s", IC7300_PLAYBACK);
    snprintf(devs[1].name, sizeof(devs[1].name), "%s", "USB Audio CODEC Analog Stereo");

    char buf[2048];
    int n = ui_device_list_to_json("capture", devs, 2, IC7300_CAPTURE, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);

    /* Whole string present, not a 63-character prefix of it. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, IC7300_CAPTURE),
                                 "capture id truncated in the device-list JSON");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, IC7300_PLAYBACK),
                                 "playback id truncated in the device-list JSON");
}

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
    st.peer_snr_db         = -3.75;  /* renders at one decimal */
    st.peer_snr_valid      = true;
    return st;
}

/* A peer SNR of 0.0 dB is a real, meaningful reading; "nothing heard yet" is
 * not.  Conflating them would show a newcomer "they hear us at 0 dB" before
 * the far side has said anything at all -- the opposite of the reassurance
 * issue #230 asks for.  peer_snr_valid is what keeps them apart. */
void test_peer_snr_unknown_is_flagged_not_zero(void)
{
    ui_status_t st = sample();
    st.peer_snr_db    = UI_SNR_UNKNOWN_DB;
    st.peer_snr_valid = false;

    char buf[1024];
    TEST_ASSERT_TRUE(ui_status_to_json(&st, buf, sizeof(buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"peer_snr_valid\":false"));
    /* The value must ALSO be unmistakable, so a client that ignores the flag
     * cannot render "0.0 dB" as if the far side had reported it. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"peer_snr\":-99.9"));
    TEST_ASSERT_NULL(strstr(buf, "\"peer_snr\":0.0"));

    /* and a genuine 0 dB report is NOT flagged unknown */
    st.peer_snr_db    = 0.0;
    st.peer_snr_valid = true;
    TEST_ASSERT_TRUE(ui_status_to_json(&st, buf, sizeof(buf)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"peer_snr\":0.0"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"peer_snr_valid\":true"));
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
        "\"audio_error\":\"\","
        "\"peer_snr\":-3.8,"
        "\"peer_snr_valid\":true}";

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
    RUN_TEST(test_peer_snr_unknown_is_flagged_not_zero);
    RUN_TEST(test_audio_failure_is_reported);
    RUN_TEST(test_direction_follows_ptt);
    RUN_TEST(test_booleans_render_as_json_literals);
    RUN_TEST(test_truncation_is_reported_not_emitted);
    RUN_TEST(test_null_arguments_are_rejected);
    RUN_TEST(test_pulseaudio_device_id_survives_the_ui_struct);
    RUN_TEST(test_long_device_id_round_trips_through_json);
    return UNITY_END();
}
