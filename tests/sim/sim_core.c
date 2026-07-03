/* tests/sim/sim_core.c
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica
 *
 * Discrete-event scheduler driving two arq_session_t FSMs through a
 * deterministic lossy virtual channel. */

#include "sim_core.h"
#include "sim_clock.h"
#include "sim_translate.h"
#include "arq_fsm.h"
#include "arq_timing.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

/* ======================================================================
 * Pending event queue
 * ====================================================================== */

#define SIM_PENDING_MAX 32

typedef enum {
    SIM_PENDING_FRAME,       /* deliver a translated frame to target */
    SIM_PENDING_TX_COMPLETE  /* inject ARQ_EV_TX_COMPLETE to target (sender) */
} sim_pending_kind_t;

typedef struct {
    uint64_t           fire_at_ms;
    sim_endpoint_t    *target;
    sim_pending_kind_t kind;
    /* for SIM_PENDING_FRAME: */
    uint8_t            frame[1280];
    size_t             frame_len;
    float              rx_snr;
} sim_pending_t;

/* ======================================================================
 * sim_t
 * ====================================================================== */

struct sim {
    sim_endpoint_t  *a;
    sim_endpoint_t  *b;
    sim_channel_t   *ch;

    sim_pending_t    pending[SIM_PENDING_MAX];
    int              pending_count;

    arq_timing_ctx_t timing;  /* shared timing context (metrics only, not correctness) */
};

/* ======================================================================
 * Internal helpers
 * ====================================================================== */

/* Add a pending event. Asserts on queue overflow (shouldn't happen for
 * stop-and-wait sessions, but catches bugs early). */
static void enqueue(sim_t *s, const sim_pending_t *p)
{
    assert(s->pending_count < SIM_PENDING_MAX && "pending queue overflow");
    s->pending[s->pending_count++] = *p;
}

/* Drain outframes from one endpoint, schedule TX_COMPLETE for the sender
 * and (if the channel doesn't erase the frame) a FRAME delivery for the peer. */
static void drain_outframes_from(sim_t *s, sim_endpoint_t *sender,
                                 sim_endpoint_t *peer, uint64_t now_ms)
{
    sim_outframe_t of;
    while (sim_endpoint_take_outframe(sender, &of))
    {
        uint32_t airtime = sim_channel_airtime_ms(of.mode, of.len);
        uint64_t tx_end  = now_ms + airtime;

        /* TX_COMPLETE back to sender at TX-end time. */
        sim_pending_t tx_done = {
            .fire_at_ms = tx_end,
            .target     = sender,
            .kind       = SIM_PENDING_TX_COMPLETE,
        };
        enqueue(s, &tx_done);

        /* Frame delivery to peer (may be erased). */
        uint64_t deliver_at = 0;
        int dir = (sender == s->a) ? 0 : 1;
        if (sim_channel_schedule(s->ch, now_ms, dir, of.mode, of.len, &deliver_at))
        {
            sim_pending_t frame_ev = {
                .fire_at_ms = deliver_at,
                .target     = peer,
                .kind       = SIM_PENDING_FRAME,
                .frame_len  = of.len,
                .rx_snr     = 12.0f,  /* fixed simulated SNR; adequate for mode inference */
            };
            memcpy(frame_ev.frame, of.buf, of.len);
            enqueue(s, &frame_ev);
        }
    }
}

static void drain_all_outframes(sim_t *s, uint64_t now_ms)
{
    drain_outframes_from(s, s->a, s->b, now_ms);
    drain_outframes_from(s, s->b, s->a, now_ms);
}

