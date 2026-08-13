/* Mercury C Engine Bridge — thin CGo wrapper
 *
 * Delegates all init / teardown to mercury_engine_init() /
 * mercury_engine_shutdown() in common/mercury_engine.c so there
 * is exactly one copy of the init sequence.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

#include "cfg_utils.h"
#include "mercury_engine.h"
#include "mercury_cli.h"
#include "mercury_version.h"
#include "ui_communication.h"
#include "modem.h"
#include "modem_stats.h"   /* MODEM_STATS_NSPEC */

/* Print the startup version banner (same text the daemon prints), so the UI
 * announces its version on the terminal too.  Resolved here, in a unit the
 * libmercury_core.a rule recompiles every build, so the git hash stays fresh. */
void mercury_print_version(void)
{
    mercury_print_version_banner();
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/*  mercury_init(argc, argv, default_config, log_path)                 */
/*                                                                     */
/*  Runs the SAME CLI/config parser the daemon uses (Go forwards its   */
/*  os.Args here), then delegates to mercury_engine_init().            */
/*    default_config — mercury.ini path used when -C is absent (the    */
/*                     UI's per-user writable copy).                    */
/*    log_path       — engine log file (the UI's writable path); an    */
/*                     explicit -L overrides it.                        */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/*  mercury_precheck(argc, argv, default_config)                       */
/*                                                                     */
/*  Handle the informational CLI actions (-h/-l/-z/-K) exactly as the  */
/*  daemon does — print to the terminal.  Called from Go BEFORE the    */
/*  window is created so `mercury-ui -h` shows help instead of opening */
/*  the GUI.  Returns non-zero if handled (Go should exit), 0 to run.  */
/* ------------------------------------------------------------------ */
int mercury_precheck(int argc, char **argv, const char *default_config)
{
    mercury_cli_t cli;
    if (mercury_cli_parse(argc, argv,
                          (default_config && default_config[0]) ? default_config : "mercury.ini",
                          &cli) != 0) {
        /* Go exits without running C's atexit flush, so a piped stdout/stderr
         * would otherwise lose the buffered getopt/usage text — flush now. */
        fflush(stdout);
        fflush(stderr);
        return 1;   /* parse error already reported → exit */
    }
    int handled = mercury_cli_run_info_action(&cli,
                      (argc > 0 && argv) ? argv[0] : "mercury-ui") ? 1 : 0;
    fflush(stdout);
    fflush(stderr);
    return handled;
}

int mercury_init(int argc, char **argv, const char *default_config, const char *log_path)
{
    mercury_cli_t cli;
    if (mercury_cli_parse(argc, argv,
                          (default_config && default_config[0]) ? default_config : "mercury.ini",
                          &cli) != 0)
        return -1;

    /* The single-binary UI always runs the engine (the list/help actions are
     * daemon-only) and always enables the UI websocket. */
    cli.cfg.ui_enabled = true;

    const char *log = cli.log_file_path ? cli.log_file_path : log_path;
    return mercury_engine_init(&cli.cfg, cli.cfg_path, log, cli.log_file_jsonl,
                               cli.startup_mode, cli.test_mode);
}

/* ------------------------------------------------------------------ */
/*  mercury_request_shutdown()                                         */
/*                                                                     */
/*  Sets the global shutdown flag so background threads begin their    */
/*  exit sequences.  Call BEFORE mercury_shutdown().                    */
/* ------------------------------------------------------------------ */
void mercury_request_shutdown(void)
{
    shutdown_ = true;
}

/* ------------------------------------------------------------------ */
/*  mercury_shutdown()                                                 */
/*                                                                     */
/*  Delegates to mercury_engine_shutdown().                            */
/* ------------------------------------------------------------------ */
void mercury_shutdown(void)
{
    mercury_engine_shutdown();
}

/* ------------------------------------------------------------------ */
/*  mercury_ui_preload_device_lists()                                  */
/*                                                                     */
/*  Trigger radio-enumeration during engine init, on the main thread,   */
/*  so the Go goroutine that later reads the list never enters hamlib's */
/*  backend-loading path inside a CGo context.                          */
/* ------------------------------------------------------------------ */
void mercury_ui_preload_device_lists(void)
{
    ui_comm_preload_radio_list();
}

/* ------------------------------------------------------------------ */
/*  In-process UI link                                                 */
/*                                                                     */
/*  Thin forwarders: the engine owns the state and the command          */
/*  handling, so local and remote clients cannot drift apart.           */
/* ------------------------------------------------------------------ */
int mercury_ui_get_status(ui_status_t *out)
{
    return ui_comm_get_status(out) ? 1 : 0;
}

int mercury_ui_get_spectrum(float *out, int max_bins, int *sample_rate_hz,
                            unsigned long long *seq)
{
    if (!out || max_bins <= 0)
        return 0;

    int nbins = (max_bins < MODEM_STATS_NSPEC) ? max_bins : MODEM_STATS_NSPEC;
    uint64_t s = 0;
    int sr = modem_get_rx_spectrum_seq(out, nbins, &s);
    if (sr <= 0)
        return 0;
    if (sample_rate_hz)
        *sample_rate_hz = sr;
    if (seq)
        *seq = (unsigned long long)s;
    return nbins;
}

int mercury_ui_command(const char *command, const char *value,
                       const char *value2, const char *value3)
{
    return ui_comm_command(command, value, value2, value3);
}

int mercury_ui_get_audio_devices(int kind, ui_device_t *out, int max,
                                 char *selected, int selected_len)
{
    return ui_comm_get_audio_devices((ui_device_kind_t)kind, out, max,
                                     selected, (size_t)selected_len);
}

int mercury_ui_get_radio_list(ui_device_t *out, int max,
                              char *selected, int selected_len,
                              char *device_path, int device_path_len,
                              int *serial_speed)
{
    return ui_comm_get_radio_list(out, max, selected, (size_t)selected_len,
                                  device_path, (size_t)device_path_len,
                                  serial_speed);
}

int mercury_ui_get_input_channel(void)
{
    return ui_comm_get_input_channel();
}

void mercury_ui_set_waterfall(bool enabled)
{
    ui_comm_set_waterfall(enabled);
}

void mercury_ui_get_tcp_ports(int *arq_base_port, int *broadcast_port)
{
    ui_comm_get_tcp_ports(arq_base_port, broadcast_port);
}
