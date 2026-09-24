/* tests/sim/carousel_bench.c -- prototype: an erasure-coded "carousel" ARQ on
 * the two-FSM sim's channel, to compare with the stop-and-wait ARQs.
 *
 *   carousel_bench <seed> <channel> [bidir]      (channel as in ab_bench)
 *
 * The receiver drives.  Mercury's modem cannot tell which mode a burst is in:
 * it runs a control decoder (DATAC16) and ONE payload decoder, bound to the
 * mode it expects.  So the side that listens chooses the mode -- and it is also
 * the side that sees what arrives.  Per direction:
 *
 *   - the driver (receiver) sends a POLL in the control mode: what each open
 *     block still needs, and "send me up to N frames in mode M";
 *   - the sender answers with a round -- one keydown of up to N back-to-back
 *     bursts in M -- and keys only when polled;
 *   - every frame says how many follow, so the driver knows when the round
 *     ends and polls again; a round that never comes is re-polled after its
 *     window.  Only the driver keeps a timer, so the two sides cannot race;
 *   - the driver takes the turn with a HANDOVER poll that also announces its
 *     own first round, sent in the same keydown: the peer's control decoder
 *     reads the poll and rebinds its payload decoder in the gap (as in #310).
 *
 * Data is cut into blocks of up to 96 pieces of 24 bytes, Reed-Solomon coded
 * over GF(256) (systematic: the first K pieces are the data, any K decode).
 * The pieces fit every mode, so a block never has to be re-encoded when the
 * mode changes.  No sequence numbers and no per-frame ACKs.
 *
 * Wire sizes (the bench keeps the fields in structs):
 *   DATA  3 B: frames left | unopened data ; poll id | highest block opened
 *              (both mod 16) ; the SNR measured on the peer's polls
 *         per block 2 B: block id (mod 16) | K | pad ; per piece 1 B index
 *   POLL  13 B in DATAC16: type, has-data, poll id, mode, N, loss, base,
 *              need of 8 blocks, SNR; on a handover the round's mode and length
 *
 * Channel, airtime and the half-duplex medium are the sim's (sim_channel.c +
 * the ARQ mode table), with the same guards Mercury uses and carrier sense
 * before every keying (as ab_bench's SIM_CS=400).
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
#define FRAME_HDR       3
#define SEG_HDR         2
#define WIN             8         /* blocks a round may carry at once       */
#ifndef SLOW_RUNG_FRAMES
#define SLOW_RUNG_FRAMES 3        /* round cap on the two slowest rungs      */
#endif
#ifndef TURN_QUANTUM_MS
#define TURN_QUANTUM_MS 120000    /* a turn may run this long before the peer's */
#endif
#define FB_UNSEEN       255       /* poll: no piece of this block yet       */
#ifndef MAX_KEYDOWN_MS
#define MAX_KEYDOWN_MS  30000     /* airtime cap per round                  */
#endif
#define HEAD_MS         110       /* tx delay + head silence                */
#define TAIL_MS         200
#define GUARD_MS        ARQ_CHANNEL_GUARD_MS_DEFAULT   /* reply guard         */
#define ISS_GUARD_MS    ARQ_ISS_POST_ACK_GUARD_MS_DEFAULT
#define CHAIN_GAP_MS    ARQ_ACK_DATA_GAP_MS   /* poll -> first burst, one keydown */
#ifndef BURST_GAP_MS
#define BURST_GAP_MS    0         /* between the bursts of a round           */
#endif
#define WINDOW_MARGIN_MS 1000     /* a round that never came: re-poll after this */
#define CS_ACQ_MS       400       /* carrier sense: sync this long after keying */
#define SENDER_SILENCE_MS 90000   /* sender heard no poll this long: nudge  */
#define SENSE_SLACK_MS  1000      /* sender should be keyed by now, or the poll was lost */
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
static uint64_t level_air(int lv) { return mode_air(LADDER[lv]); }
static uint64_t round_air(int lv, int n)
{
    return (uint64_t)n * level_air(lv) + (uint64_t)(n > 0 ? n - 1 : 0) * BURST_GAP_MS;
}

