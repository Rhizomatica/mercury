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
