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

/* Open the CM108 HID endpoint and park PTT off.
 * config->device may name an explicit /dev/hidrawN; empty selects the first
 * CM108-class device found.  Returns 0 on success, -1 on error. */
int cm108_ptt_open(const ptt_config_t *config);

/* Assert or release PTT on the configured GPIO.  0 on success, -1 on error. */
int cm108_ptt_set(bool on);

/* Release PTT and close the device.  Safe when not open. */
void cm108_ptt_close(void);

/* Build the 5-byte HID output report that keys (or releases) GPIO `gpio`.
 * Exposed so the exact wire bytes can be checked without hardware: an
 * off-by-one here keys the wrong pin and is otherwise invisible.
 * Returns 0 on success, -1 if gpio is outside 1..4. */
int cm108_ptt_report(bool on, int gpio, unsigned char out[5]);

/* Describe the CM108-class devices present, one line per device, into buf.
 * Returns the number found (which may exceed what fits in buf). */
int cm108_ptt_list(char *buf, size_t buf_size);

#endif /* CM108_PTT_H_ */
