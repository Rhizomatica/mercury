/* Mercury Engine — shared init / teardown implementation
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "cfg_utils.h"
#include "ui_communication.h"
#include "mercury_engine.h"
#include "virtual_clock.h"

/* ---- shared globals ---- */
volatile bool shutdown_ = false;
static generic_modem_t g_modem;
static pthread_t g_radio_capture, g_radio_playback;
static int g_audio_system = -1;
static ui_ctx_t g_ui_ctx;
static mercury_config g_mcfg;
static bool g_initialized = false;
static char g_input_dev[512];
static char g_output_dev[512];

static int apply_audio_defaults(int audio_system, char *input_dev, size_t in_size,
                                char *output_dev, size_t out_size)
{
    switch (audio_system)
    {
    case AUDIO_SUBSYSTEM_ALSA:
        if (input_dev[0] == 0)  strcpy(input_dev, "default");
        if (output_dev[0] == 0) strcpy(output_dev, "default");
        break;
    case AUDIO_SUBSYSTEM_PULSE:
    case AUDIO_SUBSYSTEM_WASAPI:
    case AUDIO_SUBSYSTEM_DSOUND:
        break;
    case AUDIO_SUBSYSTEM_OSS:
        if (input_dev[0] == 0)  snprintf(input_dev, in_size, "/dev/dsp");
        if (output_dev[0] == 0) snprintf(output_dev, out_size, "/dev/dsp");
        break;
    default:
        break;
    }
    return 0;
}

