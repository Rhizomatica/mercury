/* Mercury CLI argument parser — see mercury_cli.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Extracted verbatim from main()'s two-pass getopt so the standalone daemon and
 * the embedded UI apply the exact same options and config-override logic.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "mercury_cli.h"
#include "freedv_api.h"
#include "ldpc_codes.h"
#include "audioio/audioio.h"
#include "radio_io.h"

/* Startup-mode table (index -> FreeDV mode).  Non-static: shared with the
 * modem, and referenced by main()'s -l listing. */
int freedv_modes[] = { FREEDV_MODE_DATAC1,
                       FREEDV_MODE_DATAC3,
                       FREEDV_MODE_DATAC0,
                       FREEDV_MODE_DATAC4,
                       FREEDV_MODE_DATAC13,
                       FREEDV_MODE_DATAC14,
                       FREEDV_MODE_FSK_LDPC,
                       FREEDV_MODE_DATAC15,
                       FREEDV_MODE_DATAC16,
                       FREEDV_MODE_DATAC17,
                       FREEDV_MODE_QAM16C2 };

char *freedv_mode_names[] = { "DATAC1",
                              "DATAC3",
                              "DATAC0",
                              "DATAC4",
                              "DATAC13",
                              "DATAC14",
                              "FSK_LDPC",
                              "DATAC15",
                              "DATAC16",
                              "DATAC17",
                              "QAM16C2" };

