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
    TEST_ASSERT_TRUE(cfg_read(&r, TMP));
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_SERIAL_RTS, r.ptt.method);
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
    r.ptt.method = PTT_METHOD_SERIAL_RTS;
    snprintf(r.ptt.device, sizeof(r.ptt.device), "COM9");
    TEST_ASSERT_TRUE(cfg_read(&r, TMP));
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_SERIAL_RTS, r.ptt.method);
    TEST_ASSERT_EQUAL_STRING("COM9", r.ptt.device);
}

/* Out-of-range INI values are rejected (field keeps its pre-read default). */
void test_arq_tunables_clamp_rejects_garbage(void)
{
    FILE *f = fopen(TMP, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("[arq]\nchannel_guard_ms = 999999\nkeepalive_miss_limit = 0\n"
          "peer_payload_hold_s = 0\nstartup_max_s = 999\n", f);
    fclose(f);

    mercury_config r;
    cfg_set_defaults(&r);
    TEST_ASSERT_TRUE(cfg_read(&r, TMP));
    TEST_ASSERT_EQUAL_INT(700, r.channel_guard_ms);   /* out of 200..3000 -> default kept */
    TEST_ASSERT_EQUAL_INT(5,   r.keepalive_miss_limit);/* out of 2..20   -> default kept  */
    TEST_ASSERT_EQUAL_INT(15,  r.peer_payload_hold_s); /* out of 1..120  -> default kept  */
    TEST_ASSERT_EQUAL_INT(10,  r.startup_max_s);       /* out of 2..60   -> default kept  */
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
    return UNITY_END();
}
