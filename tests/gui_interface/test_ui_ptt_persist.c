/*
 * A PTT change made in the UI must survive a restart.
 *
 * Both front-ends change PTT with set_ptt_config: the web page over the
 * websocket, the Fyne app through mercury_ui_command().  The handler applies the
 * new config to the radio backend and writes it to the INI the modem was started
 * with.  Nothing tested the second half, so an edit could apply a change for the
 * running session and lose it on restart without any test noticing.
 *
 * The legacy set_radio_config command was removed (issue #219): it could only
 * express none, hermes_shm or a Hamlib model, so on a serial or CM108 station it
 * overwrote the PTT method and cleared the port, then saved that.  The last test
 * checks that an old client sending it is rejected and leaves mercury.ini as it
 * was.
 *
 * ui_communication.c is linked for real, together with the real INI code;
 * everything it would reach beyond that (radio backend, audio, modem, ARQ,
 * websocket) is a stub below.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "ui_communication.h"
#include "ui_history.h"
#include "hermes_log.h"
#include "message_store.h"
#include "../radio_io/radio_io.h"
#include "../audioio/audioio.h"
#include "../modem/modem.h"
#include "../data_interfaces/net.h"
#include "../data_interfaces/tcp_interfaces.h"
#include "../common/mercury_engine.h"

/* ---- the radio backend: remembers what it was last given ---------------- */

static ptt_config_t g_radio;
static int          g_radio_restarts;

void radio_io_get_config(ptt_config_t *config) { *config = g_radio; }
int  radio_io_restart(const ptt_config_t *config)
{
    g_radio = *config;
    g_radio_restarts++;
    return 0;
}
bool radio_io_get_frequency(bool allow_poll, uint64_t *frequency_hz, uint64_t *age_ms)
{ (void)allow_poll; (void)frequency_hz; (void)age_ms; return false; }
int  radio_io_get_radio_list(char ids[][16], char names[][64], int max_count)
{ (void)ids; (void)names; (void)max_count; return 0; }

/* ---- everything else the UI layer can reach: inert ---------------------- */

_Atomic bool shutdown_ = false;

void hermes_logf(hermes_log_level_t level, const char *component, const char *fmt, ...)
{ (void)level; (void)component; (void)fmt; }

void arq_conn_get_calls(char *my_call, char *src_addr, char *dst_addr, size_t bufsz)
{
    if (bufsz == 0) return;
    if (my_call)  my_call[0]  = '\0';
    if (src_addr) src_addr[0] = '\0';
    if (dst_addr) dst_addr[0] = '\0';
}
bool arq_get_peer_snr_x10(int *snr_x10) { (void)snr_x10; return false; }
bool arq_get_runtime_snapshot(arq_runtime_snapshot_t *snapshot) { (void)snapshot; return false; }

int  audioio_available_subsystems(int *subsystems, int max) { (void)subsystems; (void)max; return 0; }
bool audioio_health_ok(char *reason, size_t reasonlen)
{ if (reason && reasonlen) reason[0] = '\0'; return true; }
void audioio_health_reason(char *buf, size_t buflen) { if (buf && buflen) buf[0] = '\0'; }
int  audioio_restart(const char *capture_dev, const char *playback_dev,
                     int audio_subsys, int capture_channel_layout)
{ (void)capture_dev; (void)playback_dev; (void)audio_subsys; (void)capture_channel_layout; return 0; }
int  audioio_wait_healthy(int timeout_ms) { (void)timeout_ms; return 0; }
int  get_soundcard_list(int audio_system, int mode,
                        char ids[][AUDIO_DEV_STR_MAX], char dev_names[][AUDIO_DEV_STR_MAX], int max_count)
{ (void)audio_system; (void)mode; (void)ids; (void)dev_names; (void)max_count; return 0; }

int   modem_get_rx_spectrum_seq(float *out_dB, int max_bins, uint64_t *seq_out)
{ (void)out_dB; (void)max_bins; (void)seq_out; return 0; }
float modem_get_tx_gain(void) { return 1.0f; }
float modem_get_tx_peak_dbfs(void) { return -100.0f; }
void  modem_set_spectrum_enabled(bool enabled) { (void)enabled; }
void  modem_set_tx_gain(float linear) { (void)linear; }

