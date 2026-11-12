#ifndef MERCURY_GPIO_PTT_H
#define MERCURY_GPIO_PTT_H

#include <stdbool.h>
#include "radio_io.h"

int gpio_ptt_open(const ptt_config_t *config);
int gpio_ptt_set(bool on);
void gpio_ptt_close(void);

#endif
