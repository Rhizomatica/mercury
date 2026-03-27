/* HERMES Modem
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
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

#define VERSION__ "1.9.5"
#ifndef GIT_HASH
#define GIT_HASH "unknown000"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#ifdef __linux__
#include <sched.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#endif


#include "freedv_api.h"
#include "ldpc_codes.h"
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

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

int freedv_modes[] = { FREEDV_MODE_DATAC1,
                       FREEDV_MODE_DATAC3,
                       FREEDV_MODE_DATAC0,
                       FREEDV_MODE_DATAC4,
                       FREEDV_MODE_DATAC13,
                       FREEDV_MODE_DATAC14,
                       FREEDV_MODE_FSK_LDPC };

char *freedv_mode_names[] = { "DATAC1",
                              "DATAC3",
                              "DATAC0",
                              "DATAC4",
                              "DATAC13",
                              "DATAC14",
                              "FSK_LDPC" };

volatile bool shutdown_ = false; // global shutdown flag

static void handle_termination_signal(int sig)
{
    (void)sig;
    shutdown_ = true;
    HLOGI("main", "Termination signal received, shutting down...");
    msleep(1500); // give some time for the main loop to exit and for the modem and interfaces to shutdown gracefully
    exit(0); // in case the main loop is stuck for some reason, we force exit after a delay
}

static int parse_rx_channel_layout(const char *value)
{
    if (!value)
        return -1;
    if (!strcmp(value, "left") || !strcmp(value, "LEFT"))
        return LEFT;
    if (!strcmp(value, "right") || !strcmp(value, "RIGHT"))
        return RIGHT;
    if (!strcmp(value, "stereo") || !strcmp(value, "STEREO"))
        return STEREO;
    return -1;
}

static void print_usage(const char *prog)
{
    printf("Usage modes: \n");
    printf("%s -m [mode_index] -i [device] -o [device] -x [sound_system] -p [arq_tcp_base_port] -b [broadcast_tcp_port] -f [freedv_verbosity] -k [rx_input_channel] [-G] [-T] [-U ui_port] [-W]\n", prog);
    printf("%s [-h -l -z]\n", prog);
    printf("\nOptions:\n");
    printf(" -c [cpu_nr]                Run on CPU [cpu_nr]. Use -1 to disable CPU selection, which is the default.\n");
    printf(" -m [mode_index]            Startup payload mode index shown in \"-l\" output. Used for broadcast and idle/disconnected ARQ decode. Default is 1 (DATAC3)\n");
    printf(" -s [mode_index]            Legacy alias for -m.\n");
    printf(" -f [freedv_verbosity]      FreeDV modem verbosity level (0..3). Default is 0.\n");
    printf(" -k [rx_input_channel]      Capture input channel: left, right, or stereo. Default is left.\n");
    printf(" -i [device]                Radio Capture device id (eg: \"plughw:0,0\").\n");
    printf(" -o [device]                Radio Playback device id (eg: \"plughw:0,0\").\n");
    printf(" -x [sound_system]          Sets the sound system or IO API to use: alsa, pulse, dsound, wasapi or shm. Default is alsa on Linux and dsound on Windows.\n");
    printf(" -p [arq_tcp_base_port]     Sets the ARQ TCP base port (control is base_port, data is base_port + 1). Default is 8300.\n");
    printf(" -b [broadcast_tcp_port]    Sets the broadcast TCP port. Default is 8100.\n");
    printf(" -U [ui_port]               Sets the UI port (WebSocket port). Default is 10000. Requires -G.\n");
    printf(" -W                         Disable waterfall/spectrum data sent to the UI (used to spare CPU).\n");
    printf(" -G                         Enable UI communication (WebSocket server for mercury-qt). Off by default.\n");
    printf(" -T                         Use WSS (WebSocket Secure/TLS) for UI communication. Requires -G. Default uses plain WS (no TLS).\n");
    printf(" -l                         Lists all modulator/coding modes.\n");
    printf(" -z                         Lists all available sound cards.\n");
    printf(" -v                         Verbose mode. Prints more information during execution.\n");
    printf(" -L <path>                  Write log to file (TIMING level and above).\n");
    printf(" -J                         Use JSONL format for log file (requires -L).\n");
    printf(" -R [radio_model]           Sets HAMLIB radio model.\n");
    printf(" -A [radio_address]         Sets HAMLIB radio device file or ip:port address.\n");
#ifdef HAVE_HERMES_SHM
    printf(" -S                         Use HERMES's shared memory interface instead of HAMLIB (Do not use -R and -A in this case).\n");
#else
    printf(" -S                         HERMES shared memory radio control (Linux-only; unavailable in this build).\n");
#endif
    printf(" -C [config_file]           Path to init configuration file (INI format). Default is mercury.ini in the current directory.\n");
    printf(" -K                         List HAMLIB supported radio models.\n");
    printf(" -t                         Test TX mode.\n");
    printf(" -r                         Test RX mode.\n");
    printf(" -h                         Prints this help.\n");
}

int main(int argc, char *argv[])
{
#if defined(__linux__)
    printf("\e[0;31mRhizomatica Mercury Version %s (git %.8s)\e[0m\n", VERSION__, GIT_HASH); // we go red
#elif defined(_WIN32)
    printf("Rhizomatica Mercury Version %s (git %.8s)\n", VERSION__, GIT_HASH);
#endif

#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        fprintf(stderr, "WSAStartup failed\n");
        return EXIT_FAILURE;
    }
#endif
    int verbose = 0;
    const int mode_count = (int)(sizeof(freedv_modes) / sizeof(freedv_modes[0]));
    int cpu_nr = -1;
    bool list_modes = false;
    bool list_sndcards = false;
    const char *log_file_path = NULL;
    bool log_file_jsonl = false;
    int base_tcp_port = DEFAULT_ARQ_PORT; // default ARQ TCP port
    int broadcast_port = DEFAULT_BROADCAST_PORT; // default broadcast TCP port
    int audio_system = -1; // default audio system
    char *input_dev = (char *) malloc(MAX_PATH);
    char *output_dev = (char *) malloc(MAX_PATH);
    uint16_t ui_port = UI_DEFAULT_PORT;
    bool waterfall_enabled = true;
    bool ui_enabled = false;
    bool tls_enabled = false;
    int startup_payload_mode = FREEDV_MODE_DATAC3;
    int freedv_verbosity = 0;
    int rx_input_channel = LEFT;
    
    input_dev[0] = 0;
    output_dev[0] = 0;

    int test_mode = 0;

    int radio_type = RADIO_TYPE_NONE;
    char radio_device[1024] = "";
    bool list_radio_models = false;

    // --- Load init configuration file ---
    // Pre-scan argv for -C option; fall back to "mercury.ini" in cwd
    const char *cfg_path = "mercury.ini";
    for (int i = 1; i < argc - 1; i++)
    {
        if (!strcmp(argv[i], "-C"))
        {
            cfg_path = argv[i + 1];
            break;
        }
    }

    mercury_config mcfg;
    cfg_set_defaults(&mcfg);
    if (access(cfg_path, R_OK) == 0)
    {
        if (cfg_read(&mcfg, cfg_path))
        {
            printf("Loaded configuration from %s\n", cfg_path);
            // Apply config-file values as initial defaults
            ui_enabled         = mcfg.ui_enabled;
            ui_port            = mcfg.ui_port;
            tls_enabled        = mcfg.tls_enabled;
            waterfall_enabled  = mcfg.waterfall_enabled;
            radio_type         = mcfg.radio_type;
            if (mcfg.radio_device[0])
                strncpy(radio_device, mcfg.radio_device, sizeof(radio_device) - 1);
            if (mcfg.input_device[0])
                strncpy(input_dev, mcfg.input_device, MAX_PATH - 1);
            if (mcfg.output_device[0])
                strncpy(output_dev, mcfg.output_device, MAX_PATH - 1);
            rx_input_channel   = mcfg.capture_channel;
            audio_system       = mcfg.sound_system;
            base_tcp_port      = mcfg.arq_tcp_base_port;
            broadcast_port     = mcfg.broadcast_tcp_port;
        }
    }

    // CLI arguments override config-file values
    int opt;
    while ((opt = getopt(argc, argv, "hc:s:m:f:k:li:o:x:p:b:zvtrL:JR:U:A:C:SKWGT")) != -1)
    {
        switch (opt)
        {
        case 'U':
            if (optarg)
                ui_port = atoi(optarg);
            break;
        case 'W':
            waterfall_enabled = false;
            break;
        case 'G':
            ui_enabled = true;
            break;
        case 'T':
            tls_enabled = true;
            break;
	case 't':
            test_mode = 1;
            break;
	case 'r':
            test_mode = 2;
            break;
        case 'i':
            if (optarg)
                strncpy(input_dev, optarg, MAX_PATH-1);
            break;
        case 'o':
            if (optarg)
                strncpy(output_dev, optarg, MAX_PATH-1);
            break;
        case 'c':
            if (optarg)
                cpu_nr = atoi(optarg);
            break;
        case 'f':
            if (optarg)
            {
                char *endptr = NULL;
                long verbosity = strtol(optarg, &endptr, 10);
                if (endptr == optarg || *endptr != '\0' || verbosity < 0 || verbosity > 3)
                {
                    fprintf(stderr, "Invalid FreeDV verbosity '%s'. Valid range is 0..3.\n", optarg);
                    return EXIT_FAILURE;
                }
                freedv_verbosity = (int)verbosity;
            }
            break;
        case 'k':
            if (optarg)
            {
                int parsed_layout = parse_rx_channel_layout(optarg);
                if (parsed_layout < 0)
                {
                    fprintf(stderr, "Invalid RX input channel '%s'. Use left, right, or stereo.\n", optarg);
                    return EXIT_FAILURE;
                }
                rx_input_channel = parsed_layout;
            }
            break;
        case 'p':
            if (optarg)
                base_tcp_port = atoi(optarg);
            break;
        case 'b':
            if (optarg)
                broadcast_port = atoi(optarg);
            break;
        case 'x':
            if (!strcmp(optarg, "alsa"))
                audio_system = AUDIO_SUBSYSTEM_ALSA;
            if (!strcmp(optarg, "pulse"))
                audio_system = AUDIO_SUBSYSTEM_PULSE;
            if (!strcmp(optarg, "dsound"))
                audio_system = AUDIO_SUBSYSTEM_DSOUND;
            if (!strcmp(optarg, "wasapi"))
                audio_system = AUDIO_SUBSYSTEM_WASAPI;
            if (!strcmp(optarg, "oss"))
                audio_system = AUDIO_SUBSYSTEM_OSS;
            if (!strcmp(optarg, "coreaudio"))
                audio_system = AUDIO_SUBSYSTEM_COREAUDIO;
            if (!strcmp(optarg, "aaudio"))
                audio_system = AUDIO_SUBSYSTEM_AAUDIO;
            if (!strcmp(optarg, "shm"))
                audio_system = AUDIO_SUBSYSTEM_SHM;
            break;
        case 'z':
            list_sndcards = true;
            break;
        case 's':
        case 'm':
            if (optarg)
            {
                char *endptr = NULL;
                long mode_index = strtol(optarg, &endptr, 10);
                if (endptr == optarg || *endptr != '\0' || mode_index < 0 || mode_index >= mode_count)
                {
                    fprintf(stderr, "Invalid mode index '%s'. Use -l to list valid mode indexes (0..%d).\n",
                            optarg, mode_count - 1);
                    return EXIT_FAILURE;
                }
                startup_payload_mode = freedv_modes[(int)mode_index];
            }
            break;
        case 'l':
            list_modes = true;
            break;
        case 'v':
            printf("Verbose mode enabled.\n");
            verbose = 1;
            break;
        case 'L':
            log_file_path = optarg;
            break;
        case 'J':
            log_file_jsonl = true;
            break;
        case 'R':
            if (optarg)
            {
                char *endptr = NULL;
                long parsed_radio_type = strtol(optarg, &endptr, 10);
                if (endptr == optarg || *endptr != '\0' || parsed_radio_type <= 0)
                {
                    fprintf(stderr, "Invalid radio model '%s'. Expected a positive integer HAMLIB model ID (>0).\n", optarg);
                    return EXIT_FAILURE;
                }
                radio_type = (int)parsed_radio_type;
            }
            break;
        case 'A':
            if (optarg)
                strncpy(radio_device, optarg, sizeof(radio_device) - 1);
            break;
        case 'S':
#ifdef HAVE_HERMES_SHM
            radio_type = RADIO_TYPE_SHM;
#else
            fprintf(stderr, "Error: -S (HERMES shared memory radio control) is only available on Linux builds.\n");
            return EXIT_FAILURE;
#endif
            break;
        case 'C':
            /* already handled in pre-scan above */
            break;
        case 'K':
            list_radio_models = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    

    if (radio_type == RADIO_TYPE_SHM && radio_device[0])
    {
        fprintf(stderr, "Error: -S (HERMES SHM) and -A (HAMLIB device) are mutually exclusive.\n");
        return EXIT_FAILURE;
    }

    if (list_radio_models)
    {
        radio_io_list_models();
        return EXIT_SUCCESS;
    }

    if (list_modes)
    {
        printf("Available modulation modes:\n");
        for (int i = 0; i < mode_count; i++)
        {
            printf("Mode index: %d\n", i);
            printf("Opening mode %s (%d)\n", freedv_mode_names[i], freedv_modes[i]);

            struct freedv *freedv = freedv_open(freedv_modes[i]);

            if (freedv == NULL) {
                printf("Failed to open mode %d\n", freedv_modes[i]);
                return 1;
            }

            if (freedv_verbosity > 0)
                freedv_set_verbose(freedv, freedv_verbosity);
            else if (verbose)
                freedv_set_verbose(freedv, 2);

            size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
            size_t payload_bytes_per_modem_frame = bytes_per_modem_frame - 2; /* 16 bits used for the CRC */

            printf("Modem frame size: %d bits\n", freedv_get_bits_per_modem_frame(freedv));
            printf("payload_bytes_per_modem_frame: %zu\n", payload_bytes_per_modem_frame);
            printf("n_tx_modem_samples: %d\n", freedv_get_n_tx_modem_samples(freedv));
            printf("freedv_get_n_max_modem_samples: %d\n", freedv_get_n_max_modem_samples(freedv));
            printf("modem_sample_rate: %d Hz\n", freedv_get_modem_sample_rate(freedv));
            
            if (freedv_modes[i] != FREEDV_MODE_FSK_LDPC && verbose) {
                freedv_ofdm_print_info(freedv);
            }
            printf("\n");
      
            freedv_close(freedv);

        }

        printf("Available LDPC codes:\n");
        ldpc_codes_list();

        return EXIT_SUCCESS;
    }

    signal(SIGINT, handle_termination_signal);
    signal(SIGTERM, handle_termination_signal);


    if (cpu_nr != -1)
    {
#if defined(__linux__)
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(cpu_nr, &mask);
        sched_setaffinity(0, sizeof(mask), &mask);
        printf("RUNNING ON CPU Nr %d\n", sched_getcpu());
#else
        cpu_nr = -1;
#endif
    }

    // set some defaults... in case the user did not select
    if (audio_system == -1)
    {
        audio_system = audioio_pick_default_subsystem();
    }

    printf("Audio System: ");
    switch(audio_system)
    {
    case AUDIO_SUBSYSTEM_ALSA:
        if(input_dev[0] == 0)
            strcpy(input_dev, "default");
        if(output_dev[0] == 0)
            strcpy(output_dev, "default");
        printf("Advanced Linux Sound Architecture (ALSA)\n");
        break;
    case AUDIO_SUBSYSTEM_PULSE:
        if (input_dev[0] == 0)
        {
            free(input_dev);
            input_dev = NULL;
        }
        if (output_dev[0] == 0)
        {
            free(output_dev);
            output_dev = NULL;
        }
        printf("PulseAudio\n");
        break;
    case AUDIO_SUBSYSTEM_WASAPI:
        if (input_dev[0] == 0)
        {
            free(input_dev);
            input_dev = NULL;
        }
        if (output_dev[0] == 0)
        {
            free(output_dev);
            output_dev = NULL;
        }
        printf("Windows Audio Session API (WASAPI)\n");
        break;
    case AUDIO_SUBSYSTEM_DSOUND:
        if (input_dev[0] == 0)
        {
            free(input_dev);
            input_dev = NULL;
        }
        if (output_dev[0] == 0)
        {
            free(output_dev);
            output_dev = NULL;
        }
        printf("Microsoft DirectSound (DSOUND)\n");
        break;
    case AUDIO_SUBSYSTEM_OSS:
        if (input_dev[0] == 0)
        {
            sprintf(input_dev, "/dev/dsp");
        }
        if (output_dev[0] == 0)
        {
            sprintf(output_dev, "/dev/dsp");
        }
        printf("Open Sound System (OSS)\n");
        break;
    case AUDIO_SUBSYSTEM_COREAUDIO:
        printf("CoreAudio (UNSUPPORTED)\n");
        break;
    case AUDIO_SUBSYSTEM_AAUDIO:
        printf("Android AAudio (UNSUPPORTED)\n");
        break;
    case AUDIO_SUBSYSTEM_SHM:
        printf("Shared Memory (SHM)\n");
        break;
    default:
        printf("Selected audio system not supported. Trying to continue.\n");
    }
    
    if (list_sndcards)
    {
        list_soundcards(audio_system);
        if (input_dev)
            free(input_dev);
        if (output_dev)
            free(output_dev);
        return EXIT_SUCCESS;
    }    

    if (hermes_log_init(1024) == 0)
    {
        hermes_log_set_level(verbose ? HERMES_LOG_LEVEL_DEBUG : HERMES_LOG_LEVEL_INFO);
        if (log_file_path)
            hermes_log_set_file(log_file_path,
                                verbose ? HERMES_LOG_LEVEL_DEBUG : HERMES_LOG_LEVEL_TIMING,
                                log_file_jsonl);
        HLOGI("main", "Async logger initialized (min_level=%s)", verbose ? "DEBUG" : "INFO");
    }
    else
    {
        fprintf(stderr, "Warning: async logger unavailable\n");
    }

    generic_modem_t g_modem;
    pthread_t radio_capture, radio_playback;
    
    if (audio_system != AUDIO_SUBSYSTEM_SHM)
    {
        HLOGI("main", "Initializing I/O from Sound Card");
        audioio_init_internal(input_dev, output_dev, audio_system, rx_input_channel, &radio_capture, &radio_playback);
    }

    if (radio_type != RADIO_TYPE_NONE)
    {
        if (radio_io_init(radio_type, radio_device) != 0)
        {
            fprintf(stderr, "Failed to initialize radio control.\n");
            hermes_log_shutdown();
            return EXIT_FAILURE;
        }
    }

    HLOGI("main", "Initializing Modem");
    init_modem(&g_modem, startup_payload_mode, 1, test_mode, freedv_verbosity); // frames per burst is 1 for now
    
    if (arq_init(g_modem.payload_bytes_per_modem_frame, g_modem.mode) != EXIT_SUCCESS)
    {
        fprintf(stderr, "Failed to initialize ARQ subsystem.\n");
        shutdown_ = true;
        if (audio_system != AUDIO_SUBSYSTEM_SHM)
            audioio_deinit(&radio_capture, &radio_playback);
        shutdown_modem(&g_modem);
        hermes_log_shutdown();
        return EXIT_FAILURE;
    }

    broadcast_run(&g_modem);

    HLOGI("main", "Initializing TCP interfaces with base port %d and broadcast port %d", base_tcp_port, broadcast_port);
    if (interfaces_init(base_tcp_port, broadcast_port, g_modem.payload_bytes_per_modem_frame) != EXIT_SUCCESS)
    {
        fprintf(stderr, "Failed to initialize TCP interfaces.\n");
        shutdown_ = true;
        interfaces_shutdown();
        if (audio_system != AUDIO_SUBSYSTEM_SHM)
            audioio_deinit(&radio_capture, &radio_playback);
        shutdown_modem(&g_modem);
        HLOGI("main", "Shutting down");
        hermes_log_shutdown();
        return EXIT_FAILURE;
    }

    // ---- Initialize UI communication (WebSocket to mercury-qt) ----
    ui_ctx_t ui_ctx;
    if (ui_enabled)
    {
        HLOGI("main", "Initializing UI communication (WebSocket port %u | TLS %s | Waterfall %s)",
               ui_port, tls_enabled ? "WSS" : "WS", waterfall_enabled ? "enabled" : "disabled");
        if (ui_comm_init(&ui_ctx, (uint16_t)ui_port, tls_enabled,
                         waterfall_enabled ? 1 : 0,
                         audio_system, input_dev, output_dev, rx_input_channel) != 0)
        {
            // Non-fatal: mercury can run without UI
            HLOGW("main", "UI communication init failed. Running without GUI.");
        }
    }
    else
    {
        memset(&ui_ctx, 0, sizeof(ui_ctx));
        HLOGI("main", "UI communication disabled (use -G to enable).");
    }

    while (!shutdown_)
        msleep(500);

    if (audio_system != AUDIO_SUBSYSTEM_SHM)
    {
        audioio_deinit(&radio_capture, &radio_playback);
    }
    
    if (ui_enabled)
        ui_comm_shutdown(&ui_ctx);

    if (radio_type != RADIO_TYPE_NONE)
    {
        radio_io_shutdown();
    }

    shutdown_modem(&g_modem);
    HLOGI("main", "Shutting down");
    hermes_log_shutdown();

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;

}
