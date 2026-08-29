/* HERMES Modem - Mercury Configuration Utilities
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Pedro Messetti <pedromessetti.rhizomatica@gmail.com>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "cfg_utils.h"
#include "cm108_ptt.h"

static const char *TMP = "test_cfg_roundtrip.ini";

void setUp(void)   { }
void tearDown(void){ unlink(TMP); }

/* Defaults must equal the historical compile-time constants. */
void test_defaults_match_constants(void)
{
    mercury_config c;
    cfg_set_defaults(&c);
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_NONE, c.ptt.method);
    TEST_ASSERT_EQUAL_INT(RADIO_TYPE_NONE, c.ptt.hamlib_model);
    TEST_ASSERT_EQUAL_STRING("", c.ptt.device);
    TEST_ASSERT_EQUAL_INT(10,  c.data_retry_slots);
    TEST_ASSERT_EQUAL_INT(6,   c.mode_hold_after_downgrade_s);
    TEST_ASSERT_EQUAL_INT(2,   c.ladder_up_successes);
    TEST_ASSERT_EQUAL_INT(2,   c.retry_downgrade_threshold);
    TEST_ASSERT_EQUAL_INT(700, c.channel_guard_ms);
    TEST_ASSERT_EQUAL_INT(900, c.iss_post_ack_guard_ms);
    TEST_ASSERT_EQUAL_INT(20,  c.keepalive_interval_s);
    TEST_ASSERT_EQUAL_INT(5,   c.keepalive_miss_limit);
    TEST_ASSERT_EQUAL_INT(15,  c.peer_payload_hold_s);
    TEST_ASSERT_EQUAL_INT(10,  c.startup_max_s);
    TEST_ASSERT_EQUAL_INT(2000, c.retry_stagger_ms);
    /* Opt-in: hosts hold transmissions while BUSY is asserted, so this must
     * never switch on under a station that did not ask for it. */
    TEST_ASSERT_FALSE(c.busy_detect);
    TEST_ASSERT_EQUAL_INT(10,   c.busy_threshold_db);
    TEST_ASSERT_EQUAL_INT(3,    c.busy_hysteresis_db);
    TEST_ASSERT_EQUAL_INT(300,  c.busy_on_debounce_ms);
    TEST_ASSERT_EQUAL_INT(1500, c.busy_hang_ms);
}

/* Set non-default values, write, read back, assert preserved. */
void test_arq_tunables_roundtrip(void)
{
    mercury_config w;
    cfg_set_defaults(&w);
    w.data_retry_slots            = 14;
    w.mode_hold_after_downgrade_s = 3;
    w.ladder_up_successes         = 4;
    w.retry_downgrade_threshold   = 3;
    w.channel_guard_ms            = 800;
    w.iss_post_ack_guard_ms       = 1000;
    w.keepalive_interval_s        = 30;
    w.keepalive_miss_limit        = 8;
    w.peer_payload_hold_s         = 25;
    w.startup_max_s               = 20;
    w.retry_stagger_ms            = 0;      /* off-switch must round-trip */
    w.ptt.method                  = PTT_METHOD_HAMLIB;
    snprintf(w.ptt.device, sizeof(w.ptt.device), "COM7");
    w.ptt.hamlib_model            = 1049;
    w.ptt.hamlib_serial_speed     = 115200;
    w.ptt.hamlib_log_level        = 3;
    w.busy_detect                 = true;   /* non-default: proves it round-trips */
    w.busy_threshold_db           = 14;
    w.busy_hysteresis_db          = 5;
    w.busy_on_debounce_ms         = 500;
    w.busy_hang_ms                = 2000;
    TEST_ASSERT_TRUE(cfg_write(&w, TMP));

    mercury_config r;
    cfg_set_defaults(&r);
    TEST_ASSERT_TRUE(cfg_read(&r, TMP));
    TEST_ASSERT_EQUAL_INT(14,   r.data_retry_slots);
    TEST_ASSERT_EQUAL_INT(3,    r.mode_hold_after_downgrade_s);
    TEST_ASSERT_EQUAL_INT(4,    r.ladder_up_successes);
    TEST_ASSERT_EQUAL_INT(3,    r.retry_downgrade_threshold);
    TEST_ASSERT_EQUAL_INT(800,  r.channel_guard_ms);
    TEST_ASSERT_EQUAL_INT(1000, r.iss_post_ack_guard_ms);
    TEST_ASSERT_EQUAL_INT(30,   r.keepalive_interval_s);
    TEST_ASSERT_EQUAL_INT(8,    r.keepalive_miss_limit);
    TEST_ASSERT_EQUAL_INT(25,   r.peer_payload_hold_s);
    TEST_ASSERT_EQUAL_INT(20,   r.startup_max_s);
    TEST_ASSERT_EQUAL_INT(0,    r.retry_stagger_ms);   /* 0 (off) preserved */
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_HAMLIB, r.ptt.method);
    TEST_ASSERT_EQUAL_STRING("COM7", r.ptt.device);
    TEST_ASSERT_EQUAL_INT(1049, r.ptt.hamlib_model);
    TEST_ASSERT_EQUAL_INT(115200, r.ptt.hamlib_serial_speed);
    TEST_ASSERT_EQUAL_INT(3, r.ptt.hamlib_log_level);
    TEST_ASSERT_TRUE(r.busy_detect);
    TEST_ASSERT_EQUAL_INT(14,   r.busy_threshold_db);
    TEST_ASSERT_EQUAL_INT(5,    r.busy_hysteresis_db);
    TEST_ASSERT_EQUAL_INT(500,  r.busy_on_debounce_ms);
    TEST_ASSERT_EQUAL_INT(2000, r.busy_hang_ms);
}

