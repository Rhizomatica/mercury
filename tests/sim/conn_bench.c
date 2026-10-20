/* Connect and disconnect times over the two-FSM sim (2.0 plan, P2).
 *
 *   conn_bench <seed> <channel> [callee]
 *   channel := cliff:<snr_db> | fade:<snr_db>[:<doppler_hz>] | nvis
 *
 * Unlike ab_bench the channel applies from the first CALL: the connect is
 * what is measured.  B listens, A calls.  B's application writes a 64-byte
 * banner the moment B is connected, as UUCP's and a Winlink RMS's do (the
 * callee speaks first); once A has it A sends 1 KB, B's application answers
 * it with 16 bytes (a file's acknowledgement), and once A has those A hangs
 * up -- or, with "callee", A answers with 8 bytes and B hangs up once it has
 * them: an application hangs up after its peer's last word, by when its own
 * are acknowledged.  Prints, in seconds of virtual time:
 *   connect=   A's CONNECT to A connected (-1: not in 10 min)
 *   connect_b= ...to B connected
 *   banner=    ...to B's banner delivered to A: what a user waits for
 *   disc=      the hang-up to both ends idle (-1: not in 10 min)
 *   disc_x=    ...to the end that hung up idle
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

/* As ab_bench: measured NVIS (pathsim --midlat-dist-nvis --snr 10). */
static const sim_mode_per_t NVIS[] = {
    { FREEDV_MODE_DATAC15, 0.20 }, { FREEDV_MODE_DATAC16, 0.20 },
    { FREEDV_MODE_DATAC13, 0.30 }, { FREEDV_MODE_DATAC14, 0.30 },
    { FREEDV_MODE_DATAC4,  0.45 }, { FREEDV_MODE_DATAC3,  0.67 },
    { FREEDV_MODE_DATAC1,  0.89 }, { FREEDV_MODE_DATAC17, 0.93 },
    { FREEDV_MODE_QAM16C2, 0.95 },
};

#define LIMIT_MS (10 * 60 * 1000ULL)

