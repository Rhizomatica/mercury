/* tests/sim/idle_drop.c -- how often an idle session ends on the 240 s
 * lost-peer watchdog (ARQ_CAR_PEER_LOST_MS): two real arq_fsm sessions on the
 * two-FSM sim connect, then stay idle while frames are lost.  A bench, not
 * part of `make test`; built by `make -C tests idle_drop`.
 *
 *   idle_drop <snr_db> <per> <seeds> <idle_s> [fade_hz]
 *
 * snr_db is the sim's SNR (sim_set_snr: each mode's threshold).  per is the
 * share of frames lost, set once both ends are CONNECTED; with fade_hz > 0,
 * Rayleigh fading around snr_db instead, and per is ignored.  A run counts as
 * ended when either end leaves CONNECTED.  It prints one line per ended run,
 * then the runs ended, their mean time, the caller's keydowns per run and the
 * collisions.  The sim sets no pattern callback, so the floor's continuation
 * slots are not modelled here: floor_idle.c measures the floor.
 *
 * Copyright (C) 2026 Joseph Freivald
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sim_clock.h"
#include "sim_channel.h"
#include "sim_endpoint.h"
#include "sim_core.h"

#include "arq_fsm.h"
#include "arq.h"
#include "freedv_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static arq_conn_state_t state(sim_endpoint_t *ep) { return sim_endpoint_session(ep)->conn_state; }

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: idle_drop <snr_db> <per> <seeds> <idle_s> [fade_hz]\n"); return 2; }
    double snr = atof(argv[1]), per = atof(argv[2]);
    int seeds = atoi(argv[3]);
    uint64_t idle_ms = (uint64_t)atoll(argv[4]) * 1000ULL;
    double fade_hz = argc > 5 ? atof(argv[5]) : 0.0;
    int dropped = 0, noconn = 0;
    double sum_drop_s = 0;
    long a_keys_total = 0; long coll_total = 0;
    for (int seed = 1; seed <= seeds; seed++) {
        sim_channel_cfg_t chan = { .seed = (uint64_t)seed * 7919u + 13u, .per = 0.0, .guard_ms = 100 };
        sim_t *s = sim_create(&chan, "A0AAA", "B0BBB");
        sim_set_half_duplex(s, true);
        sim_set_carrier_sense(s, true, 400);
        sim_set_snr(s, snr);
        arq_event_t listen = { .id = ARQ_EV_APP_LISTEN };
        sim_inject(s, sim_b(s), &listen);
        arq_event_t conn = { .id = ARQ_EV_APP_CONNECT };
        snprintf(conn.remote_call, CALLSIGN_MAX_SIZE, "%s", "B0BBB");
        sim_inject(s, sim_a(s), &conn);
        int i;
        for (i = 0; i < 6000 && !(state(sim_a(s)) == ARQ_CONN_CONNECTED &&
                                  state(sim_b(s)) == ARQ_CONN_CONNECTED); i++)
            sim_run_until_idle(s, 100);
        if (!(state(sim_a(s)) == ARQ_CONN_CONNECTED && state(sim_b(s)) == ARQ_CONN_CONNECTED)) {
            noconn++;
            sim_destroy(s);
            continue;
        }
        if (fade_hz > 0) sim_set_fading(s, snr, fade_hz);
        else sim_set_per(s, per);
        uint64_t t;
        int akeys = 0;
        bool was = sim_keyed(s, sim_a(s));
        for (t = 0; t < idle_ms; t += 100) {
            sim_run_until_idle(s, 100);
            bool k = sim_keyed(s, sim_a(s));
            if (k && !was) akeys++;
            was = k;
            if (state(sim_a(s)) != ARQ_CONN_CONNECTED || state(sim_b(s)) != ARQ_CONN_CONNECTED) break;
        }
        a_keys_total += akeys;
        if (t < idle_ms) {
            dropped++;
            sum_drop_s += t / 1000.0;
            printf("seed %d: dropped at %.1f s idle (A %s, B %s), A keydowns %d\n", seed, t / 1000.0,
                   state(sim_a(s)) == ARQ_CONN_CONNECTED ? "conn" : "ended",
                   state(sim_b(s)) == ARQ_CONN_CONNECTED ? "conn" : "ended", akeys);
        }
        coll_total += sim_collisions(s);
        sim_destroy(s);
    }
    int ran = seeds - noconn;
    printf("snr=%.1f per=%.2f fade_hz=%.2f idle=%llus: %d/%d dropped (no connect %d), mean drop time %.0f s, "
           "A keydowns/run %.1f\n",
           snr, per, fade_hz, (unsigned long long)(idle_ms / 1000), dropped, ran, noconn,
           dropped ? sum_drop_s / dropped : 0.0, ran ? (double)a_keys_total / ran : 0.0);
    printf("collisions total %ld\n", coll_total);
    return 0;
}
