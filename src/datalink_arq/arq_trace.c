/* See arq_trace.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "arq_trace.h"

#ifndef ARQ_TRACE_ENABLED

/* Compiled out entirely.  The macros in arq_trace.h are no-ops without the
 * flag, so nothing calls these -- but leaving the definitions in would still
 * put the 128 KB record ring in .bss of every release build, which is the
 * opposite of the "release pays nothing" contract this header advertises.
 * ISO C forbids an empty translation unit, hence the typedef. */
typedef int arq_trace_not_compiled_in;

#else


#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#include "hermes_log.h"
#include "virtual_clock.h"

#define ARQ_TRACE_MAX 8192

typedef struct {
    uint64_t t_ms;
    uint8_t  kind;
    uint8_t  a;
    uint8_t  b;
    uint16_t extra;
} arq_trace_rec_t;

static arq_trace_rec_t  g_tr[ARQ_TRACE_MAX];
static _Atomic unsigned g_tr_n;
static uint64_t         g_tr_t0;

void arq_trace_add(uint8_t kind, uint8_t a, uint8_t b, uint16_t extra)
{
    unsigned i = atomic_fetch_add_explicit(&g_tr_n, 1u, memory_order_relaxed);
    if (i >= ARQ_TRACE_MAX)
        return;                     /* full: stop, never wrap -- the interesting
                                     * part of a connect failure is the start */
    uint64_t now = time_now_ms();
    if (i == 0)
        g_tr_t0 = now;
    g_tr[i].t_ms  = now;
    g_tr[i].kind  = kind;
    g_tr[i].a     = a;
    g_tr[i].b     = b;
    g_tr[i].extra = extra;
}

static const char *kind_name(uint8_t k)
{
    switch (k) {
    case ARQ_TR_RX_FRAME: return "RX_FRAME";
    case ARQ_TR_EV_IN:    return "EV_IN   ";
    case ARQ_TR_EV_OUT:   return "EV_OUT  ";
    case ARQ_TR_TX_START: return "TX_START";
    case ARQ_TR_TX_END:   return "TX_END  ";
    case ARQ_TR_DROP:     return "DROP    ";
    default:              return "?       ";
    }
}

void arq_trace_dump(const char *why)
{
    unsigned n = atomic_load_explicit(&g_tr_n, memory_order_relaxed);
    if (n == 0)
        return;
    if (n > ARQ_TRACE_MAX)
        n = ARQ_TRACE_MAX;

    /* Records are stamped with time_now_ms(), which is process UPTIME -- fine
     * within one process, useless for lining two of them up.  Diagnosing a
     * half-duplex collision means overlaying the caller's transmissions on the
     * answerer's, so every record also gets a wall-clock stamp, derived once
     * here from the current uptime/wall pair.  Without this the two traces
     * silently share an axis they do not actually share. */
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    uint64_t now_ms  = time_now_ms();
    int64_t  wall_ms = (int64_t)wall.tv_sec * 1000 + wall.tv_nsec / 1000000;

    HLOGI("arq-trace", "==== trace (%s): %u records ====", why ? why : "", n);
    for (unsigned i = 0; i < n; i++)
    {
        int64_t   w = wall_ms - (int64_t)(now_ms - g_tr[i].t_ms);
        time_t    sec = (time_t)(w / 1000);
        struct tm tmv;
        char      hhmmss[16];
        localtime_r(&sec, &tmv);
        strftime(hhmmss, sizeof(hhmmss), "%H:%M:%S", &tmv);
        HLOGI("arq-trace", "TRACE %s.%03d %8.3f %s a=%3u b=%3u x=%u",
              hhmmss, (int)(w % 1000),
              (double)(g_tr[i].t_ms - g_tr_t0) / 1000.0,
              kind_name(g_tr[i].kind), g_tr[i].a, g_tr[i].b, g_tr[i].extra);
    }
    HLOGI("arq-trace", "==== end trace ====");
}

#endif /* ARQ_TRACE_ENABLED */
