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