int mercury_engine_init(const mercury_config *cfg,
                        const char *config_path,
                        const char *log_path,
                        bool log_jsonl,
                        int startup_mode,
                        int test_mode)
{
    if (g_initialized) return 0;

    shutdown_ = false;

    memcpy(&g_mcfg, cfg, sizeof(mercury_config));

#ifdef _WIN32
    {
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
        {
            fprintf(stderr, "mercury_engine: WSAStartup failed\n");
            return -1;
        }
    }
#endif

    int  audio_system    = cfg->sound_system;
    bool ui_enabled       = cfg->ui_enabled;
    uint16_t ui_port      = cfg->ui_port ? cfg->ui_port : 10000;
    bool tls_enabled      = cfg->tls_enabled;
    bool waterfall_enabled = cfg->waterfall_enabled;
    int  radio_type       = cfg->radio_type;
    char radio_device[1024];
    snprintf(radio_device, sizeof(radio_device), "%s", cfg->radio_device);
    int  radio_serial_speed = cfg->radio_serial_speed;
    int  freedv_verbosity   = cfg->freedv_verbosity;
    int  hamlib_log_level   = cfg->hamlib_log_level;
    int  base_tcp_port      = cfg->arq_tcp_base_port ? cfg->arq_tcp_base_port : 8300;
    int  broadcast_port     = cfg->broadcast_tcp_port ? cfg->broadcast_tcp_port : 8100;
    bool verbose            = cfg->verbose;

    snprintf(g_input_dev,  sizeof(g_input_dev),  "%s", cfg->input_device);
    snprintf(g_output_dev, sizeof(g_output_dev), "%s", cfg->output_device);

    int rx_input_channel = cfg->capture_channel;

    /* ---- ARQ tuning ---- */
    arq_set_no_progress_timeout_s(cfg->no_progress_timeout_s);
    arq_set_disconnect_drain_timeout_s(cfg->disconnect_drain_timeout_s);
    /* Data-plane retries only: must NOT inflate CALL/ACCEPT (see arq.c). Using
     * the all-slots setter here regressed connection setup — the IRS lingered
     * ~90 s in ACCEPTING after the caller gave up. */
    arq_set_data_retry_slots(cfg->data_retry_slots);
    arq_set_mode_hold_after_downgrade_s(cfg->mode_hold_after_downgrade_s);
    arq_set_ladder_up_successes(cfg->ladder_up_successes);
    arq_set_retry_downgrade_threshold(cfg->retry_downgrade_threshold);
    arq_set_channel_guard_ms(cfg->channel_guard_ms);
    arq_set_iss_post_ack_guard_ms(cfg->iss_post_ack_guard_ms);
    arq_set_keepalive_interval_s(cfg->keepalive_interval_s);
    arq_set_keepalive_miss_limit(cfg->keepalive_miss_limit);
    arq_set_peer_payload_hold_s(cfg->peer_payload_hold_s);
    arq_set_startup_max_s(cfg->startup_max_s);
    modem_set_tx_gain(powf(10.0f, cfg->tx_gain_db / 20.0f));
    modem_set_tx_delay_ms(cfg->tx_delay_ms);
    modem_set_busy_cfg((float)cfg->busy_threshold_db,
                       (float)cfg->busy_hysteresis_db,
                       (uint32_t)cfg->busy_on_debounce_ms,
                       (uint32_t)cfg->busy_hang_ms);
    modem_set_busy_detect_enabled(cfg->busy_detect);

    /* ---- pick audio subsystem ---- */
    g_audio_system = audio_system;
    if (g_audio_system == -1)
        g_audio_system = audioio_pick_default_subsystem();

    apply_audio_defaults(g_audio_system, g_input_dev, sizeof(g_input_dev),
                         g_output_dev, sizeof(g_output_dev));

    /* -x sock is the virtual-clock lockstep bench transport: switch the
     * process time base BEFORE any modem/ARQ thread starts, so every
     * time_now_ms() read in this run sees a single (virtual) clock. */
    if (g_audio_system == AUDIO_SUBSYSTEM_SOCK)
        virtual_clock_enable(VIRTUAL_CLOCK_EPOCH_MS);

    /* ---- logging ---- */
    if (hermes_log_init(1024) == 0)
    {
        hermes_log_set_level(verbose ? HERMES_LOG_LEVEL_DEBUG : HERMES_LOG_LEVEL_INFO);
        if (log_path && log_path[0])
            hermes_log_set_file(log_path,
                                verbose ? HERMES_LOG_LEVEL_DEBUG : HERMES_LOG_LEVEL_TIMING,
                                log_jsonl);
        HLOGI("engine", "Logger initialised (min_level=%s)", verbose ? "DEBUG" : "INFO");
    }
    else
    {
        fprintf(stderr, "mercury_engine: async logger unavailable\n");
    }

    /* ---- audio I/O ---- */
    if (g_audio_system != AUDIO_SUBSYSTEM_SHM)
    {
        HLOGI("engine", "Initialising audio I/O (subsystem=%d)", g_audio_system);
        if (audioio_init_internal(g_input_dev, g_output_dev, g_audio_system,
                                  rx_input_channel,
                                  &g_radio_capture, &g_radio_playback) != 0)
        {
            fprintf(stderr, "mercury_engine: audio I/O init failed\n");
            hermes_log_shutdown();
            return -1;
        }
    }

    /* ---- radio I/O ---- */
    if (radio_io_init(radio_type, radio_device, hamlib_log_level, radio_serial_speed) != 0)
    {
        fprintf(stderr, "mercury_engine: radio init failed\n");
        if (g_audio_system != AUDIO_SUBSYSTEM_SHM)
            audioio_deinit(&g_radio_capture, &g_radio_playback);
        hermes_log_shutdown();
        return -1;
    }

    /* ---- modem ---- */
    HLOGI("engine", "Initialising modem (mode=%d)", startup_mode);
    init_modem(&g_modem, startup_mode, 1, test_mode, freedv_verbosity);
    modem_set_spectrum_enabled(ui_enabled && waterfall_enabled);

    tnc_set_intervals(cfg->tnc_keepalive_s, cfg->tnc_buffer_report_ms);

    /* ---- ARQ ---- */
    if (arq_init(g_modem.payload_bytes_per_modem_frame, g_modem.mode) != EXIT_SUCCESS)
    {
        fprintf(stderr, "mercury_engine: ARQ init failed\n");
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
    HLOGI("engine", "Initialising TCP interfaces (arq=%d, bc=%d)", base_tcp_port, broadcast_port);
    if (interfaces_init(base_tcp_port, broadcast_port,
                        g_modem.payload_bytes_per_modem_frame) != EXIT_SUCCESS)
    {
        fprintf(stderr, "mercury_engine: TCP init failed\n");
        shutdown_ = true;
        interfaces_shutdown();
        shutdown_modem(&g_modem);
        if (g_audio_system != AUDIO_SUBSYSTEM_SHM)
            audioio_deinit(&g_radio_capture, &g_radio_playback);
        hermes_log_shutdown();
        return -1;
    }

    /* ---- sync g_mcfg with final runtime values ---- */
    g_mcfg.ui_enabled        = ui_enabled;
    g_mcfg.ui_port           = ui_port;
    g_mcfg.tls_enabled       = tls_enabled;
    g_mcfg.waterfall_enabled = waterfall_enabled;
    g_mcfg.radio_type        = radio_type;
    snprintf(g_mcfg.radio_device,  sizeof(g_mcfg.radio_device),  "%s", radio_device);
    snprintf(g_mcfg.input_device,  sizeof(g_mcfg.input_device),  "%s", g_input_dev);
    snprintf(g_mcfg.output_device, sizeof(g_mcfg.output_device), "%s", g_output_dev);
    g_mcfg.capture_channel   = rx_input_channel;
    g_mcfg.sound_system      = g_audio_system;
    g_mcfg.arq_tcp_base_port = base_tcp_port;
    g_mcfg.broadcast_tcp_port = broadcast_port;
    g_mcfg.verbose           = verbose;
    g_mcfg.freedv_verbosity  = freedv_verbosity;
    g_mcfg.hamlib_log_level  = hamlib_log_level;
    g_mcfg.radio_serial_speed = radio_serial_speed;

    /* ---- UI / WebSocket ---- */
    if (ui_enabled)
    {
        HLOGI("engine", "Initialising UI websocket (port=%u, tls=%d, waterfall=%d)",
              ui_port, tls_enabled, waterfall_enabled);
        if (ui_comm_init(&g_ui_ctx, ui_port, tls_enabled,
                         waterfall_enabled ? 1 : 0,
                         g_audio_system, g_input_dev, g_output_dev, rx_input_channel,
                         &g_mcfg, config_path) != 0)
        {
            HLOGW("engine", "UI communication init failed — running headless");
        }
    }
    else
    {
        memset(&g_ui_ctx, 0, sizeof(g_ui_ctx));
        HLOGI("engine", "UI communication disabled");
    }

    g_initialized = true;
    HLOGI("engine", "Mercury engine initialised");
    return 0;
}

void mercury_engine_shutdown(void)
{
    if (!g_initialized) return;

    HLOGI("engine", "Shutting down");

    shutdown_ = true;

    interfaces_shutdown();
    shutdown_modem(&g_modem);

    if (g_audio_system != AUDIO_SUBSYSTEM_SHM)
        audioio_deinit(&g_radio_capture, &g_radio_playback);

    ui_comm_shutdown(&g_ui_ctx);
    radio_io_shutdown();

    HLOGI("engine", "Mercury engine shut down");
    hermes_log_shutdown();

#ifdef _WIN32
    WSACleanup();
#endif

    g_initialized = false;
}

bool mercury_engine_is_initialized(void)
{
    return g_initialized;
}
