/* How a session of the two-FSM sim ends, as a trace of the state that
 * specs/teardown/SessionTeardown.tla models: trace validation.
 *
 *   teardown_trace <seed> [per] [who] [sessions]
 *
 * B listens, A calls; once both are connected each queues a few bytes, and
 * after a while `who` hangs up: "M" (A's host DISCONNECT), "S" (B's), "both",
 * "mabort" or "sabort" (the host's ABORT), random from the seed when not
 * given; every frame is lost with probability `per`.  With `sessions` 2, A
 * calls again once the first session is over.  After every FSM dispatch it
 * reads both sessions and prints, as a TLA+ module (TraceData), each change
 * of what the spec sees of an end e (M = A, S = B):
 *
 *   st     "idle", "calling", "conn" (B: ACCEPTING, its ACCEPT fallback --
 *          LISTENING with accept_fallback: the connect, which this spec
 *          takes in one step, is specs/connect's -- or CONNECTED), "ending"
 *          (DISCONNECTING) or "reply" (DISCONNECTED, the reply to the peer's
 *          DISCONNECT still to key: ARQ_TD_REPLYING)
 *   sid    its session id, numbered in order of appearance (0: none yet)
 *   sent   its DISCONNECT is out (ARQ_TD_SENT), while ending (-1: unseen;
 *          the code resets it as the session ends, the spec keeps it)
 *   mdrain A, while connected: its host's DISCONNECT held for the drain
 *          (ARQ_TD_DRAINING) (-1: unseen)
 *
 * The rest -- the frames on their way, the last words, the host's notices
 * -- is left to TLC.  specs/teardown/trace_check.sh runs both.  It stops once
 * both ends are idle with the sessions done, or at 30 min of virtual time.
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
#define MAX_ROWS  8192
#define MAX_IDS   32

typedef struct {
    const char *st[2];
    int sid[2], sent[2], mdrain;   /* sent, mdrain: 0, 1, -1 unseen */
} row_t;

static row_t rows[MAX_ROWS];
static int   nrows;
static int   sids[MAX_IDS], nsids;
static sim_endpoint_t *A, *B;

static int number(int raw)
{
    for (int i = 0; i < nsids; i++)
        if (sids[i] == raw) return i + 1;
    if (nsids == MAX_IDS) { fprintf(stderr, "too many ids\n"); exit(2); }
    sids[nsids++] = raw;
    return nsids;
}

static const char *state(const arq_session_t *s)
{
    switch (s->conn_state) {
    case ARQ_CONN_CALLING:       return "calling";
    case ARQ_CONN_ACCEPTING:
    case ARQ_CONN_CONNECTED:     return "conn";
    case ARQ_CONN_DISCONNECTING: return "ending";
    case ARQ_CONN_LISTENING:     if (s->accept_fallback) return "conn";
                                 /* fall through */
    default:                     return s->teardown == ARQ_TD_REPLYING ? "reply" : "idle";
    }
}

static void record(sim_t *s, void *ctx)
{
    (void)s; (void)ctx;
    const arq_session_t *e[2] = { sim_endpoint_session(A), sim_endpoint_session(B) };
    row_t r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < 2; i++) {
        r.st[i]   = state(e[i]);
        r.sid[i]  = e[i]->session_id ? number(e[i]->session_id) : 0;
        r.sent[i] = e[i]->conn_state == ARQ_CONN_DISCONNECTING ? e[i]->teardown == ARQ_TD_SENT : -1;
    }
    r.mdrain = e[0]->conn_state == ARQ_CONN_CONNECTED ? e[0]->teardown == ARQ_TD_DRAINING : -1;
    if (nrows && !memcmp(&rows[nrows - 1], &r, sizeof(r)))
        return;
    if (nrows == MAX_ROWS) { fprintf(stderr, "trace too long\n"); exit(2); }
    rows[nrows++] = r;
}

static bool idle(sim_endpoint_t *ep)
{
    arq_conn_state_t st = sim_endpoint_session(ep)->conn_state;
    return (st == ARQ_CONN_DISCONNECTED || st == ARQ_CONN_LISTENING) &&
           sim_endpoint_session(ep)->teardown == ARQ_TD_NONE;
}

static void inject(sim_t *s, sim_endpoint_t *ep, arq_event_id_t id)
{
    arq_event_t ev = { .id = id };
    sim_inject(s, ep, &ev);
}

