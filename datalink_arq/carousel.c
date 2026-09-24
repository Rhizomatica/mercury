/* datalink_arq/carousel.c -- the carousel data plane (see carousel.h)
 *
 * The design was chosen on tests/sim/carousel_bench.c, which runs this module;
 * tests/sim/README.md has the measurements behind each choice.
 *
 * Wire formats (no compatibility with the stop-and-wait ARQ is kept):
 *
 *   DATA, in a payload mode, filling the frame:
 *     b0  frames left in the round (4) | data not yet cut into blocks (1) |
 *         the rung the SNR measured on the peer's polls starts it on (3)
 *     b1  poll id (4) | highest block opened, mod 16 (4)
 *     then segments, one per block: 4 bytes
 *         block id mod 16 (4) | K - 1 (7) | piece count (6) | first piece
 *         index (8) | pad bytes in the block's last data piece (5) | 0 (2)
 *       and count pieces with consecutive indices (mod 256).  A zero count
 *       ends the frame.
 *   POLL / HANDOVER / STATUS, in the control mode, CAR_POLL_BYTES:
 *     b0  type (2) | has data (1) | poll id (4) | 0
 *     b1  level (3) | frames asked for (4) | 0
 *     b2  loss (4) | window base, mod 16 (4)
 *     b3..b10  pieces each block from the base still needs (255: none seen)
 *     b11 the start rung for the peer (3) | handover round's level (3) | 0 (2)
 *     b12 handover round's frames (4) | its id (4)
 *     b13 highest block opened (4) | data not yet cut (1) | 0 (3)
 *   The session id costs no bytes: the modem seeds the frame CRC with it.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "carousel.h"
#include "arq_protocol.h"
#include "freedv_api.h"

#include <math.h>
#include <string.h>

/* ---- parameters ---------------------------------------------------------- *
 * Data pieces per block (CAR_MAX_K): K + repair <= 256, so K is kept well
 * under it.  A sender that only knows HOW MANY pieces the receiver lacks --
 * not which -- must send pieces it has never sent, or it may resend ones the
 * receiver already has; at K = 255 there was one repair piece, and after a few
 * lost rounds the indices wrapped and rounds delivered nothing new. */
#define FRAME_HDR        2
#define SEG_HDR          4
#define FB_UNSEEN        255
#define SLOW_RUNG_FRAMES 3        /* round cap on the two slowest rungs      */
#define TURN_QUANTUM_MS  120000   /* a turn may run this long before the peer's */
#define MAX_KEYDOWN_MS   30000    /* airtime cap per round                  */
#define HEAD_MS          110      /* tx delay + head silence                */
#define TAIL_MS          200
#define GUARD_MS         ARQ_CHANNEL_GUARD_MS_DEFAULT
#define ISS_GUARD_MS     ARQ_ISS_POST_ACK_GUARD_MS_DEFAULT
#define CHAIN_GAP_MS     ARQ_ACK_DATA_GAP_MS   /* poll -> first burst, one keydown */
/* Between the bursts of a round (utils/burst_train_probe).  With no gap the
 * payload decoder misses bursts after the first: DATAC15 decoded 72/120 at
 * -5 dB AWGN against 120/120 alone, and DATAC3 and QAM16C2 lost frames under
 * fading.  At 100 ms every mode matched isolated bursts, except QAM16C2 on
 * ITU moderate fading, which still lost 6 % (297/360 against 316) and needs
 * 200 ms (324/328).  Gap is airtime: 200 ms everywhere cost up to 7 % on the
 * sim's 8 dB fading channel. */
#define WINDOW_MARGIN_MS 1000     /* a round that never came: re-poll after this */
#define SENSE_MS         1400     /* after keying, the sender is heard by now */
#define CARRIER_CHECK_MS 250      /* carrier re-checked this often */
#define SENDER_SILENCE_MS 90000   /* sender heard no poll this long: nudge  */

enum { M_DATA, M_POLL, M_HANDOVER, M_STATUS };
enum { AFTER_NONE, AFTER_POLL, AFTER_ROUND };

static const int LADDER[CAR_NLEVELS] = {
    FREEDV_MODE_DATAC15, FREEDV_MODE_DATAC4, FREEDV_MODE_DATAC3,
    FREEDV_MODE_DATAC1, FREEDV_MODE_DATAC17, FREEDV_MODE_QAM16C2,
};

static uint32_t burst_gap_ms(int lv) { return LADDER[lv] == FREEDV_MODE_QAM16C2 ? 200 : 100; }
int car_level_mode(int level) { return LADDER[level]; }
bool car_is_sending(const car_t *c) { return c->sending; }
bool car_is_idle(const car_t *c) { return c->idle; }

static int level_of_mode(int mode)
{
    for (int lv = 0; lv < CAR_NLEVELS; lv++)
        if (LADDER[lv] == mode) return lv;
    return -1;
}

static int mode_payload(int mode)
{
    const arq_mode_timing_t *tm = arq_protocol_mode_timing(mode);
    return tm ? tm->payload_bytes : CAR_POLL_BYTES;
}
static uint64_t mode_air(int mode)
{
    const arq_mode_timing_t *tm = arq_protocol_mode_timing(mode);
    return tm ? (uint64_t)(tm->frame_duration_s * 1000.0f + 0.5f) : 4400;
}
static uint64_t level_air(int lv) { return mode_air(LADDER[lv]); }
static int pieces_per_frame(int lv) { return (mode_payload(LADDER[lv]) - FRAME_HDR - SEG_HDR) / CAR_PIECE; }
static uint64_t round_air(int lv, int n)
{
    return (uint64_t)n * level_air(lv) + (uint64_t)(n > 0 ? n - 1 : 0) * burst_gap_ms(lv);
}

