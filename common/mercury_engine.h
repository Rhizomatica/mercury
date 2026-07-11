/* Mercury Engine — shared init / teardown
 *
 * Extracted from main.c and mercury_bridge.c so both the standalone
 * CLI binary and the CGo Fyne UI call the same code path.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_ENGINE_H
#define MERCURY_ENGINE_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "cfg_utils.h"
#include "ui_communication.h"
#include "modem.h"

extern volatile bool shutdown_;

/* ------------------------------------------------------------------ */
/*  mercury_engine_init()                                              */
/*                                                                     */
/*  cfg   – pre-loaded config (defaults + INI applied)                 */
/*  config_path – path to mercury.ini (used for UI write-back)         */
/*  log_path – log file path (NULL = no file log)                      */
/*  log_jsonl – use JSONL format for the log file                      */
/*  startup_mode – FreeDV mode (e.g. FREEDV_MODE_DATAC3)              */
/*  test_mode – 0 = normal, 1 = TX test, 2 = RX test                  */
/*                                                                     */
/*  Returns 0 on success, non-zero on failure.                         */
/* ------------------------------------------------------------------ */
int mercury_engine_init(const mercury_config *cfg,
                        const char *config_path,
                        const char *log_path,
                        bool log_jsonl,
                        int startup_mode,
                        int test_mode);

/* ------------------------------------------------------------------ */
/*  mercury_engine_shutdown()                                          */
/*                                                                     */
/*  Call AFTER setting shutdown_ = true.                                */
/* ------------------------------------------------------------------ */
void mercury_engine_shutdown(void);

/* ------------------------------------------------------------------ */
/*  mercury_engine_is_initialized()                                    */
/*                                                                     */
/*  Returns true iff mercury_engine_init() completed successfully      */
/*  and mercury_engine_shutdown() has not been called.                 */
/* ------------------------------------------------------------------ */
bool mercury_engine_is_initialized(void);

#endif
