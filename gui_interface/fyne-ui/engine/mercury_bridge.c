/* Mercury C Engine Bridge — implementation
 *
 * Re-wires main.c's init / teardown sequence into three library-callable
 * functions: mercury_init(), mercury_request_shutdown(), mercury_shutdown().
 *
 * The idle loop lives in main() is replaced by CGo callbacks: the Go side
 * controls shutdown by setting shutdown_ = true, and the C side runs its
 * work (modem, audio I/O, websocket server) on background pthreads.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#endif

#include "freedv_api.h"
#include "arq.h"
#include "modem.h"
#include "broadcast.h"
#include "defines_modem.h"
#include "audioio/audioio.h"
#include "tcp_interfaces.h"
#include "hermes_log.h"
#include "radio_io.h"
#include "gui_interface/ui_communication.h"
#include "cfg_utils.h"

static ui_ctx_t g_ui_ctx;
static generic_modem_t g_modem;
static pthread_t g_radio_capture, g_radio_playback;
static int g_audio_system = -1;
static bool g_initialized = false;

/* --- storage for mercury config (one-time runtime snapshot) --- */
static mercury_config g_mcfg;

/* --- bridged globals (replace main.c's file-scope variables) --- */

volatile bool shutdown_ = false;

static volatile sig_atomic_t g_signal_count = 0;

static void handle_termination_signal(int sig)
{
    (void)sig;
    if (g_signal_count)
    {
        static const char msg[] = "Caught second signal, forcing exit.\n";
        if (write(STDERR_FILENO, msg, sizeof(msg) - 1)) {}
        _exit(1);
    }
    g_signal_count = 1;
    static const char msg[] = "Signal received, shutting down...\n";
    if (write(STDERR_FILENO, msg, sizeof(msg) - 1)) {}
    shutdown_ = true;
}

static int parse_rx_channel_layout(const char *value)
{
    if (!value) return -1;
    if (!strcmp(value, "left") || !strcmp(value, "LEFT"))   return LEFT;
    if (!strcmp(value, "right") || !strcmp(value, "RIGHT")) return RIGHT;
    if (!strcmp(value, "stereo") || !strcmp(value, "STEREO")) return STEREO;
    return -1;
}

