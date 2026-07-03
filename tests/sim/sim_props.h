/* tests/sim/sim_props.h
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#ifndef SIM_PROPS_H
#define SIM_PROPS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sim_core.h"
#include "sim_endpoint.h"

typedef struct { bool ok; char detail[256]; } sim_verdict_t;

/* Check that exactly sent_len bytes were delivered to dst, matching sent[]. */
sim_verdict_t sim_prop_integrity(sim_t *s, sim_endpoint_t *src, sim_endpoint_t *dst,
                                  const uint8_t *sent, size_t sent_len);

/* Check that both sessions are disconnected or are in a stable connected-idle
 * state with no channel frames pending. */
sim_verdict_t sim_prop_both_idle_or_disconnected(sim_t *s);

/* Check that the endpoint's payload_mode equals (or is the floor of) floor_mode
 * within the current session (within_cycles is a reserved hint for future use). */
sim_verdict_t sim_prop_mode_floor_reached(sim_endpoint_t *ep, int floor_mode,
                                           int within_cycles);

#endif
