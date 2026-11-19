/* HERMES Modem - GPIO line PTT backend (Linux GPIO character device)
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One output line, requested through libgpiod 2.x.  The polarity is the
 * kernel's (active-low when gpio_invert), so "on" is always ACTIVE here.
 * The request keeps its own file descriptor, so the chip is closed as soon
 * as the line is ours.
 */

#include "gpio_ptt.h"

#include <errno.h>
#include <string.h>

#include "../common/hermes_log.h"

#define GPIO_LOG_TAG "gpio-ptt"

#ifdef HAVE_LIBGPIOD

#include <gpiod.h>

static struct gpiod_line_request *g_req;
static unsigned int g_line;

int mercury_gpio_ptt_open(const ptt_config_t *config)
{
    if (!config)
        return -1;
    mercury_gpio_ptt_close();

    const char *path = config->gpio_chip[0] ? config->gpio_chip : PTT_GPIO_CHIP_DEFAULT;
    unsigned int line = config->gpio_line;

    struct gpiod_chip *chip = gpiod_chip_open(path);
    if (!chip) {
        HLOGE(GPIO_LOG_TAG, "%s: %s", path, strerror(errno));
        return -1;
    }

    struct gpiod_line_settings *ls = gpiod_line_settings_new();
    struct gpiod_line_config *lc = gpiod_line_config_new();
    struct gpiod_request_config *rc = gpiod_request_config_new();
    struct gpiod_line_request *req = NULL;

    if (ls && lc && rc &&
        gpiod_line_settings_set_direction(ls, GPIOD_LINE_DIRECTION_OUTPUT) == 0 &&
        gpiod_line_settings_set_output_value(ls, GPIOD_LINE_VALUE_INACTIVE) == 0) {
        gpiod_line_settings_set_active_low(ls, config->gpio_invert);
        gpiod_request_config_set_consumer(rc, "mercury-ptt");
        if (gpiod_line_config_add_line_settings(lc, &line, 1, ls) == 0)
            req = gpiod_chip_request_lines(chip, rc, lc);
    }
    int err = errno;

    gpiod_request_config_free(rc);
    gpiod_line_config_free(lc);
    gpiod_line_settings_free(ls);
    gpiod_chip_close(chip);

    if (!req) {
        /* EBUSY: another program holds the line; EINVAL: no such line. */
        HLOGE(GPIO_LOG_TAG, "%s line %u: %s", path, line, strerror(err));
        return -1;
    }
    g_req = req;
    g_line = line;
    return 0;
}

int mercury_gpio_ptt_set(bool on)
{
    if (!g_req)
        return -1;
    if (gpiod_line_request_set_value(g_req, g_line,
            on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE) < 0) {
        HLOGE(GPIO_LOG_TAG, "PTT %s: %s", on ? "on" : "off", strerror(errno));
        return -1;
    }
    return 0;
}

void mercury_gpio_ptt_close(void)
{
    if (!g_req)
        return;
    /* Unkey before giving the line back: the kernel keeps its last value. */
    gpiod_line_request_set_value(g_req, g_line, GPIOD_LINE_VALUE_INACTIVE);
    gpiod_line_request_release(g_req);
    g_req = NULL;
}

#else /* !HAVE_LIBGPIOD */

int mercury_gpio_ptt_open(const ptt_config_t *config)
{
    (void)config;
    HLOGE(GPIO_LOG_TAG, "GPIO PTT is not in this build (needs libgpiod 2.x, Linux)");
    return -1;
}

int mercury_gpio_ptt_set(bool on)
{
    (void)on;
    return -1;
}

void mercury_gpio_ptt_close(void)
{
}

#endif