/* ------------------------------------------------------------------ */
/*  mercury_init(config_path, log_path, verbose)                      */
/*                                                                     */
/*  config_path — path to mercury.ini (may be NULL → "mercury.ini")   */
/*  log_path    — path for the engine log file (or NULL = no file log) */
/*  verbose     — 0 = quiet, 1 = debug-level logging to file            */
/* ------------------------------------------------------------------ */
int mercury_init(const char *config_path, const char *log_path, int verbose)
{
    if (g_initialized) return 0;

    shutdown_ = false;
    g_signal_count = 0;

    signal(SIGINT,  handle_termination_signal);
    signal(SIGTERM, handle_termination_signal);

#ifdef _WIN32
    {
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
        {
            fprintf(stderr, "mercury_bridge: WSAStartup failed\n");
            return -1;
        }
    }
#endif

    /* ---- load configuration ---- */
    const char *ini = config_path ? config_path : "mercury.ini";
    cfg_set_defaults(&g_mcfg);
    if (access(ini, R_OK) == 0)
        cfg_read(&g_mcfg, ini);

    bool   ui_enabled        = true;  /* always on for the integrated UI */
    uint16_t ui_port         = g_mcfg.ui_port ? g_mcfg.ui_port : 10000;
    bool   tls_enabled       = g_mcfg.tls_enabled;
    bool   waterfall_enabled = g_mcfg.waterfall_enabled;
    int    radio_type        = g_mcfg.radio_type;
    char   radio_device[1024];
    strncpy(radio_device, g_mcfg.radio_device, sizeof(radio_device) - 1);
    radio_device[sizeof(radio_device) - 1] = '\0';
    int    radio_serial_speed = g_mcfg.radio_serial_speed;
    int    freedv_verbosity   = g_mcfg.freedv_verbosity;
    int    hamlib_log_level   = g_mcfg.hamlib_log_level;
    int    base_tcp_port      = g_mcfg.arq_tcp_base_port ? g_mcfg.arq_tcp_base_port : 8300;
    int    broadcast_port     = g_mcfg.broadcast_tcp_port ? g_mcfg.broadcast_tcp_port : 8100;

    char input_dev[512];
    char output_dev[512];
    strncpy(input_dev,  g_mcfg.input_device,  sizeof(input_dev) - 1);
    strncpy(output_dev, g_mcfg.output_device, sizeof(output_dev) - 1);
    input_dev[sizeof(input_dev) - 1] = '\0';
    output_dev[sizeof(output_dev) - 1] = '\0';

    int rx_input_channel = g_mcfg.capture_channel;
    int startup_mode = FREEDV_MODE_DATAC3;
    int test_mode = 0;

    g_audio_system = g_mcfg.sound_system;

    /* ---- ARQ tuning from config ---- */
    arq_set_no_progress_timeout_s(g_mcfg.no_progress_timeout_s);
    arq_set_disconnect_drain_timeout_s(g_mcfg.disconnect_drain_timeout_s);
    arq_set_data_retry_slots(g_mcfg.data_retry_slots);
    arq_set_mode_hold_after_downgrade_s(g_mcfg.mode_hold_after_downgrade_s);
    arq_set_ladder_up_successes(g_mcfg.ladder_up_successes);
    arq_set_retry_downgrade_threshold(g_mcfg.retry_downgrade_threshold);
    arq_set_channel_guard_ms(g_mcfg.channel_guard_ms);
    arq_set_iss_post_ack_guard_ms(g_mcfg.iss_post_ack_guard_ms);
    arq_set_keepalive_interval_s(g_mcfg.keepalive_interval_s);
    arq_set_keepalive_miss_limit(g_mcfg.keepalive_miss_limit);
    arq_set_peer_payload_hold_s(g_mcfg.peer_payload_hold_s);
    arq_set_startup_max_s(g_mcfg.startup_max_s);
    modem_set_tx_gain(powf(10.0f, g_mcfg.tx_gain_db / 20.0f));
    modem_set_busy_cfg((float)g_mcfg.busy_threshold_db,
                       (float)g_mcfg.busy_hysteresis_db,
                       (uint32_t)g_mcfg.busy_on_debounce_ms,
                       (uint32_t)g_mcfg.busy_hang_ms);
    modem_set_busy_detect_enabled(g_mcfg.busy_detect);

    /* ---- pick audio subsystem ---- */
    if (g_audio_system == -1)
        g_audio_system = audioio_pick_default_subsystem();

    if (g_audio_system == AUDIO_SUBSYSTEM_ALSA)
    {
        if (input_dev[0]  == 0) strcpy(input_dev,  "default");
        if (output_dev[0] == 0) strcpy(output_dev, "default");
    }

    /* ---- logging ---- */
    if (hermes_log_init(1024) == 0)
    {
        hermes_log_set_level(verbose ? HERMES_LOG_LEVEL_DEBUG : HERMES_LOG_LEVEL_INFO);
        if (log_path && log_path[0])
            hermes_log_set_file(log_path,
                                verbose ? HERMES_LOG_LEVEL_DEBUG : HERMES_LOG_LEVEL_TIMING,
                                false);
        HLOGI("bridge", "Async logger initialised (min_level=%s)", verbose ? "DEBUG" : "INFO");
    }
    else
    {
        fprintf(stderr, "mercury_bridge: async logger unavailable\n");
    }

    /* ---- audio I/O ---- */
    if (g_audio_system != AUDIO_SUBSYSTEM_SHM)
    {
        HLOGI("bridge", "Initialising audio I/O (subsystem=%d)", g_audio_system);
        if (audioio_init_internal(input_dev, output_dev, g_audio_system,
                                  rx_input_channel,
                                  &g_radio_capture, &g_radio_playback) != 0)
        {
            fprintf(stderr, "mercury_bridge: audio I/O init failed\n");
            hermes_log_shutdown();
            return -1;
        }
    }

    /* ---- radio I/O ---- */
    if (radio_io_init(radio_type, radio_device, hamlib_log_level, radio_serial_speed) != 0)
    {
        fprintf(stderr, "mercury_bridge: radio init failed\n");
        if (g_audio_system != AUDIO_SUBSYSTEM_SHM)
            audioio_deinit(&g_radio_capture, &g_radio_playback);
        hermes_log_shutdown();
        return -1;
    }

    /* ---- modem ---- */
    HLOGI("bridge", "Initialising modem (mode=%d)", startup_mode);
    init_modem(&g_modem, startup_mode, 1, test_mode, freedv_verbosity);
    modem_set_spectrum_enabled(waterfall_enabled);

    tnc_set_intervals(g_mcfg.tnc_keepalive_s, g_mcfg.tnc_buffer_report_ms);

    /* ---- ARQ ---- */
    if (arq_init(g_modem.payload_bytes_per_modem_frame, g_modem.mode) != EXIT_SUCCESS)
    {
        fprintf(stderr, "mercury_bridge: ARQ init failed\n");
        shutdown_ = true;
        shutdown_modem(&g_modem);
        if (g_audio_system != AUDIO_SUBSYSTEM_SHM)
            audioio_deinit(&g_radio_capture, &g_radio_playback);
        hermes_log_shutdown();
        return -1;
    }

    /* ---- broadcast ---- */
    broadcast_run(&g_modem);

    /* ---- TCP interfaces ---- */
    HLOGI("bridge", "Initialising TCP interfaces (arq=%d, bc=%d)", base_tcp_port, broadcast_port);
    if (interfaces_init(base_tcp_port, broadcast_port,
                        g_modem.payload_bytes_per_modem_frame) != EXIT_SUCCESS)
    {
        fprintf(stderr, "mercury_bridge: TCP init failed\n");
        shutdown_ = true;
        interfaces_shutdown();
        shutdown_modem(&g_modem);
        if (g_audio_system != AUDIO_SUBSYSTEM_SHM)
            audioio_deinit(&g_radio_capture, &g_radio_playback);
        hermes_log_shutdown();
        return -1;
    }

    /* ---- sync mcfg with final runtime values ---- */
    g_mcfg.ui_enabled        = ui_enabled;
    g_mcfg.ui_port           = ui_port;
    g_mcfg.tls_enabled       = tls_enabled;
    g_mcfg.waterfall_enabled = waterfall_enabled;
    g_mcfg.radio_type        = radio_type;
    strncpy(g_mcfg.radio_device, radio_device, sizeof(g_mcfg.radio_device) - 1);
    g_mcfg.radio_device[sizeof(g_mcfg.radio_device) - 1] = '\0';
    strncpy(g_mcfg.input_device,  input_dev,  sizeof(g_mcfg.input_device) - 1);
    g_mcfg.input_device[sizeof(g_mcfg.input_device) - 1] = '\0';
    strncpy(g_mcfg.output_device, output_dev, sizeof(g_mcfg.output_device) - 1);
    g_mcfg.output_device[sizeof(g_mcfg.output_device) - 1] = '\0';
    g_mcfg.capture_channel   = rx_input_channel;
    g_mcfg.sound_system      = g_audio_system;
    g_mcfg.arq_tcp_base_port = base_tcp_port;
    g_mcfg.broadcast_tcp_port = broadcast_port;

    /* ---- UI / WebSocket ---- */
    HLOGI("bridge", "Initialising UI websocket (port=%u, tls=%d, waterfall=%d)",
          ui_port, tls_enabled, waterfall_enabled);

    if (ui_comm_init(&g_ui_ctx, ui_port, tls_enabled,
                     waterfall_enabled ? 1 : 0,
                     g_audio_system, input_dev, output_dev, rx_input_channel,
                     &g_mcfg, ini) != 0)
    {
        HLOGW("bridge", "UI communication init failed — running headless");
    }

    g_initialized = true;
    HLOGI("bridge", "Mercury engine initialised successfully");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  mercury_request_shutdown()                                         */
/*                                                                     */
/*  Sets the global shutdown flag.  All background threads poll this   */
/*  flag and will begin their graceful-exit sequence.  Call this       */
/*  BEFORE mercury_shutdown().                                         */
/* ------------------------------------------------------------------ */
void mercury_request_shutdown(void)
{
    shutdown_ = true;
}

/* ------------------------------------------------------------------ */
/*  mercury_shutdown()                                                 */
/*                                                                     */
/*  Graceful teardown in the same order as main.c.  Joins all          */
/*  background threads and frees resources.                            */
/* ------------------------------------------------------------------ */
void mercury_shutdown(void)
{
    if (!g_initialized) return;

    HLOGI("bridge", "Shutting down Mercury engine");

    shutdown_ = true;

#ifndef _WIN32
    alarm(10);
#endif

    interfaces_shutdown();
    shutdown_modem(&g_modem);

    if (g_audio_system != AUDIO_SUBSYSTEM_SHM)
        audioio_deinit(&g_radio_capture, &g_radio_playback);

    ui_comm_shutdown(&g_ui_ctx);

    radio_io_shutdown();

    HLOGI("bridge", "Mercury engine shut down");
    hermes_log_shutdown();

#ifdef _WIN32
    WSACleanup();
#endif

    g_initialized = false;
}