int mercury_cli_mode_count(void)
{
    return (int)(sizeof(freedv_modes) / sizeof(freedv_modes[0]));
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

void mercury_cli_print_usage(const char *prog)
{
    printf("Usage modes: \n");
    printf("%s -m [mode_index] -i [device] -o [device] -x [sound_system] -p [arq_tcp_base_port] -b [broadcast_tcp_port] -f [freedv_verbosity] -H [hamlib_log_level] -k [rx_input_channel] [-P ptt_method] [-A ptt_device] [-G] [-T] [-U ui_port] [-W]\n", prog);
    printf("%s [-h -l -z]\n", prog);
    printf("\nOptions:\n");
    printf(" -c [cpu_nr]                Run on CPU [cpu_nr]. Use -1 to disable CPU selection, which is the default.\n");
    printf(" -m [mode_index]            Startup payload mode index shown in \"-l\" output. Used for broadcast and idle/disconnected ARQ decode. Default is 1 (DATAC3)\n");
    printf(" -s [mode_index]            Legacy alias for -m.\n");
    printf(" -f [freedv_verbosity]      FreeDV modem verbosity level (0..3). Default is 0.\n");
    printf(" -H [hamlib_log_level]      Hamlib radio log level (0..6). Default is 0.\n");
    printf(" -k [rx_input_channel]      Capture input channel: left, right, or stereo. Default is left.\n");
    printf(" -i [device]                Radio Capture device id (eg: \"plughw:0,0\").\n");
    printf(" -o [device]                Radio Playback device id (eg: \"plughw:0,0\").\n");
    printf(" -x [sound_system]          Sets the sound system or IO API to use: alsa, pulse, oss, coreaudio, aaudio, dsound, wasapi, shm, null, fifo or sock. Default is alsa on Linux, dsound on Windows.\n");
    printf("                            null, fifo and sock are developer/test backends; fifo uses raw s32le PCM at 8 kHz via -i/-o paths; sock is a framed, virtual-clock lockstep transport (MERCURY_AUDIO_SOCK=<path>).\n");
    printf(" -p [arq_tcp_base_port]     Sets the ARQ TCP base port (control is base_port, data is base_port + 1). Default is 8300.\n");
    printf(" -b [broadcast_tcp_port]    Sets the broadcast TCP port. Default is 8100.\n");
    printf(" -U [ui_port]               Sets the UI port (WebSocket port). Default is 10000. Requires -G.\n");
    printf(" -W                         Disable waterfall/spectrum data sent to the UI (used to spare CPU). Requires -G.\n");
    printf(" -G                         Enable UI communication (WebSocket server for mercury-qt). Off by default.\n");
    printf(" -T                         Use WSS (WebSocket Secure/TLS) for UI communication. Requires -G. Default uses plain WS (no TLS).\n");
    printf(" -l                         Lists all modulator/coding modes.\n");
    printf(" -z                         Lists all available sound cards.\n");
    printf(" -v                         Verbose mode. Prints more information during execution.\n");
    printf(" -L <path>                  Write log to file (TIMING level and above).\n");
    printf(" -J                         Use JSONL format for log file (requires -L).\n");
    printf(" -P [ptt_method]            PTT method: none, hamlib, serial, cm108, or hermes_shm.\n");
    printf(" -R [radio_model]           Sets HAMLIB radio model and selects Hamlib PTT.\n");
    printf(" -A [ptt_device]            PTT serial device or HAMLIB device/ip:port endpoint.\n");
#ifdef HAVE_HERMES_SHM
    printf(" -S                         Select HERMES shared-memory PTT (Linux shorthand for -P hermes_shm).\n");
#else
    printf(" -S                         HERMES shared-memory PTT (Linux-only; unavailable in this build).\n");
#endif
    printf(" -C [config_file]           Path to init configuration file (INI format). Default is mercury.ini in the current directory.\n");
    printf(" -K                         List HAMLIB supported radio models.\n");
    printf(" -t                         Test TX mode.\n");
    printf(" -r                         Test RX mode.\n");
    printf(" -h                         Prints this help.\n");
}

int mercury_cli_parse(int argc, char **argv,
                      const char *default_cfg_path, mercury_cli_t *out)
{
    if (!out)
        return -1;

    const int mode_count = mercury_cli_mode_count();
    const char *optstring = "hc:s:m:f:H:k:li:o:x:p:b:zvtrL:JP:R:U:A:C:SKWGT";

    memset(out, 0, sizeof(*out));
    cfg_set_defaults(&out->cfg);
    out->startup_mode   = FREEDV_MODE_DATAC3;
    out->test_mode      = 0;
    out->cpu_nr         = -1;
    out->log_file_path  = NULL;
    out->log_file_jsonl = false;
    out->action         = MERCURY_CLI_RUN;
    snprintf(out->cfg_path, sizeof(out->cfg_path), "%s",
             (default_cfg_path && default_cfg_path[0]) ? default_cfg_path : "mercury.ini");

    /* First pass: extract -C config path only. */
    int opt;
    optind = 1;
    while ((opt = getopt(argc, argv, optstring)) != -1)
    {
        if (opt == 'C' && optarg)
            snprintf(out->cfg_path, sizeof(out->cfg_path), "%s", optarg);
    }

    /* Load config file — fields not overridden by CLI keep these values. */
    if (access(out->cfg_path, R_OK) == 0)
    {
        if (cfg_read(&out->cfg, out->cfg_path))
            printf("Loaded configuration from %s\n", out->cfg_path);
    }

    /* Device names are meaningful only to the sound system they came from, so
     * remember what the config asked for before the CLI gets a chance to
     * change it. */
    int cfg_sound_system = out->cfg.sound_system;
    bool cli_set_sound_system = false, cli_set_input = false, cli_set_output = false;

    /* Second pass: CLI arguments override config-file values. */
    optind = 1;
    while ((opt = getopt(argc, argv, optstring)) != -1)
    {
        switch (opt)
        {
        case 'U':
            if (optarg)
            {
                char *endptr = NULL;
                long value = strtol(optarg, &endptr, 10);
                if (endptr == optarg || *endptr != '\0' || value <= 0 || value > 65535)
                {
                    fprintf(stderr, "Invalid UI port '%s'. Valid range is 1..65535.\n", optarg);
                    return -1;
                }
                out->cfg.ui_port = (uint16_t)value;
            }
            break;
        case 'W':
            out->cfg.waterfall_enabled = false;
            break;
        case 'G':
            out->cfg.ui_enabled = true;
            break;
        case 'T':
            out->cfg.tls_enabled = true;
            break;
        case 't':
            out->test_mode = 1;
            break;
        case 'r':
            out->test_mode = 2;
            break;
        case 'i':
            if (optarg)
            {
                snprintf(out->cfg.input_device, sizeof(out->cfg.input_device), "%s", optarg);
                cli_set_input = true;
            }
            break;
        case 'o':
            if (optarg)
            {
                snprintf(out->cfg.output_device, sizeof(out->cfg.output_device), "%s", optarg);
                cli_set_output = true;
            }
            break;
        case 'c':
            if (optarg)
                out->cpu_nr = atoi(optarg);
            break;
        case 'f':
            if (optarg)
            {
                char *endptr = NULL;
                long verbosity = strtol(optarg, &endptr, 10);
                if (endptr == optarg || *endptr != '\0' || verbosity < 0 || verbosity > 3)
                {
                    fprintf(stderr, "Invalid FreeDV verbosity '%s'. Valid range is 0..3.\n", optarg);
                    return -1;
                }
                out->cfg.freedv_verbosity = (int)verbosity;
            }
            break;
        case 'H':
            if (optarg)
            {
                char *endptr = NULL;
                long log_level = strtol(optarg, &endptr, 10);
                if (endptr == optarg || *endptr != '\0' || log_level < 0 || log_level > 6)
                {
                    fprintf(stderr, "Invalid Hamlib log level '%s'. Valid range is 0..6.\n", optarg);
                    return -1;
                }
                out->cfg.ptt.hamlib_log_level = (int)log_level;
            }
            break;
        case 'k':
            if (optarg)
            {
                int parsed_layout = parse_rx_channel_layout(optarg);
                if (parsed_layout < 0)
                {
                    fprintf(stderr, "Invalid RX input channel '%s'. Use left, right, or stereo.\n", optarg);
                    return -1;
                }
                out->cfg.capture_channel = parsed_layout;
            }
            break;
        case 'p':
            if (optarg)
            {
                char *endptr = NULL;
                long value = strtol(optarg, &endptr, 10);
                if (endptr == optarg || *endptr != '\0' || value <= 0 || value > 65535)
                {
                    fprintf(stderr, "Invalid ARQ TCP base port '%s'. Valid range is 1..65535.\n", optarg);
                    return -1;
                }
                out->cfg.arq_tcp_base_port = (int)value;
            }
            break;
        case 'b':
            if (optarg)
            {
                char *endptr = NULL;
                long value = strtol(optarg, &endptr, 10);
                if (endptr == optarg || *endptr != '\0' || value <= 0 || value > 65535)
                {
                    fprintf(stderr, "Invalid broadcast TCP port '%s'. Valid range is 1..65535.\n", optarg);
                    return -1;
                }
                out->cfg.broadcast_tcp_port = (int)value;
            }
            break;
        case 'x':
            cli_set_sound_system = true;
            if (!strcmp(optarg, "alsa"))
                out->cfg.sound_system = AUDIO_SUBSYSTEM_ALSA;
            if (!strcmp(optarg, "pulse"))
                out->cfg.sound_system = AUDIO_SUBSYSTEM_PULSE;
            if (!strcmp(optarg, "dsound"))
                out->cfg.sound_system = AUDIO_SUBSYSTEM_DSOUND;
            if (!strcmp(optarg, "wasapi"))
                out->cfg.sound_system = AUDIO_SUBSYSTEM_WASAPI;
            if (!strcmp(optarg, "oss"))
                out->cfg.sound_system = AUDIO_SUBSYSTEM_OSS;
            if (!strcmp(optarg, "coreaudio"))
                out->cfg.sound_system = AUDIO_SUBSYSTEM_COREAUDIO;
            if (!strcmp(optarg, "aaudio"))
                out->cfg.sound_system = AUDIO_SUBSYSTEM_AAUDIO;
            if (!strcmp(optarg, "shm"))
                out->cfg.sound_system = AUDIO_SUBSYSTEM_SHM;
            if (!strcmp(optarg, "null"))
                out->cfg.sound_system = AUDIO_SUBSYSTEM_NULL;
            if (!strcmp(optarg, "fifo"))
                out->cfg.sound_system = AUDIO_SUBSYSTEM_FIFO;
            if (!strcmp(optarg, "sock"))
                out->cfg.sound_system = AUDIO_SUBSYSTEM_SOCK;
            break;
        case 'z':
            out->action = MERCURY_CLI_LIST_SNDCARDS;
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
                    return -1;
                }
                out->startup_mode = freedv_modes[(int)mode_index];
            }
            break;
        case 'l':
            out->action = MERCURY_CLI_LIST_MODES;
            break;
        case 'v':
            out->cfg.verbose = true;
            break;
        case 'L':
            out->log_file_path = optarg;
            break;
        case 'J':
            out->log_file_jsonl = true;
            break;
        case 'R':
            if (optarg)
            {
                char *endptr = NULL;
                long parsed_radio_type = strtol(optarg, &endptr, 10);
                if (endptr == optarg || *endptr != '\0' || parsed_radio_type <= 0)
                {
                    fprintf(stderr, "Invalid radio model '%s'. Expected a positive integer HAMLIB model ID (>0).\n", optarg);
                    return -1;
                }
                out->cfg.ptt.method = PTT_METHOD_HAMLIB;
                out->cfg.ptt.hamlib_model = (int)parsed_radio_type;
            }
            break;
        case 'P':
            if (optarg)
            {
                ptt_method_t method;
                if (!cfg_ptt_method_parse(optarg, &method))
                {
                    fprintf(stderr, "Invalid PTT method '%s'. Use none, hamlib, serial, cm108, or hermes_shm.\n",
                            optarg);
                    return -1;
                }
#ifndef HAVE_HERMES_SHM
                if (method == PTT_METHOD_HERMES_SHM)
                {
                    fprintf(stderr, "Error: HERMES shared-memory PTT is unavailable in this build.\n");
                    return -1;
                }
#endif
                out->cfg.ptt.method = method;
            }
            break;
        case 'A':
            if (optarg)
                snprintf(out->cfg.ptt.device, sizeof(out->cfg.ptt.device), "%s", optarg);
            break;
        case 'S':
#ifdef HAVE_HERMES_SHM
            out->cfg.ptt.method = PTT_METHOD_HERMES_SHM;
#else
            fprintf(stderr, "Error: -S (HERMES shared memory radio control) is only available on Linux builds.\n");
            return -1;
#endif
            break;
        case 'C':
            /* handled in first pass */
            break;
        case 'K':
            out->action = MERCURY_CLI_LIST_RADIOS;
            break;
        case 'h':
            out->action = MERCURY_CLI_HELP;
            break;
        default:
            mercury_cli_print_usage(argv[0]);
            return -1;
        }
    }

    /* -x selected a different sound system than the config file's, and the
     * matching device was not given on the command line.  A PulseAudio sink
     * name means nothing to OSS, so keeping it guarantees a failed open;
     * fall back to that sound system's default device instead. */
    if (cli_set_sound_system && out->cfg.sound_system != cfg_sound_system)
    {
        if (!cli_set_input && out->cfg.input_device[0])
        {
            printf("Ignoring input_device '%s' from %s: it belongs to sound system '%s', "
                   "not '%s'; using the default device.\n",
                   out->cfg.input_device, out->cfg_path,
                   cfg_sound_system_name(cfg_sound_system),
                   cfg_sound_system_name(out->cfg.sound_system));
            out->cfg.input_device[0] = '\0';
        }
        if (!cli_set_output && out->cfg.output_device[0])
        {
            printf("Ignoring output_device '%s' from %s: it belongs to sound system '%s', "
                   "not '%s'; using the default device.\n",
                   out->cfg.output_device, out->cfg_path,
                   cfg_sound_system_name(cfg_sound_system),
                   cfg_sound_system_name(out->cfg.sound_system));
            out->cfg.output_device[0] = '\0';
        }
    }

    if (out->cfg.ptt.method == PTT_METHOD_HAMLIB && out->cfg.ptt.hamlib_model <= 0)
    {
        fprintf(stderr, "Error: Hamlib PTT requires a positive radio model (-R).\n");
        return -1;
    }

    if (out->cfg.ptt.method == PTT_METHOD_SERIAL && !out->cfg.ptt.device[0])
    {
        fprintf(stderr, "Error: serial_rts PTT requires a device path (-A).\n");
        return -1;
    }

    return 0;
}

