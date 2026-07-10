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

int  mercury_init(const char *config_path, const char *log_path, int verbose);
void mercury_shutdown(void);
void mercury_request_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