/* ---- messages ------------------------------------------------------------ */
typedef struct {
    uint8_t block;       /* mod 16 on the wire; full id after resolution */
    int     K, count, first, pad;
    const uint8_t *pieces;
} seg_t;

typedef struct {
    int      type;
    int      snr_level;
    uint32_t poll_id;
    int      left;
    bool     unopened;
    uint8_t  hi;          /* mod 16 */
    int      nseg;
    seg_t    seg[CAR_WIN];
    int      level, n, loss16;
    uint8_t  base;        /* mod 16 */
    int      need[CAR_WIN];
    bool     has_data;
    int      h_level, h_n;
    uint32_t h_id;
} msg_t;

static void encode_ctl(const msg_t *m, uint8_t *b)
{
    memset(b, 0, CAR_POLL_BYTES);
    int type = m->type == M_POLL ? 0 : m->type == M_HANDOVER ? 1 : 2;
    b[0] = (uint8_t)(type << 6 | (m->has_data ? 1 : 0) << 5 | (m->poll_id & 0x0F) << 1);
    b[1] = (uint8_t)((m->level & 7) << 5 | (m->n & 0x0F) << 1);
    b[2] = (uint8_t)((m->loss16 & 0x0F) << 4 | (m->base & 0x0F));
    for (int o = 0; o < CAR_WIN; o++) b[3 + o] = (uint8_t)m->need[o];
    b[11] = (uint8_t)((m->snr_level & 7) << 5 | (m->h_level & 7) << 2);
    b[12] = (uint8_t)((m->h_n & 0x0F) << 4 | (m->h_id & 0x0F));
    b[13] = (uint8_t)((m->hi & 0x0F) << 4 | (m->unopened ? 1 : 0) << 3);
}

static bool decode_ctl(const uint8_t *b, size_t len, msg_t *m)
{
    if (len < CAR_POLL_BYTES) return false;
    memset(m, 0, sizeof(*m));
    int type = b[0] >> 6;
    if (type > 2) return false;
    m->type = type == 0 ? M_POLL : type == 1 ? M_HANDOVER : M_STATUS;
    m->has_data = (b[0] >> 5) & 1;
    m->poll_id = (b[0] >> 1) & 0x0F;
    m->level = b[1] >> 5;
    m->n = (b[1] >> 1) & 0x0F;
    m->loss16 = b[2] >> 4;
    m->base = b[2] & 0x0F;
    for (int o = 0; o < CAR_WIN; o++) m->need[o] = b[3 + o];
    m->snr_level = b[11] >> 5;
    m->h_level = (b[11] >> 2) & 7;
    m->h_n = b[12] >> 4;
    m->h_id = b[12] & 0x0F;
    m->hi = b[13] >> 4;
    m->unopened = (b[13] >> 3) & 1;
    if (m->level >= CAR_NLEVELS || m->h_level >= CAR_NLEVELS || m->snr_level >= CAR_NLEVELS) return false;
    return true;
}

static bool decode_data(const uint8_t *b, size_t len, msg_t *m)
{
    if (len < FRAME_HDR) return false;
    memset(m, 0, sizeof(*m));
    m->type = M_DATA;
    m->left = b[0] >> 4;
    m->unopened = (b[0] >> 3) & 1;
    m->snr_level = b[0] & 7;
    m->poll_id = b[1] >> 4;
    m->hi = b[1] & 0x0F;
    if (m->snr_level >= CAR_NLEVELS) return false;
    size_t pos = FRAME_HDR;
    while (pos + SEG_HDR <= len && m->nseg < CAR_WIN) {
        uint32_t h = (uint32_t)b[pos] << 24 | (uint32_t)b[pos + 1] << 16 | (uint32_t)b[pos + 2] << 8 | b[pos + 3];
        seg_t *g = &m->seg[m->nseg];
        g->block = (uint8_t)(h >> 28);
        g->K = (int)((h >> 21) & 0x7F) + 1;
        g->count = (int)((h >> 15) & 0x3F);
        g->first = (int)((h >> 7) & 0xFF);
        g->pad = (int)((h >> 2) & 0x1F);
        if (!g->count) break;
        pos += SEG_HDR;
        if (g->K > CAR_MAX_K || g->pad >= CAR_PIECE || pos + (size_t)g->count * CAR_PIECE > len) return false;
        g->pieces = b + pos;
        pos += (size_t)g->count * CAR_PIECE;
        m->nseg++;
    }
    return true;
}

static void put_seg_hdr(uint8_t *p, const car_sblock_t *s, int count, int first)
{
    int pad = s->K * CAR_PIECE - s->len;
    uint32_t h = (uint32_t)(s->id & 0x0F) << 28 | (uint32_t)(s->K - 1) << 21 |
                 (uint32_t)count << 15 | (uint32_t)(first & 0xFF) << 7 | (uint32_t)pad << 2;
    p[0] = (uint8_t)(h >> 24); p[1] = (uint8_t)(h >> 16); p[2] = (uint8_t)(h >> 8); p[3] = (uint8_t)h;
}

/* A block id sent mod 16, as the full id nearest the window base. */
static uint8_t resolve16(uint8_t base, int v4)
{
    int d = (v4 - base) & 0x0F;
    if (d >= 8) d -= 16;
    return (uint8_t)(base + d);
}

