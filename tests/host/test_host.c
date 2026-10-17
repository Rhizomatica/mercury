/* host/: the daemon build's audio, radio and UI modules, driven through
 * host/mercury_host.h the way a host program (hermes-radio-daemon) drives
 * them.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "unity.h"
#include "audioio.h"
#include "mercury_host.h"
#include "radio_io.h"
#include "ring_buffer_posix.h"
#include "ui_transport.h"

static pthread_t s_cap, s_play;

void setUp(void)
{
    TEST_ASSERT_EQUAL_INT(0, audioio_init_internal(NULL, NULL, 0, 0, &s_cap, &s_play));
}

void tearDown(void)
{
    audioio_deinit(&s_cap, &s_play);
    mercury_host_set_radio(NULL);
    mercury_host_set_ui(NULL);
    ui_transport_shutdown();
}

/* RX goes into the capture ring as the int32 samples every audioio backend
 * writes (S16 in the top half). */
static void test_rx_reaches_capture_ring(void)
{
    int16_t rx[3] = { 1, -32768, 32767 };
    int16_t tx[3];
    int32_t ring[3];

    TEST_ASSERT_EQUAL_UINT(0, mercury_host_audio_exchange(rx, tx, 3));
    TEST_ASSERT_EQUAL_UINT(3 * sizeof(int32_t), size_buffer(capture_buffer));
    read_buffer(capture_buffer, (uint8_t *) ring, sizeof(ring));
    TEST_ASSERT_EQUAL_INT32(1 << 16, ring[0]);
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, ring[1]);
    TEST_ASSERT_EQUAL_INT32(32767 << 16, ring[2]);
}

/* TX drains what the modem queued, then silence; a NULL rx still drains. */
static void test_tx_drains_then_silence(void)
{
    int32_t ring[2] = { 1000 << 16, -1000 << 16 };
    int16_t tx[5];

    write_buffer(playback_buffer, (uint8_t *) ring, sizeof(ring));
    memset(tx, 0x55, sizeof(tx));
    TEST_ASSERT_EQUAL_UINT(2, mercury_host_audio_exchange(NULL, tx, 5));
    TEST_ASSERT_EQUAL_INT16(1000, tx[0]);
    TEST_ASSERT_EQUAL_INT16(-1000, tx[1]);
    TEST_ASSERT_EQUAL_INT16(0, tx[2]);
    TEST_ASSERT_EQUAL_INT16(0, tx[4]);
    TEST_ASSERT_EQUAL_UINT(0, size_buffer(playback_buffer));
    TEST_ASSERT_EQUAL_UINT(0, size_buffer(capture_buffer));
}

/* More TX queued than the block takes: the rest waits for the next block. */
static void test_tx_longer_than_block(void)
{
    int32_t ring[600];
    int16_t tx[400];

    for (int i = 0; i < 600; i++)
        ring[i] = i << 16;
    write_buffer(playback_buffer, (uint8_t *) ring, sizeof(ring));
    TEST_ASSERT_EQUAL_UINT(400, mercury_host_audio_exchange(NULL, tx, 400));
    TEST_ASSERT_EQUAL_INT16(399, tx[399]);
    TEST_ASSERT_EQUAL_UINT(200, mercury_host_audio_exchange(NULL, tx, 400));
    TEST_ASSERT_EQUAL_INT16(400, tx[0]);
    TEST_ASSERT_EQUAL_INT16(0, tx[200]);
}

/* Before init and after deinit: silence, and nothing touched. */
static void test_silence_when_stopped(void)
{
    int16_t rx[4] = { 7, 7, 7, 7 }, tx[4] = { 9, 9, 9, 9 };

    audioio_deinit(&s_cap, &s_play);
    TEST_ASSERT_NULL(capture_buffer);
    TEST_ASSERT_EQUAL_UINT(0, mercury_host_audio_exchange(rx, tx, 4));
    TEST_ASSERT_EQUAL_INT16(0, tx[0]);
    TEST_ASSERT_EQUAL_INT16(0, tx[3]);
}

