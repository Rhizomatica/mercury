/* tests/sim/sim_core.h
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#ifndef SIM_CORE_H
#define SIM_CORE_H
#include <stdint.h>
#include <stdbool.h>
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

/* Half-duplex shared medium: two stations keyed at once destroy each other's
 * frames.  OFF by default so existing tests keep their channel semantics; turn
 * it on for anything about turn coordination, where a collision IS the bug. */
void            sim_set_half_duplex(sim_t *s, bool on);
int             sim_collisions(sim_t *s);

/* Carrier sense: a station hears the peer (decoder sync, what listen-before-
 * talk reads) from acq_ms after the peer keys until it unkeys, unless it is
 * keyed itself; senders also get TX_STARTED.  OFF by default. */
void            sim_set_carrier_sense(sim_t *s, bool on, uint32_t acq_ms);
/* Is this endpoint on the air right now? */
bool            sim_keyed(sim_t *s, sim_endpoint_t *ep);

/* Fade controls: change channel loss / delivered-frame SNR mid-simulation. */
void            sim_set_per(sim_t *s, double per);
void            sim_set_rx_snr(sim_t *s, float snr_db);
/* Coherent fade: cliff-model channel SNR + delivered-frame SNR together. */
void            sim_set_snr(sim_t *s, double snr_db);

/* Empirical per-mode erasure table (see sim_channel_set_mode_per) plus the
 * SNR stamped on delivered frames — models ISI-limited channels (e.g. NVIS
 * disturbed) where the SNR reads healthy while fast modes fail. */
void            sim_set_mode_per(sim_t *s, const sim_mode_per_t *table,
                                 int count, float rx_snr_db);

#endif
