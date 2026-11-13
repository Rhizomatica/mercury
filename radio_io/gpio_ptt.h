#ifndef MERCURY_GPIO_PTT_H
#define MERCURY_GPIO_PTT_H

#include <stdbool.h>
#include "radio_io.h"

int mercury_gpio_ptt_open(const ptt_config_t *config);
int mercury_gpio_ptt_set(bool on);
void mercury_gpio_ptt_close(void);

#endif
