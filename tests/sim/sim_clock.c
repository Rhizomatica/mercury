/* tests/sim/sim_clock.c
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#include "sim_clock.h"

/* Provided by tests/datalink_arq/arq_test_stubs.c (link seam over hermes_uptime_ms). */
extern void mock_set_uptime_ms(uint64_t ms);

static uint64_t s_now_ms;

void sim_clock_reset(uint64_t start_ms)
{
    s_now_ms = start_ms;
    mock_set_uptime_ms(s_now_ms);
}

uint64_t sim_clock_now(void) { return s_now_ms; }

void sim_clock_set(uint64_t ms)
{
    /* Clock only moves forward in a discrete-event sim. */
    if (ms < s_now_ms) ms = s_now_ms;
    s_now_ms = ms;
    mock_set_uptime_ms(s_now_ms);
}