char *msg_store_snapshot(size_t *count_out, size_t *len_out)
{ if (count_out) *count_out = 0; if (len_out) *len_out = 0; return NULL; }
int      net_get_status(int port_type) { (void)port_type; return 0; }
uint32_t tnc_get_last_bitrate_bps(void) { return 0; }
float    tnc_get_last_snr(void) { return 0.0f; }

int  ui_device_list_to_json(const char *type_name, const ui_device_t *devs, int count,
                            const char *selected, char *buf, size_t buflen)
{ (void)type_name; (void)devs; (void)count; (void)selected; (void)buf; (void)buflen; return 0; }
void ui_devices_disambiguate(ui_device_t *devs, int count) { (void)devs; (void)count; }
char *ui_history_frame_build(const char *snap, size_t count, size_t snap_len)
{ (void)snap; (void)count; (void)snap_len; return NULL; }
int  ui_status_to_json(const ui_status_t *st, char *buf, size_t buflen)
{ (void)st; (void)buf; (void)buflen; return 0; }

int  ws_broadcast_binary(ws_ctx_t *ctx, const void *data, size_t len)
{ (void)ctx; (void)data; (void)len; return 0; }
int  ws_broadcast_json(ws_ctx_t *ctx, const char *json) { (void)ctx; (void)json; return 0; }
int  ws_init(ws_ctx_t *ctx, uint16_t port, ws_command_callback_t cmd_callback, void *cb_data,
             ws_connect_callback_t connect_callback, void *connect_cb_data, bool tls_enabled,
             const char *tls_cert_path, const char *tls_key_path)
{ (void)ctx; (void)port; (void)cmd_callback; (void)cb_data; (void)connect_callback;
  (void)connect_cb_data; (void)tls_enabled; (void)tls_cert_path; (void)tls_key_path; return 0; }
void ws_shutdown(ws_ctx_t *ctx) { (void)ctx; }

/* ---- fixture ------------------------------------------------------------- */

static char    g_ini[256];
static ui_ctx_t g_ctx;

/* A station on Hamlib, as saved in its INI -- the state an operator starts from. */
void setUp(void)
{
    snprintf(g_ini, sizeof(g_ini), "/tmp/mercury_ui_ptt_persist_%d.ini", (int)getpid());
    remove(g_ini);

    memset(&g_ctx, 0, sizeof(g_ctx));
    pthread_mutex_init(&g_ctx.cfg_mutex, NULL);
    cfg_set_defaults(&g_ctx.cfg);
    g_ctx.cfg.ptt.method       = PTT_METHOD_HAMLIB;
    g_ctx.cfg.ptt.hamlib_model = 1049;
    snprintf(g_ctx.cfg.ptt.device, sizeof(g_ctx.cfg.ptt.device), "/dev/ttyUSB1");
    snprintf(g_ctx.cfg_path, sizeof(g_ctx.cfg_path), "%s", g_ini);
    TEST_ASSERT_TRUE(cfg_write(&g_ctx.cfg, g_ini));

    g_radio          = g_ctx.cfg.ptt;
    g_radio_restarts = 0;
}

void tearDown(void)
{
    pthread_mutex_destroy(&g_ctx.cfg_mutex);
    remove(g_ini);
}

static int send_cmd(const char *name, const char *v1, const char *v2, const char *v3,
                    const char *v4, const char *v5, const char *v6, const char *v7)
{
    ws_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.command, sizeof(cmd.command), "%s", name);
    snprintf(cmd.value,  sizeof(cmd.value),  "%s", v1 ? v1 : "");
    snprintf(cmd.value2, sizeof(cmd.value2), "%s", v2 ? v2 : "");
    snprintf(cmd.value3, sizeof(cmd.value3), "%s", v3 ? v3 : "");
    snprintf(cmd.value4, sizeof(cmd.value4), "%s", v4 ? v4 : "");
    snprintf(cmd.value5, sizeof(cmd.value5), "%s", v5 ? v5 : "");
    snprintf(cmd.value6, sizeof(cmd.value6), "%s", v6 ? v6 : "");
    snprintf(cmd.value7, sizeof(cmd.value7), "%s", v7 ? v7 : "");
    return ui_comm_handle_command(&g_ctx, &cmd);
}