/* Fire one pending event. */
static void fire_pending(sim_t *s, sim_pending_t *p)
{
    sim_endpoint_set_active(p->target);

    if (p->kind == SIM_PENDING_TX_COMPLETE)
    {
        arq_event_t ev = { .id = ARQ_EV_TX_COMPLETE };
        arq_fsm_dispatch(sim_endpoint_session(p->target), &ev);
        return;
    }

    /* SIM_PENDING_FRAME */
    arq_event_t ev;
    bool ok = sim_translate_frame(p->frame, p->frame_len, p->rx_snr,
                                   sim_endpoint_call(p->target), &ev);
    if (ok)
        arq_fsm_dispatch(sim_endpoint_session(p->target), &ev);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

sim_t *sim_create(const sim_channel_cfg_t *chan_cfg,
                  const char *call_a, const char *call_b)
{
    sim_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->a  = sim_endpoint_create(call_a, call_b);
    s->b  = sim_endpoint_create(call_b, call_a);
    s->ch = sim_channel_create(chan_cfg);
    if (!s->a || !s->b || !s->ch) { sim_destroy(s); return NULL; }

    arq_timing_init(&s->timing);
    /* Register shared callbacks once; they read s_active for per-call context. */
    arq_fsm_set_callbacks(sim_endpoint_callbacks());
    arq_fsm_set_timing(&s->timing);

    /* Initialise virtual clock at 1000 ms so FSMs never see 0-time deadlines. */
    sim_clock_reset(1000);
    return s;
}

void sim_destroy(sim_t *s)
{
    if (!s) return;
    sim_endpoint_destroy(s->a);
    sim_endpoint_destroy(s->b);
    sim_channel_destroy(s->ch);
    free(s);
}

sim_endpoint_t *sim_a(sim_t *s) { return s->a; }
sim_endpoint_t *sim_b(sim_t *s) { return s->b; }

int sim_frames_in_flight(sim_t *s) { return s->pending_count; }

void sim_inject(sim_t *s, sim_endpoint_t *ep, const arq_event_t *ev)
{
    sim_endpoint_set_active(ep);
    arq_fsm_dispatch(sim_endpoint_session(ep), ev);
    drain_all_outframes(s, sim_clock_now());
}

uint64_t sim_run_until_idle(sim_t *s, uint64_t max_ms)
{
    uint64_t start = sim_clock_now();

    for (;;)
    {
        uint64_t now = sim_clock_now();

        /* Drain any outframes generated by prior dispatches. */
        drain_all_outframes(s, now);

        /* Compute timeouts for both FSMs. */
        int ta = arq_fsm_timeout_ms(sim_endpoint_session(s->a), now);
        int tb = arq_fsm_timeout_ms(sim_endpoint_session(s->b), now);

        /* Idle: no pending events, no scheduled deadlines. */
        if (s->pending_count == 0 && ta == INT_MAX && tb == INT_MAX)
            break;

        /* Find the earliest next event among pending entries and FSM deadlines. */
        uint64_t next = UINT64_MAX;
        for (int i = 0; i < s->pending_count; i++)
            if (s->pending[i].fire_at_ms < next)
                next = s->pending[i].fire_at_ms;

        if (ta != INT_MAX) {
            uint64_t da = now + (uint64_t)ta;
            if (da < next) next = da;
        }
        if (tb != INT_MAX) {
            uint64_t db = now + (uint64_t)tb;
            if (db < next) next = db;
        }

        /* Enforce max_ms cap. */
        if (next == UINT64_MAX || next > start + max_ms) {
            sim_clock_set(start + max_ms);
            break;
        }

        sim_clock_set(next);
        now = sim_clock_now();

        /* Fire FSM deadline(s) at this time — process A first, then B. */
        int ta2 = arq_fsm_timeout_ms(sim_endpoint_session(s->a), now);
        if (ta2 != INT_MAX && ta2 <= 0)
        {
            arq_session_t *sa = sim_endpoint_session(s->a);
            sa->deadline_ms = UINT64_MAX;
            arq_event_t tev = { .id = sa->deadline_event };
            sim_endpoint_set_active(s->a);
            arq_fsm_dispatch(sa, &tev);
            drain_all_outframes(s, now);
        }

        int tb2 = arq_fsm_timeout_ms(sim_endpoint_session(s->b), now);
        if (tb2 != INT_MAX && tb2 <= 0)
        {
            arq_session_t *sb = sim_endpoint_session(s->b);
            sb->deadline_ms = UINT64_MAX;
            arq_event_t tev = { .id = sb->deadline_event };
            sim_endpoint_set_active(s->b);
            arq_fsm_dispatch(sb, &tev);
            drain_all_outframes(s, now);
        }

        /* Fire all pending events whose time has arrived. */
        int i = 0;
        while (i < s->pending_count)
        {
            if (s->pending[i].fire_at_ms <= now)
            {
                sim_pending_t p = s->pending[i];
                /* Remove by swap with last. */
                s->pending[i] = s->pending[--s->pending_count];
                fire_pending(s, &p);
                drain_all_outframes(s, now);
                /* Restart scan since new entries may have been added. */
                i = 0;
            }
            else
            {
                i++;
            }
        }
    }

    return sim_clock_now() - start;
}
