/* tests/sim/connect_bench.c
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Rhizomatica
 *
 * Connect-latency instrument.
 *
 * A connect costs three DATAC16 bursts — CALL, ACCEPT, and a confirming ACK
 * that carries about four useful bytes — so most of the time to connect is
 * spent modulating, not deciding. Nothing measured that: the suite only
 * asserted that a connect eventually happened. This reports, in virtual time
 * so the numbers are deterministic:
 *
 *   caller   ms until the CALLER sees CONNECTED (it needs 2 frames)
 *   callee   ms until the CALLEE sees CONNECTED (it needs the 3rd)
 *   frames   transmissions by each side, patterns included
 *
 * Swept over per-frame erasure probability, several seeds per point, so a
 * change to the handshake can be judged on expected time-to-connect rather
 * than on a single lucky run.
 *
 *   cd tests && make connect_bench && ./connect_bench
 */
#include "sim_core.h"
#include "sim_endpoint.h"
#include "sim_channel.h"
#include "sim_clock.h"
#include "arq_fsm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONNECT_CAP_MS  180000u   /* virtual ceiling: 5 CALLs at 8 s + slack */

typedef struct {
    int      ok;            /* both ends reached CONNECTED */
    uint64_t caller_ms;
    uint64_t callee_ms;
    unsigned frames_a, frames_b;
} run_t;

static run_t one_connect(double per, uint64_t seed)
{
    run_t r; memset(&r, 0, sizeof r);
    sim_channel_cfg_t chan = { .seed = seed, .per = per, .guard_ms = 100 };
    sim_t *s = sim_create(&chan, "A0AAA", "B0BBB");
    if (!s) return r;

    uint64_t t0 = sim_clock_now();

    /* B must be listening before A calls, or the CALL lands on a deaf station. */
    arq_event_t listen = { .id = ARQ_EV_APP_LISTEN };
    sim_inject(s, sim_b(s), &listen);

    sim_endpoint_reset_stats(sim_a(s));
    sim_endpoint_reset_stats(sim_b(s));

    arq_event_t conn = { .id = ARQ_EV_APP_CONNECT };
    snprintf(conn.remote_call, sizeof conn.remote_call, "%s", "B0BBB");
    sim_inject(s, sim_a(s), &conn);

    sim_run_until_idle(s, CONNECT_CAP_MS);

    uint64_t ca = sim_endpoint_connected_at_ms(sim_a(s));
    uint64_t cb = sim_endpoint_connected_at_ms(sim_b(s));
    r.ok        = (ca != 0 && cb != 0);
    r.caller_ms = ca ? ca - t0 : 0;
    r.callee_ms = cb ? cb - t0 : 0;
    r.frames_a  = sim_endpoint_tx_frames(sim_a(s));
    r.frames_b  = sim_endpoint_tx_frames(sim_b(s));
    sim_destroy(s);
    return r;
}

int main(int argc, char **argv)
{
    int trials = (argc > 1) ? atoi(argv[1]) : 8;

    printf("Connect latency (virtual time, %d seeds per point)\n\n", trials);
    printf("%8s %9s %12s %12s %10s %12s\n",
           "PER", "connects", "caller(ms)", "callee(ms)", "frames", "censored");

    const double pers[] = { 0.0, 0.1, 0.2, 0.3, 0.5 };
    for (unsigned p = 0; p < sizeof pers / sizeof pers[0]; p++)
    {
        unsigned ok = 0, fr = 0;
        uint64_t sum_caller = 0, sum_callee = 0, sum_censored = 0;
        for (int t = 0; t < trials; t++)
        {
            run_t r = one_connect(pers[p], 1000u + (uint64_t)t * 7919u);
            /* Censored mean: a failure counts as the cap, so improving the
             * success rate cannot masquerade as a latency regression. Means
             * over successes ALONE are survivor-biased — when a change makes
             * previously-hopeless connects succeed, those slow successes pull
             * the success-only mean up while the link actually got better. */
            sum_censored += r.ok ? r.callee_ms : CONNECT_CAP_MS;
            if (!r.ok) continue;
            ok++;
            sum_caller += r.caller_ms;
            sum_callee += r.callee_ms;
            fr         += r.frames_a + r.frames_b;
        }
        if (ok)
            printf("%8.2f %6u/%-3d %12.0f %12.0f %10.1f %12.0f\n",
                   pers[p], ok, trials,
                   (double)sum_caller / ok, (double)sum_callee / ok,
                   (double)fr / ok, (double)sum_censored / trials);
        else
            printf("%8.2f %6u/%-3d %12s %12s %10s %12.0f\n",
                   pers[p], ok, trials, "-", "-", "-",
                   (double)sum_censored / trials);
        fflush(stdout);
    }
    return 0;
}
