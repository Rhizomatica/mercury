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
#include "modem_mfsk.h"            /* MERCURY_MODE_MFSK */
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
#ifndef MFSK_CS_EXTRA_MS
#define MFSK_CS_EXTRA_MS 1000
#endif
#ifndef CS_ACQ_MS
#define CS_ACQ_MS       400       /* carrier sense: sync this long after keying */
#endif

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
    uint64_t bound_at;             /* ...since */
    uint64_t tx_start, tx_end;     /* current/last keydown */
    /* The frames of that keydown, for CAR_CS_DECODABLE: a decoder syncs on a
     * frame only from its preamble, so only one it was bound to by then. */
    struct { uint64_t start; int mode; bool sync; } kd[CAR_KEYDOWN_MAX];
    int      kd_n;
} station_t;

static station_t S[2];
static sim_channel_t *ch;
static int collisions;
static int bad_frames;
static uint64_t now_ms;
static bool trace;
static double snr_now = 12.0;          /* the SNR stamped on delivered frames */
static bool cs_decodable;              /* CAR_CS_DECODABLE: carrier sense needs a decodable frame */
/* CAR_SNR_BIAS: the estimate as the bench read it at 10 %: DATAC15, DATAC4 and
 * DATAC16 frames about 3.5 dB below DATAC3 and DATAC1 on the same link. */
static bool snr_biased;
static bool asym;                                      /* an asym:A:B channel */
static double snr_dir[2];                              /* its SNR, by sender */
/* Never exactly 0.0: car_on_frame takes that for "no estimate", which a
 * decoded frame always has -- at 0 dB (cliff:0, fade:0) the sim ran with no
 * SNR at all. */
static double frame_snr(const station_t *rx)
{
    double snr = asym ? snr_dir[rx->id ^ 1] : snr_now;
    return snr == 0.0 ? 0.001 : snr;
}
static double snr_bias(int mode)
{
    if (!snr_biased) return 0.0;
    return mode == FREEDV_MODE_DATAC15 || mode == FREEDV_MODE_DATAC4 || mode == ARQ_CONTROL_MODE ? -3.5 : 0.0;
}
static double step_from, step_to, step_at_s = -1.0;   /* a step:A:B:T channel */