/* What the modem would load on its next start. */
static mercury_config reload(void)
{
    mercury_config r;
    cfg_set_defaults(&r);
    TEST_ASSERT_TRUE_MESSAGE(cfg_read(&r, g_ini), "saved INI does not parse");
    return r;
}

/* ---- tests --------------------------------------------------------------- */

/* The case #219 broke: switching to a serial interface (an AIOC / DigiRig
 * keying both lines, RTS inverted).  It must be applied AND be what the next
 * start reads back -- method, port and every serial option. */
void test_set_ptt_config_serial_is_saved(void)
{
    TEST_ASSERT_EQUAL_INT(0, send_cmd("set_ptt_config", "serial", "/dev/ttyACM0",
                                      "", "", "both", "rts", ""));

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_radio_restarts, "change was not applied to the radio");
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_SERIAL, g_radio.method);

    mercury_config r = reload();
    TEST_ASSERT_EQUAL_INT_MESSAGE(PTT_METHOD_SERIAL, r.ptt.method,
        "PTT method was applied but not saved: it is lost on restart");
    TEST_ASSERT_EQUAL_STRING("/dev/ttyACM0", r.ptt.device);
    TEST_ASSERT_EQUAL_INT(PTT_LINE_BOTH, r.ptt.serial_line);
    TEST_ASSERT_TRUE(r.ptt.serial_invert_rts);
    TEST_ASSERT_FALSE(r.ptt.serial_invert_dtr);
}

/* The sbitx stations: hermes_shm, the INI form of -S. */
void test_set_ptt_config_hermes_shm_is_saved(void)
{
    TEST_ASSERT_EQUAL_INT(0, send_cmd("set_ptt_config", "hermes_shm", "",
                                      "", "", "", "", ""));
    mercury_config r = reload();
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_HERMES_SHM, r.ptt.method);
}

/* A CM108 interface, chosen by its GPIO pin. */
void test_set_ptt_config_cm108_is_saved(void)
{
    TEST_ASSERT_EQUAL_INT(0, send_cmd("set_ptt_config", "cm108", "",
                                      "", "", "", "", "4"));
    mercury_config r = reload();
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_CM108, r.ptt.method);
    TEST_ASSERT_EQUAL_INT(4, r.ptt.cm108_gpio);
}

/* A change the handler refuses must leave the saved config alone. */
void test_rejected_set_ptt_config_does_not_touch_the_ini(void)
{
    TEST_ASSERT_NOT_EQUAL(0, send_cmd("set_ptt_config", "carrier_pigeon", "",
                                      "", "", "", "", ""));
    TEST_ASSERT_EQUAL_INT(0, g_radio_restarts);
    mercury_config r = reload();
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_HAMLIB, r.ptt.method);
    TEST_ASSERT_EQUAL_STRING("/dev/ttyUSB1", r.ptt.device);
}

/* Issue #219: an old client still sending set_radio_config (mercury-qt, a
 * stale cached web page) must be refused -- it used to switch a serial or CM108
 * station to Hamlib or "none", clear its port, and save that. */
void test_legacy_set_radio_config_is_refused(void)
{
    TEST_ASSERT_EQUAL_INT(0, send_cmd("set_ptt_config", "serial", "/dev/ttyACM0",
                                      "", "", "both", "rts", ""));
    g_radio_restarts = 0;

    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, send_cmd("set_radio_config", "-1", "",
                                              "", "", "", "", ""),
        "the removed legacy command was accepted");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_radio_restarts,
        "the legacy command restarted the radio");

    mercury_config r = reload();
    TEST_ASSERT_EQUAL_INT(PTT_METHOD_SERIAL, r.ptt.method);
    TEST_ASSERT_EQUAL_STRING("/dev/ttyACM0", r.ptt.device);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_set_ptt_config_serial_is_saved);
    RUN_TEST(test_set_ptt_config_hermes_shm_is_saved);
    RUN_TEST(test_set_ptt_config_cm108_is_saved);
    RUN_TEST(test_rejected_set_ptt_config_does_not_touch_the_ini);
    RUN_TEST(test_legacy_set_radio_config_is_refused);
    return UNITY_END();
}
