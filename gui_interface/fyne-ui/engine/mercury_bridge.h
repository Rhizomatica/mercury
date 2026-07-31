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

/* The UI status struct is defined once, in the engine — the bridge hands out
 * that same type rather than declaring a parallel copy. */
#include "ui_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the engine.  argc/argv are the process args forwarded from Go so the
 * same CLI parser the daemon uses applies here.  default_config is the config
 * path used when -C is absent; log_path is the engine log file. */
/* Handle -h/-l/-z/-K (print to terminal); call before creating the window.
 * Returns non-zero if an info action was handled (the caller should exit). */
/* Print the startup version banner (same text the daemon prints). */
void mercury_print_version(void);
int  mercury_precheck(int argc, char **argv, const char *default_config);
int  mercury_init(int argc, char **argv, const char *default_config, const char *log_path);
void mercury_shutdown(void);
void mercury_request_shutdown(void);

/* ---- In-process UI link ----------------------------------------------------
 * The embedded UI pulls state and pushes commands through these calls instead
 * of opening a websocket back to its own process.  The websocket server keeps
 * running for remote clients (HERMES web UI, a desktop UI on another machine).
 *
 * Pull, not callbacks: the engine's publisher threads never call into Go, so a
 * busy or stopped UI can never stall the modem, and there is no cgocallback on
 * the audio-adjacent threads. */

/* Copy the latest status snapshot.  Returns 1 on success, 0 before the engine
 * has published its first one. */
int mercury_ui_get_status(ui_status_t *out);

/* Copy the current RX power spectrum (dB).  Returns the number of bins written
 * (0 if none available yet) and sets *sample_rate_hz when non-NULL. */
int mercury_ui_get_spectrum(float *out, int max_bins, int *sample_rate_hz);

/* Run a UI command through the same handler the websocket path uses.
 * Unused values may be NULL.  Returns 0 on success. */
int mercury_ui_command(const char *command, const char *value,
                       const char *value2, const char *value3);

/* Device pickers.  The websocket path pushes these to a client when it
 * connects; a local UI asks for them instead.  Both read the same enumerators,
 * so the two see the same devices and the same selection. */
int mercury_ui_get_audio_devices(int kind, ui_device_t *out, int max,
                                 char *selected, int selected_len);
int mercury_ui_get_radio_list(ui_device_t *out, int max,
                              char *selected, int selected_len,
                              char *device_path, int device_path_len,
                              int *serial_speed);
int mercury_ui_get_input_channel(void);

#ifdef __cplusplus
}
#endif

#endif
