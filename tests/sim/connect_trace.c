/* The connect handshake of the two-FSM sim, as a trace of the state that
 * specs/connect/ConnectHandshake.tla models: trace validation.
 *
 *   connect_trace <seed> [per] [connects] [calls]
 *
 * B listens, A calls (CONNECT `connects` times, 1 or 2: again once a CONNECT
 * gave up), with `calls` CALLs a CONNECT (ARQ_CALL_RETRY_SLOTS) and every
 * frame lost with probability `per` (random from the seed when not given).
 * After every FSM dispatch it reads both sessions, and prints, as a TLA+
 * module (TraceData), each change of what the spec sees:
 *
 *   cSt   A: "calling", "connected" or "idle"
 *   cSid  A's session id, numbered in order of appearance (0: none yet)
 *   cNonce  the nonce A took from its ACCEPT, while connected (-1: unseen)
 *   eSt   B: "listening", "accepting", "fallback" (listening with its seed
 *         kept: accept_fallback) or "connected"
 *   eSid, eNonce  B's, while it holds a session (-1: unseen)
 *   cLevel  the rung A's first round goes on (car_tx_level), while connected
 *   eLevel  the rung B's ACCEPT names (accept_level), while it holds one
 *           (both rung + 1: the spec's 0 is "none"; -1: unseen)
 *
 * Each seed also picks the SNR (-2..20 dB: the start rungs follow it) and
 * whether two stations keyed at once destroy each other's frames.
 *
 * The rest of the spec's state (CALLs left, the ACCEPTs in the air, levels,
 * bandwidths, encryption) is not read: TLC finds values for it, or finds
 * that there are none.  specs/connect/TraceHandshake.tla checks the trace;
 * specs/connect/trace_check.sh runs both.  It stops once both are connected,
 * or both idle with the CONNECTs done, or at 30 min of virtual time.
 */
#include "sim_clock.h"
#include "sim_channel.h"
#include "sim_endpoint.h"
#include "sim_core.h"
#include "arq_fsm.h"
#include "arq_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT_MS  (30 * 60 * 1000ULL)
#define STEP_MS   250
#define MAX_ROWS  4096
#define MAX_IDS   32

typedef struct {
    const char *c_st, *e_st;
    int c_sid, c_nonce, e_sid, e_nonce, c_level, e_level;
} row_t;

static row_t rows[MAX_ROWS];
static int   nrows;
static int   sids[MAX_IDS], nsids;       /* raw session ids, in order seen */
static int   nonces[MAX_IDS], nnonces;   /* raw nonces, in order seen */
static sim_endpoint_t *A, *B;

static int number(int *tab, int *n, int raw)
{
    for (int i = 0; i < *n; i++)
        if (tab[i] == raw) return i + 1;
    if (*n == MAX_IDS) { fprintf(stderr, "too many ids\n"); exit(2); }
    tab[(*n)++] = raw;
    return *n;
}

static const char *c_state(const arq_session_t *s)
{
    return s->conn_state == ARQ_CONN_CALLING   ? "calling"
         : s->conn_state == ARQ_CONN_CONNECTED ? "connected" : "idle";
}

static const char *e_state(const arq_session_t *s)
{
    switch (s->conn_state) {
    case ARQ_CONN_LISTENING: return s->accept_fallback ? "fallback" : "listening";
    case ARQ_CONN_ACCEPTING: return "accepting";
    case ARQ_CONN_CONNECTED: return "connected";
    default:                 return "other";
    }
}