/* ---- timers and the medium ----------------------------------------------- */
static void arm(car_t *c, int t, uint64_t at) { c->deadline[t] = at ? at : 1; }
static void disarm(car_t *c, int t) { c->deadline[t] = 0; }

uint64_t car_next_deadline(const car_t *c)
{
    uint64_t d = UINT64_MAX;
    for (int t = 0; t < CAR_NTIMERS; t++)
        if (c->deadline[t] && c->deadline[t] < d) d = c->deadline[t];
    return d;
}

static bool peer_keyed(car_t *c, uint64_t now)
{
    bool k = c->io.peer_keyed && c->io.peer_keyed(c->io.ctx);
    if (k) c->last_carrier_ms = now;
    return k;
}

/* Listen before talk: true (and the timer re-armed) when we must wait for the
 * peer to be off the air for a guard. */
static bool defer_if_busy(car_t *c, int t, uint64_t now)
{
    if (c->tx_busy || peer_keyed(c, now)) { arm(c, t, now + CARRIER_CHECK_MS); return true; }
    if (c->last_carrier_ms && now < c->last_carrier_ms + GUARD_MS) {
        arm(c, t, c->last_carrier_ms + GUARD_MS);
        return true;
    }
    return false;
}

static void keydown(car_t *c, car_frame_t *fr, int n, int after)
{
    c->tx_busy = true;
    c->after_tx = after;
    c->io.keydown(c->io.ctx, fr, n);
}

static void bind_rx(car_t *c, int lv)
{
    c->rx_level = lv;
    if (c->io.bind_rx) c->io.bind_rx(c->io.ctx, LADDER[lv]);
}

/* ---- sender --------------------------------------------------------------- *
 * One block per round held a round to ~2.3 KB, so on a fast mode every two or
 * three frames paid a whole turnaround.  Bigger pieces would have fixed that,
 * but a block cut for a fast mode does not fit a slow one: on a drop of two
 * rungs it had to be re-encoded and its progress was lost, which left runs
 * unfinished.  Instead a round carries up to CAR_WIN blocks, each of small
 * pieces that fit every mode, and the poll reports what each still needs. */
static bool has_data(const car_t *c)
{
    return c->nsb > 0 || (c->io.tx_pending && c->io.tx_pending(c->io.ctx) > 0);
}

static bool unopened(const car_t *c)
{
    return c->io.tx_pending && c->io.tx_pending(c->io.ctx) > 0;
}

static void open_block(car_t *c)
{
    uint8_t buf[CAR_MAX_K * CAR_PIECE];
    size_t len = c->io.tx_read(c->io.ctx, buf, sizeof(buf));
    if (!len) return;
    car_sblock_t *s = &c->sb[c->nsb++];
    s->id = c->next_blk_id++;
    s->len = (int)len;
    s->K = (int)((len + CAR_PIECE - 1) / CAR_PIECE);
    s->next = 0;
    s->need = s->K;
    memset(s->data, 0, sizeof(s->data));
    memcpy(s->data, buf, len);
}

static void piece_bytes(const car_sblock_t *s, int idx, uint8_t *out)
{
    if (idx < s->K) { memcpy(out, s->data[idx], CAR_PIECE); return; }
    const uint8_t *d[CAR_MAX_K];
    for (int i = 0; i < s->K; i++) d[i] = s->data[i];
    rs_encode_repair(s->K, CAR_PIECE, d, idx - s->K, out);
}

/* A poll's view of my blocks: retire the delivered ones.  Ids travel mod 16;
 * every open block lies within CAR_WIN of the receiver's base. */
static void apply_need(car_t *c, const msg_t *m)
{
    int keep = 0;
    size_t retired = 0;
    for (int b = 0; b < c->nsb; b++) {
        int off = (c->sb[b].id - m->base) & 0x0F;
        int need;
        if (off >= CAR_WIN)                need = 0;   /* behind the base: delivered */
        else if (m->need[off] == FB_UNSEEN) need = c->sb[b].K;
        else                               need = m->need[off];
        if (need == 0) { retired += (size_t)c->sb[b].len; continue; }
        c->sb[b].need = need;
        if (keep != b) c->sb[keep] = c->sb[b];
        keep++;
    }
    c->nsb = keep;
    if (retired && c->io.tx_confirmed) c->io.tx_confirmed(c->io.ctx, retired);
}

/* Build a round of at most n frames on level lv, answering poll_id, into
 * fr[].  Returns the frame count (0: nothing to send). */