static int s_key_calls, s_key_last;
static int key_cb(bool tx, void *user)
{
    s_key_calls++;
    s_key_last = tx;
    return *(int *) user;
}
static bool freq_cb(uint64_t *hz, void *user) { (void) user; *hz = 7050000; return true; }

/* PTT and frequency go to the host; shutdown unkeys only a keyed radio. */
static void test_radio_ops(void)
{
    int rc = 0;
    mercury_host_radio_ops ops = { key_cb, freq_cb, &rc };
    uint64_t hz = 0, age = 99;

    TEST_ASSERT_FALSE(radio_io_enabled());
    TEST_ASSERT_EQUAL_INT(-1, radio_io_key_on());
    mercury_host_set_radio(&ops);
    TEST_ASSERT_TRUE(radio_io_enabled());

    s_key_calls = 0;
    TEST_ASSERT_EQUAL_INT(0, radio_io_key_on());
    TEST_ASSERT_EQUAL_INT(1, s_key_last);
    radio_io_shutdown();
    TEST_ASSERT_EQUAL_INT(2, s_key_calls);
    TEST_ASSERT_EQUAL_INT(0, s_key_last);
    radio_io_shutdown();
    TEST_ASSERT_EQUAL_INT(2, s_key_calls);

    rc = -1;
    TEST_ASSERT_EQUAL_INT(-1, radio_io_key_on());

    TEST_ASSERT_TRUE(radio_io_get_frequency(true, &hz, &age));
    TEST_ASSERT_EQUAL_UINT64(7050000, hz);
    TEST_ASSERT_EQUAL_UINT64(0, age);
}

static char s_json[256];
static int s_cmds, s_connects;
static void json_cb(const char *json, void *user) { (void) user; snprintf(s_json, sizeof(s_json), "%s", json); }
static int cmd_cb(const ws_command_t *cmd, void *user)
{
    (void) user;
    s_cmds++;
    return strcmp(cmd->command, "set_waterfall") == 0 ? 0 : 5;
}
static void connect_cb(void *user) { (void) user; s_connects++; }

/* Broadcasts reach the host's ops; its clients' commands and connects reach
 * the handlers ui_communication registered. */
static void test_ui_both_ways(void)
{
    mercury_host_ui_ops ops = { json_cb, NULL, NULL };
    const char *cmd = "{\"command\":\"set_waterfall\",\"value\":\"on\"}";

    TEST_ASSERT_EQUAL_INT(-1, mercury_host_ui_command_json(cmd, strlen(cmd)));
    TEST_ASSERT_EQUAL_INT(0, ui_transport_init(10000, false, NULL, NULL, cmd_cb, connect_cb, NULL));

    s_json[0] = '\0';
    TEST_ASSERT_EQUAL_INT(0, ui_transport_broadcast_json("{\"a\":1}"));
    TEST_ASSERT_EQUAL_STRING("", s_json);
    mercury_host_set_ui(&ops);
    TEST_ASSERT_EQUAL_INT(0, ui_transport_broadcast_json("{\"a\":1}"));
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", s_json);
    TEST_ASSERT_EQUAL_INT(0, ui_transport_broadcast_binary("x", 1));

    s_cmds = 0;
    TEST_ASSERT_EQUAL_INT(0, mercury_host_ui_command_json(cmd, strlen(cmd)));
    TEST_ASSERT_EQUAL_INT(1, s_cmds);
    const char *other = "{\"command\":\"nope\"}";
    TEST_ASSERT_EQUAL_INT(5, mercury_host_ui_command_json(other, strlen(other)));
    TEST_ASSERT_EQUAL_INT(-1, mercury_host_ui_command_json("{oops", 5));

    s_connects = 0;
    mercury_host_ui_client_connected();
    TEST_ASSERT_EQUAL_INT(1, s_connects);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rx_reaches_capture_ring);
    RUN_TEST(test_tx_drains_then_silence);
    RUN_TEST(test_tx_longer_than_block);
    RUN_TEST(test_silence_when_stopped);
    RUN_TEST(test_radio_ops);
    RUN_TEST(test_ui_both_ways);
    return UNITY_END();
}
