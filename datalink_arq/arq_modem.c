/* HERMES Modem — ARQ Modem interface: action queue and PTT callbacks
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_modem.h"
#include "arq_protocol.h"

#include <pthread.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "../modem/freedv/freedv_api.h"

/* ======================================================================
 * Action queue — ring buffer of arq_action_t entries
 * ====================================================================== */

#define MAX_QUEUE_CAPACITY 128

static arq_action_t    g_queue[MAX_QUEUE_CAPACITY];
static size_t          g_head;
static size_t          g_tail;
static size_t          g_count;
static size_t          g_cap;
static pthread_mutex_t g_qmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_qcond = PTHREAD_COND_INITIALIZER;
static bool            g_shutdown = false;

int arq_modem_queue_init(size_t capacity)
{
    if (capacity == 0 || capacity > MAX_QUEUE_CAPACITY)
        capacity = MAX_QUEUE_CAPACITY;

    pthread_mutex_lock(&g_qmtx);
    g_head = g_tail = g_count = 0;
    g_cap = capacity;
    g_shutdown = false;
    pthread_mutex_unlock(&g_qmtx);
    return 0;
}

void arq_modem_queue_shutdown(void)
{
    pthread_mutex_lock(&g_qmtx);
    g_shutdown = true;
    pthread_cond_broadcast(&g_qcond);
    pthread_mutex_unlock(&g_qmtx);
}

int arq_modem_enqueue(const arq_action_t *action)
{
    if (!action) return -1;

    pthread_mutex_lock(&g_qmtx);
    if (g_shutdown || g_count >= g_cap)
    {
        pthread_mutex_unlock(&g_qmtx);
        return -1;
    }
    g_queue[g_tail] = *action;
    g_tail = (g_tail + 1) % g_cap;
    g_count++;
    pthread_cond_signal(&g_qcond);
    pthread_mutex_unlock(&g_qmtx);
    return 0;
}

bool arq_modem_dequeue(arq_action_t *action, int timeout_ms)
{
    if (!action) return false;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000LL;
    if (ts.tv_nsec >= 1000000000LL)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000LL;
    }

    pthread_mutex_lock(&g_qmtx);
    while (g_count == 0 && !g_shutdown)
    {
        if (pthread_cond_timedwait(&g_qcond, &g_qmtx, &ts) == ETIMEDOUT)
            break;
    }
    if (g_count == 0)
    {
        pthread_mutex_unlock(&g_qmtx);
        return false;
    }
    /* Priority: control/mode-switch before payload */
    size_t idx = g_head;
    size_t prio_idx = g_head;
    bool found_prio = false;
    for (size_t i = 0; i < g_count; i++)
    {
        size_t j = (g_head + i) % g_cap;
        if (g_queue[j].type == ARQ_ACTION_TX_CONTROL ||
            g_queue[j].type == ARQ_ACTION_MODE_SWITCH)
        {
            prio_idx = j;
            found_prio = true;
            break;
        }
    }
    idx = found_prio ? prio_idx : g_head;

    *action = g_queue[idx];

    /* Compact the ring by copying head up to idx */
    if (idx != g_head)
    {
        /* Shift elements from head..idx-1 forward one position */
        size_t cur = idx;
        while (cur != g_head)
        {
            size_t prev = (cur == 0) ? (g_cap - 1) : (cur - 1);
            g_queue[cur] = g_queue[prev];
            cur = prev;
        }
    }
    g_head = (g_head + 1) % g_cap;
    g_count--;
    pthread_mutex_unlock(&g_qmtx);
    return true;
}

/* ======================================================================
 * PTT event injection — set by arq.c via arq_modem_set_event_fn()
 * ====================================================================== */

static void (*g_inject_event)(int mode, bool ptt_on) = NULL;

void arq_modem_set_event_fn(void (*fn)(int mode, bool ptt_on))
{
    g_inject_event = fn;
}

void arq_modem_ptt_on(int mode, size_t frame_size)
{
    (void)frame_size;
    if (g_inject_event)
        g_inject_event(mode, true);
}

void arq_modem_ptt_off(void)
{
    if (g_inject_event)
        g_inject_event(-1, false);
}

/* ======================================================================
 * Mode selection
 * ====================================================================== */

int arq_modem_preferred_rx_mode(const arq_session_t *sess)
{
    /* Always receive in control mode (DATAC16) to catch all frame types */
    (void)sess;
    return ARQ_CONTROL_MODE;
}

int arq_modem_preferred_tx_mode(const arq_session_t *sess)
{
    if (!sess) return ARQ_CONTROL_MODE;

    if (sess->conn_state == ARQ_CONN_CONNECTED &&
        (sess->dflow_state == ARQ_DFLOW_DATA_TX ||
         sess->dflow_state == ARQ_DFLOW_IDLE_ISS))
        return sess->payload_mode;

    return sess->control_mode;
}
