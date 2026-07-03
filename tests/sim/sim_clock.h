/* tests/sim/sim_clock.h
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#ifndef SIM_CLOCK_H
#define SIM_CLOCK_H
#include <stdint.h>
void     sim_clock_reset(uint64_t start_ms);
uint64_t sim_clock_now(void);
void     sim_clock_set(uint64_t ms);
#endif
