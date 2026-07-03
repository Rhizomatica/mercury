/* tests/sim/sim_core.h
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#ifndef SIM_CORE_H
#define SIM_CORE_H
#include <stdint.h>
#include <stddef.h>
#include "sim_channel.h"
#include "sim_endpoint.h"
#include "arq_fsm.h"

typedef struct sim sim_t;

sim_t          *sim_create(const sim_channel_cfg_t *chan_cfg,
                            const char *call_a, const char *call_b);
void            sim_destroy(sim_t *s);
sim_endpoint_t *sim_a(sim_t *s);
sim_endpoint_t *sim_b(sim_t *s);

/* Inject an app event into one endpoint. */
void            sim_inject(sim_t *s, sim_endpoint_t *ep, const arq_event_t *ev);

/* Run the event loop until no channel frames in flight AND both FSM deadlines
 * are INT_MAX-idle, OR until max_ms virtual time elapses.
 * Returns virtual ms elapsed. */
uint64_t        sim_run_until_idle(sim_t *s, uint64_t max_ms);

/* Number of pending channel events (for liveness checks). */
int             sim_frames_in_flight(sim_t *s);

#endif
