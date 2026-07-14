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

#define VERSION__ "1.9.9"
#ifndef GIT_HASH
#define GIT_HASH "unknown000"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <math.h>

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
#include "mercury_engine.h"
#include "mercury_cli.h"

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

static volatile sig_atomic_t g_signal_count = 0;

static void handle_termination_signal(int sig)
{
    (void)sig;
    if (g_signal_count)
    {
        static const char msg[] = "Caught second signal, forcing exit.\n";
        (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(1);
    }
    g_signal_count = 1;
    static const char msg[] = "Signal received, shutting down...\n";
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    shutdown_ = true;
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

    /* Parse CLI + config into one struct (shared with the UI bridge). */
    mercury_cli_t cli;
    if (mercury_cli_parse(argc, argv, "mercury.ini", &cli) != 0)
        return EXIT_FAILURE;

    /* -h/-l/-z/-K print and exit. */
    if (mercury_cli_run_info_action(&cli, argv[0]))
        return EXIT_SUCCESS;

    signal(SIGINT, handle_termination_signal);
    signal(SIGTERM, handle_termination_signal);

    if (cli.cpu_nr != -1)
    {
#if defined(__linux__)
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(cli.cpu_nr, &mask);
        sched_setaffinity(0, sizeof(mask), &mask);
        printf("RUNNING ON CPU Nr %d\n", sched_getcpu());
#endif
    }

    /* Bring up the logger before engine init so the banner/printf context and
     * the engine's own startup logs share one destination. */
    if (hermes_log_init(1024) == 0)
    {
        hermes_log_set_level(cli.cfg.verbose ? HERMES_LOG_LEVEL_DEBUG : HERMES_LOG_LEVEL_INFO);
        if (cli.log_file_path)
            hermes_log_set_file(cli.log_file_path,
                                cli.cfg.verbose ? HERMES_LOG_LEVEL_DEBUG : HERMES_LOG_LEVEL_TIMING,
                                cli.log_file_jsonl);
        HLOGI("main", "Async logger initialized (min_level=%s)", cli.cfg.verbose ? "DEBUG" : "INFO");
    }
    else
    {
        fprintf(stderr, "Warning: async logger unavailable\n");
    }

    if (mercury_engine_init(&cli.cfg, cli.cfg_path, cli.log_file_path, cli.log_file_jsonl,
                            cli.startup_mode, cli.test_mode) != 0)
    {
        fprintf(stderr, "Mercury engine init failed.\n");
        return EXIT_FAILURE;
    }

    while (!shutdown_)
        msleep(500);

#ifndef _WIN32
    alarm(10);
#endif

    mercury_engine_shutdown();

    return 0;
}
