/* tests/sim/carousel_bench.c -- prototype: an erasure-coded "carousel" ARQ on
 * the two-FSM sim's channel, to compare with the stop-and-wait ARQs.
 *
 *   carousel_bench <seed> <channel> [bidir]      (channel as in ab_bench)
 *
 * Protocol (prototype, not wire-compatible with anything):
 *   - The sender cuts its backlog into a block of K pieces of P bytes (K <= 255,
 *     Reed-Solomon over GF(256), systematic: pieces 0..K-1 are the data).
 *   - A round is one keydown of back-to-back frames; each frame carries as many
 *     pieces as the current mode holds, a 5-byte frame header and a 1-byte
 *     index per piece.  Round 1 sends the K data pieces plus a margin of repair
 *     pieces sized to the loss the receiver last reported; later rounds send
 *     exactly the shortfall (plus margin).
 *   - After each round the receiver sends ONE feedback frame (control mode):
 *     block id, pieces still needed (0 = done), the loss it saw, faster/slower/
 *     same, and whether it has data of its own.
 *   - A block is done when need == 0.  The turn hands over at block
 *     boundaries, when the done-feedback says "I have data".
 *   - No sequence numbers and no per-frame ACKs: a duplicate piece is simply
 *     not new, a lost feedback costs one extra round, a stale block id is
 *     answered with "done".
 *
 * Channel, airtime and the half-duplex medium are the sim's (sim_channel.c +
 * the ARQ mode table), with the same guards Mercury uses.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sim_channel.h"
#include "rs_erasure.h"
#include "arq_protocol.h"
#include "freedv_api.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- parameters ---------------------------------------------------------- */
#define XFER_BYTES      8192
#define PIECE           24        /* fits DATAC15 (30 B) with the headers */
/* Data pieces per block.  K + repair <= 256, so K is kept well under it: the
 * rest are fresh repair pieces, and a sender that only knows HOW MANY pieces
 * the receiver lacks -- not which -- must send pieces it has never sent, or it
 * may resend ones the receiver already has.  At K = 255 there was one repair
 * piece: after a few lost rounds the indices wrapped and rounds delivered
 * nothing new. */
#define MAX_K           96
#define FRAME_HDR       5         /* kind|block, K, len(2), frames-left      */
#define MAX_KEYDOWN_MS  30000     /* airtime cap per round                  */
#define HEAD_MS         110       /* tx delay + head silence                */
#define TAIL_MS         200
#define GUARD_MS        ARQ_CHANNEL_GUARD_MS_DEFAULT   /* reply guard         */
#define ISS_GUARD_MS    ARQ_ISS_POST_ACK_GUARD_MS_DEFAULT
#define FB_MARGIN_MS    1000
#define HANDOVER_MS     2500      /* new sender waits this, then listens      */
#define LIMIT_MS        (30ULL * 60 * 1000)

static const int LADDER[] = { FREEDV_MODE_DATAC15, FREEDV_MODE_DATAC4, FREEDV_MODE_DATAC3,
                              FREEDV_MODE_DATAC1, FREEDV_MODE_DATAC17, FREEDV_MODE_QAM16C2 };
#define NLADDER ((int)(sizeof(LADDER) / sizeof(LADDER[0])))
#define FB_MODE FREEDV_MODE_DATAC16

static int mode_payload(int mode)
{
    const arq_mode_timing_t *tm = arq_protocol_mode_timing(mode);
    return tm ? tm->payload_bytes : 14;
}
static uint64_t mode_air(int mode) { return sim_channel_airtime_ms(mode, 0); }
static int pieces_per_frame(int mode) { return (mode_payload(mode) - FRAME_HDR) / (PIECE + 1); }

/* ---- frames and events ---------------------------------------------------- */
typedef struct {
    int      kind;                 /* 0 data, 1 feedback */
    int      mode;
    uint8_t  block;
    int      K, len, left;         /* data: frames left in this round after this one */
    int      n;                    /* pieces */
    int      idx[64];
    uint8_t  piece[64][PIECE];
    int      round_frames;         /* data: frames in the round (loss estimate) */
    uint32_t round_id;
    int      need, loss16, advice, has_data;    /* feedback */
} frame_t;

