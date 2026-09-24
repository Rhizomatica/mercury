/* tests/sim/carousel_sim.c -- two carousel data planes (datalink_arq/carousel.c)
 * on the sim's channel; carousel_bench and test_carousel run it.
 *
 * Two carousel instances, each with what Mercury's runtime gives it: a
 * half-duplex medium, a control decoder that hears every DATAC16 frame and one
 * payload decoder that hears only the mode it is bound to, and carrier sense
 * (a decoder in sync from CS_ACQ_MS after the peer keys until CS_ACQ_MS after
 * its last frame ends).  Channel, airtime and guards are the sim's
 * (sim_channel.c and the ARQ mode table), as in ab_bench with SIM_CS=400.
 *
 * The session is connected when the run starts, as in ab_bench (the connect is
 * not measured): the callee's ACCEPT was the first poll.
 *
 *   CAR_TRACE=1  print every keydown
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "carousel_sim.h"
#include "sim_channel.h"
#include "carousel.h"
#include "arq_protocol.h"
#include "freedv_api.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAD_MS         110       /* tx delay + head silence (as carousel.c) */
#define TAIL_MS         200
#define CS_ACQ_MS       400       /* carrier sense: sync this long after keying */

/* ---- events ---------------------------------------------------------------- */
enum { EV_ARRIVE, EV_TXEND };
typedef struct {
    uint64_t t;
    int      type, st;             /* st = station the event is for */
    int      mode;
    size_t   len;
    uint8_t *bytes;
    uint64_t f_start, f_end;       /* EV_ARRIVE: the frame's air interval */
} event_t;

#define MAXEV 4096
static event_t evq[MAXEV];
static int     nev;

static void push(event_t e)
{
    if (nev >= MAXEV) { fprintf(stderr, "event queue full\n"); exit(3); }
    evq[nev++] = e;
}
static int pop(event_t *e)
{
    if (!nev) return 0;
    int b = 0;
    for (int i = 1; i < nev; i++) if (evq[i].t < evq[b].t) b = i;
    *e = evq[b];
    evq[b] = evq[--nev];
    return 1;
}
static uint64_t next_event_time(void)
{
    uint64_t t = UINT64_MAX;
    for (int i = 0; i < nev; i++) if (evq[i].t < t) t = evq[i].t;
    return t;
}

/* ---- stations ------------------------------------------------------------- */
typedef struct {
    int      id;
    car_t    car;
    uint8_t  tx[CAR_SIM_BYTES];  size_t tx_len, tx_read;
    uint8_t  rx[CAR_SIM_BYTES + 64]; size_t rx_len;
    int      rx_mode;              /* the payload decoder's binding */
    uint64_t tx_start, tx_end;     /* current/last keydown */
} station_t;

static station_t S[2];
static sim_channel_t *ch;
static int collisions;
static uint64_t now_ms;
static bool trace;
static double snr_now = 12.0;          /* the SNR stamped on delivered frames */

static void io_keydown(void *ctx, const car_frame_t *fr, int n)
{
    station_t *s = ctx, *peer = &S[s->id ^ 1];
    uint64_t t = now_ms + HEAD_MS;
    s->tx_start = now_ms;
    if (trace) printf("%9.1f %c keys:", now_ms / 1000.0, 'A' + s->id);
    for (int i = 0; i < n; i++) {
        if (i) t += fr[i].gap_ms;
        uint64_t air = sim_channel_airtime_ms(fr[i].mode, 0), d;
        if (trace) {
            static const char *K[] = { "poll", "handover", "status", "?" };
            if (fr[i].mode == ARQ_CONTROL_MODE) printf(" %s", K[fr[i].bytes[0] >> 6]);
            else printf(" data(%d)", fr[i].mode);
        }
        if (sim_channel_schedule(ch, t, s->id, fr[i].mode, 0, &d)) {
            uint8_t *b = malloc(fr[i].len);
            memcpy(b, fr[i].bytes, fr[i].len);
            event_t e = { .t = t + air, .type = EV_ARRIVE, .st = peer->id, .mode = fr[i].mode,
                          .len = fr[i].len, .bytes = b, .f_start = t, .f_end = t + air };
            push(e);
        }
        t += air;
    }
    s->tx_end = t + TAIL_MS;
    if (trace) printf("  until %.1f\n", s->tx_end / 1000.0);
    event_t e = { .t = s->tx_end, .type = EV_TXEND, .st = s->id };
    push(e);
}

static void io_bind_rx(void *ctx, int mode) { ((station_t *)ctx)->rx_mode = mode; }

static bool io_peer_keyed(void *ctx)
{
    const station_t *p = &S[((station_t *)ctx)->id ^ 1];
    return p->tx_end && p->tx_start + CS_ACQ_MS <= now_ms && now_ms < p->tx_end - TAIL_MS + CS_ACQ_MS;
}

static size_t io_tx_read(void *ctx, uint8_t *buf, size_t max)
{
    station_t *s = ctx;
    size_t n = s->tx_len - s->tx_read;
    if (n > max) n = max;
    memcpy(buf, s->tx + s->tx_read, n);
    s->tx_read += n;
    return n;
}
static size_t io_tx_pending(void *ctx) { station_t *s = ctx; return s->tx_len - s->tx_read; }

static void io_deliver(void *ctx, const uint8_t *buf, size_t len)
{
    station_t *s = ctx;
    if (trace) printf("%9.1f %c delivered %zu (total %zu)\n", now_ms / 1000.0, 'A' + s->id, len, s->rx_len + len);
    if (s->rx_len + len > sizeof(s->rx)) len = sizeof(s->rx) - s->rx_len;
    memcpy(s->rx + s->rx_len, buf, len);
    s->rx_len += len;
}

