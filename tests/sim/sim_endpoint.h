/* tests/sim/sim_endpoint.h
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#ifndef SIM_ENDPOINT_H
#define SIM_ENDPOINT_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "arq_fsm.h"

typedef struct sim_endpoint sim_endpoint_t;

typedef struct {
    uint8_t buf[1280];
    size_t  len;
    int     packet_type;
    int     mode;
    int     burst_remaining;
    bool    present;
} sim_outframe_t;

sim_endpoint_t            *sim_endpoint_create(const char *my_call, const char *peer_call);
void                       sim_endpoint_destroy(sim_endpoint_t *ep);
arq_session_t             *sim_endpoint_session(sim_endpoint_t *ep);
const char                *sim_endpoint_call(sim_endpoint_t *ep);
void                       sim_endpoint_queue_tx(sim_endpoint_t *ep,
                                                  const uint8_t *data, size_t len);
size_t                     sim_endpoint_delivered(sim_endpoint_t *ep,
                                                   uint8_t *out, size_t out_cap);

const arq_fsm_callbacks_t *sim_endpoint_callbacks(void);
void                       sim_endpoint_set_active(sim_endpoint_t *ep);
sim_endpoint_t            *sim_endpoint_active(void);
bool                       sim_endpoint_take_outframe(sim_endpoint_t *ep, sim_outframe_t *out);
#endif
