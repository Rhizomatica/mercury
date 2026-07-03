/* tests/sim/sim_props.c
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica */
#include "sim_props.h"
#include "arq_fsm.h"
#include <string.h>
#include <stdio.h>

sim_verdict_t sim_prop_integrity(sim_t *s, sim_endpoint_t *src, sim_endpoint_t *dst,
                                  const uint8_t *sent, size_t sent_len)
{
    (void)s;
    (void)src;

    sim_verdict_t v = { .ok = false };

    uint8_t delivered[256 * 1024];
    size_t got = sim_endpoint_delivered(dst, delivered, sizeof(delivered));

    if (got != sent_len) {
        snprintf(v.detail, sizeof(v.detail),
                 "length mismatch: delivered %zu bytes, expected %zu",
                 got, sent_len);
        return v;
    }

    for (size_t i = 0; i < sent_len; i++) {
        if (delivered[i] != sent[i]) {
            snprintf(v.detail, sizeof(v.detail),
                     "mismatch at offset %zu: got 0x%02x expected 0x%02x",
                     i, delivered[i], sent[i]);
            return v;
        }
    }

    v.ok = true;
    snprintf(v.detail, sizeof(v.detail), "ok: %zu bytes delivered intact", sent_len);
    return v;
}

sim_verdict_t sim_prop_both_idle_or_disconnected(sim_t *s)
{
    sim_verdict_t v = { .ok = false };

    arq_session_t *sa = sim_endpoint_session(sim_a(s));
    arq_session_t *sb = sim_endpoint_session(sim_b(s));

    bool a_ok = (sa->conn_state == ARQ_CONN_DISCONNECTED ||
                 sa->conn_state == ARQ_CONN_LISTENING    ||
                 (sa->conn_state == ARQ_CONN_CONNECTED &&
                  (sa->dflow_state == ARQ_DFLOW_IDLE_ISS ||
                   sa->dflow_state == ARQ_DFLOW_IDLE_IRS)));

    bool b_ok = (sb->conn_state == ARQ_CONN_DISCONNECTED ||
                 sb->conn_state == ARQ_CONN_LISTENING    ||
                 (sb->conn_state == ARQ_CONN_CONNECTED &&
                  (sb->dflow_state == ARQ_DFLOW_IDLE_ISS ||
                   sb->dflow_state == ARQ_DFLOW_IDLE_IRS)));

    if (!a_ok || !b_ok) {
        snprintf(v.detail, sizeof(v.detail),
                 "not idle: A=%s/%s B=%s/%s in-flight=%d",
                 arq_conn_state_name(sa->conn_state),
                 arq_dflow_state_name(sa->dflow_state),
                 arq_conn_state_name(sb->conn_state),
                 arq_dflow_state_name(sb->dflow_state),
                 sim_frames_in_flight(s));
        return v;
    }

    v.ok = true;
    snprintf(v.detail, sizeof(v.detail), "both idle or disconnected");
    return v;
}

sim_verdict_t sim_prop_mode_floor_reached(sim_endpoint_t *ep, int floor_mode,
                                           int within_cycles)
{
    (void)within_cycles;
    sim_verdict_t v = { .ok = false };
    arq_session_t *sess = sim_endpoint_session(ep);

    if (sess->payload_mode == floor_mode) {
        v.ok = true;
        snprintf(v.detail, sizeof(v.detail),
                 "mode floor reached: payload_mode=%d", sess->payload_mode);
    } else {
        snprintf(v.detail, sizeof(v.detail),
                 "mode floor NOT reached: payload_mode=%d expected %d",
                 sess->payload_mode, floor_mode);
    }
    return v;
}
