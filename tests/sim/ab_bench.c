/* Deterministic ARQ throughput/integrity bench over a channel matrix.
 *
 *   ab_bench <seed> <channel>
 *   channel := clean | awgn:<per> | cliff:<snr_db> | nvis
 *
 * Connects two real arq_fsm sessions over the two-FSM sim, applies the named
 * channel to an 8 KB transfer, and prints delivered/total, integrity,
 * final mode and conn_state after 30 virtual minutes.  It links against the
 * arq_fsm.c of whatever tree it is compiled in, so an A/B comparison is:
 *
 *   gcc ... -I<treeA/...> ab_bench.c <sim srcs> <treeA arq_*.c> -o benchA
 *   gcc ... -I<treeB/...> ab_bench.c <sim srcs> <treeB arq_*.c> -o benchB
 *   for ch in clean awgn:0.25 cliff:-5 nvis; do for s in $(seq 1 16); do
 *     benchA $s $ch; benchB $s $ch; done; done   # compare delivered/integrity
 *
 * NB: the sim channel PRNG is a single shared stream, so once the two builds
 * make different mode/timing decisions they consume it differently and see
 * different erasure realizations.  Per-seed A/B is therefore only loosely
 * controlled; trust the AGGREGATE over many seeds, and always the integrity
 * flag (which must be OK on every run).  Used for the S1 fade-cliff merge
 * decision — see docs/S1-FADECLIFF-DECISION.md.
 */
#include "sim_clock.h"
#include "sim_channel.h"
#include "sim_endpoint.h"
#include "sim_core.h"
#include "arq_fsm.h"
#include "arq_protocol.h"
#include "freedv_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Measured NVIS (pathsim --midlat-dist-nvis --snr 10, freedv raw tools). */
static const sim_mode_per_t NVIS[] = {
    { FREEDV_MODE_DATAC15, 0.20 }, { FREEDV_MODE_DATAC16, 0.20 },
    { FREEDV_MODE_DATAC13, 0.30 }, { FREEDV_MODE_DATAC14, 0.30 },
    { FREEDV_MODE_DATAC4,  0.45 }, { FREEDV_MODE_DATAC3,  0.67 },
    { FREEDV_MODE_DATAC1,  0.89 }, { FREEDV_MODE_DATAC17, 0.93 },
    { FREEDV_MODE_QAM16C2, 0.95 },
};

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <seed> <channel>\n", argv[0]); return 1; }
    uint64_t seed = (uint64_t)atoll(argv[1]);
    const char *chan_spec = argv[2];

    sim_channel_cfg_t chan = { .seed = seed, .per = 0.02, .guard_ms = 150 };
    sim_t *s = sim_create(&chan, "A0AAA", "B0BBB");
    if (!s) { fprintf(stderr, "sim_create failed\n"); return 1; }

    arq_event_t listen = { .id = ARQ_EV_APP_LISTEN };
    sim_inject(s, sim_b(s), &listen);
    arq_event_t conn = { .id = ARQ_EV_APP_CONNECT };
    snprintf(conn.remote_call, CALLSIGN_MAX_SIZE, "%s", "B0BBB");
    sim_inject(s, sim_a(s), &conn);
    sim_run_until_idle(s, 30000);
    if (sim_endpoint_session(sim_a(s))->conn_state != ARQ_CONN_CONNECTED) {
        printf("seed=%llu chan=%s delivered=0/8192 integrity=OK final_mode=-1 conn=NOCONNECT\n",
               (unsigned long long)seed, chan_spec);
        sim_destroy(s); return 0;
    }

    /* Apply channel after connect (connect always on the clean 2% floor). */
    if (strncmp(chan_spec, "awgn:", 5) == 0)
        sim_set_per(s, atof(chan_spec + 5));
    else if (strncmp(chan_spec, "cliff:", 6) == 0)
        sim_set_snr(s, atof(chan_spec + 6));
    else if (strcmp(chan_spec, "nvis") == 0)
        sim_set_mode_per(s, NVIS, (int)(sizeof(NVIS)/sizeof(NVIS[0])), 10.0f);
    /* "clean" leaves the 2% floor. */

    static uint8_t blob[8192];
    for (int i = 0; i < (int)sizeof(blob); i++) blob[i] = (uint8_t)((i * 13 + 7) & 0xFF);
    sim_endpoint_queue_tx(sim_a(s), blob, sizeof(blob));
    arq_event_t dready = { .id = ARQ_EV_APP_DATA_READY };
    sim_inject(s, sim_a(s), &dready);

    sim_run_until_idle(s, 30 * 60 * 1000);

    static uint8_t got[65536];
    size_t n = sim_endpoint_delivered(sim_b(s), got, sizeof(got));
    int ok = (n <= sizeof(blob)) && memcmp(got, blob, n) == 0;
    arq_session_t *sa = sim_endpoint_session(sim_a(s));
    printf("seed=%llu chan=%s delivered=%zu/%zu integrity=%s final_mode=%d conn=%d\n",
           (unsigned long long)seed, chan_spec, n, sizeof(blob),
           ok ? "OK" : "CORRUPT", sa->payload_mode, (int)sa->conn_state);
    sim_destroy(s);
    return ok ? 0 : 2;
}