void test_legacy_radio_config_maps_to_hamlib_ptt(void)
{
    FILE *f = fopen(TMP, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("[main]\nradio_model = 1049\nradio_device = COM3\n"
          "radio_serial_speed = 38400\nhamlib_log_level = 4\n", f);
    fclose(f);

    mercury_config r;
    cfg_set_defaults(&r);
    TEST_ASSERT_TRUE(cfg_read(&r, TMP));
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_HAMLIB, r.ptt.method);
    TEST_ASSERT_EQUAL_INT(1049, r.ptt.hamlib_model);
    TEST_ASSERT_EQUAL_STRING("COM3", r.ptt.device);
    TEST_ASSERT_EQUAL_INT(38400, r.ptt.hamlib_serial_speed);
    TEST_ASSERT_EQUAL_INT(4, r.ptt.hamlib_log_level);
}

void test_explicit_ptt_method_overrides_legacy_backend(void)
{
    FILE *f = fopen(TMP, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("[main]\nradio_model = 1049\nradio_device = COM3\n"
          "[ptt]\nmethod = serial_rts\ndevice = COM7\n", f);
    fclose(f);

    mercury_config r;
    cfg_set_defaults(&r);
    r.ptt.serial_line = PTT_LINE_BOTH;
    r.ptt.serial_invert_rts = true;
    r.ptt.serial_invert_dtr = true;
    TEST_ASSERT_TRUE(cfg_read(&r, TMP));
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_SERIAL, r.ptt.method);
    TEST_ASSERT_EQUAL_INT(PTT_LINE_RTS, r.ptt.serial_line);
    TEST_ASSERT_FALSE(r.ptt.serial_invert_rts);
    TEST_ASSERT_FALSE(r.ptt.serial_invert_dtr);
    TEST_ASSERT_EQUAL_STRING("COM7", r.ptt.device);
    /* Backend-specific settings survive method switches. */
    TEST_ASSERT_EQUAL_INT(1049, r.ptt.hamlib_model);
}

void test_invalid_ptt_method_rejects_config(void)
{
    FILE *f = fopen(TMP, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("[ptt]\nmethod = carrier_pigeon\n", f);
    fclose(f);

    mercury_config r;
    cfg_set_defaults(&r);
    TEST_ASSERT_FALSE(cfg_read(&r, TMP));
}

void test_partial_config_preserves_preselected_ptt_method(void)
{
    FILE *f = fopen(TMP, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("[main]\nui_enabled = true\n", f);
    fclose(f);

    mercury_config r;
    cfg_set_defaults(&r);
    r.ptt.method = PTT_METHOD_SERIAL;
    snprintf(r.ptt.device, sizeof(r.ptt.device), "COM9");
    TEST_ASSERT_TRUE(cfg_read(&r, TMP));
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_SERIAL, r.ptt.method);
    TEST_ASSERT_EQUAL_STRING("COM9", r.ptt.device);
}

/* Out-of-range INI values are rejected (field keeps its pre-read default). */
/* The sbitx field stations run on legacy [main] radio_model = 0.  If that ever
 * stops mapping to hermes_shm they simply never key, silently, on air. */
void test_legacy_radio_model_zero_maps_to_hermes_shm(void)
{
    FILE *f = fopen(TMP, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("[main]\nradio_model = 0\n", f);
    fclose(f);

    mercury_config r;
    cfg_set_defaults(&r);
    TEST_ASSERT_TRUE(cfg_read(&r, TMP));
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_HERMES_SHM, r.ptt.method);
    TEST_ASSERT_EQUAL_INT(RADIO_TYPE_NONE, r.ptt.hamlib_model);
}

/* "serial_rts" shipped as a method name before DTR and inversion existed and
 * is already in people's mercury.ini.  It has to keep meaning plain RTS. */
void test_serial_rts_alias_still_parses(void)
{
    ptt_method_t m = PTT_METHOD_NONE;
    TEST_ASSERT_TRUE(cfg_ptt_method_parse("serial_rts", &m));
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_SERIAL, m);

    TEST_ASSERT_TRUE(cfg_ptt_method_parse("serial", &m));
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_SERIAL, m);
    TEST_ASSERT_TRUE(cfg_ptt_method_parse("cm108", &m));
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_CM108, m);
    TEST_ASSERT_FALSE(cfg_ptt_method_parse("nonsense", &m));

    ptt_config_t config;
    memset(&config, 0, sizeof(config));
    config.serial_line = PTT_LINE_BOTH;
    config.serial_invert_rts = true;
    config.serial_invert_dtr = true;
    TEST_ASSERT_TRUE(cfg_ptt_config_set_method(&config, "serial_rts"));
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_SERIAL, config.method);
    TEST_ASSERT_EQUAL_INT(PTT_LINE_RTS, config.serial_line);
    TEST_ASSERT_FALSE(config.serial_invert_rts);
    TEST_ASSERT_FALSE(config.serial_invert_dtr);
}

