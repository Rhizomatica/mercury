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
#define FRAME_HDR       2         /* kind, frames left in the round         */
#define SEG_HDR         3         /* block id, K, pad bytes in the last piece */
#define WIN             8         /* blocks a round may carry at once       */
#ifndef SLOW_RUNG_FRAMES
#define SLOW_RUNG_FRAMES 3        /* round cap on the two slowest rungs      */
#endif
#ifndef TURN_QUANTUM_MS
#define TURN_QUANTUM_MS 120000    /* a turn may run this long before the peer's */
#endif
#define FB_UNSEEN       255       /* feedback: no piece of this block yet   */
#define MAX_KEYDOWN_MS  30000     /* airtime cap per round                  */
#define HEAD_MS         110       /* tx delay + head silence                */
#define TAIL_MS         200
#define GUARD_MS        ARQ_CHANNEL_GUARD_MS_DEFAULT   /* reply guard         */
#define ISS_GUARD_MS    ARQ_ISS_POST_ACK_GUARD_MS_DEFAULT
#define FB_MARGIN_MS    1000
#define HANDOVER_MS     2500      /* new sender waits this, then listens      */
#ifndef LIMIT_MS
#define LIMIT_MS        (30ULL * 60 * 1000)
#endif

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
static int pieces_per_frame(int mode) { return (mode_payload(mode) - FRAME_HDR - SEG_HDR) / (PIECE + 1); }

/* ---- frames and events ---------------------------------------------------- */
typedef struct {                   /* the pieces of one block in a data frame */
    uint8_t  block;
    int      K, len;
    int      n;
    int      idx[64];
    uint8_t  piece[64][PIECE];
} seg_t;