/* ---- run ------------------------------------------------------------------ */
void carousel_sim_run(uint64_t seed, const char *chan, bool bidir, uint64_t limit_ms, car_sim_result_t *res)
{
    trace = getenv("CAR_TRACE") != NULL;
    nev = 0;
    collisions = 0;
    memset(S, 0, sizeof(S));

    sim_channel_cfg_t cfg = { .seed = seed, .per = 0.02, .guard_ms = 150 };
    ch = sim_channel_create(&cfg);
    static const sim_mode_per_t NVIS[] = {
        { FREEDV_MODE_DATAC15, 0.20 }, { FREEDV_MODE_DATAC16, 0.20 },
        { FREEDV_MODE_DATAC13, 0.30 }, { FREEDV_MODE_DATAC14, 0.30 },
        { FREEDV_MODE_DATAC4,  0.45 }, { FREEDV_MODE_DATAC3,  0.67 },
        { FREEDV_MODE_DATAC1,  0.89 }, { FREEDV_MODE_DATAC17, 0.93 },
        { FREEDV_MODE_QAM16C2, 0.95 },
    };
    double snr_db = 12.0;                  /* what the sim stamps on frames */
    if (!strncmp(chan, "fade:", 5)) {
        double m = 0, d = 0.5;
        sscanf(chan + 5, "%lf:%lf", &m, &d);
        sim_channel_set_fading(ch, m, d);
        snr_db = m;
    }
    else if (!strncmp(chan, "awgn:", 5)) sim_channel_set_per(ch, atof(chan + 5));
    else if (!strncmp(chan, "cliff:", 6)) { sim_channel_set_snr(ch, atof(chan + 6)); snr_db = atof(chan + 6); }
    else if (!strcmp(chan, "nvis")) {
        sim_channel_set_mode_per(ch, NVIS, (int)(sizeof(NVIS) / sizeof(NVIS[0])));
        snr_db = 10.0;
    }
    snr_now = snr_db;
    if (getenv("CAR_NOHINT")) snr_db = -99.0;

    for (int i = 0; i < 2; i++) {
        station_t *s = &S[i];
        s->id = i;
        car_io_t io = { .keydown = io_keydown, .bind_rx = io_bind_rx, .peer_keyed = io_peer_keyed,
                        .tx_read = io_tx_read, .tx_pending = io_tx_pending, .deliver = io_deliver,
                        .ctx = s };
        car_init(&s->car, &io, car_start_level((float)snr_db), car_start_level((float)snr_db));
        s->rx_mode = -1;
    }
    for (int i = 0; i < CAR_SIM_BYTES; i++) {
        S[0].tx[i] = (uint8_t)((i * 13 + 7) & 0xFF);
        S[1].tx[i] = (uint8_t)((i * 29 + 3) & 0xFF);
    }
    S[0].tx_len = CAR_SIM_BYTES;
    S[1].tx_len = bidir ? CAR_SIM_BYTES : 0;

    now_ms = 0;
    car_start_receiver(&S[1].car, 0);
    car_start_sender(&S[0].car, 0);

    uint64_t done_ms = 0;
    for (;;) {
        uint64_t t = next_event_time();
        for (int i = 0; i < 2; i++) {
            uint64_t d = car_next_deadline(&S[i].car);
            if (d < t) t = d;
        }
        if (t == UINT64_MAX || t > limit_ms) break;
        now_ms = t;
        event_t e;
        while (nev && next_event_time() <= now_ms && pop(&e)) {
            station_t *s = &S[e.st];
            if (e.type == EV_TXEND) {
                car_on_tx_done(&s->car, now_ms);
                continue;
            }
            /* half duplex: a station hears nothing while it transmits */
            if (s->tx_end > e.f_start && s->tx_start < e.f_end && s->tx_end) {
                collisions++;
                if (trace) printf("%9.1f COLLISION at %c\n", now_ms / 1000.0, 'A' + s->id);
            } else if (e.mode == ARQ_CONTROL_MODE) {
                car_on_frame(&s->car, now_ms, e.bytes, e.len, e.mode, true, (float)snr_now);
            } else if (e.mode == s->rx_mode) {
                car_on_frame(&s->car, now_ms, e.bytes, e.len, e.mode, false, (float)snr_now);
            }
            free(e.bytes);
        }
        for (int i = 0; i < 2; i++) car_on_time(&S[i].car, now_ms);
        if (S[1].rx_len >= S[0].tx_len && S[0].rx_len >= S[1].tx_len) { done_ms = now_ms; break; }
    }
    while (nev) { event_t e; pop(&e); free(e.bytes); }

    res->a2b = S[1].rx_len;
    res->b2a = S[0].rx_len;
    res->intact = !memcmp(S[1].rx, S[0].tx, S[1].rx_len < S[0].tx_len ? S[1].rx_len : S[0].tx_len) &&
                  !memcmp(S[0].rx, S[1].tx, S[0].rx_len < S[1].tx_len ? S[0].rx_len : S[1].tx_len);
    res->done_ms = done_ms;
    res->collisions = collisions;
    res->stalled = !done_ms && car_next_deadline(&S[0].car) == UINT64_MAX &&
                   car_next_deadline(&S[1].car) == UINT64_MAX;
    sim_channel_destroy(ch);
}
