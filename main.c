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
#include "arq_trace.h"
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
#include "mercury_version.h"

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

static volatile sig_atomic_t g_signal_count = 0;

#ifndef _WIN32
/* A forced exit must never leave the transmitter keyed.
 *
 * The first signal starts an orderly shutdown, which finishes the frame in
 * flight and unkeys.  A second one used to _exit() on the spot -- mid-frame,
 * PTT still asserted.  On a hermes_shm station the radio daemon then held the
 * key: an IC-7100 stayed in transmit for 4 h 40 min after `sudo timeout -s INT
 * ... mercury -t` delivered its one SIGINT twice (timeout signals the whole
 * process group and sudo relays it too).
 *
 * So: a second signal within DUPLICATE_WINDOW_MS of the first is the same
 * request delivered twice, and is ignored.  A later one -- someone insisting --
 * still forces the exit, but through a thread that drops PTT first: unkeying
 * takes locks (radio_cmd's shared mutexes, hamlib), which a signal handler must
 * not touch, while sem_post() is async-signal-safe.
 *
 * alarm() is the backstop if even the unkey hangs, and it BOUNDS the hang: it
 * does not guarantee the unkey.  SIGALRM's default action ends the process as
 * it stands, and no handler could do better -- the unkey needs the very locks
 * that may be what is stuck.  The grace is therefore long enough for a slow
 * CAT unkey with hamlib retries, the same bound main() gives an orderly
 * shutdown. */
#include <semaphore.h>
#include <pthread.h>
#include <time.h>

#define DUPLICATE_WINDOW_MS 1000
#define FORCED_EXIT_GRACE_S 10

static sem_t                 g_force_exit_sem;
static struct timespec       g_first_signal_ts;
static volatile sig_atomic_t g_force_exit_ready = 0;  /* the unkey thread is up */
static volatile sig_atomic_t g_forcing          = 0;  /* a forced exit is under way */

static void *forced_exit_thread(void *arg)
{
    (void)arg;
    while (sem_wait(&g_force_exit_sem) != 0)
        ;                                   /* EINTR */
    if (radio_io_enabled())
        radio_io_key_off();
    static const char msg[] = "Transmitter unkeyed; exiting.\n";
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
    return NULL;
}

static void start_forced_exit_thread(void)
{
    pthread_t th;
    if (g_force_exit_ready)
        return;
    if (sem_init(&g_force_exit_sem, 0, 0) != 0)
        return;
    if (pthread_create(&th, NULL, forced_exit_thread, NULL) != 0)
    {
        sem_destroy(&g_force_exit_sem);
        return;
    }
    pthread_detach(th);
    g_force_exit_ready = 1;
}
#endif

static void handle_termination_signal(int sig)
{
    (void)sig;
    if (g_signal_count)
    {
#ifndef _WIN32
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (long)(now.tv_sec - g_first_signal_ts.tv_sec) * 1000L +
                  (now.tv_nsec - g_first_signal_ts.tv_nsec) / 1000000L;
        if (ms < DUPLICATE_WINDOW_MS)
            return;                         /* the same request, delivered twice */
        if (g_forcing)
            return;                         /* already on its way out */
        g_forcing = 1;
        if (!g_force_exit_ready)
        {
            /* No unkey thread (it could not be created): the old behaviour,
             * rather than sem_post() on a semaphore that may not exist. */
            static const char msg[] = "Caught second signal, forcing exit.\n";
            (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
            _exit(1);
        }
        static const char msg[] = "Caught second signal, unkeying and forcing exit.\n";
        (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
        alarm(FORCED_EXIT_GRACE_S);
        sem_post(&g_force_exit_sem);
        return;
#else
        /* Windows still exits here with PTT as it stands. */
        static const char msg[] = "Caught second signal, forcing exit.\n";
        (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(1);
#endif
    }
#ifndef _WIN32
    /* Timestamp first: a duplicate that lands right after the count flips must
     * find it.  (The handlers also block each other; see below.) */
    clock_gettime(CLOCK_MONOTONIC, &g_first_signal_ts);
#endif
    g_signal_count = 1;
    static const char msg[] = "Signal received, shutting down...\n";
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    shutdown_ = true;
}

/* SIGINT/SIGTERM -> handle_termination_signal.  On POSIX via sigaction, with
 * each signal blocking the other while the handler runs: they are distinct
 * signals, so without the mask a SIGTERM could interrupt the SIGINT handler
 * half-way and read its state half-written.  Also brings up the unkey thread,
 * only on the paths that can transmit (not -h/-V/-l). */
static void install_termination_handlers(void)
{
#ifndef _WIN32
    start_forced_exit_thread();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_termination_signal;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGINT);
    sigaddset(&sa.sa_mask, SIGTERM);
    /* What signal() gave us on glibc.  On BSD/macOS signal() did not restart
     * interrupted syscalls, so this makes them restart there too (at most one
     * more msleep() before the shutdown loop sees the flag). */
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#else
    signal(SIGINT, handle_termination_signal);
    signal(SIGTERM, handle_termination_signal);
#endif
}

int main(int argc, char *argv[])
{
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

    if (cli.action == MERCURY_CLI_TEST_PTT)
    {
        install_termination_handlers();
        return mercury_cli_run_ptt_test(&cli) == 0
                   ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /* -h/-l/-z/-K/-V print and exit. */
    if (mercury_cli_run_info_action(&cli, argv[0]))
        return EXIT_SUCCESS;

    /* Announce the version only when the modem is actually starting, not for
     * the informational actions above (so -V etc. print a single line). */
    mercury_print_version_banner();

    install_termination_handlers();

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

    /* Diagnosis build only: the answerer never disconnects, so its trace would
     * otherwise be lost.  No-op unless ARQ_TRACE_ENABLED. */
    ARQ_TRACE_DUMP("shutdown");

#ifndef _WIN32
    alarm(10);
#endif

    mercury_engine_shutdown();

    return 0;
}
