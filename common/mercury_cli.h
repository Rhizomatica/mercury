/* Mercury CLI argument parser — shared by the standalone daemon (main.c) and
 * the embedded Fyne UI bridge, so both honor the same command-line options and
 * config-override logic from one place.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MERCURY_CLI_H
#define MERCURY_CLI_H

#include <stdbool.h>
#include "cfg_utils.h"

typedef enum {
    MERCURY_CLI_RUN = 0,        /* proceed to run the engine       */
    MERCURY_CLI_LIST_MODES,     /* -l : list modulation modes      */
    MERCURY_CLI_LIST_SNDCARDS,  /* -z : list sound cards           */
    MERCURY_CLI_LIST_RADIOS,    /* -K : list HAMLIB radio models   */
    MERCURY_CLI_TEST_PTT,       /* -Q : pulse configured PTT       */
    MERCURY_CLI_HELP,           /* -h : print usage                */
} mercury_cli_action_t;

/* Everything main()/the bridge need out of the command line. */
typedef struct {
    mercury_config cfg;          /* defaults + config file + CLI overrides   */
    char           cfg_path[1024];
    int            startup_mode;  /* FreeDV payload mode (-m/-s)              */
    int            test_mode;     /* 0 normal, 1 TX test, 2 RX test           */
    int            cpu_nr;        /* -c CPU pin, -1 = none                    */
    const char    *log_file_path; /* -L (NULL if unset; points into argv)     */
    bool           log_file_jsonl;/* -J                                       */
    mercury_cli_action_t action;
} mercury_cli_t;

/* Parse argv into |out|.  |default_cfg_path| is used when -C is not given
 * (the daemon passes "mercury.ini"; the UI passes its per-user writable path).
 * Reads the config file, then applies CLI overrides on top.
 * Returns 0 on success (inspect out->action), -1 on a parse/validation error
 * (an error message + usage have already been printed). */
int  mercury_cli_parse(int argc, char **argv,
                       const char *default_cfg_path, mercury_cli_t *out);

void mercury_cli_print_usage(const char *prog);

/* Execute an informational action (-h/-l/-z/-K): print usage / list modes /
 * list sound cards / list radio models.  Returns true if such an action was
 * handled (the caller should exit), false for MERCURY_CLI_RUN or -Q. */
bool mercury_cli_run_info_action(const mercury_cli_t *cli, const char *prog);

/* Open the configured backend, key for one second, unkey, close and exit.
 * Returns 0 on success, -1 if opening or changing PTT state fails. */
int mercury_cli_run_ptt_test(const mercury_cli_t *cli);

/* Startup-mode table (index -> FreeDV mode); freedv_mode_names is also
 * referenced by the modem. */
extern int   freedv_modes[];
extern char *freedv_mode_names[];
int  mercury_cli_mode_count(void);

#endif /* MERCURY_CLI_H */