/* ---- frames and events ---------------------------------------------------- */
typedef struct {                   /* the pieces of one block in a data frame */
    uint8_t  block;
    int      K, len;
    int      n;
    int      idx[64];
    uint8_t  piece[64][PIECE];
} seg_t;

enum { F_DATA, F_POLL, F_STATUS };
typedef struct {
    int      kind;
    int      mode;
    int      snr_level;            /* where the SNR the sender measures of its peer starts the peer */
    /* data */
    int      level;                /* data: the level it is sent on */
    uint32_t poll_id;              /* data/status: the poll it answers; poll: its id */
    int      left;                 /* frames left in this round after this one */
    bool     unopened;             /* sender still has data not yet cut into blocks */
    uint8_t  hi;                   /* highest block the sender has opened */
    int      nseg;
    seg_t    seg[WIN];
    /* poll */
    int      n;                    /* send me up to n frames on 'level' (0: ack only) */
    int      loss16;
    uint8_t  base;                 /* first block of the receiver's window */
    int      need[WIN];            /* pieces still needed per block, FB_UNSEEN, 0 = done */
    bool     has_data;
    bool     handover;             /* I take the turn; my round follows in this keydown */
    int      h_level, h_n;
    uint32_t h_id;
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
enum { T_POLL, T_SEND, T_WAIT, T_SENSE, NTIMERS };
typedef struct {
    int      id;
    uint8_t  tx[XFER_BYTES];  size_t tx_len;
    uint8_t  rx[XFER_BYTES + 64]; size_t rx_len;
    bool     sending;                /* holds the turn */
    bool     idle;                   /* nothing in flight either way */
    int      rx_level;               /* what the payload decoder is bound to */
    int      snr_level;              /* where the SNR I measure of the peer starts it */

    /* ---- as sender ---- */
    struct {
        uint8_t id;
        int     K, len, next, need;          /* next piece index to send */
        uint8_t data[RS_MAX_PIECES][PIECE];
    } sb[WIN];
    int      nsb;
    uint8_t  next_blk_id;
    size_t   tx_alloc;               /* bytes already cut into blocks */
    int      tx_level, tx_n;         /* as the last poll directed (-1: never polled) */
    uint32_t tx_poll_id;
    double   tx_loss;                /* the driver's loss estimate, for the margin */
    bool     handover_unconfirmed;   /* my handover round is out, no poll yet */
    int      peer_snr_level;         /* where the SNR the peer measures of me starts me */

    /* ---- as driver: the peer's direction, measured here ---- */
    double   lv_sent[NLADDER];       /* frames sent on each level (decayed)  */
    double   lv_lost[NLADDER];       /* ...and lost (decayed)                */
    int      lv_rounds[NLADDER];     /* rounds measured on each level (0 = unknown) */
    int      lv_dead_run[NLADDER];   /* frames lost in a row, none delivered */
    uint32_t lv_round_at[NLADDER];   /* poll count at the last measurement */
    uint64_t lv_probe_at[NLADDER];   /* when a level was last measured */
    uint64_t lv_backoff_ms[NLADDER]; /* re-probe interval for a dead level */
    uint32_t polls;                  /* polls measured (probe staleness) */
    double   loss_est;
    uint32_t poll_id;                /* current poll (mod 16) */
    int      poll_level, poll_n;
    int      round_seen, round_frames; /* frames of the current poll's round */
    bool     round_heard;            /* carrier or a frame of it seen */
    int      silent_polls;           /* polls in a row the sender never keyed for */
    bool     status_seen;            /* the sender answered: nothing to send */
    bool     peer_unopened;          /* from the sender's latest frame */
    uint8_t  peer_hi; bool peer_hi_known;
    uint64_t drive_start;            /* when the peer's current turn began */
    struct {
        bool    known, done;
        int     K, len, have;
        bool    got[RS_MAX_PIECES];
        uint8_t piece[RS_MAX_PIECES][PIECE];
        uint8_t out[MAX_K * PIECE];
    } rb[WIN];
    uint8_t  rbase;                  /* next block to deliver */
    int      done_in_round;          /* blocks completed during the current round */

    /* medium */
    uint64_t tx_start, tx_end;       /* current/last transmission */
    uint32_t tgen[NTIMERS];
} station_t;

static station_t S[2];
static sim_channel_t *ch;
static int collisions;
static uint64_t now_ms;
static bool trace;                         /* CAR_TRACE: print every keydown */

static void arm(station_t *s, int timer, uint64_t at)
{
    event_t e = { .t = at, .type = EV_TIMER, .st = s->id, .timer = timer, .gen = ++s->tgen[timer] };
    push(e);
}
static void disarm(station_t *s, int timer) { s->tgen[timer]++; }

/* Carrier sense: the peer's keydown is heard once a decoder syncs on it. */
static bool peer_keyed(const station_t *s)
{
    const station_t *p = &S[s->id ^ 1];
    return p->tx_end > now_ms && p->tx_start + CS_ACQ_MS <= now_ms;
}
/* Listen before talk: true (and the timer re-armed) when we must wait. */
static bool defer_if_busy(station_t *s, int timer)
{
    if (!peer_keyed(s)) return false;
    arm(s, timer, S[s->id ^ 1].tx_end + GUARD_MS);
    return true;
}

/* Transmit a keydown of n frames from station s, starting now.  gap[i] is the
 * silence before frame i (i > 0); NULL for none. */
static void transmit(station_t *s, frame_t **fr, int n, const uint64_t *gap)
{
    station_t *peer = &S[s->id ^ 1];
    uint64_t t = now_ms + HEAD_MS;
    s->tx_start = now_ms;
    int fr_kind[65]; bool fr_hand[65];
    for (int i = 0; i < n && i < 65; i++) { fr_kind[i] = fr[i]->kind; fr_hand[i] = fr[i]->handover; }
    for (int i = 0; i < n; i++) {
        if (i && gap) t += gap[i];
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
    if (trace) {
        static const char *K[] = { "data", "poll", "status" };
        printf("%9.1f %c keys %.1f-%.1f:", now_ms / 1000.0, 'A' + s->id, now_ms / 1000.0, s->tx_end / 1000.0);
        for (int i = 0; i < n; i++)
            printf(" %s%s", K[fr_kind[i]], fr_kind[i] == F_POLL && fr_hand[i] ? "(handover)" : "");
        printf("\n");
    }
    event_t e = { .t = s->tx_end, .type = EV_TXEND, .st = s->id };
    push(e);
}

/* ---- sender --------------------------------------------------------------- *
 * One block per round held a round to ~2.3 KB (96 pieces of 24 bytes), so on a
 * fast mode every two or three frames paid a whole turnaround.  Bigger pieces
 * would have fixed that, but a block cut for a fast mode does not fit a slow
 * one: on a drop of two rungs it had to be re-encoded and its progress was
 * lost, which left runs unfinished.  Instead a round carries up to WIN blocks,
 * each still of small pieces that fit every mode, and the poll reports what
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

static bool has_data(const station_t *s) { return s->nsb > 0 || s->tx_alloc < s->tx_len; }

/* A poll's view of my blocks: retire the delivered ones.  Ids travel mod 16;
 * every open block lies within 8 of the receiver's base. */
static void apply_need(station_t *s, const frame_t *f)
{
    int keep = 0;
    for (int b = 0; b < s->nsb; b++) {
        int off = (uint8_t)(s->sb[b].id - f->base) & 0x0F;
        int need;
        if (off >= WIN)          need = 0;                 /* behind the base: delivered */
        else if (f->need[off] == FB_UNSEEN) need = s->sb[b].K;
        else                     need = f->need[off];
        if (need == 0) continue;
        s->sb[b].need = need;
        if (keep != b) s->sb[keep] = s->sb[b];
        keep++;
    }
    s->nsb = keep;
}

/* Build a round of at most n frames on level lv, answering poll_id.  Returns
 * the frame count (0: nothing to send). */
static int build_round(station_t *s, int lv, int n, uint32_t poll_id, frame_t **fr)
{
    while (s->nsb < WIN && s->tx_alloc < s->tx_len)
        open_block(s);
    if (!s->nsb || n < 1) return 0;
    int mode = LADDER[lv];
    int room = mode_payload(mode) - FRAME_HDR;

    /* What each open block still needs, with a margin for the loss seen. */
    double margin = s->tx_loss * 1.3 + 0.05;
    int want[WIN], want_total = 0;
    for (int b = 0; b < s->nsb; b++) {
        want[b] = (int)ceil(s->sb[b].need * (1.0 + margin));
        want_total += want[b];
    }
    int ppf = pieces_per_frame(mode);
    int frames = (want_total + ppf - 1) / ppf;
    if (frames > n) frames = n;
    if (frames < 1) frames = 1;

    int nf = 0, b = 0;
    while (nf < frames) {
        frame_t *x = calloc(1, sizeof(*x));
        x->kind = F_DATA; x->mode = mode; x->level = lv; x->poll_id = poll_id;
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
    for (int i = 0; i < nf; i++) {
        fr[i]->left = nf - 1 - i;
        fr[i]->unopened = s->tx_alloc < s->tx_len;
        fr[i]->hi = (uint8_t)(s->next_blk_id - 1);
        fr[i]->snr_level = s->snr_level;
    }
    return nf;
}

/* A sender waits for a poll: after a handover, until the first poll should
 * have come (then the handover is repeated); otherwise a long silence. */
static void arm_sender_wait(station_t *s)
{
    uint64_t t = s->handover_unconfirmed
               ? s->tx_end + GUARD_MS + HEAD_MS + mode_air(FB_MODE) + TAIL_MS + WINDOW_MARGIN_MS
               : s->tx_end + SENDER_SILENCE_MS;
    arm(s, T_WAIT, t);
}

static void send_round(station_t *s)
{
    frame_t *fr[64];
    int nf = build_round(s, s->tx_level, s->tx_n, s->tx_poll_id, fr);
    if (!nf) {
        frame_t *x = calloc(1, sizeof(*x));      /* polled with nothing to send */
        x->kind = F_STATUS; x->mode = FB_MODE; x->poll_id = s->tx_poll_id;
        x->hi = (uint8_t)(s->next_blk_id - 1); x->snr_level = s->snr_level;
        fr[0] = x; nf = 1;
    }
    uint64_t gap[64] = {0};
    for (int i = 1; i < nf; i++) gap[i] = BURST_GAP_MS;
    transmit(s, fr, nf, gap);
    arm_sender_wait(s);
}

/* ---- link adaptation: goodput, measured per level ------------------------ *
 * With erasure coding every piece received is useful, so a level's goodput is
 * its raw rate times the fraction that gets through.  Flat loss hits every
 * mode alike, so the fastest wins; past a cliff a mode delivers nothing and
 * drops out by itself.  Above the best, the first rung that COULD beat it (its
 * raw rate exceeds the best's measured goodput) is probed with a one-frame
 * round when it has never been measured or its measurement is stale -- a probe
 * costs one frame, a wrong climb costs a round.  Only rungs that could win:
 * the ladder is not monotonic in rate (DATAC4 carries one 24-byte piece in
 * 5.8 s, below DATAC15), and probing only the next rung stranded the sender
 * under it.  The driver runs this, on what it received. */
#define PROBE_EVERY   8     /* polls before a measured rung is re-probed...  */
#define DEAD_PROBE_MS     60000   /* a dead rung is re-probed after this...  */
#define DEAD_PROBE_MAX_MS 480000  /* ...doubling to at most this             */
#define LV_DECAY      0.8   /* per measured round: memory of ~5 frames */
#define DEAD_RUN      4     /* consecutive frames lost, none delivered: dead */

static double level_rate(int lv)          /* raw piece bytes per ms of airtime */
{
    return (double)(pieces_per_frame(LADDER[lv]) * PIECE) / (double)level_air(lv);
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

static void measure_level(station_t *s, int lv, int frames, double loss)
{
    s->lv_sent[lv] = LV_DECAY * s->lv_sent[lv] + frames;
    s->lv_lost[lv] = LV_DECAY * s->lv_lost[lv] + frames * loss;
    if (loss >= 1.0) s->lv_dead_run[lv] += frames;
    else             s->lv_dead_run[lv] = 0;
    s->lv_rounds[lv]++;
    s->lv_round_at[lv] = ++s->polls;
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
 * its poll both lost), and the ceiling then locked out a DATAC3 and a DATAC1
 * that were delivering -- the sender crawled on DATAC15 to the end.  Evidence
 * of delivery beats the inference. */
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

/* Nothing measured yet, the peer starts where the SNR measured here puts it
 * (see hint_level).  Only a start: the first round there is a one-frame
 * probe, and measured goodput decides from then on, so a misleading SNR
 * (ISI-limited NVIS reads 10 dB) costs a few frames.  Climbing by probes from
 * DATAC15 instead cost 25-40 s of airtime per turn on a 15 dB fading channel. */
static int choose_level(const station_t *s)
{
    int best = -1;
    double best_gp = -1.0;
    for (int lv = 0; lv < NLADDER; lv++) {
        if (!s->lv_rounds[lv] || !level_allowed(s, lv)) continue;
        double gp = level_rate(lv) * level_delivery(s, lv);
        if (gp > best_gp) { best_gp = gp; best = lv; }
    }
    if (best < 0)
        return s->lv_rounds[s->snr_level] ? 0 : s->snr_level;
    /* Nothing measured gets through: go down to a rung not tried yet. */
    if (best_gp <= 0.0) {
        for (int lv = best - 1; lv >= 0; lv--)
            if (!s->lv_rounds[lv]) return lv;
    }
    for (int up = best + 1; up < NLADDER; up++) {
        if (level_rate(up) <= best_gp)
            continue;                   /* cannot win even with no loss */
        if (!level_allowed(s, up)) {
            if (!reprobe_due(s, up)) break;
        } else if (s->lv_rounds[up] && s->polls - s->lv_round_at[up] < PROBE_EVERY) {
            break;                      /* measured and fresh: the argmax decided */
        }
        return up;                      /* a probe: its earned round is one frame */
    }
    return best;
}

/* The peer's blocks not yet delivered, from the base up to the highest it has
 * opened: -1 when unknown. */
static int peer_outstanding(const station_t *s)
{
    if (!s->peer_hi_known) return -1;
    int d = (int8_t)(s->peer_hi - s->rbase);
    return d < 0 ? 0 : d + 1;
}

/* How many frames to ask for on level lv: what it has earned (one frame more
 * than it recently delivered, so a probe is one frame, a proven mode gets full
 * rounds and a dead one never costs more than a frame), what fits a keydown,
 * and -- when every block of the sender is known here -- what is still needed. */
static int poll_size(const station_t *s, int lv)
{
    int cap = (int)(MAX_KEYDOWN_MS / level_air(lv));
    if (lv <= 1 && cap > SLOW_RUNG_FRAMES) cap = SLOW_RUNG_FRAMES;   /* slow rungs */
    int earned = 1 + (int)(s->lv_sent[lv] - s->lv_lost[lv]);
    if (cap > earned) cap = earned;
    int out = peer_outstanding(s);
    if (out >= 0 && out <= WIN && !s->peer_unopened) {
        double margin = s->loss_est * 1.3 + 0.05;
        int want = 0; bool all_known = true;
        for (int o = 0; o < out; o++) {
            int slot = (uint8_t)(s->rbase + o) % WIN;
            if (!s->rb[slot].known) { all_known = false; break; }
            if (!s->rb[slot].done) want += (int)ceil((s->rb[slot].K - s->rb[slot].have) * (1.0 + margin));
        }
        if (all_known) {
            int ppf = pieces_per_frame(LADDER[lv]);
            int need = (want + ppf - 1) / ppf;
            if (need < cap) cap = need;
        }
    }
    return cap < 1 ? 1 : cap;
}

/* ---- driver --------------------------------------------------------------- */
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
    s->done_in_round++;
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

/* Everything the peer opened has been delivered, and it has nothing unopened. */
static bool peer_direction_done(const station_t *s)
{
    return s->status_seen || (!s->peer_unopened && peer_outstanding(s) == 0);
}

static frame_t *make_poll(station_t *s)
{
    frame_t *x = calloc(1, sizeof(*x));
    x->kind = F_POLL; x->mode = FB_MODE;
    x->base = s->rbase;
    for (int o = 0; o < WIN; o++) {
        int slot = (uint8_t)(s->rbase + o) % WIN;
        if (!s->rb[slot].known) x->need[o] = FB_UNSEEN;
        else if (s->rb[slot].done) x->need[o] = 0;
        else x->need[o] = s->rb[slot].K - s->rb[slot].have;
    }
    x->loss16 = (int)lround(s->loss_est * 15.0);
    x->has_data = has_data(s);
    x->snr_level = s->snr_level;
    return x;
}

/* The current poll's round is over by 'end': poll then. */
static void arm_poll(station_t *s, uint64_t end)
{
    arm(s, T_POLL, end + GUARD_MS);
}

static void start_driving(station_t *s, uint32_t poll_id, int lv, int n, uint64_t round_start)
{
    s->sending = false;
    s->idle = false;
    s->poll_id = poll_id; s->poll_level = lv; s->poll_n = n;
    s->rx_level = lv;
    s->round_seen = 0; s->round_frames = n; s->status_seen = false;
    s->round_heard = true;
    s->done_in_round = 0;
    s->drive_start = now_ms;
    disarm(s, T_SEND); disarm(s, T_WAIT);
    arm_poll(s, round_start + round_air(lv, n) + TAIL_MS + WINDOW_MARGIN_MS);
}

/* Take the turn: the handover poll and my first round, in one keydown. */
static void send_handover(station_t *s)
{
    frame_t *fr[65];
    frame_t *p = make_poll(s);
    p->handover = true;
    s->sending = true;
    s->idle = false;
    s->tx_poll_id = (s->tx_poll_id + 1) & 0x0F;
    /* Where the peer last put me, or where the SNR it measures of me starts me. */
    int lv = s->tx_level >= 0 ? s->tx_level : s->peer_snr_level;
    int n = s->tx_n > 0 ? s->tx_n : 1;
    int nd = build_round(s, lv, n, s->tx_poll_id, fr + 1);
    p->h_level = lv; p->h_n = nd; p->h_id = s->tx_poll_id;
    fr[0] = p;
    uint64_t gap[65] = {0};
    gap[1] = CHAIN_GAP_MS;
    for (int i = 2; i <= nd; i++) gap[i] = BURST_GAP_MS;
    s->handover_unconfirmed = true;
    disarm(s, T_POLL);
    transmit(s, fr, 1 + nd, gap);
    arm_sender_wait(s);
}

static void send_poll(station_t *s, int lv, int n)
{
    frame_t *p = make_poll(s);
    s->poll_id = (s->poll_id + 1) & 0x0F;
    p->poll_id = s->poll_id;
    p->level = lv; p->n = n;
    if (n) { s->poll_level = lv; s->poll_n = n; s->rx_level = lv; }
    s->round_seen = 0; s->round_frames = n; s->status_seen = false; s->done_in_round = 0;
    s->round_heard = false;
    frame_t *fr[1] = { p };
    transmit(s, fr, 1, NULL);
    if (!n) return;
    uint64_t start = s->tx_end - TAIL_MS + ISS_GUARD_MS;   /* poll heard, guard, key */
    arm(s, T_SENSE, start + CS_ACQ_MS + SENSE_SLACK_MS);
    arm_poll(s, start + HEAD_MS + round_air(lv, n) + TAIL_MS + WINDOW_MARGIN_MS);
}

/* The poll timer: the round is over (or never came).  Measure it, then poll,
 * take the turn, or go quiet. */
static void on_poll_timer(station_t *s)
{
    if (s->sending || s->idle) return;
    if (defer_if_busy(s, T_POLL)) return;
    if (!s->status_seen) {
        int frames = s->round_frames > 0 ? s->round_frames : 1;
        double loss = 1.0 - (double)s->round_seen / frames;
        if (loss < 0) loss = 0;
        s->loss_est = 0.5 * s->loss_est + 0.5 * loss;
        measure_level(s, s->poll_level, frames, loss);
    }
    deliver_in_order(s);

    bool done = peer_direction_done(s);
    uint64_t held = now_ms - s->drive_start;
    bool quantum = (s->done_in_round && held >= TURN_QUANTUM_MS) || held >= 2 * TURN_QUANTUM_MS;
    if (has_data(s) && (done || quantum)) {
        /* When the peer still has data its open blocks are only suspended:
         * both sides keep their state and carry on when the turn comes back. */
        send_handover(s);
        return;
    }
    if (done) {
        s->idle = true;
        send_poll(s, 0, 0);                  /* ack only: nothing left either way */
        return;
    }
    int lv = choose_level(s);
    send_poll(s, lv, poll_size(s, lv));
}

/* The sense timer: by now the sender should be keyed.  Silence means it never
 * heard the poll, so poll again at once rather than sit out the round's window
 * -- and do not score the mode for it.  Silence on a second poll in a row is
 * left to the window and scored: a mode too weak even to sync must still be
 * found dead. */
static void on_sense_timer(station_t *s)
{
    if (s->sending || s->idle || s->round_heard) return;
    if (peer_keyed(s)) {
        /* The sender is on the air: poll when its carrier drops (sync lost for
         * CS_ACQ_MS), not at the window computed for the round we asked for.
         * The keydown may be something else -- a repeated handover -- and a
         * window timer ran into the sender's repeat timer (NVIS seed 28).
         * Decoded frames re-arm this from what they say. */
        s->round_heard = true; s->silent_polls = 0;
        arm_poll(s, S[s->id ^ 1].tx_end + CS_ACQ_MS);
        return;
    }
    if (++s->silent_polls >= 2) return;
    disarm(s, T_POLL);
    send_poll(s, s->poll_level, s->poll_n);
}

static void take_frame_pieces(station_t *s, const frame_t *f)
{
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
    s->peer_unopened = f->unopened;
    s->peer_hi = f->hi; s->peer_hi_known = true;
    s->peer_snr_level = f->snr_level;
}

static void on_data(station_t *s, const frame_t *f)
{
    if (f->level != s->rx_level) return;  /* the payload decoder is on another mode */
    if (s->sending) {
        /* Data while we hold the turn: the peer took it with a handover poll
         * we missed.  Follow it -- our blocks stay suspended. */
        s->handover_unconfirmed = false;
        start_driving(s, f->poll_id, f->level, f->left + 1, now_ms - level_air(f->level));
    }
    s->idle = false;
    s->round_heard = true; s->silent_polls = 0;
    take_frame_pieces(s, f);
    if (f->poll_id == s->poll_id) {
        s->round_seen++;
        s->round_frames = s->round_seen + f->left;   /* the frames say how many there are */
    }
    deliver_in_order(s);
    uint64_t end = now_ms + (uint64_t)f->left * (level_air(f->level) + BURST_GAP_MS) + TAIL_MS;
    arm_poll(s, end);
}

static void on_poll(station_t *s, const frame_t *f)
{
    s->peer_snr_level = f->snr_level;
    s->handover_unconfirmed = false;
    if (f->handover) {
        /* The peer takes the turn; its round follows in this keydown. */
        apply_need(s, f);
        start_driving(s, f->h_id, f->h_level, f->h_n, now_ms + CHAIN_GAP_MS);
        return;
    }
    /* A poll of my direction.  If I thought I was driving, the peer never
     * heard my handover and still polls me: be the sender again. */
    disarm(s, T_POLL);
    apply_need(s, f);
    s->tx_poll_id = f->poll_id;
    s->tx_loss = f->loss16 / 15.0;
    disarm(s, T_WAIT);
    if (f->n == 0) {                          /* ack only: the peer is done with us */
        s->sending = false;
        if (!has_data(s)) { s->idle = true; return; }
        s->tx_n = 1;
        arm(s, T_SEND, now_ms + ISS_GUARD_MS);
        return;
    }
    s->sending = true;
    s->idle = false;
    s->tx_level = f->level; s->tx_n = f->n;
    s->rx_level = f->level;                   /* where the peer's data will come too */
    arm(s, T_SEND, now_ms + ISS_GUARD_MS);
}

static void on_status(station_t *s, const frame_t *f)
{
    if (s->sending) return;
    if (f->poll_id == s->poll_id) s->status_seen = true;
    s->round_heard = true; s->silent_polls = 0;
    s->peer_hi = f->hi; s->peer_hi_known = true; s->peer_unopened = false;
    arm_poll(s, now_ms);
}

/* ---- run ------------------------------------------------------------------ */
static double snr_hint_db = 12.0;          /* what the sim stamps on frames */

/* The start rung for an SNR, mapped the way trunk enters a mode from DATAC15
 * (threshold plus hysteresis). */
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
    trace = getenv("CAR_TRACE") != NULL;

    for (int i = 0; i < 2; i++) {
        memset(&S[i], 0, sizeof(S[i]));
        S[i].id = i; S[i].loss_est = 0.1; S[i].tx_loss = 0.1;
        S[i].snr_level = S[i].peer_snr_level = hint_level(snr_hint_db);
        S[i].tx_level = -1;
    }
    for (int i = 0; i < XFER_BYTES; i++) {
        S[0].tx[i] = (uint8_t)((i * 13 + 7) & 0xFF);
        S[1].tx[i] = (uint8_t)((i * 29 + 3) & 0xFF);
    }
    S[0].tx_len = XFER_BYTES;
    S[1].tx_len = bidir ? XFER_BYTES : 0;

    /* The session is connected (as in ab_bench, the connect is not measured):
     * B's ACCEPT was the first poll, a one-frame probe on the rung the SNR it
     * measured on the CALL supports.  A answers now. */
    now_ms = 0;
    int lv0 = S[1].snr_level;
    S[0].sending = true;
    S[0].tx_level = lv0; S[0].tx_n = 1; S[0].tx_poll_id = 1;
    start_driving(&S[1], 1, lv0, 1, HEAD_MS);
    arm(&S[0], T_SEND, 0);

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
                if (trace) printf("%9.1f COLLISION at %c (kind %d)\n", now_ms / 1000.0, 'A' + s->id, e.f->kind);
                free(e.f);
                continue;
            }
            switch (e.f->kind) {
            case F_DATA:   on_data(s, e.f); break;
            case F_POLL:   on_poll(s, e.f); break;
            case F_STATUS: on_status(s, e.f); break;
            }
            free(e.f);
        } else if (e.type == EV_TIMER) {
            if (e.gen != s->tgen[e.timer]) continue;         /* disarmed/re-armed */
            switch (e.timer) {
            case T_POLL:
                on_poll_timer(s);
                break;
            case T_SENSE:
                on_sense_timer(s);
                break;
            case T_SEND:
                if (defer_if_busy(s, T_SEND)) break;
                s->sending = true;
                send_round(s);
                break;
            case T_WAIT:
                /* No poll.  After a handover the peer may have missed it:
                 * repeat it (fresh pieces are never wasted).  Otherwise the
                 * driver has gone quiet: nudge it with a round. */
                if (!s->sending) break;
                if (defer_if_busy(s, T_WAIT)) break;
                if (s->handover_unconfirmed) send_handover(s);
                else send_round(s);
                break;
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
