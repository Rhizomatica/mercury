/* HERMES Modem - GPIO line PTT backend (Linux GPIO character device)
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPIO_PTT_H_
#define GPIO_PTT_H_

#include <stdbool.h>

#include "radio_io.h"

/* Keys the radio through one GPIO line of a Linux GPIO chip, as on a
 * Raspberry Pi header driving an optocoupler or a transistor on the PTT
 * line.  Uses libgpiod 2.x; without it (HAVE_LIBGPIOD unset, every
 * non-Linux build) open fails and says so.
 *
 * The mercury_ prefix matters: hamlib exports gpio_open(), gpio_ptt_set()
 * and friends from its own src/gpio.c, and where hamlib is linked
 * statically (macOS, Android) a clash is a link error (see cm108_ptt.h). */

/* Claim config->gpio_line on config->gpio_chip (empty: /dev/gpiochip0) as
 * an output, PTT off.  config->gpio_invert makes the line active-low.
 * Returns 0 on success, -1 on error. */
int mercury_gpio_ptt_open(const ptt_config_t *config);

/* Key (on) or release PTT.  0 on success, -1 on error or when not open. */
int mercury_gpio_ptt_set(bool on);

/* Release PTT and give the line back.  Safe when not open. */
void mercury_gpio_ptt_close(void);

#endif