static int build_round(car_t *c, int lv, int n, uint32_t poll_id, car_frame_t *fr)
{
    while (c->nsb < CAR_WIN && unopened(c))
        open_block(c);
    if (!c->nsb || n < 1) return 0;
    int mode = LADDER[lv];
    int room = mode_payload(mode);

    /* What each open block still needs, with a margin for the loss seen. */
    double margin = c->tx_loss * 1.3 + 0.05;
    int want[CAR_WIN], want_total = 0;
    for (int b = 0; b < c->nsb; b++) {
        want[b] = (int)ceil(c->sb[b].need * (1.0 + margin));
        want_total += want[b];
    }
    int ppf = pieces_per_frame(lv);
    int frames = (want_total + ppf - 1) / ppf;
    if (frames > n) frames = n;
    if (frames < 1) frames = 1;

    int nf = 0, b = 0;
    while (nf < frames) {
        car_frame_t *x = &fr[nf];
        memset(x->bytes, 0, (size_t)room);
        x->mode = mode; x->len = (size_t)room;
        x->gap_ms = nf ? burst_gap_ms(lv) : 0;
        int pos = FRAME_HDR, nseg = 0;
        uint8_t last_block = 0;
        /* Fill the frame oldest block first; a block's pieces go on past its
         * want (as extra repair) only when nothing else is left to send. */
        while (room - pos >= SEG_HDR + CAR_PIECE && nseg < CAR_WIN) {
            while (b < c->nsb && want[b] <= 0) b++;
            int bb = b;
            if (bb >= c->nsb) {
                /* every want met: top the frame up with repair for the oldest */
                bb = 0;
                if (nseg && last_block == c->sb[0].id) break;
            }
            car_sblock_t *s = &c->sb[bb];
            int hdr = pos, count = 0, first = s->next;
            pos += SEG_HDR;
            while (room - pos >= CAR_PIECE && count < 63 && (bb != b || want[bb] > 0)) {
                piece_bytes(s, s->next, x->bytes + pos);
                s->next = (s->next + 1) % RS_MAX_PIECES;   /* data, repair, then wrap */
                pos += CAR_PIECE;
                count++;
                if (bb == b) want[bb]--;
            }
            put_seg_hdr(x->bytes + hdr, s, count, first);
            last_block = s->id;
            nseg++;
            if (bb != b) break;
        }
        nf++;
        while (b < c->nsb && want[b] <= 0) b++;
        if (b >= c->nsb) break;                  /* all wants met */
    }
    for (int i = 0; i < nf; i++) {
        fr[i].bytes[0] = (uint8_t)((nf - 1 - i) << 4 | (unopened(c) ? 1 : 0) << 3 | (c->snr_level & 7));
        fr[i].bytes[1] = (uint8_t)((poll_id & 0x0F) << 4 | ((c->next_blk_id - 1) & 0x0F));
    }
    return nf;
}

/* A sender waits for a poll: after a handover, until the first poll should
 * have come (then the handover is repeated); otherwise a long silence. */
static void arm_sender_wait(car_t *c, uint64_t tx_end)
{
    uint64_t t = c->handover_unconfirmed
               ? tx_end + GUARD_MS + HEAD_MS + mode_air(ARQ_CONTROL_MODE) + TAIL_MS + WINDOW_MARGIN_MS
               : tx_end + SENDER_SILENCE_MS;
    arm(c, CAR_T_WAIT, t);
}

static void send_round(car_t *c)
{
    car_frame_t *fr = c->txbuf;
    int nf = build_round(c, c->tx_level, c->tx_n, c->tx_poll_id, fr);
    if (!nf) {
        msg_t m;                                  /* polled with nothing to send */
        memset(&m, 0, sizeof(m));
        m.type = M_STATUS; m.poll_id = c->tx_poll_id;
        m.hi = (uint8_t)(c->next_blk_id - 1); m.snr_level = c->snr_level;
        encode_ctl(&m, fr[0].bytes);
        fr[0].mode = ARQ_CONTROL_MODE; fr[0].len = CAR_POLL_BYTES; fr[0].gap_ms = 0;
        nf = 1;
    }
    keydown(c, fr, nf, AFTER_ROUND);
}

/* ---- link adaptation: goodput, measured per level ------------------------ *
 * With erasure coding every piece received is useful, so a level's goodput is
 * its raw rate times the fraction that gets through.  Flat loss hits every
 * mode alike, so the fastest wins; past a cliff a mode delivers nothing and
 * drops out by itself.  Above the best, the first rung that COULD beat it (its
 * raw rate exceeds the best's measured goodput) is probed with a one-frame
 * round when it has never been measured or its measurement is stale -- a probe
 * costs one frame, a wrong climb costs a round.  Only rungs that could win:
 * the ladder is not monotonic in rate (DATAC4 is below DATAC15), and probing
 * only the next rung stranded the sender under it.  The receiver runs this, on
 * what it received. */
#define PROBE_EVERY   8     /* polls before a measured rung is re-probed...  */
#define DEAD_PROBE_MS     60000   /* a dead rung is re-probed after this...  */
#define DEAD_PROBE_MAX_MS 480000  /* ...doubling to at most this             */
#define LV_DECAY      0.8   /* per measured round: memory of ~5 frames */
#define DEAD_RUN      4     /* consecutive frames lost, none delivered: dead */

static double level_rate(int lv)          /* raw piece bytes per ms of airtime */
{
    return (double)(pieces_per_frame(lv) * CAR_PIECE) / (double)level_air(lv);
}

/* Delivery estimated from frame counts, not from single rounds: at 25 % flat
 * loss a quarter of one-frame probes vanish whole, and judging a level on one
 * of them condemned a working mode.
 *
 * One estimator cannot both explore and avoid dead modes when raw rates span
 * 60x: an optimistic prior kept a dead QAM16C2 outscoring a working DATAC3 on
 * the cliff for 100+ rounds, and a pessimistic one never climbed on a clean
 * channel.  So the two jobs are split, using what HF modes guarantee -- a
 * faster mode needs more SNR, so above a dead mode everything is dead too:
 *   - dead is a hard verdict: DEAD_RUN frames lost in a row with nothing
 *     delivered (0.4 % by chance at 25 % flat loss; a decayed-sum test fired
 *     on flat-loss runs and froze working modes out).  The lowest dead level
 *     is a ceiling nothing at or above is chosen from, re-probed with backoff;
 *   - below it, selection uses (lost + 1/2) / (sent + 2), optimistic enough to
 *     climb, and the earned round size bounds what any trial can cost. */