/* -l : open each modulation mode and print its parameters. */
static void list_modulation_modes(int freedv_verbosity, bool verbose)
{
    const int mode_count = mercury_cli_mode_count();
    printf("Available modulation modes:\n");
    for (int i = 0; i < mode_count; i++)
    {
        printf("Mode index: %d\n", i);
        printf("Opening mode %s (%d)\n", freedv_mode_names[i], freedv_modes[i]);

        struct freedv *freedv = freedv_open(freedv_modes[i]);
        if (freedv == NULL)
        {
            printf("Failed to open mode %d\n", freedv_modes[i]);
            continue;
        }

        if (freedv_verbosity > 0)
            freedv_set_verbose(freedv, freedv_verbosity);
        else if (verbose)
            freedv_set_verbose(freedv, 2);

        size_t bytes_per_modem_frame = freedv_get_bits_per_modem_frame(freedv) / 8;
        size_t payload_bytes_per_modem_frame = bytes_per_modem_frame - 2; /* 16 bits for CRC */

        printf("Modem frame size: %d bits\n", freedv_get_bits_per_modem_frame(freedv));
        printf("payload_bytes_per_modem_frame: %zu\n", payload_bytes_per_modem_frame);
        printf("n_tx_modem_samples: %d\n", freedv_get_n_tx_modem_samples(freedv));
        printf("freedv_get_n_max_modem_samples: %d\n", freedv_get_n_max_modem_samples(freedv));
        printf("modem_sample_rate: %d Hz\n", freedv_get_modem_sample_rate(freedv));

        if (freedv_modes[i] != FREEDV_MODE_FSK_LDPC && verbose)
            freedv_ofdm_print_info(freedv);
        printf("\n");

        freedv_close(freedv);
    }

    printf("Available LDPC codes:\n");
    ldpc_codes_list();
}

bool mercury_cli_run_info_action(const mercury_cli_t *cli, const char *prog)
{
    if (!cli)
        return false;
    switch (cli->action)
    {
    case MERCURY_CLI_HELP:
        mercury_cli_print_usage(prog);
        return true;
    case MERCURY_CLI_LIST_RADIOS:
        radio_io_list_models();
        return true;
    case MERCURY_CLI_LIST_MODES:
        list_modulation_modes(cli->cfg.freedv_verbosity, cli->cfg.verbose);
        return true;
    case MERCURY_CLI_LIST_SNDCARDS:
    {
        int audio_system = cli->cfg.sound_system;
        if (audio_system == -1)
            audio_system = audioio_pick_default_subsystem();
        list_soundcards(audio_system);
        return true;
    }
    case MERCURY_CLI_RUN:
    default:
        return false;
    }
}
