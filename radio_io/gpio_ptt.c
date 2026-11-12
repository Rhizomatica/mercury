/* HERMES Modem - GPIO PTT backend using libgpiod 2.x */

#include "gpio_ptt.h"
#include "../common/hermes_log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define GPIO_LOG_TAG "gpio-ptt"

#ifdef HAVE_LIBGPIOD

#include <gpiod.h>

static struct gpiod_chip *gpio_chip;
static struct gpiod_line_request *gpio_request;
static unsigned int gpio_offset;
static bool gpio_invert;

static enum gpiod_line_value gpio_value(bool on)
{
    bool active = on != gpio_invert;
    return active ? GPIOD_LINE_VALUE_ACTIVE
                  : GPIOD_LINE_VALUE_INACTIVE;
}

int gpio_ptt_open(const ptt_config_t *config)
{
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_config = NULL;
    struct gpiod_request_config *request_config = NULL;
    const char *chip_path;
    int rc = -1;

    if (!config)
        return -1;

    if (gpio_request || gpio_chip)
        gpio_ptt_close();

    chip_path = config->gpio_chip[0]
        ? config->gpio_chip : PTT_GPIO_CHIP_DEFAULT;

    gpio_offset = config->gpio_line;
    gpio_invert = config->gpio_invert;

    gpio_chip = gpiod_chip_open(chip_path);
    if (!gpio_chip)
    {
        HLOGE(GPIO_LOG_TAG, "Cannot open GPIO chip %s: %s",
              chip_path, strerror(errno));
        goto done;
    }

    settings = gpiod_line_settings_new();
    line_config = gpiod_line_config_new();
    request_config = gpiod_request_config_new();

    if (!settings || !line_config || !request_config)
    {
        HLOGE(GPIO_LOG_TAG, "Cannot allocate libgpiod configuration");
        goto done;
    }

    if (gpiod_line_settings_set_direction(
            settings, GPIOD_LINE_DIRECTION_OUTPUT) < 0 ||
        gpiod_line_settings_set_output_value(
            settings, gpio_value(false)) < 0)
    {
        HLOGE(GPIO_LOG_TAG, "Cannot configure GPIO output: %s",
              strerror(errno));
        goto done;
    }

    if (gpiod_line_config_add_line_settings(
            line_config, &gpio_offset, 1, settings) < 0)
    {
        HLOGE(GPIO_LOG_TAG, "Cannot configure GPIO line %u: %s",
              gpio_offset, strerror(errno));
        goto done;
    }

    gpiod_request_config_set_consumer(request_config, "mercury-ptt");

    gpio_request = gpiod_chip_request_lines(
        gpio_chip, request_config, line_config);

    if (!gpio_request)
    {
        HLOGE(GPIO_LOG_TAG, "Cannot request GPIO line %u on %s: %s",
              gpio_offset, chip_path, strerror(errno));
        goto done;
    }

    HLOGI(GPIO_LOG_TAG, "GPIO PTT ready: chip=%s BCM=%u invert=%s",
          chip_path, gpio_offset, gpio_invert ? "true" : "false");
    rc = 0;

done:
    if (request_config)
        gpiod_request_config_free(request_config);
    if (line_config)
        gpiod_line_config_free(line_config);
    if (settings)
        gpiod_line_settings_free(settings);

    if (rc != 0)
        gpio_ptt_close();

    return rc;
}

int gpio_ptt_set(bool on)
{
    if (!gpio_request)
    {
        HLOGE(GPIO_LOG_TAG, "GPIO PTT is not initialized");
        return -1;
    }

    if (gpiod_line_request_set_value(
            gpio_request, gpio_offset, gpio_value(on)) < 0)
    {
        HLOGE(GPIO_LOG_TAG, "Cannot set GPIO PTT %s: %s",
              on ? "ON" : "OFF", strerror(errno));
        return -1;
    }

    HLOGD(GPIO_LOG_TAG, "GPIO PTT %s", on ? "ON" : "OFF");
    return 0;
}

void gpio_ptt_close(void)
{
    if (gpio_request)
    {
        /* Try to leave the line inactive before releasing ownership. */
        if (gpiod_line_request_set_value(
                gpio_request, gpio_offset, gpio_value(false)) < 0)
        {
            HLOGE(GPIO_LOG_TAG, "Could not set GPIO inactive during close: %s",
                  strerror(errno));
        }

        gpiod_line_request_release(gpio_request);
        gpio_request = NULL;
    }

    if (gpio_chip)
    {
        gpiod_chip_close(gpio_chip);
        gpio_chip = NULL;
    }
}

#else

int gpio_ptt_open(const ptt_config_t *config)
{
    (void)config;
    HLOGE(GPIO_LOG_TAG,
          "GPIO PTT unavailable: Mercury was built without libgpiod");
    return -1;
}

int gpio_ptt_set(bool on)
{
    (void)on;
    return -1;
}

void gpio_ptt_close(void)
{
}

#endif