static double level_delivery(const car_t *c, int lv)
{
    return 1.0 - (c->lv_lost[lv] + 0.5) / (c->lv_sent[lv] + 2.0);
}

static bool level_dead(const car_t *c, int lv) { return c->lv_dead_run[lv] >= DEAD_RUN; }

static void measure_level(car_t *c, int lv, int frames, double loss, uint64_t now)
{
    c->lv_sent[lv] = LV_DECAY * c->lv_sent[lv] + frames;
    c->lv_lost[lv] = LV_DECAY * c->lv_lost[lv] + frames * loss;
    if (loss >= 1.0) c->lv_dead_run[lv] += frames;
    else             c->lv_dead_run[lv] = 0;
    c->lv_rounds[lv]++;
    c->lv_round_at[lv] = ++c->polls;
    c->lv_probe_at[lv] = now;
    if (loss >= 0.9)
        c->lv_backoff_ms[lv] = !c->lv_backoff_ms[lv] ? DEAD_PROBE_MS
                             : c->lv_backoff_ms[lv] * 2 > DEAD_PROBE_MAX_MS ? DEAD_PROBE_MAX_MS
                             : c->lv_backoff_ms[lv] * 2;
    else
        c->lv_backoff_ms[lv] = 0;
}

/* May this level be chosen?  Not if it is dead; and not if a lower one is,
 * UNLESS it has delivered recently itself.  A verdict is only statistics: at
 * 25 % flat loss a lightly probed DATAC4 was declared dead by chance, and the
 * ceiling then locked out a DATAC3 and a DATAC1 that were delivering.
 * Evidence of delivery beats the inference. */
static bool level_allowed(const car_t *c, int lv)
{
    if (level_dead(c, lv)) return false;
    for (int b = 0; b < lv; b++)
        if (level_dead(c, b))
            return c->lv_sent[lv] - c->lv_lost[lv] >= 0.5;
    return true;
}

/* A dead or locked-out level comes back for one probe after its backoff --
 * counted in time, not rounds: slow rungs have 30 s rounds, and 64 of them
 * kept a false verdict in force for over half an hour. */
static bool reprobe_due(const car_t *c, int lv, uint64_t now)
{
    uint64_t wait = c->lv_backoff_ms[lv] ? c->lv_backoff_ms[lv] : DEAD_PROBE_MS;
    return now - c->lv_probe_at[lv] >= wait;
}

/* Nothing measured yet, the peer starts where the SNR measured here puts it.
 * Only a start: the first round there is a one-frame probe, and measured
 * goodput decides from then on, so a misleading SNR (ISI-limited NVIS reads
 * 10 dB) costs a few frames.  Climbing by probes from DATAC15 instead cost
 * 25-40 s of airtime per turn on a 15 dB fading channel. */
static int choose_level(const car_t *c, uint64_t now)
{
    int best = -1;
    double best_gp = -1.0;
    for (int lv = 0; lv < CAR_NLEVELS; lv++) {
        if (!c->lv_rounds[lv] || !level_allowed(c, lv)) continue;
        double gp = level_rate(lv) * level_delivery(c, lv);
        if (gp > best_gp) { best_gp = gp; best = lv; }
    }
    if (best < 0)
        return c->lv_rounds[c->snr_level] ? 0 : c->snr_level;
    /* Nothing measured gets through: go down to a rung not tried yet. */
    if (best_gp <= 0.0) {
        for (int lv = best - 1; lv >= 0; lv--)
            if (!c->lv_rounds[lv]) return lv;
    }
    for (int up = best + 1; up < CAR_NLEVELS; up++) {
        if (level_rate(up) <= best_gp)
            continue;                   /* cannot win even with no loss */
        if (!level_allowed(c, up)) {
            if (!reprobe_due(c, up, now)) break;
        } else if (c->lv_rounds[up] && c->polls - c->lv_round_at[up] < PROBE_EVERY) {
            break;                      /* measured and fresh: the argmax decided */
        }
        return up;                      /* a probe: its earned round is one frame */
    }
    return best;
}

/* The peer's blocks not yet delivered, from the base up to the highest it has
 * opened: -1 when unknown. */
static int peer_outstanding(const car_t *c)
{
    if (!c->peer_hi_known) return -1;
    int d = (int8_t)(c->peer_hi - c->rbase);
    return d < 0 ? 0 : d + 1;          /* hi behind the base: all delivered */
}

/* How many frames to ask for on level lv: what it has earned (one frame more
 * than it recently delivered, so a probe is one frame, a proven mode gets full
 * rounds and a dead one never costs more than a frame), what fits a keydown,
 * and -- when every block of the sender is known here -- what is still needed. */
static int poll_size(const car_t *c, int lv)
{
    int cap = (int)(MAX_KEYDOWN_MS / level_air(lv));
    if (lv <= 1 && cap > SLOW_RUNG_FRAMES) cap = SLOW_RUNG_FRAMES;   /* slow rungs */
    if (cap > 15) cap = 15;                                           /* 4 bits */
    int earned = 1 + (int)(c->lv_sent[lv] - c->lv_lost[lv]);
    if (cap > earned) cap = earned;
    int out = peer_outstanding(c);
    if (out >= 0 && !c->peer_unopened) {
        double margin = c->loss_est * 1.3 + 0.05;
        int want = 0; bool all_known = true;
        for (int o = 0; o < out; o++) {
            const car_rblock_t *r = &c->rb[(uint8_t)(c->rbase + o) % CAR_WIN];
            if (!r->known) { all_known = false; break; }
            if (!r->done) want += (int)ceil((r->K - r->have) * (1.0 + margin));
        }
        if (all_known) {
            int ppf = pieces_per_frame(lv);
            int need = (want + ppf - 1) / ppf;
            if (need < cap) cap = need;
        }
    }
    return cap < 1 ? 1 : cap;
}