static void io_keydown(void *ctx, const car_frame_t *fr, int n)
{
    station_t *s = ctx, *peer = &S[s->id ^ 1];
    uint64_t t = now_ms + HEAD_MS;
    s->tx_start = now_ms;
    s->kd_n = 0;
    if (trace) printf("%9.1f %c keys:", now_ms / 1000.0, 'A' + s->id);
    for (int i = 0; i < n; i++) {
        /* The modem refuses a frame that does not fill its mode (modem.c,
         * send_modulated_keydown): a keydown with one never goes out. */
        const arq_mode_timing_t *tm = arq_protocol_mode_timing(fr[i].mode);
        if (tm && fr[i].len != (size_t)tm->payload_bytes) {
            bad_frames++;
            if (trace) printf("%9.1f %c BAD FRAME %d: %zu bytes for mode %d\n", now_ms / 1000.0,
                              'A' + s->id, i, (size_t)fr[i].len, fr[i].mode);
        }
        if (i) t += fr[i].gap_ms;
        uint64_t air = sim_channel_airtime_ms(fr[i].mode, 0), d;
        if (trace) {
            static const char *K[] = { "poll", "handover", "status", "?" };
            if (fr[i].mode == ARQ_CONTROL_MODE) printf(" %s", K[fr[i].bytes[0] >> 6]);
            else printf(" data(%d)", fr[i].mode);
        }
        /* A decoder syncs on what it could decode above its cliff, even when
         * this frame is then lost (see io_peer_keyed). */
        s->kd[s->kd_n].start = t; s->kd[s->kd_n].mode = fr[i].mode;
        s->kd[s->kd_n].sync = sim_channel_syncable(ch, t, s->id, fr[i].mode);
        s->kd_n++;
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

static void io_bind_rx(void *ctx, int mode)
{
    station_t *s = ctx;
    if (mode != s->rx_mode) { s->rx_mode = mode; s->bound_at = now_ms; }
}
static void io_trace(void *ctx, const char *line)
{
    if (trace) printf("%9.1f %c   %s\n", now_ms / 1000.0, 'A' + ((station_t *)ctx)->id, line);
}

/* The detector reports a pattern this long after it ends (0.23-0.24 s on air,
 * Pi 4); a poll, by contrast, is decoded as it ends. */
#ifndef SIM_PATTERN_DETECT_MS
#define SIM_PATTERN_DETECT_MS 240
#endif
/* A pattern: 0.64 s on the air, detected ~10 dB below DATAC16. */
static void io_pattern(void *ctx, int kind)
{
    station_t *s = ctx, *peer = &S[s->id ^ 1];
    uint64_t t = now_ms + HEAD_MS, d;
    uint64_t air = sim_channel_airtime_ms(SIM_MODE_PATTERN, 0);
    s->tx_start = now_ms;
    s->kd_n = 0;
    if (trace) printf("%9.1f %c keys: pattern %s\n", now_ms / 1000.0, 'A' + s->id, kind ? "BREAK" : "ACK");
    if (sim_channel_schedule(ch, t, s->id, SIM_MODE_PATTERN, 0, &d)) {
        s->kd[0].start = t; s->kd[0].mode = SIM_MODE_PATTERN; s->kd[0].sync = true;
        s->kd_n = 1;
        event_t e = { .t = t + air + SIM_PATTERN_DETECT_MS, .type = EV_ARRIVE, .st = peer->id, .mode = SIM_MODE_PATTERN,
                      .len = (size_t)kind, .bytes = NULL, .f_start = t, .f_end = t + air };
        push(e);
    }
    s->tx_end = t + air + TAIL_MS;
    event_t e = { .t = s->tx_end, .type = EV_TXEND, .st = s->id };
    push(e);
}

static bool io_peer_keyed(void *ctx)
{
    const station_t *l = ctx, *p = &S[l->id ^ 1];
    /* CAR_CS_DECODABLE: carrier sense is a decoder in sync, so a keydown the
     * listener cannot decode is not sensed (on air: a DATAC1 probe at 4.5 dB,
     * and patterns, which no OFDM decoder syncs on) -- nor the part of it
     * before the first frame it can (on air: a floor frame behind a control
     * frame the listener cannot decode).  The control decoder always runs;
     * the payload decoder syncs on a frame only if bound to its mode by the
     * frame's preamble, and a pattern only when that is MFSK: rebound to the
     * floor mid-frame, on air, it missed that frame (car23 at 3 %). */
    uint64_t from = p->tx_start;
    if (cs_decodable) {
        from = UINT64_MAX;
        for (int i = 0; i < p->kd_n && from == UINT64_MAX; i++) {
            int m = p->kd[i].mode;
            bool bound = l->bound_at <= p->kd[i].start;
            if (!p->kd[i].sync) continue;
            if (m == SIM_MODE_PATTERN) {
                if (l->rx_mode == MERCURY_MODE_MFSK && bound) from = p->kd[i].start;
            } else if (m == ARQ_CONTROL_MODE || (m == l->rx_mode && bound)) {
                /* The MFSK preamble search takes longer to be sure: under a
                 * second at -7 dB (test_mfsk_modem), more near its floor. */
                from = p->kd[i].start + (m == MERCURY_MODE_MFSK ? MFSK_CS_EXTRA_MS : 0);
            }
        }
        if (from == UINT64_MAX) return false;
    }
    return p->tx_end && from + CS_ACQ_MS <= now_ms && now_ms < p->tx_end - TAIL_MS + CS_ACQ_MS;
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
    asym = false;
    cs_decodable = getenv("CAR_CS_DECODABLE") != NULL;
    snr_biased = getenv("CAR_SNR_BIAS") != NULL;
    nev = 0;
    collisions = 0;
    bad_frames = 0;
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
    else if (!strncmp(chan, "step:", 5)) {   /* step:A:B:T  A dB, then B dB from T s on */
        sscanf(chan + 5, "%lf:%lf:%lf", &step_from, &step_to, &step_at_s);
        sim_channel_set_snr(ch, step_from); snr_db = step_from;
    }
    else if (!strncmp(chan, "asym:", 5)) {   /* asym:A:B  A dB from A to B, B dB back */
        sscanf(chan + 5, "%lf:%lf", &snr_dir[0], &snr_dir[1]);
        sim_channel_set_snr_asym(ch, snr_dir[0], snr_dir[1]);
        asym = true;
        snr_db = snr_dir[0] < snr_dir[1] ? snr_dir[0] : snr_dir[1];
    }
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
                        .pattern = getenv("CAR_NOPATTERN") ? NULL : io_pattern,
                        .trace = io_trace, .ctx = s };
        /* What each end hears, and what its peer hears of it. */
        double hears = asym ? snr_dir[i ^ 1] : snr_db, heard = asym ? snr_dir[i] : snr_db;
        if (getenv("CAR_NOHINT")) hears = heard = -99.0;
        car_init(&s->car, &io, car_start_level((float)hears), car_start_level((float)heard));
        car_seed_ctl_deaf(&s->car, hears < ARQ_SNR_MIN_DATAC15_DB, heard < ARQ_SNR_MIN_DATAC15_DB);
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
        if (step_at_s >= 0 && now_ms >= (uint64_t)(step_at_s * 1000.0)) {
            sim_channel_set_snr(ch, step_to); snr_now = step_to;
            step_at_s = -1.0;
        }
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
            } else if (e.mode == SIM_MODE_PATTERN) {
                if (!getenv("CAR_NOPATTERN_RX"))
                    car_on_pattern(&s->car, now_ms, (int)e.len);
            } else if (e.mode == ARQ_CONTROL_MODE) {
                car_on_frame(&s->car, now_ms, e.bytes, e.len, e.mode, true, (float)(frame_snr(s) + snr_bias(e.mode)));
            } else if (e.mode == s->rx_mode && (!cs_decodable || s->bound_at <= e.f_start)) {
                car_on_frame(&s->car, now_ms, e.bytes, e.len, e.mode, false, (float)(frame_snr(s) + snr_bias(e.mode)));
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
    res->bad_frames = bad_frames;
    res->stalled = !done_ms && car_next_deadline(&S[0].car) == UINT64_MAX &&
                   car_next_deadline(&S[1].car) == UINT64_MAX;
    sim_channel_destroy(ch);
}