enum { EV_ARRIVE, EV_TXEND, EV_TIMER };
typedef struct {
    uint64_t t;
    int      type, st;             /* st = station the event is for */
    int      timer;                /* EV_TIMER kind */
    uint32_t gen;                  /* timer generation: stale timers ignored */
    frame_t *f;
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

/* ---- stations ------------------------------------------------------------- */
enum { T_FB_TIMEOUT, T_SEND_ROUND, T_SEND_FB, T_HANDOVER };
typedef struct {
    int      id;
    uint8_t  tx[XFER_BYTES];  size_t tx_head, tx_len;
    uint8_t  rx[XFER_BYTES + 64]; size_t rx_len;
    bool     sender;                 /* holds the turn */
    int      level;                  /* my TX ladder level */
    int      ceiling;                /* highest level allowed (learned cliff) */
    int      clean_rounds;           /* consecutive low-loss rounds, to re-probe */
    int      timeouts;               /* consecutive rounds with no feedback */
    double   loss_est;
    /* sender block */
    bool     blk_active;
    uint8_t  blk_id;
    int      blk_K, blk_len, blk_next;  /* next piece index to send */
    uint8_t  blk_data[RS_MAX_PIECES][PIECE];
    int      last_need;
    uint32_t round_id;
    /* receiver block */
    int      rb_id;                  /* block being received, -1 none */
    int      rb_K, rb_len, rb_have;
    bool     rb_got[RS_MAX_PIECES];
    uint8_t  rb_piece[RS_MAX_PIECES][PIECE];
    int      last_done;              /* last completed block id, -1 none */
    uint8_t  next_rx_block;
    uint32_t rx_round_id; int rx_round_seen, rx_round_frames;
    /* medium */
    uint64_t tx_start, tx_end;       /* current/last transmission */
    uint32_t tgen[4];
    uint64_t fb_sent_end;
} station_t;

static station_t S[2];
static sim_channel_t *ch;
static int collisions;
static uint64_t now_ms;

static void arm(station_t *s, int timer, uint64_t at)
{
    event_t e = { .t = at, .type = EV_TIMER, .st = s->id, .timer = timer, .gen = ++s->tgen[timer] };
    push(e);
}
static void disarm(station_t *s, int timer) { s->tgen[timer]++; }

/* Transmit a keydown of n frames from station s, starting now. */
static void transmit(station_t *s, frame_t **fr, int n)
{
    station_t *peer = &S[s->id ^ 1];
    uint64_t t = now_ms + HEAD_MS;
    s->tx_start = now_ms;
    for (int i = 0; i < n; i++) {
        uint64_t air = mode_air(fr[i]->mode);
        uint64_t d;
        bool ok = sim_channel_schedule(ch, t, s->id, fr[i]->mode, 0, &d);
        if (ok) {
            event_t e = { .t = t + air, .type = EV_ARRIVE, .st = peer->id, .f = fr[i],
                          .f_start = t, .f_end = t + air };
            push(e);
        } else
            free(fr[i]);
        t += air;
    }
    s->tx_end = t + TAIL_MS;
    event_t e = { .t = s->tx_end, .type = EV_TXEND, .st = s->id };
    push(e);
}

/* ---- sender --------------------------------------------------------------- */
static void new_block(station_t *s)
{
    size_t left = s->tx_len - s->tx_head;
    int maxlen = MAX_K * PIECE;
    int len = (int)(left < (size_t)maxlen ? left : (size_t)maxlen);
    s->blk_active = true;
    s->blk_len = len;
    s->blk_K = (len + PIECE - 1) / PIECE;
    memset(s->blk_data, 0, sizeof(s->blk_data));
    memcpy(s->blk_data, s->tx + s->tx_head, (size_t)len);
    s->blk_next = 0;
    s->last_need = s->blk_K;
}

static void piece_bytes(station_t *s, int idx, uint8_t *out)
{
    int K = s->blk_K;
    if (idx < K) { memcpy(out, s->blk_data[idx], PIECE); return; }
    const uint8_t *d[RS_MAX_PIECES];
    for (int i = 0; i < K; i++) d[i] = s->blk_data[i];
    rs_encode_repair(K, PIECE, d, idx - K, out);
}

static void send_round(station_t *s)
{
    if (!s->blk_active) new_block(s);
    int mode = LADDER[s->level];
    int ppf = pieces_per_frame(mode);
    double margin = s->loss_est * 1.3 + 0.05;
    int want = (int)ceil(s->last_need * (1.0 + margin));
    int frames = (want + ppf - 1) / ppf;
    int cap = (int)(MAX_KEYDOWN_MS / mode_air(mode));
    if (ppf < 10 && cap > 3) cap = 3;            /* slow rungs: short probing rounds */
    if (cap < 1) cap = 1;
    if (frames > cap) frames = cap;
    if (frames < 1) frames = 1;
    int nmax = RS_MAX_PIECES;                    /* piece indices wrap: data, repair... */
    frame_t *fr[64];
    s->round_id++;
    for (int f = 0; f < frames; f++) {
        frame_t *x = calloc(1, sizeof(*x));
        x->kind = 0; x->mode = mode; x->block = s->blk_id;
        x->K = s->blk_K; x->len = s->blk_len; x->left = frames - 1 - f;
        x->round_frames = frames; x->round_id = s->round_id;
        for (int p = 0; p < ppf && p < 64; p++) {
            int idx = s->blk_next;
            s->blk_next = (s->blk_next + 1) % nmax;   /* past the last repair: data again */
            x->idx[x->n] = idx;
            piece_bytes(s, idx, x->piece[x->n]);
            x->n++;
        }
        fr[f] = x;
    }
    transmit(s, fr, frames);
    uint64_t fb_wait = (s->tx_end - now_ms) + GUARD_MS + HEAD_MS + mode_air(FB_MODE) + TAIL_MS + FB_MARGIN_MS;
    arm(s, T_FB_TIMEOUT, now_ms + fb_wait);
}

static void on_feedback(station_t *s, const frame_t *f)
{
    if (!s->sender || !s->blk_active || f->block != s->blk_id) return;  /* stale */
    disarm(s, T_FB_TIMEOUT);
    s->timeouts = 0;
    s->loss_est = 0.5 * s->loss_est + 0.5 * (f->loss16 / 16.0);
    /* Link adaptation with memory.  A round that lost half or more marks a
     * cliff: cap the ladder one rung below the level that failed (two rungs
     * down if it was near-total).  Climb toward the cap on the receiver's
     * advice, and re-probe the cap one rung at a time only after a run of
     * clean rounds -- climbing blindly into a dead mode after every clean round
     * oscillated forever on the cliff model. */
    double loss = f->loss16 / 16.0;
    if (s->loss_est >= 0.55) {               /* smoothed: one noisy round is not a cliff */
        int drop = loss >= 0.8 ? 2 : 1;
        s->ceiling = s->level - 1 > 0 ? s->level - 1 : 0;
        s->level = s->level - drop > 0 ? s->level - drop : 0;
        s->clean_rounds = 0;
    } else {
        if (loss <= 0.25 && ++s->clean_rounds >= 6 && s->ceiling < NLADDER - 1) {
            s->ceiling++;
            s->clean_rounds = 0;
        }
        if (f->advice > 0) s->level += f->advice;
        if (s->level > s->ceiling) s->level = s->ceiling;
    }
    if (s->level < 0) s->level = 0;
    if (f->need > 0) {
        s->last_need = f->need;
        arm(s, T_SEND_ROUND, now_ms + ISS_GUARD_MS);
        return;
    }
    /* block delivered */
    s->tx_head += (size_t)s->blk_len;
    s->blk_active = false;
    s->blk_id++;
    if (f->has_data) {                  /* hand the turn over */
        s->sender = false;
        return;
    }
    if (s->tx_head < s->tx_len)
        arm(s, T_SEND_ROUND, now_ms + ISS_GUARD_MS);
    else
        s->sender = false;
}

/* ---- receiver ------------------------------------------------------------- */
static void deliver_block(station_t *s)
{
    int idx[RS_MAX_PIECES]; const uint8_t *pcs[RS_MAX_PIECES];
    static uint8_t outb[RS_MAX_PIECES][PIECE];
    uint8_t *out[RS_MAX_PIECES];
    int r = 0;
    for (int i = 0; i < RS_MAX_PIECES && r < s->rb_K; i++)
        if (s->rb_got[i]) { idx[r] = i; pcs[r] = s->rb_piece[i]; r++; }
    for (int i = 0; i < s->rb_K; i++) out[i] = outb[i];
    if (rs_decode(s->rb_K, PIECE, idx, pcs, out) == 0) {
        size_t n = (size_t)s->rb_len;
        size_t off = 0;
        for (int i = 0; i < s->rb_K && off < n; i++) {
            size_t c = n - off < PIECE ? n - off : PIECE;
            memcpy(s->rx + s->rx_len + off, out[i], c);
            off += c;
        }
        s->rx_len += n;
    }
    s->last_done = s->rb_id;
    s->next_rx_block = (uint8_t)(s->rb_id + 1);
    s->rb_id = -1;
}

static void send_feedback(station_t *s)
{
    frame_t *x = calloc(1, sizeof(*x));
    x->kind = 1; x->mode = FB_MODE;
    if (s->rb_id >= 0) {
        x->block = (uint8_t)s->rb_id;
        x->need = s->rb_K - s->rb_have;
    } else {
        x->block = (uint8_t)s->last_done;        /* stale round: say done again */
        x->need = 0;
    }
    double loss = s->rx_round_frames ? 1.0 - (double)s->rx_round_seen / s->rx_round_frames : 0;
    if (loss < 0) loss = 0;
    x->loss16 = (int)lround(loss * 16.0);
    /* With an erasure code a lossy fast mode still beats a clean slow one
     * (goodput ~ pieces per frame x (1 - loss) / airtime), so climb unless the
     * loss is heavy; two rungs after a clean round. */
    x->advice = loss == 0 ? 2 : (loss <= 0.25 ? 1 : (loss >= 0.5 ? -1 : 0));
    x->has_data = s->tx_head < s->tx_len;
    if (x->need == 0 && s->rb_id >= 0) deliver_block(s);
    frame_t *fr[1] = { x };
    transmit(s, fr, 1);
    s->fb_sent_end = s->tx_end;
    if (x->need == 0 && x->has_data && !s->sender)
        arm(s, T_HANDOVER, s->tx_end + HANDOVER_MS);
}

static void on_data(station_t *s, const frame_t *f)
{
    if (s->sender) {                      /* both think they hold the turn */
        if (!s->blk_active) s->sender = false; else return;
    }
    disarm(s, T_HANDOVER);                /* the peer is still sending */
    if (f->round_id != s->rx_round_id) {
        s->rx_round_id = f->round_id; s->rx_round_seen = 0; s->rx_round_frames = f->round_frames;
    }
    s->rx_round_seen++;
    if (s->last_done >= 0 && f->block == (uint8_t)s->last_done && s->rb_id < 0) {
        /* a round of a block we finished: the peer missed our "done" */
    } else {
        if (s->rb_id < 0 && f->block == s->next_rx_block) {
            s->rb_id = f->block; s->rb_K = f->K; s->rb_len = f->len; s->rb_have = 0;
            memset(s->rb_got, 0, sizeof(s->rb_got));
        }
        if (s->rb_id >= 0 && f->block == (uint8_t)s->rb_id) {
            for (int p = 0; p < f->n; p++) {
                int i = f->idx[p];
                if (i >= 0 && i < RS_MAX_PIECES && !s->rb_got[i] && s->rb_have < s->rb_K) {
                    s->rb_got[i] = true;
                    memcpy(s->rb_piece[i], f->piece[p], PIECE);
                    s->rb_have++;
                }
            }
        }
    }
    /* reply after the round's expected end */
    uint64_t end = now_ms + (uint64_t)f->left * mode_air(f->mode) + TAIL_MS;
    arm(s, T_SEND_FB, end + GUARD_MS);
}

/* ---- run ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <seed> <channel> [bidir]\n", argv[0]); return 1; }
    uint64_t seed = (uint64_t)atoll(argv[1]);
    const char *chan = argv[2];
    bool bidir = argc > 3 && !strcmp(argv[3], "bidir");

    sim_channel_cfg_t cfg = { .seed = seed, .per = 0.02, .guard_ms = 150 };
    ch = sim_channel_create(&cfg);
    static const sim_mode_per_t NVIS[] = {
        { FREEDV_MODE_DATAC15, 0.20 }, { FREEDV_MODE_DATAC16, 0.20 },
        { FREEDV_MODE_DATAC13, 0.30 }, { FREEDV_MODE_DATAC14, 0.30 },
        { FREEDV_MODE_DATAC4,  0.45 }, { FREEDV_MODE_DATAC3,  0.67 },
        { FREEDV_MODE_DATAC1,  0.89 }, { FREEDV_MODE_DATAC17, 0.93 },
        { FREEDV_MODE_QAM16C2, 0.95 },
    };
    if (!strncmp(chan, "awgn:", 5)) sim_channel_set_per(ch, atof(chan + 5));
    else if (!strncmp(chan, "cliff:", 6)) sim_channel_set_snr(ch, atof(chan + 6));
    else if (!strcmp(chan, "nvis")) sim_channel_set_mode_per(ch, NVIS, (int)(sizeof(NVIS) / sizeof(NVIS[0])));

    for (int i = 0; i < 2; i++) {
        memset(&S[i], 0, sizeof(S[i]));
        S[i].id = i; S[i].rb_id = -1; S[i].last_done = -1; S[i].loss_est = 0.1;
        S[i].ceiling = NLADDER - 1;
    }
    for (int i = 0; i < XFER_BYTES; i++) {
        S[0].tx[i] = (uint8_t)((i * 13 + 7) & 0xFF);
        S[1].tx[i] = (uint8_t)((i * 29 + 3) & 0xFF);
    }
    S[0].tx_len = XFER_BYTES;
    S[1].tx_len = bidir ? XFER_BYTES : 0;

    /* Session already connected (as in ab_bench, the connect is not measured
     * here); A holds the turn first. */
    S[0].sender = true;
    now_ms = 0;
    arm(&S[0], T_SEND_ROUND, 0);

    event_t e;
    uint64_t done_ms = 0;
    while (pop(&e)) {
        now_ms = e.t;
        if (now_ms > LIMIT_MS) break;
        station_t *s = &S[e.st];
        if (e.type == EV_ARRIVE) {
            /* half duplex: a station hears nothing while it transmits */
            if (s->tx_end > e.f_start && s->tx_start < e.f_end && s->tx_start != 0) {
                collisions++;
                free(e.f);
                continue;
            }
            if (e.f->kind == 0) on_data(s, e.f); else on_feedback(s, e.f);
            free(e.f);
        } else if (e.type == EV_TIMER) {
            if (e.gen != s->tgen[e.timer]) continue;         /* disarmed/re-armed */
            switch (e.timer) {
            case T_SEND_ROUND:
                if (s->sender && s->tx_head < s->tx_len) send_round(s);
                break;
            case T_FB_TIMEOUT:
                /* No feedback: the round, or the reply, was lost whole.  In a
                 * mode past the cliff that is every round, and the receiver
                 * never learns a round happened -- so the sender must count a
                 * timeout as total loss, or it repeats the dead round forever. */
                if (s->sender && s->blk_active) {
                    /* One timeout is ambiguous -- the round OR the reply was
                     * lost -- so it only nudges the estimate; two in a row at
                     * the same level is a dead mode. */
                    s->loss_est = 0.75 * s->loss_est + 0.25;
                    if (++s->timeouts >= 2) {
                        s->timeouts = 0;
                        s->ceiling = s->level - 1 > 0 ? s->level - 1 : 0;
                        s->level = s->level - 2 > 0 ? s->level - 2 : 0;
                        s->clean_rounds = 0;
                    }
                    send_round(s);
                }
                break;
            case T_SEND_FB:
                send_feedback(s);
                break;
            case T_HANDOVER: {
                station_t *p = &S[s->id ^ 1];
                if (p->tx_start > s->fb_sent_end || p->tx_end > now_ms)
                    break;                                 /* peer still sending */
                s->sender = true;
                send_round(s);
                break;
            }
            }
        }
        if (S[1].rx_len >= S[0].tx_len && S[0].rx_len >= S[1].tx_len && !done_ms)
            done_ms = now_ms;
        if (done_ms) break;
    }

    int ok_a = memcmp(S[1].rx, S[0].tx, S[1].rx_len < S[0].tx_len ? S[1].rx_len : S[0].tx_len) == 0;
    int ok_b = memcmp(S[0].rx, S[1].tx, S[0].rx_len < S[1].tx_len ? S[0].rx_len : S[1].tx_len) == 0;
    printf("seed=%llu chan=%s %s a2b=%zu b2a=%zu integrity=%s done_ms=%llu collisions=%d%s\n",
           (unsigned long long)seed, chan, bidir ? "bidir" : "oneway",
           S[1].rx_len, S[0].rx_len, (ok_a && ok_b) ? "OK" : "CORRUPT",
           (unsigned long long)done_ms, collisions, nev == 0 && !done_ms ? " STALLED" : "");
    return (ok_a && ok_b) ? 0 : 2;
}