static void record(sim_t *s, void *ctx)
{
    (void)s; (void)ctx;
    const arq_session_t *a = sim_endpoint_session(A), *b = sim_endpoint_session(B);
    row_t r = { .c_st = c_state(a), .e_st = e_state(b), .c_nonce = -1, .e_sid = -1, .e_nonce = -1,
                .c_level = -1, .e_level = -1 };
    r.c_sid = a->session_id ? number(sids, &nsids, a->session_id) : 0;
    if (!strcmp(r.c_st, "connected")) {
        r.c_nonce = number(nonces, &nnonces, a->car_nonce);
        r.c_level = a->car_tx_level + 1;
    }
    if (strcmp(r.e_st, "listening") && strcmp(r.e_st, "other")) {
        r.e_sid = number(sids, &nsids, b->session_id);
        r.e_nonce = number(nonces, &nnonces, b->car_nonce);
        if (strcmp(r.e_st, "connected"))   /* connected, the carousel moves it */
            r.e_level = (b->car_ctl_deaf ? 0 : b->car_rx_level) + 1;
    }
    if (nrows && !memcmp(&rows[nrows - 1], &r, sizeof(r)))
        return;
    if (nrows == MAX_ROWS) { fprintf(stderr, "trace too long\n"); exit(2); }
    rows[nrows++] = r;
}

static bool both(const char *c, const char *e)
{
    return nrows && !strcmp(rows[nrows - 1].c_st, c) && !strcmp(rows[nrows - 1].e_st, e);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <seed> [per] [connects] [calls]\n", argv[0]); return 1; }
    uint64_t seed = (uint64_t)atoll(argv[1]);
    srand((unsigned)seed);
    double per   = argc > 2 ? atof(argv[2]) : (rand() % 70) / 100.0;
    int connects = argc > 3 ? atoi(argv[3]) : 1 + rand() % 2;
    int calls    = argc > 4 ? atoi(argv[4]) : 2 + rand() % 3;
    double snr   = -2 + rand() % 23;
    bool hd      = rand() % 2;
    atomic_store(&arq_call_retry_slots, calls);

    sim_channel_cfg_t cfg = { .seed = seed, .per = per };
    sim_t *s = sim_create(&cfg, "A0AAA", "B0BBB");
    if (!s) return 2;
    A = sim_a(s); B = sim_b(s);
    sim_set_snr(s, snr);
    sim_set_half_duplex(s, hd);
    sim_set_dispatch_hook(s, record, NULL);

    arq_event_t listen = { .id = ARQ_EV_APP_LISTEN };
    sim_inject(s, B, &listen);
    int placed = 0;
    for (uint64_t t = 0; t < LIMIT_MS; t += STEP_MS) {
        if (both("connected", "connected"))
            break;
        if (placed < connects && sim_endpoint_session(A)->conn_state != ARQ_CONN_CALLING &&
            sim_endpoint_session(A)->conn_state != ARQ_CONN_CONNECTED) {
            arq_event_t conn = { .id = ARQ_EV_APP_CONNECT };
            snprintf(conn.remote_call, CALLSIGN_MAX_SIZE, "%s", "B0BBB");
            sim_inject(s, A, &conn);
            placed++;
        }
        if (placed == connects && both("idle", "listening"))
            break;
        sim_run_until_idle(s, STEP_MS);
    }

    printf("---- MODULE TraceData ----\nEXTENDS Integers\n");
    printf("\\* connect_trace %llu %.2f %d %d (snr %.0f dB%s)\n", (unsigned long long)seed, per,
           connects, calls, snr, hd ? ", half duplex" : "");
    printf("TSids == 1 .. %d\nTNonces == 1 .. %d\nTLevels == 1 .. 7\nTR == %d\nTS == %d\n",
           nsids ? nsids : 1, nnonces ? nnonces : 1, calls, connects);
    printf("Trace == <<\n");
    for (int i = 0; i < nrows; i++)
        printf("  [cSt |-> \"%s\", cSid |-> %d, cNonce |-> %d, cLevel |-> %d, "
               "eSt |-> \"%s\", eSid |-> %d, eNonce |-> %d, eLevel |-> %d]%s\n",
               rows[i].c_st, rows[i].c_sid, rows[i].c_nonce, rows[i].c_level,
               rows[i].e_st, rows[i].e_sid, rows[i].e_nonce, rows[i].e_level,
               i + 1 < nrows ? "," : "");
    printf(">>\n====\n");
    sim_destroy(s);
    return 0;
}
