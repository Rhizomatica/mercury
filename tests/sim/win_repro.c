/* win_repro.c — windowed-ARQ selective-repeat stall repro / state dump.
 * NOT a unit test: a manual harness that drives the two-FSM sim through a
 * mode-changing fade and dumps window state, for debugging the K>1 drain.
 *
 * Build (from tests/):
 *   SIM_SRC="sim/sim_core.c sim/sim_endpoint.c sim/sim_channel.c sim/sim_clock.c sim/sim_translate.c sim/sim_props.c"
 *   gcc -g -O0 -I../datalink_arq -I../modem/freedv -I../modem -I../data_interfaces -I../common -Isim \
 *       sim/win_repro.c $SIM_SRC datalink_arq/arq_test_stubs.c \
 *       ../datalink_arq/arq_fsm.c ../common/virtual_clock.c ../datalink_arq/arq_protocol.c \
 *       ../datalink_arq/arq_timing.c ../datalink_arq/arith.c \
 *       ../modem/freedv/libfreedvdata.a -lm -o /tmp/win_repro
 * Run:  /tmp/win_repro [seed] [xfer] [fade_snr]
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "arq_fsm.h"
#include "arq_protocol.h"
#include "sim_core.h"
#include "sim_props.h"

static void dumpwin(const char *who, arq_session_t *s)
{
    fprintf(stderr,
        "%s conn=%d dflow=%d tx_base=%u tx_seq=%u rx_expected=%u winbytes=%d "
        "txwin[", who, s->conn_state, s->dflow_state, s->tx_base, s->tx_seq,
        s->rx_expected, arq_win_bytes(s));
    for (int i = 0; i < ARQ_BURST_MAX; i++)
        fprintf(stderr, "%d", s->tx_win[i].present);
    fprintf(stderr, "] txseq[");
    for (int i = 0; i < ARQ_BURST_MAX; i++)
        fprintf(stderr, "%u,", s->tx_win[i].present ? s->tx_win[i].seq : 255);
    fprintf(stderr, "] rxwin[");
    for (int i = 0; i < ARQ_BURST_MAX; i++)
        fprintf(stderr, "%d", s->rx_win[i].present);
    fprintf(stderr, "] rxseq[");
    for (int i = 0; i < ARQ_BURST_MAX; i++)
        fprintf(stderr, "%u,", s->rx_win[i].present ? s->rx_win[i].seq : 255);
    fprintf(stderr, "] speed=%d rxspeed=%d payload=%d retries=%d\n",
        s->speed_level, s->rx_speed_level, s->payload_mode, s->tx_retries_left);
}

int main(int argc, char **argv)
{
    int    seed     = argc > 1 ? atoi(argv[1]) : 42;
    size_t xfer     = argc > 2 ? (size_t)atoi(argv[2]) : 20000;
    double fade_snr = argc > 3 ? atof(argv[3]) : -12.0;

    sim_channel_cfg_t cfg = { .seed = (uint64_t)seed, .guard_ms = 150 };
    sim_t *s = sim_create(&cfg, "A0AAA", "B0BBB");
    if (!s) { fprintf(stderr, "sim_create failed\n"); return 2; }

    arq_event_t listen = { .id = ARQ_EV_APP_LISTEN };
    sim_inject(s, sim_b(s), &listen);
    arq_event_t conn = { .id = ARQ_EV_APP_CONNECT };
    snprintf(conn.remote_call, CALLSIGN_MAX_SIZE, "B0BBB");
    sim_inject(s, sim_a(s), &conn);
    sim_run_until_idle(s, 60000);

    uint8_t *blob = malloc(xfer);
    for (size_t i = 0; i < xfer; i++) blob[i] = (uint8_t)((i + seed) & 0xFF);
    sim_endpoint_queue_tx(sim_a(s), blob, xfer);
    arq_event_t dr = { .id = ARQ_EV_APP_DATA_READY };
    sim_inject(s, sim_a(s), &dr);

    fprintf(stderr, "seed=%d xfer=%zu fade_snr=%.1f\n", seed, xfer, fade_snr);

    /* Phase 1: good band, climb + partial transfer. */
    sim_set_snr(s, 12.0);
    for (int k = 0; k < 4; k++) sim_run_until_idle(s, 45000);
    uint8_t got[65536]; size_t n = sim_endpoint_delivered(sim_b(s), got, sizeof(got));
    fprintf(stderr, "[after climb] delivered=%zu/%zu\n", n, xfer);
    dumpwin("A", sim_endpoint_session(sim_a(s)));
    dumpwin("B", sim_endpoint_session(sim_b(s)));

    /* Phase 2: fade. */
    sim_set_snr(s, fade_snr);
    for (int k = 0; k < 6; k++) sim_run_until_idle(s, 20000);
    n = sim_endpoint_delivered(sim_b(s), got, sizeof(got));
    fprintf(stderr, "[after fade] delivered=%zu/%zu\n", n, xfer);
    dumpwin("A", sim_endpoint_session(sim_a(s)));
    dumpwin("B", sim_endpoint_session(sim_b(s)));

    /* Phase 3: recover + drain, dumping periodically to catch a stall. */
    sim_set_snr(s, 12.0);
    size_t last_n = n;
    int    stalled = 0;
    for (int k = 0; k < 60; k++)
    {
        sim_run_until_idle(s, 60000);
        n = sim_endpoint_delivered(sim_b(s), got, sizeof(got));
        arq_session_t *A = sim_endpoint_session(sim_a(s));
        if (n == last_n)
        {
            if (++stalled == 3)
            {
                fprintf(stderr, "[STALL @k=%d] delivered=%zu/%zu conn=%d\n",
                        k, n, xfer, A->conn_state);
                dumpwin("A", A);
                dumpwin("B", sim_endpoint_session(sim_b(s)));
            }
        }
        else { stalled = 0; last_n = n; }
        if (n >= xfer) { fprintf(stderr, "[COMPLETE @k=%d]\n", k); break; }
    }

    n = sim_endpoint_delivered(sim_b(s), got, sizeof(got));
    /* integrity of the in-order prefix */
    size_t good = 0;
    while (good < n && good < xfer && got[good] == blob[good]) good++;
    fprintf(stderr, "FINAL delivered=%zu/%zu, in-order-correct-prefix=%zu\n",
            n, xfer, good);
    free(blob);
    sim_destroy(s);
    return (n >= xfer) ? 0 : 1;
}
