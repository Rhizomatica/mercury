/* HERMES Modem — Virtual clock implementation. See virtual_clock.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "virtual_clock.h"
#include "hermes_log.h"

#include <stdatomic.h>

static atomic_bool          g_enabled = false;
static atomic_uint_fast64_t g_now_ms  = 0;

void virtual_clock_enable(uint64_t epoch_ms)
{
    atomic_store_explicit(&g_now_ms, epoch_ms, memory_order_relaxed);
    atomic_store_explicit(&g_enabled, true, memory_order_relaxed);
}

void virtual_clock_set(uint64_t now_ms)
{
    atomic_store_explicit(&g_now_ms, now_ms, memory_order_relaxed);
}

bool virtual_clock_enabled(void)
{
    return atomic_load_explicit(&g_enabled, memory_order_relaxed);
}

uint64_t time_now_ms(void)
{
    if (atomic_load_explicit(&g_enabled, memory_order_relaxed))
        return (uint64_t)atomic_load_explicit(&g_now_ms, memory_order_relaxed);
    return hermes_uptime_ms();
}