/* ---- receiver ------------------------------------------------------------ */
static void decode_block(car_t *c, car_rblock_t *r)
{
    int idx[CAR_MAX_K] = {0};
    const uint8_t *pcs[CAR_MAX_K] = {0};
    static uint8_t outb[CAR_MAX_K][CAR_PIECE];
    uint8_t *out[CAR_MAX_K];
    int n = 0;
    for (int i = 0; i < RS_MAX_PIECES && n < r->K; i++)
        if (r->got[i]) { idx[n] = i; pcs[n] = r->piece[i]; n++; }
    for (int i = 0; i < r->K; i++) out[i] = outb[i];
    if (rs_decode(r->K, CAR_PIECE, idx, pcs, out) == 0)
        for (int i = 0; i < r->K; i++) memcpy(r->piece[i], out[i], CAR_PIECE);
    r->done = true;
    c->done_in_round++;
}

/* Hand complete blocks to the application in order, sliding the window.
 * A decoded block's data pieces are in piece[0..K-1]. */
static void deliver_in_order(car_t *c)
{
    for (;;) {
        car_rblock_t *r = &c->rb[c->rbase % CAR_WIN];
        if (!r->known || !r->done) break;
        uint8_t buf[CAR_MAX_K * CAR_PIECE];
        for (int i = 0; i < r->K; i++) memcpy(buf + i * CAR_PIECE, r->piece[i], CAR_PIECE);
        if (c->io.deliver) c->io.deliver(c->io.ctx, buf, (size_t)r->len);
        memset(r, 0, sizeof(*r));
        c->rbase++;
    }
}

/* Everything the peer opened has been delivered, and it has nothing unopened. */
static bool peer_direction_done(const car_t *c)
{
    return c->status_seen || (!c->peer_unopened && peer_outstanding(c) == 0);
}

static void fill_poll(const car_t *c, msg_t *m)
{
    memset(m, 0, sizeof(*m));
    m->base = c->rbase;
    for (int o = 0; o < CAR_WIN; o++) {
        const car_rblock_t *r = &c->rb[(uint8_t)(c->rbase + o) % CAR_WIN];
        m->need[o] = !r->known ? FB_UNSEEN : r->done ? 0 : r->K - r->have;
    }
    m->loss16 = (int)lround(c->loss_est * 15.0);
    m->has_data = has_data(c);
    m->snr_level = c->snr_level;
}

static void arm_poll(car_t *c, uint64_t end) { arm(c, CAR_T_POLL, end + GUARD_MS); }

static void start_driving(car_t *c, uint32_t poll_id, int lv, int n, uint64_t round_start, uint64_t now)
{
    c->sending = false;
    c->idle = false;
    c->handover_unconfirmed = false;
    c->poll_id = poll_id; c->poll_level = lv; c->poll_n = n;
    bind_rx(c, lv);
    c->round_seen = 0; c->round_frames = n; c->status_seen = false;
    c->round_heard = true;
    c->done_in_round = 0;
    c->drive_start = now;
    disarm(c, CAR_T_SEND); disarm(c, CAR_T_WAIT); disarm(c, CAR_T_SENSE);
    arm_poll(c, round_start + HEAD_MS + round_air(lv, n) + TAIL_MS + WINDOW_MARGIN_MS);
}

/* Take the turn: the handover poll and my first round, in one keydown. */
static void send_handover(car_t *c)
{
    car_frame_t *fr = c->txbuf;
    msg_t m;
    fill_poll(c, &m);
    m.type = M_HANDOVER;
    c->sending = true;
    c->idle = false;
    c->tx_poll_id = (c->tx_poll_id + 1) & 0x0F;
    /* Where the peer last put me, or where the SNR it measures of me starts me. */
    int lv = c->tx_level >= 0 ? c->tx_level : c->peer_snr_level;
    int n = c->tx_n > 0 ? c->tx_n : 1;
    int nd = build_round(c, lv, n, c->tx_poll_id, fr + 1);
    m.h_level = lv; m.h_n = nd; m.h_id = c->tx_poll_id;
    m.hi = (uint8_t)(c->next_blk_id - 1);
    m.unopened = unopened(c);
    encode_ctl(&m, fr[0].bytes);
    fr[0].mode = ARQ_CONTROL_MODE; fr[0].len = CAR_POLL_BYTES; fr[0].gap_ms = 0;
    if (nd) fr[1].gap_ms = CHAIN_GAP_MS;
    c->handover_unconfirmed = true;
    disarm(c, CAR_T_POLL); disarm(c, CAR_T_SENSE);
    keydown(c, fr, 1 + nd, AFTER_ROUND);
}

static void send_poll(car_t *c, int lv, int n)
{
    car_frame_t *fr = c->txbuf;
    msg_t m;
    fill_poll(c, &m);
    m.type = M_POLL;
    c->poll_id = (c->poll_id + 1) & 0x0F;
    m.poll_id = c->poll_id;
    m.level = lv; m.n = n;
    if (n) { c->poll_level = lv; c->poll_n = n; bind_rx(c, lv); }
    c->round_seen = 0; c->round_frames = n; c->status_seen = false; c->done_in_round = 0;
    c->round_heard = false;
    encode_ctl(&m, fr[0].bytes);
    fr[0].mode = ARQ_CONTROL_MODE; fr[0].len = CAR_POLL_BYTES; fr[0].gap_ms = 0;
    disarm(c, CAR_T_POLL); disarm(c, CAR_T_SENSE);
    keydown(c, fr, 1, n ? AFTER_POLL : AFTER_NONE);
}