void test_serial_rts_alias_overrides_serial_options(void)
{
    FILE *f = fopen(TMP, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("[ptt]\nmethod = serial_rts\nline = both\ninvert = both\n", f);
    fclose(f);

    mercury_config r;
    cfg_set_defaults(&r);
    TEST_ASSERT_TRUE(cfg_read(&r, TMP));
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_SERIAL, r.ptt.method);
    TEST_ASSERT_EQUAL_INT(PTT_LINE_RTS, r.ptt.serial_line);
    TEST_ASSERT_FALSE(r.ptt.serial_invert_rts);
    TEST_ASSERT_FALSE(r.ptt.serial_invert_dtr);
}

/* An AIOC needs both lines driven with RTS inverted; this is the exact config
 * from the AIOC documentation, round-tripped through the INI. */
void test_aioc_serial_config_roundtrips(void)
{
    FILE *f = fopen(TMP, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("[ptt]\nmethod = serial\ndevice = /dev/ttyACM0\n"
          "line = both\ninvert = rts\n", f);
    fclose(f);

    mercury_config r;
    cfg_set_defaults(&r);
    TEST_ASSERT_TRUE(cfg_read(&r, TMP));
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_SERIAL, r.ptt.method);
    TEST_ASSERT_EQUAL_STRING("/dev/ttyACM0", r.ptt.device);
    TEST_ASSERT_EQUAL_INT(PTT_LINE_BOTH, r.ptt.serial_line);
    TEST_ASSERT_TRUE(r.ptt.serial_invert_rts);
    TEST_ASSERT_FALSE(r.ptt.serial_invert_dtr);
}

/* Defaults must be the safe/common case: RTS, not inverted, GPIO3. */
void test_ptt_defaults_are_rts_noninverted_gpio3(void)
{
    mercury_config r;
    cfg_set_defaults(&r);
    TEST_ASSERT_EQUAL_INT(PTT_LINE_RTS, r.ptt.serial_line);
    TEST_ASSERT_FALSE(r.ptt.serial_invert_rts);
    TEST_ASSERT_FALSE(r.ptt.serial_invert_dtr);
    TEST_ASSERT_EQUAL_INT(PTT_CM108_GPIO_DEFAULT, r.ptt.cm108_gpio);
    TEST_ASSERT_EQUAL_INT(3, r.ptt.cm108_gpio);
}

void test_invalid_line_and_invert_are_rejected(void)
{
    ptt_line_t line = PTT_LINE_RTS;
    TEST_ASSERT_TRUE(cfg_ptt_line_parse("dtr", &line));
    TEST_ASSERT_EQUAL_INT(PTT_LINE_DTR, line);
    TEST_ASSERT_FALSE(cfg_ptt_line_parse("cts", &line));

    bool ir = false, id = false;
    TEST_ASSERT_TRUE(cfg_ptt_invert_parse("both", &ir, &id));
    TEST_ASSERT_TRUE(ir); TEST_ASSERT_TRUE(id);
    TEST_ASSERT_FALSE(cfg_ptt_invert_parse("sometimes", &ir, &id));
}

/* The CM108 wire bytes.  GPIO n is bit n-1: byte 2 is its output value and
 * byte 3 is the output mask, which must remain set when unkeying. */
