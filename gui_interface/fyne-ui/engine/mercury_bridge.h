/* Mercury C Engine Bridge — CGo integration layer
 *
 * Exposes the Mercury HF modem engine as a library so the Fyne Go UI
 * can drive it in-process (single binary, no subprocess).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_BRIDGE_H
#define MERCURY_BRIDGE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the engine.  argc/argv are the process args forwarded from Go so the
 * same CLI parser the daemon uses applies here.  default_config is the config
 * path used when -C is absent; log_path is the engine log file. */
/* Handle -h/-l/-z/-K (print to terminal); call before creating the window.
 * Returns non-zero if an info action was handled (the caller should exit). */
int  mercury_precheck(int argc, char **argv, const char *default_config);
int  mercury_init(int argc, char **argv, const char *default_config, const char *log_path);
void mercury_shutdown(void);
void mercury_request_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