/* The host's ABORT: a DISCONNECT that drops what is held (arq.c) */
static void abort_session(sim_t *s, sim_endpoint_t *ep)
{
    arq_event_t ev = { .id = ARQ_EV_APP_DISCONNECT, .abort = true };
    sim_inject(s, ep, &ev);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <seed> [per] [who] [sessions]\n", argv[0]); return 1; }
    static const char *WHO[] = { "M", "S", "both", "mabort", "sabort" };
    uint64_t seed = (uint64_t)atoll(argv[1]);
    srand((unsigned)seed);
    double per       = argc > 2 ? atof(argv[2]) : (rand() % 40) / 100.0;
    const char *who  = argc > 3 ? argv[3] : WHO[rand() % 5];
    int sessions     = argc > 4 ? atoi(argv[4]) : 1 + rand() % 2;
    int bytes_a      = rand() % 300, bytes_b = rand() % 300;
    uint64_t hang_ms = 5000 + (uint64_t)(rand() % 60) * 1000;

    sim_channel_cfg_t cfg = { .seed = seed, .per = per };
    sim_t *s = sim_create(&cfg, "A0AAA", "B0BBB");
    if (!s) return 2;
    A = sim_a(s); B = sim_b(s);
    sim_set_dispatch_hook(s, record, NULL);
    inject(s, B, ARQ_EV_APP_LISTEN);

    int placed = 0;
    bool hung = false;
    uint64_t up_at = 0;
    static uint8_t data[300];
    for (int i = 0; i < (int)sizeof(data); i++) data[i] = (uint8_t)(i * 7 + seed);
    for (uint64_t t = 0; t < LIMIT_MS; t += STEP_MS) {
        arq_conn_state_t sa = sim_endpoint_session(A)->conn_state;
        if (placed < sessions && idle(A) && (placed == 0 || hung)) {
            arq_event_t conn = { .id = ARQ_EV_APP_CONNECT };
            snprintf(conn.remote_call, CALLSIGN_MAX_SIZE, "%s", "B0BBB");
            sim_inject(s, A, &conn);
            placed++;
            hung = false;
            up_at = 0;
        }
        if (!up_at && sa == ARQ_CONN_CONNECTED &&
            sim_endpoint_session(B)->conn_state == ARQ_CONN_CONNECTED) {
            up_at = t;
            if (bytes_a) { sim_endpoint_queue_tx(A, data, (size_t)bytes_a); inject(s, A, ARQ_EV_APP_DATA_READY); }
            if (bytes_b) { sim_endpoint_queue_tx(B, data, (size_t)bytes_b); inject(s, B, ARQ_EV_APP_DATA_READY); }
        }
        if (up_at && !hung && t - up_at >= hang_ms) {
            hung = true;
            if (!strcmp(who, "M") || !strcmp(who, "both")) inject(s, A, ARQ_EV_APP_DISCONNECT);
            if (!strcmp(who, "S") || !strcmp(who, "both")) inject(s, B, ARQ_EV_APP_DISCONNECT);
            if (!strcmp(who, "mabort")) abort_session(s, A);
            if (!strcmp(who, "sabort")) abort_session(s, B);
        }
        /* a CONNECT that gave up, or a session that ended by itself, counts */
        if (placed && !up_at && idle(A) && sa != ARQ_CONN_CALLING)
            hung = true;
        if (placed == sessions && hung && idle(A) && idle(B))
            break;
        sim_run_until_idle(s, STEP_MS);
    }

    printf("---- MODULE TraceData ----\nEXTENDS Integers\n");
    printf("\\* teardown_trace %llu %.2f %s %d (bytes %d/%d, hang-up after %llu s)\n",
           (unsigned long long)seed, per, who, sessions, bytes_a, bytes_b,
           (unsigned long long)(hang_ms / 1000));
    printf("TNS == %d\nTSIDS == 1 .. %d\nTLOSS == %s\nTABORT == %s\n", sessions,
           nsids ? nsids : 1, per > 0 ? "TRUE" : "FALSE", strstr(who, "abort") ? "TRUE" : "FALSE");
    printf("Trace == <<\n");
    for (int i = 0; i < nrows; i++)
        printf("  [st |-> [M |-> \"%s\", S |-> \"%s\"], sid |-> [M |-> %d, S |-> %d], "
               "sent |-> [M |-> %d, S |-> %d], mdrain |-> %d]%s\n",
               rows[i].st[0], rows[i].st[1], rows[i].sid[0], rows[i].sid[1],
               rows[i].sent[0], rows[i].sent[1], rows[i].mdrain, i + 1 < nrows ? "," : "");
    printf(">>\n====\n");
    sim_destroy(s);
    return 0;
}