typedef struct {
    int      kind;                 /* 0 data, 1 feedback */
    int      mode;
    int      left;                 /* data: frames left in this round after this one */
    int      nseg;
    seg_t    seg[WIN];
    int      round_frames;         /* data: frames in the round (loss estimate) */
    uint32_t round_id;             /* data: this round; feedback: the round it answers */
    /* feedback */
    uint8_t  base;                 /* first block of the receiver's window */
    int      need[WIN];            /* pieces still needed per block, FB_UNSEEN, 0 = done */
    int      loss16, advice, has_data;
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
    uint8_t  tx[XFER_BYTES];  size_t tx_len;
    uint8_t  rx[XFER_BYTES + 64]; size_t rx_len;
    bool     sender;                 /* holds the turn */
    int      level;                  /* my TX ladder level */
    int      round_level;            /* level the outstanding round went out on */
    double   lv_sent[NLADDER];       /* frames sent on each level (decayed)  */
    double   lv_lost[NLADDER];       /* ...and lost (decayed)                */
    int      lv_rounds[NLADDER];     /* rounds measured on each level (0 = unknown) */
    int      round_frames;           /* frames in the outstanding round      */
    int      lv_dead_run[NLADDER];   /* frames lost in a row, none delivered */
    uint32_t lv_round_at[NLADDER];   /* round_id of the last measurement */
    uint64_t lv_probe_at[NLADDER];   /* when a dead level was last measured */
    uint64_t lv_backoff_ms[NLADDER]; /* re-probe interval for a dead level */
    int      timeouts;               /* consecutive rounds with no feedback */
    double   loss_est;
    /* sender: a window of open blocks, oldest first */
    struct {
        uint8_t id;
        int     K, len, next, need;          /* next piece index to send */
        uint8_t data[RS_MAX_PIECES][PIECE];
    } sb[WIN];
    int      nsb;
    uint8_t  next_blk_id;
    size_t   tx_alloc;               /* bytes already cut into blocks */
    uint32_t round_id;
    uint64_t turn_start;             /* when this station last took the turn */
    /* receiver: blocks base .. base+WIN-1, slot = id % WIN */
    struct {
        bool    known, done;
        int     K, len, have;
        bool    got[RS_MAX_PIECES];
        uint8_t piece[RS_MAX_PIECES][PIECE];
        uint8_t out[MAX_K * PIECE];
    } rb[WIN];
    uint8_t  rbase;                  /* next block to deliver */
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

/* ---- sender --------------------------------------------------------------- *
 * One block per round held a round to ~2.3 KB (96 pieces of 24 bytes), so on a
 * fast mode every two or three frames paid a whole feedback turnaround.  Bigger
 * pieces would have fixed that, but a block cut for a fast mode does not fit a
 * slow one: on a drop of two rungs it had to be re-encoded and its progress was
 * lost, which left runs unfinished.  Instead a round carries up to WIN blocks,
 * each still of small pieces that fit every mode, and the feedback reports what
 * each one still needs.  Nothing is ever re-encoded. */
static void open_block(station_t *s)
{
    int i = s->nsb++;
    size_t left = s->tx_len - s->tx_alloc;
    int maxlen = MAX_K * PIECE;
    int len = (int)(left < (size_t)maxlen ? left : (size_t)maxlen);
    s->sb[i].id = s->next_blk_id++;
    s->sb[i].len = len;
    s->sb[i].K = (len + PIECE - 1) / PIECE;
    s->sb[i].next = 0;
    s->sb[i].need = s->sb[i].K;
    memset(s->sb[i].data, 0, sizeof(s->sb[i].data));
    memcpy(s->sb[i].data, s->tx + s->tx_alloc, (size_t)len);
    s->tx_alloc += (size_t)len;
}

static void piece_bytes(station_t *s, int b, int idx, uint8_t *out)
{
    int K = s->sb[b].K;
    if (idx < K) { memcpy(out, s->sb[b].data[idx], PIECE); return; }
    const uint8_t *d[RS_MAX_PIECES];
    for (int i = 0; i < K; i++) d[i] = s->sb[b].data[i];
    rs_encode_repair(K, PIECE, d, idx - K, out);
}

/* ---- link adaptation: goodput, measured per level ------------------------ *
 * The first version adapted on round-to-round advice plus a learned ceiling.
 * With the mode pinned to the best one, this carousel beats the stop-and-wait
 * ARQ on the cliff channels and ties it at 25 % flat loss; adaptive, it lost
 * 13 of 20 runs at 25 % and was 20-50 % slower on the cliffs.  The ceiling
 * tripped on flat-loss noise and never came back, and the advice climbed into
 * dead modes.
 *
 * So choose on what each level actually delivers.  With erasure coding every
 * piece received is useful, so a level's goodput is simply its raw rate times
 * the fraction that gets through.  Flat loss hits every mode alike, so the
 * fastest wins; past a cliff a mode delivers nothing and drops out by itself.
 * Above the best, the first rung that COULD beat it (its raw rate exceeds the
 * best's measured goodput) is probed with a one-frame round when it has never
 * been measured or its measurement is stale -- a probe costs one frame, a
 * wrong climb costs a round.  Only rungs that could win: the ladder is not
 * monotonic in rate (DATAC4 carries one 24-byte piece in 5.8 s, below
 * DATAC15), and probing only the next rung stranded the sender under it. */
#define PROBE_EVERY   8     /* rounds before a measured rung is re-probed...  */
#define DEAD_PROBE_MS     60000   /* a dead rung is re-probed after this...  */
#define DEAD_PROBE_MAX_MS 480000  /* ...doubling to at most this             */
#define LV_DECAY      0.8   /* per measured round: memory of ~5 frames */
#define DEAD_RUN      4     /* consecutive frames lost, none delivered: dead */

static double level_rate(int lv)          /* raw piece bytes per ms of airtime */
{
    int mode = LADDER[lv];
    return (double)(pieces_per_frame(mode) * PIECE) / (double)mode_air(mode);
}

/* Delivery estimated from frame counts, not from single rounds: at 25 % flat
 * loss a quarter of one-frame probes vanish whole, and judging a level on one
 * of them condemned a working mode.
 *
 * One estimator cannot both explore and avoid dead modes when raw rates span
 * 60x: an optimistic prior kept a dead QAM16C2 (12x DATAC3's rate) outscoring
 * a working DATAC3 on the cliff for 100+ rounds, and a pessimistic one never
 * climbed on a clean channel.  So the two jobs are split, using what HF modes
 * guarantee -- a faster mode needs more SNR, so above a dead mode everything is
 * dead too:
 *   - dead is a hard verdict: DEAD_RUN frames lost in a row with nothing
 *     delivered (0.4 % by chance at 25 % flat loss; a decayed-sum test fired
 *     on flat-loss runs and froze working modes out).  The lowest dead level
 *     is a ceiling nothing at or above is chosen from, re-probed with backoff
 *     to notice the channel improving;
 *   - below it, selection uses (lost + 1/2) / (sent + 2), optimistic enough to
 *     climb, and the earned round size bounds what any trial can cost. */
static double level_delivery(const station_t *s, int lv)
{
    return 1.0 - (s->lv_lost[lv] + 0.5) / (s->lv_sent[lv] + 2.0);
}

static bool level_dead(const station_t *s, int lv)
{
    return s->lv_dead_run[lv] >= DEAD_RUN;
}

static void measure_level(station_t *s, int lv, double loss)
{
    s->lv_sent[lv] = LV_DECAY * s->lv_sent[lv] + s->round_frames;
    s->lv_lost[lv] = LV_DECAY * s->lv_lost[lv] + s->round_frames * loss;
    if (loss >= 1.0) s->lv_dead_run[lv] += s->round_frames;
    else             s->lv_dead_run[lv] = 0;
    s->lv_rounds[lv]++;
    s->lv_round_at[lv] = s->round_id;
    s->lv_probe_at[lv] = now_ms;
    if (loss >= 0.9)
        s->lv_backoff_ms[lv] = !s->lv_backoff_ms[lv] ? DEAD_PROBE_MS
                             : s->lv_backoff_ms[lv] * 2 > DEAD_PROBE_MAX_MS ? DEAD_PROBE_MAX_MS
                             : s->lv_backoff_ms[lv] * 2;
    else
        s->lv_backoff_ms[lv] = 0;
}

/* May this level be chosen?  Not if it is dead; and not if a lower one is,
 * UNLESS it has delivered recently itself.  "Above a dead mode everything is
 * dead" holds for real SNR cliffs, but a verdict is only statistics: at 25 %
 * flat loss a lightly probed DATAC4 was declared dead by chance (a probe and
 * its feedback both lost), and the ceiling then locked out a DATAC3 and a
 * DATAC1 that were delivering -- the sender crawled on DATAC15 to the end.
 * Evidence of delivery beats the inference. */
static bool level_allowed(const station_t *s, int lv)
{
    if (level_dead(s, lv)) return false;
    for (int b = 0; b < lv; b++)
        if (level_dead(s, b))
            return s->lv_sent[lv] - s->lv_lost[lv] >= 0.5;
    return true;
}

/* A dead or locked-out level comes back for one probe after its backoff --
 * counted in time, not rounds: slow rungs have 30 s rounds, and 64 of them
 * kept a false verdict in force for over half an hour. */
static bool reprobe_due(const station_t *s, int lv)
{
    uint64_t wait = s->lv_backoff_ms[lv] ? s->lv_backoff_ms[lv] : DEAD_PROBE_MS;
    return now_ms - s->lv_probe_at[lv] >= wait;
}

static void choose_level(station_t *s)
{
    int best = -1;
    double best_gp = -1.0;
    for (int lv = 0; lv < NLADDER; lv++) {
        if (!s->lv_rounds[lv] || !level_allowed(s, lv)) continue;
        double gp = level_rate(lv) * level_delivery(s, lv);
        if (gp > best_gp) { best_gp = gp; best = lv; }
    }
    if (best < 0) { s->level = 0; return; }
    /* Nothing measured gets through: go down to a rung not tried yet. */
    if (best_gp <= 0.0) {
        for (int lv = best - 1; lv >= 0; lv--)
            if (!s->lv_rounds[lv]) { s->level = lv; return; }
    }
    for (int up = best + 1; up < NLADDER; up++) {
        if (level_rate(up) <= best_gp)
            continue;                   /* cannot win even with no loss */
        if (!level_allowed(s, up)) {
            if (!reprobe_due(s, up)) break;
        } else if (s->lv_rounds[up] && s->round_id - s->lv_round_at[up] < PROBE_EVERY) {
            break;                      /* measured and fresh: the argmax decided */
        }
        s->level = up;                  /* a probe: its earned round is one frame */
        return;
    }
    s->level = best;
}

/* Where a sender starts: the SNR its peer reported (in Mercury, the connect
 * exchange measures both directions), mapped the way trunk enters a mode from
 * DATAC15 -- threshold plus hysteresis.  Only a start: the first round on it is
 * a one-frame probe like any other, and measured goodput decides from there,
 * so a misleading SNR (ISI-limited NVIS reads 10 dB) costs a few frames.
 * Climbing by probes from DATAC15 instead cost 25-40 s of airtime per turn on
 * a 15 dB fading channel, where trunk goes straight to DATAC17. */
static double snr_hint_db = 12.0;          /* what the sim stamps on frames */

static int hint_level(double snr_db)
{
    static const struct { int mode; float min_db; } T[] = {
        { FREEDV_MODE_QAM16C2, ARQ_SNR_MIN_QAM16C2_DB }, { FREEDV_MODE_DATAC17, ARQ_SNR_MIN_DATAC17_DB },
        { FREEDV_MODE_DATAC1,  ARQ_SNR_MIN_DATAC1_DB },  { FREEDV_MODE_DATAC3,  ARQ_SNR_MIN_DATAC3_DB },
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (snr_db >= T[i].min_db + ARQ_SNR_HYST_DB)
            for (int lv = 0; lv < NLADDER; lv++)
                if (LADDER[lv] == T[i].mode) return lv;
    return 0;                  /* DATAC4 is slower than DATAC15 here: never a start */
}

static void send_round(station_t *s)
{
    while (s->nsb < WIN && s->tx_alloc < s->tx_len)
        open_block(s);
    if (!s->nsb) return;
    s->round_level = s->level;
    int mode = LADDER[s->level];
    int room = mode_payload(mode) - FRAME_HDR;

    /* What each open block still needs, with a margin for the loss seen. */
    double margin = s->loss_est * 1.3 + 0.05;
    int want[WIN], want_total = 0;
    for (int b = 0; b < s->nsb; b++) {
        want[b] = (int)ceil(s->sb[b].need * (1.0 + margin));
        want_total += want[b];
    }
    int ppf = pieces_per_frame(mode);
    int frames = (want_total + ppf - 1) / ppf;
    int cap = (int)(MAX_KEYDOWN_MS / mode_air(mode));
    if (s->level <= 1 && cap > SLOW_RUNG_FRAMES) cap = SLOW_RUNG_FRAMES;   /* slow rungs */
    /* A level earns its round size: at most one frame more than it has
     * recently delivered (decayed), so a probe is one frame, a proven mode
     * gets full rounds, and a dead one never costs more than a frame. */
    int earned = 1 + (int)(s->lv_sent[s->level] - s->lv_lost[s->level]);
    if (cap > earned) cap = earned;
    if (cap < 1) cap = 1;
    if (frames > cap) frames = cap;
    if (frames < 1) frames = 1;

    frame_t *fr[64];
    int nf = 0, b = 0;
    s->round_id++;
    while (nf < frames) {
        frame_t *x = calloc(1, sizeof(*x));
        x->kind = 0; x->mode = mode; x->round_id = s->round_id;
        int bytes = room;
        /* Fill the frame oldest block first; a block's pieces go on past its
         * want (as extra repair) only when nothing else is left to send. */
        while (bytes >= SEG_HDR + PIECE + 1 && x->nseg < WIN) {
            while (b < s->nsb && want[b] <= 0) b++;
            int bb = b;
            if (bb >= s->nsb) {
                /* every want met: top the frame up with repair for the oldest */
                bb = 0;
                if (x->nseg && x->seg[x->nseg - 1].block == s->sb[0].id) break;
            }
            seg_t *g = &x->seg[x->nseg++];
            g->block = s->sb[bb].id; g->K = s->sb[bb].K; g->len = s->sb[bb].len;
            bytes -= SEG_HDR;
            while (bytes >= PIECE + 1 && g->n < 64 && (bb != b || want[bb] > 0)) {
                int idx = s->sb[bb].next;
                s->sb[bb].next = (idx + 1) % RS_MAX_PIECES;   /* data, repair, then wrap */
                g->idx[g->n] = idx;
                piece_bytes(s, bb, idx, g->piece[g->n]);
                g->n++;
                bytes -= PIECE + 1;
                if (bb == b) want[bb]--;
            }
            if (bb != b) break;
        }
        fr[nf++] = x;
        while (b < s->nsb && want[b] <= 0) b++;
        if (b >= s->nsb) break;                  /* all wants met */
    }
    s->round_frames = nf;
    for (int i = 0; i < nf; i++) { fr[i]->round_frames = nf; fr[i]->left = nf - 1 - i; }
    transmit(s, fr, nf);
    uint64_t fb_wait = (s->tx_end - now_ms) + GUARD_MS + HEAD_MS + mode_air(FB_MODE) + TAIL_MS + FB_MARGIN_MS;
    arm(s, T_FB_TIMEOUT, now_ms + fb_wait);
}

static void on_feedback(station_t *s, const frame_t *f)
{
    if (!s->sender || !s->nsb || f->round_id != s->round_id) return;  /* stale */
    disarm(s, T_FB_TIMEOUT);
    s->timeouts = 0;
    s->loss_est = 0.5 * s->loss_est + 0.5 * (f->loss16 / 16.0);
    measure_level(s, s->round_level, f->loss16 / 16.0);
    choose_level(s);
    /* Update each open block; retire the complete ones. */
    int keep = 0, retired = 0;
    for (int b = 0; b < s->nsb; b++) {
        int off = (uint8_t)(s->sb[b].id - f->base);
        int need;
        if (off >= 128)          need = 0;                 /* behind the base: delivered */
        else if (off >= WIN)     need = s->sb[b].need;     /* not in its window yet */
        else if (f->need[off] == FB_UNSEEN) need = s->sb[b].K;
        else                     need = f->need[off];
        if (need == 0) { retired++; continue; }
        s->sb[b].need = need;
        if (keep != b) s->sb[keep] = s->sb[b];
        keep++;
    }
    s->nsb = keep;
    /* The peer has data: hand it the turn once ours has run a quantum -- at a
     * block boundary, or at any round once it has run two.  Holding the turn
     * until every open block finished starved the peer (the window takes a
     * whole 8 KB transfer at once, and on NVIS the peer never got the turn in
     * 30 minutes); yielding at every block boundary switched turns so often
     * that 25 % loss got 25 % slower.  Open blocks are only suspended: both
     * sides keep their state and carry on when the turn comes back. */
    bool more = s->nsb || s->tx_alloc < s->tx_len;
    uint64_t held = now_ms - s->turn_start;
    bool yield = f->has_data &&
                 ((retired && held >= TURN_QUANTUM_MS) || held >= 2 * TURN_QUANTUM_MS);
    if (more && !yield) {
        arm(s, T_SEND_ROUND, now_ms + ISS_GUARD_MS);
        return;
    }
    s->sender = false;                           /* done, or handing the turn over */
}

/* ---- receiver ------------------------------------------------------------- */
static void decode_block(station_t *s, int slot)
{
    int idx[RS_MAX_PIECES] = {0}; const uint8_t *pcs[RS_MAX_PIECES] = {0};
    static uint8_t outb[RS_MAX_PIECES][PIECE];
    uint8_t *out[RS_MAX_PIECES];
    int r = 0, K = s->rb[slot].K;
    for (int i = 0; i < RS_MAX_PIECES && r < K; i++)
        if (s->rb[slot].got[i]) { idx[r] = i; pcs[r] = s->rb[slot].piece[i]; r++; }
    for (int i = 0; i < K; i++) out[i] = outb[i];
    if (rs_decode(K, PIECE, idx, pcs, out) == 0)
        for (int i = 0; i < K; i++) memcpy(s->rb[slot].out + i * PIECE, out[i], PIECE);
    s->rb[slot].done = true;
}

/* Hand complete blocks to the application in order, sliding the window. */
static void deliver_in_order(station_t *s)
{
    for (;;) {
        int slot = s->rbase % WIN;
        if (!s->rb[slot].known || !s->rb[slot].done) break;
        memcpy(s->rx + s->rx_len, s->rb[slot].out, (size_t)s->rb[slot].len);
        s->rx_len += (size_t)s->rb[slot].len;
        memset(&s->rb[slot], 0, sizeof(s->rb[slot]));
        s->rbase++;
    }
}

static void send_feedback(station_t *s)
{
    frame_t *x = calloc(1, sizeof(*x));
    x->kind = 1; x->mode = FB_MODE;
    x->round_id = s->rx_round_id;
    x->base = s->rbase;
    for (int o = 0; o < WIN; o++) {
        int slot = (uint8_t)(s->rbase + o) % WIN;
        if (!s->rb[slot].known) x->need[o] = FB_UNSEEN;
        else if (s->rb[slot].done) x->need[o] = 0;
        else x->need[o] = s->rb[slot].K - s->rb[slot].have;
    }

    double loss = s->rx_round_frames ? 1.0 - (double)s->rx_round_seen / s->rx_round_frames : 0;
    if (loss < 0) loss = 0;
    x->loss16 = (int)lround(loss * 16.0);
    x->advice = loss == 0 ? 2 : (loss <= 0.25 ? 1 : (loss >= 0.5 ? -1 : 0));
    x->has_data = s->tx_alloc < s->tx_len || s->nsb > 0;
    frame_t *fr[1] = { x };
    transmit(s, fr, 1);
    s->fb_sent_end = s->tx_end;
    /* The sender yields at a block boundary when we have data.  Arm the
     * takeover after every feedback rather than only when we saw a block
     * complete: if that round's feedback was lost the sender still retires the
     * block and yields, and waiting for a completion we never reported left
     * both sides idle.  A sender that carries on cancels this with its next
     * round, and the takeover listens before keying. */
    if (x->has_data && !s->sender)
        arm(s, T_HANDOVER, s->tx_end + HANDOVER_MS);
}

static void on_data(station_t *s, const frame_t *f)
{
    if (s->sender) {                      /* both think they hold the turn */
        if (!s->nsb) s->sender = false; else return;
    }
    disarm(s, T_HANDOVER);                /* the peer is still sending */
    if (f->round_id != s->rx_round_id) {
        s->rx_round_id = f->round_id; s->rx_round_seen = 0; s->rx_round_frames = f->round_frames;
    }
    s->rx_round_seen++;
    for (int g = 0; g < f->nseg; g++) {
        const seg_t *sg = &f->seg[g];
        int off = (uint8_t)(sg->block - s->rbase);
        if (off >= WIN) continue;         /* delivered already, or beyond the window */
        int slot = sg->block % WIN;
        if (!s->rb[slot].known) {
            s->rb[slot].known = true;
            s->rb[slot].K = sg->K; s->rb[slot].len = sg->len;
        }
        if (s->rb[slot].done) continue;
        for (int p = 0; p < sg->n; p++) {
            int i = sg->idx[p];
            if (i >= 0 && i < RS_MAX_PIECES && !s->rb[slot].got[i] && s->rb[slot].have < s->rb[slot].K) {
                s->rb[slot].got[i] = true;
                memcpy(s->rb[slot].piece[i], sg->piece[p], PIECE);
                s->rb[slot].have++;
            }
        }
        if (s->rb[slot].have >= s->rb[slot].K) decode_block(s, slot);
    }
    deliver_in_order(s);
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
    if (!strncmp(chan, "fade:", 5)) {
        double m = 0, d = 0.5;
        sscanf(chan + 5, "%lf:%lf", &m, &d);
        sim_channel_set_fading(ch, m, d);
        snr_hint_db = m;
    }
    else if (!strncmp(chan, "awgn:", 5)) sim_channel_set_per(ch, atof(chan + 5));
    else if (!strncmp(chan, "cliff:", 6)) { sim_channel_set_snr(ch, atof(chan + 6)); snr_hint_db = atof(chan + 6); }
    else if (!strcmp(chan, "nvis")) {
        sim_channel_set_mode_per(ch, NVIS, (int)(sizeof(NVIS) / sizeof(NVIS[0])));
        snr_hint_db = 10.0;
    }
    if (getenv("CAR_NOHINT")) snr_hint_db = -99.0;

    for (int i = 0; i < 2; i++) {
        memset(&S[i], 0, sizeof(S[i]));
        S[i].id = i; S[i].loss_est = 0.1;
        S[i].level = hint_level(snr_hint_db);
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
                if (s->sender) send_round(s);
                break;
            case T_FB_TIMEOUT:
                /* No feedback: the round, or the reply, was lost whole.  In a
                 * mode past the cliff that is every round, and the receiver
                 * never learns a round happened -- so the sender must count a
                 * timeout as total loss, or it repeats the dead round forever. */
                if (s->sender && s->nsb) {
                    /* One timeout is ambiguous -- the round OR the reply was
                     * lost -- so it only nudges the estimate; two in a row at
                     * the same level is a dead mode. */
                    /* The feedback travels in the robust control mode, so a
                     * timeout is almost always the ROUND lost, not the reply:
                     * score it as total loss.  Scoring it as half let a dead
                     * fast mode (half of 100 B/s) outscore a working slow one
                     * (25 B/s) on the cliff channel, and whole rounds went
                     * into it. */
                    s->loss_est = 0.75 * s->loss_est + 0.25;
                    s->timeouts++;
                    measure_level(s, s->round_level, 1.0);
                    choose_level(s);
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
                s->turn_start = now_ms;
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
    sim_channel_destroy(ch);
    return (ok_a && ok_b) ? 0 : 2;
}