/* The poll timer: the round is over (or never came).  Measure it, then poll,
 * take the turn, or go quiet. */
static void on_poll_timer(car_t *c, uint64_t now)
{
    if (c->sending || c->idle) return;
    if (defer_if_busy(c, CAR_T_POLL, now)) return;
    if (!c->status_seen) {
        int frames = c->round_frames > 0 ? c->round_frames : 1;
        double loss = 1.0 - (double)c->round_seen / frames;
        if (loss < 0) loss = 0;
        c->loss_est = 0.5 * c->loss_est + 0.5 * loss;
        measure_level(c, c->poll_level, frames, loss, now);
    }
    deliver_in_order(c);

    bool done = peer_direction_done(c);
    uint64_t held = now - c->drive_start;
    bool quantum = (c->done_in_round && held >= TURN_QUANTUM_MS) || held >= 2 * TURN_QUANTUM_MS;
    if (has_data(c) && (done || quantum)) {
        /* When the peer still has data its open blocks are only suspended:
         * both sides keep their state and carry on when the turn comes back. */
        send_handover(c);
        return;
    }
    if (done) {
        c->idle = true;
        send_poll(c, 0, 0);                  /* ack only: nothing left either way */
        return;
    }
    int lv = choose_level(c, now);
    send_poll(c, lv, poll_size(c, lv));
}

/* The sense timer: by now the sender should be keyed.  Silence means it never
 * heard the poll, so poll again at once rather than sit out the round's window
 * -- and do not score the mode for it.  Silence on a second poll in a row is
 * left to the window and scored: a mode too weak even to sync must still be
 * found dead.  While it is on the air with nothing decoded yet, keep watching:
 * poll when its carrier drops, not at the window computed for the round we
 * asked for -- the keydown may be something else, a repeated handover, and a
 * window timer ran into the sender's repeat timer. */
static void on_sense_timer(car_t *c, uint64_t now)
{
    if (c->sending || c->idle || c->round_seen) return;
    if (peer_keyed(c, now)) {
        c->round_heard = true; c->silent_polls = 0;
        arm(c, CAR_T_SENSE, now + CARRIER_CHECK_MS);
        return;
    }
    if (c->round_heard) { arm_poll(c, now); return; }   /* its carrier dropped */
    if (++c->silent_polls >= 2) return;
    send_poll(c, c->poll_level, c->poll_n);
}

static void take_pieces(car_t *c, const msg_t *m)
{
    for (int g = 0; g < m->nseg; g++) {
        const seg_t *sg = &m->seg[g];
        int off = (sg->block - c->rbase) & 0x0F;
        if (off >= CAR_WIN) continue;         /* delivered already */
        car_rblock_t *r = &c->rb[(uint8_t)(c->rbase + off) % CAR_WIN];
        if (!r->known) {
            r->known = true;
            r->K = sg->K; r->len = sg->K * CAR_PIECE - sg->pad;
        }
        if (r->done) continue;
        for (int p = 0; p < sg->count; p++) {
            int i = (sg->first + p) % RS_MAX_PIECES;
            if (!r->got[i] && r->have < r->K) {
                r->got[i] = true;
                memcpy(r->piece[i], sg->pieces + p * CAR_PIECE, CAR_PIECE);
                r->have++;
            }
        }
        if (r->have >= r->K) decode_block(c, r);
    }
    c->peer_unopened = m->unopened;
    c->peer_hi = resolve16(c->rbase, m->hi);
    c->peer_hi_known = true;
    c->peer_snr_level = m->snr_level;
}

static void on_data(car_t *c, uint64_t now, const msg_t *m, int lv)
{
    if (lv != c->rx_level) return;   /* the payload decoder is on another mode */
    if (c->sending) {
        /* Data while we hold the turn: the peer took it with a handover poll
         * we missed.  Follow it -- our blocks stay suspended. */
        start_driving(c, m->poll_id, lv, m->left + 1, now - level_air(lv) - HEAD_MS, now);
    }
    c->idle = false;
    c->round_heard = true; c->silent_polls = 0;
    take_pieces(c, m);
    if (m->poll_id == c->poll_id) {
        c->round_seen++;
        c->round_frames = c->round_seen + m->left;   /* the frames say how many there are */
    }
    deliver_in_order(c);
    arm_poll(c, now + (uint64_t)m->left * (level_air(lv) + burst_gap_ms(lv)) + TAIL_MS);
}

