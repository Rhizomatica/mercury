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

/* ------------------------------------------------------------------ */
/*  mercury_init(config_path, log_path, verbose)                      */
/*                                                                     */
/*  Loads mercury.ini and delegates to mercury_engine_init().          */
/*  log_path may be NULL (= no file log).                              */
/* ------------------------------------------------------------------ */
int mercury_init(const char *config_path, const char *log_path, int verbose)
{
    mercury_config cfg;
    cfg_set_defaults(&cfg);

    const char *ini = config_path ? config_path : "mercury.ini";
    if (access(ini, R_OK) == 0)
        cfg_read(&cfg, ini);

    /* The bridge always enables the UI (single-binary mode). */
    cfg.ui_enabled = true;

    return mercury_engine_init(&cfg, ini, log_path, false,
                                FREEDV_MODE_DATAC3, 0);
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
