/* arq_early_pending -- see arq_early_pending.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "arq_early_pending.h"

#include <string.h>

void arq_ep_init(arq_ep_state_t *s)
{
    memset(s, 0, sizeof(*s));
}

static bool over_budget(const arq_ep_state_t *s, uint64_t now_ms)
{
    for (unsigned i = 0; i < ARQ_EP_FALSE_BUDGET; i++)
        if (s->false_ms[i] == 0 || now_ms - s->false_ms[i] >= ARQ_EP_FALSE_WINDOW_MS)
            return false;
    return true;
}

static arq_ep_action_t cancel(arq_ep_state_t *s, uint64_t now_ms)
{
    s->active   = false;
    s->clear_ms = 0;
    s->false_ms[s->false_head] = now_ms ? now_ms : 1;
    s->false_head = (s->false_head + 1) % ARQ_EP_FALSE_BUDGET;
    s->n_cancelled++;
    return ARQ_EP_SEND_CANCEL;
}

arq_ep_action_t arq_ep_step(arq_ep_state_t *s, uint64_t now_ms, bool eligible,
                            bool in_flight, uint64_t age_ms)
{
    /* A younger lock than last time is a new frame, even if the poll never
     * saw the gap between the two. */
    bool new_lock = in_flight && age_ms < s->last_age_ms;
    s->last_age_ms = in_flight ? age_ms : 0;

    if (!eligible)
    {
        s->active   = false;
        s->spent    = false;
        s->clear_ms = 0;
        return ARQ_EP_NONE;
    }

    if (s->active)
    {
        if (in_flight)
        {
            s->clear_ms = 0;
            if (age_ms > ARQ_EP_MAX_LOCK_MS)
            {
                s->spent = true;
                return cancel(s, now_ms);
            }
            return ARQ_EP_NONE;
        }
        if (s->clear_ms == 0)
            s->clear_ms = now_ms ? now_ms : 1;
        if (now_ms - s->clear_ms >= ARQ_EP_GRACE_MS)
            return cancel(s, now_ms);
        return ARQ_EP_NONE;
    }

    if (s->spent && (!in_flight || new_lock))
        s->spent = false;
    if (s->spent || !in_flight || age_ms < ARQ_EP_MIN_LOCK_MS || age_ms > ARQ_EP_MAX_LOCK_MS)
    {
        if (!in_flight)
            s->suppressed = false;
        return ARQ_EP_NONE;
    }
    if (over_budget(s, now_ms))
    {
        if (!s->suppressed)
        {
            s->suppressed = true;
            s->n_suppressed++;
        }
        return ARQ_EP_NONE;
    }
    s->active     = true;
    s->suppressed = false;
    s->clear_ms   = 0;
    s->n_sent++;
    return ARQ_EP_SEND_PENDING;
}

void arq_ep_promoted(arq_ep_state_t *s)
{
    if (!s->active)
        return;
    s->active   = false;
    s->clear_ms = 0;
    s->n_promoted++;
}