static void on_ctl(car_t *c, uint64_t now, const msg_t *m)
{
    c->peer_snr_level = m->snr_level;
    if (m->type == M_STATUS) {
        if (c->sending) return;
        if (m->poll_id == c->poll_id) c->status_seen = true;
        c->round_heard = true; c->silent_polls = 0;
        c->peer_hi = resolve16(c->rbase, m->hi);
        c->peer_hi_known = true; c->peer_unopened = false;
        arm_poll(c, now);
        return;
    }
    c->handover_unconfirmed = false;
    if (m->type == M_HANDOVER) {
        /* The peer takes the turn; its round follows in this keydown. */
        apply_need(c, m);
        c->peer_unopened = m->unopened;
        c->peer_hi = resolve16(c->rbase, m->hi);
        c->peer_hi_known = true;
        start_driving(c, m->h_id, m->h_level, m->h_n, now + CHAIN_GAP_MS - HEAD_MS, now);
        return;
    }
    /* A poll of my direction.  If I thought I was driving, the peer never
     * heard my handover and still polls me: be the sender again. */
    disarm(c, CAR_T_POLL); disarm(c, CAR_T_SENSE); disarm(c, CAR_T_WAIT);
    apply_need(c, m);
    c->tx_poll_id = m->poll_id;
    c->tx_loss = m->loss16 / 15.0;
    if (m->n == 0) {                          /* ack only: the peer is done with us */
        c->sending = false;
        if (!has_data(c)) { c->idle = true; return; }
        c->tx_n = 1;
        arm(c, CAR_T_SEND, now + ISS_GUARD_MS);
        return;
    }
    c->sending = true;
    c->idle = false;
    c->tx_level = m->level; c->tx_n = m->n;
    bind_rx(c, m->level);                     /* where the peer's data will come too */
    arm(c, CAR_T_SEND, now + ISS_GUARD_MS);
}

/* ---- entry points ---------------------------------------------------------- */
static const struct { int mode; float min_db; } START[] = {
    { FREEDV_MODE_QAM16C2, ARQ_SNR_MIN_QAM16C2_DB }, { FREEDV_MODE_DATAC17, ARQ_SNR_MIN_DATAC17_DB },
    { FREEDV_MODE_DATAC1,  ARQ_SNR_MIN_DATAC1_DB },  { FREEDV_MODE_DATAC3,  ARQ_SNR_MIN_DATAC3_DB },
};

int car_start_level(float snr_db)
{
    for (size_t i = 0; i < sizeof(START) / sizeof(START[0]); i++)
        if (snr_db >= START[i].min_db + ARQ_SNR_HYST_DB)
            return level_of_mode(START[i].mode);
    return 0;                  /* DATAC4 is slower than DATAC15 here: never a start */
}

void car_init(car_t *c, const car_io_t *io, int start_level)
{
    memset(c, 0, sizeof(*c));
    c->io = *io;
    c->loss_est = c->tx_loss = 0.1;
    c->snr_level = c->peer_snr_level = start_level;
    c->tx_level = -1;
}

/* The caller: the callee's ACCEPT was the first poll, a one-frame probe on the
 * start rung.  Answer it now. */
void car_start_sender(car_t *c, uint64_t now)
{
    c->sending = true;
    c->tx_level = c->peer_snr_level; c->tx_n = 1; c->tx_poll_id = 1;
    arm(c, CAR_T_SEND, now);
}

/* The callee: the ACCEPT is on the air, as poll 1. */
void car_start_receiver(car_t *c, uint64_t now)
{
    start_driving(c, 1, c->snr_level, 1, now, now);
}

void car_on_frame(car_t *c, uint64_t now, const uint8_t *bytes, size_t len, int mode, bool control)
{
    msg_t m;
    c->last_carrier_ms = now;
    if (control) {
        if (decode_ctl(bytes, len, &m)) on_ctl(c, now, &m);
        return;
    }
    int lv = level_of_mode(mode);
    if (lv >= 0 && decode_data(bytes, len, &m)) on_data(c, now, &m, lv);
}

void car_on_tx_done(car_t *c, uint64_t now)
{
    c->tx_busy = false;
    int after = c->after_tx;
    c->after_tx = AFTER_NONE;
    if (after == AFTER_ROUND) {
        arm_sender_wait(c, now);
    } else if (after == AFTER_POLL) {
        /* The peer heard the poll as its last frame ended, keys after its
         * guard, and should be heard by SENSE_MS after that. */
        uint64_t start = now - TAIL_MS + ISS_GUARD_MS;
        arm(c, CAR_T_SENSE, start + SENSE_MS);
        arm_poll(c, start + HEAD_MS + round_air(c->poll_level, c->poll_n) + TAIL_MS + WINDOW_MARGIN_MS);
    }
}

void car_on_app_data(car_t *c, uint64_t now)
{
    /* Idle, with something to send: take the turn with a handover, after
     * listening (the T_WAIT path repeats an unconfirmed handover). */
    if (c->idle && !c->tx_busy && has_data(c)) {
        c->idle = false;
        c->sending = true;
        c->handover_unconfirmed = true;
        disarm(c, CAR_T_POLL); disarm(c, CAR_T_SENSE); disarm(c, CAR_T_SEND);
        arm(c, CAR_T_WAIT, now);
    }
}

void car_on_time(car_t *c, uint64_t now)
{
    for (;;) {
        int due = -1;
        for (int t = 0; t < CAR_NTIMERS; t++)
            if (c->deadline[t] && c->deadline[t] <= now && (due < 0 || c->deadline[t] < c->deadline[due]))
                due = t;
        if (due < 0) return;
        c->deadline[due] = 0;
        switch (due) {
        case CAR_T_POLL:
            on_poll_timer(c, now);
            break;
        case CAR_T_SENSE:
            on_sense_timer(c, now);
            break;
        case CAR_T_SEND:
            if (defer_if_busy(c, CAR_T_SEND, now)) break;
            c->sending = true;
            send_round(c);
            break;
        case CAR_T_WAIT:
            /* No poll.  After a handover the peer may have missed it: repeat
             * it (fresh pieces are never wasted).  Otherwise the receiver has
             * gone quiet: nudge it with a round. */
            if (!c->sending) break;
            if (defer_if_busy(c, CAR_T_WAIT, now)) break;
            if (c->handover_unconfirmed) send_handover(c);
            else send_round(c);
            break;
        }
    }
}
