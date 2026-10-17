/* HERMES Modem - CM108-class USB sound-chip GPIO PTT backend
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CM108_PTT_H_
#define CM108_PTT_H_

#include <stdbool.h>
#include <stddef.h>

#include "radio_io.h"

/* The mercury_ prefix is not decoration: hamlib exports cm108_ptt_set() and
 * cm108_ptt_get() from its own src/cm108.c.  On Linux and Windows hamlib is a
 * shared library so a clash resolves silently at load time; on macOS it is
 * linked statically (radio_io/hamlib-macos/lib/libhamlib.a) and the duplicate
 * symbol is a hard link error.  That asymmetry means the collision is
 * invisible on two of the three platforms we ship. */

/* Open the CM108 HID endpoint and park PTT off.
 * config->device may name an explicit /dev/hidrawN; empty selects the first
 * CM108-class device found.  Returns 0 on success, -1 on error. */
int mercury_cm108_open(const ptt_config_t *config);

/* Assert or release PTT on the configured GPIO.  0 on success, -1 on error. */
int mercury_cm108_set(bool on);

/* Release PTT and close the device.  Safe when not open. */
void mercury_cm108_close(void);

/* Build the 5-byte HID output report that keys (or releases) GPIO `gpio`.
 * Exposed so the exact wire bytes can be checked without hardware: an
 * off-by-one here keys the wrong pin and is otherwise invisible.
 * Returns 0 on success, -1 if gpio is outside 1..4. */
int mercury_cm108_report(bool on, int gpio, unsigned char out[5]);

/* Return true only for canonical Linux hidraw device paths (/dev/hidrawN).
 * The raw Linux transport uses this before opening an explicit path so a
 * stale serial/Hamlib device cannot receive a CM108 HID report. */
bool mercury_cm108_is_hidraw_path(const char *path);

/* Describe the CM108-class devices present, one line per device, into buf.
 * Returns the number found (which may exceed what fits in buf). */
int mercury_cm108_list(char *buf, size_t buf_size);

#endif /* CM108_PTT_H_ */