void test_cm108_report_encoding(void)
{
    unsigned char r[5];

    TEST_ASSERT_EQUAL_INT(0, mercury_cm108_report(true, 3, r));
    TEST_ASSERT_EQUAL_HEX8(0x00, r[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, r[1]);
    TEST_ASSERT_EQUAL_HEX8(0x04, r[2]);   /* GPIO3 -> bit 2 */
    TEST_ASSERT_EQUAL_HEX8(0x04, r[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00, r[4]);

    TEST_ASSERT_EQUAL_INT(0, mercury_cm108_report(false, 3, r));
    TEST_ASSERT_EQUAL_HEX8(0x00, r[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, r[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, r[2]);
    TEST_ASSERT_EQUAL_HEX8(0x04, r[3]);   /* still select GPIO3 for update */
    TEST_ASSERT_EQUAL_HEX8(0x00, r[4]);

    TEST_ASSERT_EQUAL_INT(0, mercury_cm108_report(true, 1, r));
    TEST_ASSERT_EQUAL_HEX8(0x01, r[2]);
    TEST_ASSERT_EQUAL_INT(0, mercury_cm108_report(true, 4, r));
    TEST_ASSERT_EQUAL_HEX8(0x08, r[2]);

    /* Out of range must be refused, not silently clamped. */
    TEST_ASSERT_EQUAL_INT(-1, mercury_cm108_report(true, 0, r));
    TEST_ASSERT_EQUAL_INT(-1, mercury_cm108_report(true, 5, r));
}

void test_cm108_linux_device_path_validation(void)
{
    TEST_ASSERT_TRUE(mercury_cm108_is_hidraw_path("/dev/hidraw0"));
    TEST_ASSERT_TRUE(mercury_cm108_is_hidraw_path("/dev/hidraw123"));

    TEST_ASSERT_FALSE(mercury_cm108_is_hidraw_path(NULL));
    TEST_ASSERT_FALSE(mercury_cm108_is_hidraw_path(""));
    TEST_ASSERT_FALSE(mercury_cm108_is_hidraw_path("/dev/ttyUSB0"));
    TEST_ASSERT_FALSE(mercury_cm108_is_hidraw_path("/dev/hidraw"));
    TEST_ASSERT_FALSE(mercury_cm108_is_hidraw_path("/dev/hidraw0/extra"));
    TEST_ASSERT_FALSE(mercury_cm108_is_hidraw_path("/tmp/hidraw0"));
}

void test_arq_tunables_clamp_rejects_garbage(void)
{
    FILE *f = fopen(TMP, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("[arq]\nchannel_guard_ms = 999999\nkeepalive_miss_limit = 0\n"
          "peer_payload_hold_s = 0\nstartup_max_s = 999\n"
          "retry_stagger_ms = 999999\n", f);
    fclose(f);

    mercury_config r;
    cfg_set_defaults(&r);
    TEST_ASSERT_TRUE(cfg_read(&r, TMP));
    TEST_ASSERT_EQUAL_INT(700, r.channel_guard_ms);   /* out of 200..3000 -> default kept */
    TEST_ASSERT_EQUAL_INT(5,   r.keepalive_miss_limit);/* out of 2..20   -> default kept  */
    TEST_ASSERT_EQUAL_INT(15,  r.peer_payload_hold_s); /* out of 1..120  -> default kept  */
    TEST_ASSERT_EQUAL_INT(10,  r.startup_max_s);       /* out of 2..60   -> default kept  */
    TEST_ASSERT_EQUAL_INT(2000, r.retry_stagger_ms);    /* out of 0..5000 -> default kept  */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_match_constants);
    RUN_TEST(test_arq_tunables_roundtrip);
    RUN_TEST(test_arq_tunables_clamp_rejects_garbage);
    RUN_TEST(test_legacy_radio_config_maps_to_hamlib_ptt);
    RUN_TEST(test_explicit_ptt_method_overrides_legacy_backend);
    RUN_TEST(test_invalid_ptt_method_rejects_config);
    RUN_TEST(test_partial_config_preserves_preselected_ptt_method);
    RUN_TEST(test_legacy_radio_model_zero_maps_to_hermes_shm);
    RUN_TEST(test_serial_rts_alias_still_parses);
    RUN_TEST(test_serial_rts_alias_overrides_serial_options);
    RUN_TEST(test_aioc_serial_config_roundtrips);
    RUN_TEST(test_ptt_defaults_are_rts_noninverted_gpio3);
    RUN_TEST(test_invalid_line_and_invert_are_rejected);
    RUN_TEST(test_cm108_report_encoding);
    RUN_TEST(test_cm108_linux_device_path_validation);
    return UNITY_END();
}