static arq_conn_state_t st(sim_endpoint_t *ep) { return sim_endpoint_session(ep)->conn_state; }
static bool idle(sim_endpoint_t *ep)
{
    return st(ep) == ARQ_CONN_DISCONNECTED || st(ep) == ARQ_CONN_LISTENING;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <seed> <channel> [callee]\n", argv[0]); return 1; }
    uint64_t seed = (uint64_t)atoll(argv[1]);
    const char *ch = argv[2];
    bool callee_ends = argc > 3 && !strcmp(argv[3], "callee");

    sim_channel_cfg_t cfg = { .seed = seed, .per = 0.0, .guard_ms = 150 };
    sim_t *s = sim_create(&cfg, "A0AAA", "B0BBB");
    if (!s) return 1;
    if (!strncmp(ch, "cliff:", 6)) {
        sim_set_rx_snr(s, (float)atof(ch + 6));
        sim_set_snr(s, atof(ch + 6));
    } else if (!strncmp(ch, "fade:", 5)) {
        double m = 0, d = 0.5;
        sscanf(ch + 5, "%lf:%lf", &m, &d);
        sim_set_rx_snr(s, (float)m);
        sim_set_fading(s, m, d);
    } else if (!strcmp(ch, "nvis")) {
        sim_set_mode_per(s, NVIS, (int)(sizeof(NVIS) / sizeof(NVIS[0])), 10.0f);
    } else {
        fprintf(stderr, "unknown channel %s\n", ch); return 1;
    }

    arq_event_t listen = { .id = ARQ_EV_APP_LISTEN };
    sim_inject(s, sim_b(s), &listen);
    arq_event_t conn = { .id = ARQ_EV_APP_CONNECT };
    snprintf(conn.remote_call, CALLSIGN_MAX_SIZE, "%s", "B0BBB");
    uint64_t t0 = sim_clock_now();
    sim_inject(s, sim_a(s), &conn);

    static uint8_t banner[64], blob[1024], reply[16], bye[8], rx[2048];
    memset(banner, 'S', sizeof(banner));
    arq_event_t ready = { .id = ARQ_EV_APP_DATA_READY };
    uint64_t ta = 0, tb = 0, tbn = 0;
    while (sim_clock_now() - t0 < LIMIT_MS && !(ta && tb && tbn)) {
        if (sim_run_until_idle(s, 100) == 0) break;
        if (!ta && st(sim_a(s)) == ARQ_CONN_CONNECTED) ta = sim_clock_now();
        if (!tb && st(sim_b(s)) == ARQ_CONN_CONNECTED) {
            tb = sim_clock_now();
            sim_endpoint_queue_tx(sim_b(s), banner, sizeof(banner));
            sim_inject(s, sim_b(s), &ready);
        }
        if (tb && !tbn && sim_endpoint_delivered(sim_a(s), rx, sizeof(rx)) >= sizeof(banner)) tbn = sim_clock_now();
    }
    double c_a = ta ? (ta - t0) / 1000.0 : -1, c_b = tb ? (tb - t0) / 1000.0 : -1,
           c_bn = tbn ? (tbn - t0) / 1000.0 : -1;
    if (!(ta && tb && tbn)) {
        printf("seed=%llu chan=%s connect=%.1f connect_b=%.1f banner=%.1f disc=-1 disc_x=-1\n",
               (unsigned long long)seed, ch, c_a, c_b, c_bn);
        sim_destroy(s); return 0;
    }

    for (int i = 0; i < (int)sizeof(blob); i++) blob[i] = (uint8_t)(i * 7 + 1);
    sim_endpoint_queue_tx(sim_a(s), blob, sizeof(blob));
    sim_inject(s, sim_a(s), &ready);
    uint64_t t1 = sim_clock_now();
    while (sim_clock_now() - t1 < LIMIT_MS && sim_endpoint_delivered(sim_b(s), rx, sizeof(rx)) < sizeof(blob))
        if (sim_run_until_idle(s, 100) == 0) break;     /* nothing more will happen */
    memset(reply, 'K', sizeof(reply));
    sim_endpoint_queue_tx(sim_b(s), reply, sizeof(reply));
    sim_inject(s, sim_b(s), &ready);
    while (sim_clock_now() - t1 < LIMIT_MS &&
           sim_endpoint_delivered(sim_a(s), rx, sizeof(rx)) < sizeof(banner) + sizeof(reply))
        if (sim_run_until_idle(s, 100) == 0) break;
    if (callee_ends) {
        memset(bye, 'B', sizeof(bye));
        sim_endpoint_queue_tx(sim_a(s), bye, sizeof(bye));
        sim_inject(s, sim_a(s), &ready);
        while (sim_clock_now() - t1 < LIMIT_MS &&
               sim_endpoint_delivered(sim_b(s), rx, sizeof(rx)) < sizeof(blob) + sizeof(bye))
            if (sim_run_until_idle(s, 100) == 0) break;
    }

    sim_endpoint_t *x = callee_ends ? sim_b(s) : sim_a(s);
    arq_event_t disc = { .id = ARQ_EV_APP_DISCONNECT };
    uint64_t t2 = sim_clock_now();
    sim_inject(s, x, &disc);
    uint64_t tx = 0, tboth = 0;
    while (sim_clock_now() - t2 < LIMIT_MS && !tboth) {
        bool moved = sim_run_until_idle(s, 100) != 0;
        if (!tx && idle(x)) tx = sim_clock_now();
        if (idle(sim_a(s)) && idle(sim_b(s))) tboth = sim_clock_now();
        if (!moved) break;
    }
    printf("seed=%llu chan=%s connect=%.1f connect_b=%.1f banner=%.1f disc=%.1f disc_x=%.1f\n",
           (unsigned long long)seed, ch, c_a, c_b, c_bn,
           tboth ? (tboth - t2) / 1000.0 : -1.0, tx ? (tx - t2) / 1000.0 : -1.0);
    sim_destroy(s);
    return 0;
}
