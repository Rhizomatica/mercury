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
 *         index (8) | pad bytes in the block's last data piece (5) | 0 (1) |
 *         in the FIRST segment header only: control-deaf (1), else 0
 *       and count pieces with consecutive indices (mod 256).  A zero count
 *       ends the frame.  (The first header is always there: a payload mode
 *       frame holds at least FRAME_HDR + SEG_HDR bytes.)
 *   POLL / HANDOVER / STATUS, CAR_POLL_BYTES, in the control mode -- or on
 *   MFSK behind a byte CTL_FLOOR_MARK when the peer is control-deaf:
 *     b0  type (2) | has data (1) | poll id (4) | control-deaf (1)
 *     b1  level (3) | frames asked for (4) | 0
 *     b2  loss (4) | window base, mod 16 (4)
 *     b3..b9   pieces each block from the base still needs, 7 bits each
 *              (127: none seen)
 *     b10 the first data piece missing from the base block (255: none)
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
#include "modem_mfsk.h"            /* MERCURY_MODE_MFSK */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
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
/* With both sides holding data, a turn ends at the first round after
 * TURN_QUANTUM_MS that completed a block, and at TURN_CAP_MS at the latest.
 * Applications time out on silence: NNCP drops a peer after two 60 s ping
 * intervals without a byte, and 120/240 s turns left one side silent past
 * that on most low-SNR runs, where 30/45 s cost nothing measurable. */
#ifndef TURN_QUANTUM_MS
#define TURN_QUANTUM_MS  30000
#endif
#ifndef TURN_CAP_MS
#define TURN_CAP_MS      45000
#endif
#define MAX_KEYDOWN_MS   30000    /* airtime cap per round                  */
/* The keydown's lead before its first frame: tx delay and head silence, plus
 * the NAV header when keydowns carry one (car_set_nav_ms). */
static uint32_t g_nav_ms;
void car_set_nav_ms(uint32_t ms) { g_nav_ms = ms; }
/* Does my next keydown need a NAV header?  Only where the peer may not sense
 * it otherwise: below this SNR (as I hear it -- the path is reciprocal). */
static float g_nav_below_db = 99.0f;
void car_set_nav_below_db(float db) { g_nav_below_db = db; }
bool car_wants_nav(const car_t *c)
{
    /* Or where frames are being lost at a good SNR (NVIS: ISI, not noise):
     * a decoder that loses frames loses sync on them too. */
    return g_nav_ms && (!c->snr_valid || c->snr_ema < g_nav_below_db || c->loss_est >= 0.3);
}
static uint32_t nav_lead(const car_t *c) { return car_wants_nav(c) ? g_nav_ms : 0; }
/* Both ends decide from the same reciprocal evidence, so a keydown's lead is
 * the same whichever end predicts it. */
#define HEAD_MS          (110 + nav_lead(c))
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
#define RESEND_RUN       2        /* pieces repeated from the receiver's gap */
#define WINDOW_MARGIN_MS 1000     /* a round that never came: re-poll after this */
#define SENSE_MS         1400     /* after keying, the sender is heard by now */
#define CARRIER_CHECK_MS 250      /* carrier re-checked this often */
#define SENDER_SILENCE_MS 90000   /* sender heard no poll this long: nudge  */
/* The floor.  A real poll every FLOOR_POLL_EVERY rounds gives link adaptation
 * a chance to climb; a sender that heard nothing FLOOR_SILENT_MAX rounds in a
 * row stops streaming and waits to be polled.  A sender that missed the
 * pattern continues this much later than one that heard it, and the
 * receiver's round window allows for it. */
#define FLOOR_POLL_EVERY  4
/* Deep below the floor's exit (ARQ_SNR_MIN_DATAC15_DB) a poll cannot take the
 * session off it, and on air the poll was half the floor's idle air (a 9-15 s
 * gap where a pattern round takes 3.1 s): poll only every FLOOR_POLL_EVERY_DEEP
 * rounds there. */
#define FLOOR_DEEP_DB          3.0f
#define FLOOR_POLL_EVERY_DEEP 12
#define FLOOR_SILENT_MAX  6
#define FLOOR_LATE_MS     9000
#define PATTERN_AIR_MS    640
/* After a pattern above the floor the sender keys later than after a poll:
 * it detects the pattern ~0.25 s after it ends, where a poll is decoded as
 * it ends.  Sensed at SENSE_MS alone, the round had been on the air 0.8 s
 * and its preamble not yet found: on air the receiver took the sender for
 * one that missed the pattern and re-polled over its round. */
/* And an OFDM round is sensed only once its preamble is acquired: at 1 s
 * more, on air, a DATAC3 round at 4.5 dB, which decoded, was still unsensed
 * 1.7 s in.  (Waiting for the round's first frame to decode instead cost the
 * fast rungs a lost poll's worth -- 7.4 s frames on DATAC17 -- in the sim.) */
#define PATTERN_SENSE_EXTRA_MS 2000
/* An MFSK round is answered later than an OFDM one: on the real modems the
 * last frame decoded ~3.7 s after the sender unkeyed, and a pattern keyed
 * after it ended 5.6 s after the round -- past the 5.75 s the sender waited,
 * so it continued on silence over the answer.  And its carrier is found
 * later: a receiver that checked at SENSE_MS saw nothing yet and polled again
 * on top of the round. */
#define FLOOR_ANSWER_MS   4000
#define FLOOR_SENSE_MS    3000
/* Once answers have come, the sender waits for the next one as long as they
 * have been taking, plus this -- never longer than the fixed window.  Fixed,
 * the margin the real modems need cost 5-11 % in the sim, which answers
 * at once, wherever rounds or patterns were lost. */
#define FLOOR_DELAY_MARGIN_MS 2000
/* The peer checks its turn quantum as each round of mine ends; this much
 * before it is due, the next answer may already be its handover. */
#define HANDOVER_SOON_MS      5000

enum { M_DATA, M_POLL, M_HANDOVER, M_STATUS };
enum { AFTER_NONE, AFTER_POLL, AFTER_ROUND, AFTER_PATTERN };

/* MFSK is the floor: ~10 dB below DATAC15, at 3 pieces per 13.5 s frame. */
static const int LADDER[CAR_NLEVELS] = {
    MERCURY_MODE_MFSK,
    FREEDV_MODE_DATAC15, FREEDV_MODE_DATAC4, FREEDV_MODE_DATAC3,
    FREEDV_MODE_DATAC1, FREEDV_MODE_DATAC17, FREEDV_MODE_QAM16C2,
};

static uint32_t burst_gap_ms(int lv)
{
    return LADDER[lv] == FREEDV_MODE_QAM16C2 || LADDER[lv] == MERCURY_MODE_MFSK ? 200 : 100;
}
int car_level_mode(int level) { return LADDER[level]; }
bool car_is_idle(const car_t *c) { return c->idle; }

size_t car_tx_inflight(const car_t *c)
{
    size_t n = 0;
    for (int b = 0; b < c->nsb; b++) n += (size_t)c->sb[b].len;
    return n;
}

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

/* ---- the control plane at the floor ----------------------------------------
 * Control frames go on the control mode (DATAC16).  A peer that hears me below
 * it cannot decode them, and then nothing of mine but patterns reaches it: on
 * air (estacao2 -> gateway at 2 %) its sender, deaf to my polls, re-sent the
 * same handover for minutes, or sat waiting while I marked every rung dead.
 *
 * So each end measures the other and reports, in every frame it sends, whether
 * it hears it below the control mode (ctl_deaf, with hysteresis).  To a peer
 * that reports so, control frames go on MFSK, behind a first byte no data
 * frame has (level 7 is off the 3-bit ladder); and an end that is itself
 * control-deaf listens for its peer's control there.  Both ends act on the
 * same bit, so they agree -- a switch on local evidence alone did not, and a
 * peer still on DATAC15 missed the MFSK (sim, cliff:-5 bidir).  Nor on the
 * start rung: level 0 begins at -2 dB, where the control mode still works and
 * a 13.5 s MFSK control frame is pure cost. */
#define CTL_FLOOR_MARK   0x07
#define CTL_DEAF_ON_DB   (ARQ_SNR_MIN_DATAC15_DB - 0.5f)
#define CTL_DEAF_OFF_DB  (ARQ_SNR_MIN_DATAC15_DB + 1.0f)
static bool ctl_on_floor(const car_t *c)      { return c->io.pattern && c->peer_ctl_deaf; }
static bool peer_ctl_on_floor(const car_t *c) { return c->io.pattern && c->ctl_deaf; }
static uint64_t peer_ctl_air(const car_t *c)
{
    return peer_ctl_on_floor(c) ? level_air(0) : mode_air(ARQ_CONTROL_MODE);
}
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
    int      gap;         /* first missing data piece of the base block, -1 none */
    bool     has_data;
    int      h_level, h_n;
    uint32_t h_id;
    bool     ctl_deaf;              /* the sender hears me below the control mode */
} msg_t;

static void encode_ctl(const msg_t *m, uint8_t *b)
{
    memset(b, 0, CAR_POLL_BYTES);
    int type = m->type == M_POLL ? 0 : m->type == M_HANDOVER ? 1 : 2;
    b[0] = (uint8_t)(type << 6 | (m->has_data ? 1 : 0) << 5 | (m->poll_id & 0x0F) << 1 |
                     (m->ctl_deaf ? 1 : 0));
    b[1] = (uint8_t)((m->level & 7) << 5 | (m->n & 0x0F) << 1);
    b[2] = (uint8_t)((m->loss16 & 0x0F) << 4 | (m->base & 0x0F));
    uint64_t nb = 0;                          /* 8 x 7 bits, first block high */
    for (int o = 0; o < CAR_WIN; o++)
        nb = nb << 7 | (uint64_t)(m->need[o] == FB_UNSEEN ? 127 : m->need[o] & 0x7F);
    for (int i = 0; i < 7; i++) b[3 + i] = (uint8_t)(nb >> (8 * (6 - i)));
    b[10] = (uint8_t)(m->gap < 0 ? 255 : m->gap);
    b[11] = (uint8_t)((m->snr_level & 7) << 5 | (m->h_level & 7) << 2);
    b[12] = (uint8_t)((m->h_n & 0x0F) << 4 | (m->h_id & 0x0F));
    b[13] = (uint8_t)((m->hi & 0x0F) << 4 | (m->unopened ? 1 : 0) << 3);
}

/* A control frame: on the control mode, or on MFSK behind CTL_FLOOR_MARK to a
 * control-deaf peer (see ctl_on_floor). */
static void put_ctl(const car_t *c, car_frame_t *f, const msg_t *m)
{
    f->gap_ms = 0;
    if (ctl_on_floor(c)) {
        /* A frame fills its mode: the modem refuses anything shorter (on
         * the real modem a 15-byte MFSK control frame never went out). */
        int room = mode_payload(LADDER[0]);
        memset(f->bytes, 0, (size_t)room);
        f->bytes[0] = CTL_FLOOR_MARK;
        encode_ctl(m, f->bytes + 1);
        f->mode = LADDER[0]; f->len = room;
    } else {
        encode_ctl(m, f->bytes);
        f->mode = ARQ_CONTROL_MODE; f->len = CAR_POLL_BYTES;
    }
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
    m->ctl_deaf = b[0] & 1;
    m->level = b[1] >> 5;
    m->n = (b[1] >> 1) & 0x0F;
    m->loss16 = b[2] >> 4;
    m->base = b[2] & 0x0F;
    uint64_t nb = 0;
    for (int i = 0; i < 7; i++) nb = nb << 8 | b[3 + i];
    for (int o = 0; o < CAR_WIN; o++) {
        int v = (int)((nb >> (7 * (CAR_WIN - 1 - o))) & 0x7F);
        m->need[o] = v == 127 ? FB_UNSEEN : v;
    }
    m->gap = b[10] == 255 ? -1 : b[10];
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
    if (len >= FRAME_HDR + SEG_HDR) m->ctl_deaf = b[FRAME_HDR + SEG_HDR - 1] & 1;
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

static void car_trace(const car_t *c, const char *fmt, ...)
{
    if (!c->io.trace) return;
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    c->io.trace(c->io.ctx, line);
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
static uint32_t g_slot_ms;
static uint32_t g_repoll_extra_ms;
void car_set_repoll_extra_ms(uint32_t ms) { g_repoll_extra_ms = ms; }
void car_set_slot_ms(uint32_t ms) { g_slot_ms = ms; }
void car_set_slots(car_t *c, uint64_t epoch, int parity) { c->slot_epoch = epoch; c->slot_parity = parity & 1; }

/* A blind keydown waits for my next slot (see car_set_slots). */
static bool defer_to_slot(car_t *c, int t, uint64_t now)
{
    if (!g_slot_ms || now < c->slot_epoch) return false;
    uint64_t k = (now - c->slot_epoch + g_slot_ms - 1) / g_slot_ms;   /* next boundary */
    if ((int)(k & 1) != c->slot_parity) k++;
    uint64_t b = c->slot_epoch + k * g_slot_ms;
    if (b <= now + 20) return false;
    arm(c, t, b);
    return true;
}

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

/* A block is cut to about BLOCK_AIR_MS of airtime on the rung it is first
 * sent on (at least 8 pieces, at most CAR_MAX_K).  A lost data piece is only
 * made good once the whole block's data has gone out and repair follows, and
 * in-order delivery waits for it: a 96-piece block on DATAC15 (one piece a
 * frame, 7 minutes of airtime) stalled the stream for minutes.  Sizing by the
 * poll's early one-frame rounds instead cut 10-piece blocks on DATAC3, and
 * every other frame paid a second segment header (cliff 3 dB, 8 % slower). */
#define BLOCK_MIN_K  8
#ifndef BLOCK_AIR_MS
#define BLOCK_AIR_MS 60000
#endif
static int block_k_for(int lv)
{
    int frames = (int)((BLOCK_AIR_MS + level_air(lv) - 1) / level_air(lv));
    int k = frames * pieces_per_frame(lv);
    return k < BLOCK_MIN_K ? BLOCK_MIN_K : k > CAR_MAX_K ? CAR_MAX_K : k;
}

static void open_block(car_t *c, int max_k)
{
    uint8_t buf[CAR_MAX_K * CAR_PIECE];
    size_t len = c->io.tx_read(c->io.ctx, buf, (size_t)max_k * CAR_PIECE);
    if (!len) return;
    car_sblock_t *s = &c->sb[c->nsb++];
    s->id = c->next_blk_id++;
    s->len = (int)len;
    s->K = (int)((len + CAR_PIECE - 1) / CAR_PIECE);
    s->next = 0;
    s->need = s->K;
    s->resend = -1;
    s->sent = 0;
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
        c->sb[b].resend = off == 0 && m->gap >= 0 && m->gap < c->sb[b].K ? m->gap : -1;
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
    /* Open blocks span fewer than CAR_WIN ids from the oldest unconfirmed
     * one, not merely number fewer than CAR_WIN: ids travel mod 16, and a
     * block 8 or more ahead of the receiver's base reads as behind it.
     * Blocks complete out of order, and with small blocks the span outgrew
     * the window -- a poll then retired a block that was never delivered, and
     * the session stalled with data undelivered. */
    /* Blocks are opened only as this round needs them, so each is cut for
     * the rung it first goes out on.  Filling the whole window up front cut
     * eight DATAC15-sized blocks at a low-SNR start, and after the climb they
     * rode DATAC3 frames with extra segment headers (cliff 3 dB, 8 % slower). */
    double margin = c->tx_loss * 1.3 + 0.05;
    int ppf = pieces_per_frame(lv);
    int queued = 0;
    /* At the floor a round carries only the oldest block, so a BREAK -- "the
     * block you are on is delivered" -- can only mean that one. */
    bool floor = lv == 0 && c->io.pattern;
    for (int b = 0; b < c->nsb; b++) queued += (int)ceil(c->sb[b].need * (1.0 + margin));
    while (queued < n * ppf && c->nsb < CAR_WIN && unopened(c) && !(floor && c->nsb) &&
           (c->nsb == 0 || (uint8_t)(c->next_blk_id - c->sb[0].id) < CAR_WIN))
    {
        open_block(c, block_k_for(lv));
        queued += (int)ceil(c->sb[c->nsb - 1].need * (1.0 + margin));
    }
    if (!c->nsb || n < 1) return 0;
    for (int b = 0; b < c->nsb; b++) c->sb[b].round_sent = 0;
    int nsb_all = c->nsb;
    if (floor) { c->nsb = 1; c->floor_blk = c->sb[0].id; }
    int mode = LADDER[lv];
    int room = mode_payload(mode);

    /* What each open block still needs, with a margin for the loss seen. */
    int want[CAR_WIN], want_total = 0;
    for (int b = 0; b < c->nsb; b++) {
        want[b] = (int)ceil(c->sb[b].need * (1.0 + margin));
        want_total += want[b];
    }
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
        /* The piece the receiver's in-order stream waits for goes first, so
         * the stream advances every round trip instead of waiting for the
         * block to decode -- stop-and-wait delivered every frame at once, and
         * waiting for whole blocks left NNCP silent past its 120 s limit on
         * most low-SNR runs.  It and the next piece; duplicates cost nothing
         * but airtime. */
        for (int r = 0; nf == 0 && r < c->nsb; r++) {
            car_sblock_t *s = &c->sb[r];
            if (s->resend < 0) continue;
            int count = s->K - s->resend < RESEND_RUN ? s->K - s->resend : RESEND_RUN;
            if (room - pos < SEG_HDR + count * CAR_PIECE) break;
            put_seg_hdr(x->bytes + pos, s, count, s->resend);
            pos += SEG_HDR;
            for (int i = 0; i < count; i++, pos += CAR_PIECE)
                piece_bytes(s, s->resend + i, x->bytes + pos);
            s->resend = -1;
            nseg++;
            break;
        }
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
                if (s->sent < RS_MAX_PIECES) s->sent++;
                s->round_sent++;                   /* not the resent gap pieces: repeats */
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
    c->nsb = nsb_all;
    for (int i = 0; i < nf; i++) {
        fr[i].bytes[0] = (uint8_t)((nf - 1 - i) << 4 | (unopened(c) ? 1 : 0) << 3 | (c->snr_level & 7));
        fr[i].bytes[1] = (uint8_t)((poll_id & 0x0F) << 4 | ((c->next_blk_id - 1) & 0x0F));
        if (c->ctl_deaf) fr[i].bytes[FRAME_HDR + SEG_HDR - 1] |= 1;
    }
    return nf;
}

/* A sender waits for a poll: after a handover, until the first poll should
 * have come (then the handover is repeated); otherwise a long silence. */
static int keydown_cap(int lv);

static void arm_sender_wait(car_t *c, uint64_t tx_end)
{
    uint64_t answer = tx_end + GUARD_MS + HEAD_MS + peer_ctl_air(c) + TAIL_MS + WINDOW_MARGIN_MS;
    /* A control-deaf end hears its peer's answer only on MFSK: listen there. */
    if (peer_ctl_on_floor(c)) bind_rx(c, 0);
    /* A peer that reaches me only at the floor answers in a control mode I
     * cannot hear -- not even as carrier.  Its handover is followed by a floor
     * frame I sense only seconds into it; timed for the control frame alone,
     * my repeat went out over that frame (on air at 2 %, fourteen times in one
     * run: st2 keyed 1.2 s into the gateway's MFSK, after a handover it never
     * heard).  Or it polls, and when I do not key, polls again: my repeat
     * then went out over the re-poll (sim, asym:-10:14 bidir).  Wait out both:
     * the floor frame's sense time, and a second control keydown. */
    if (c->drove_peer && c->poll_level == 0 && c->io.pattern)
        answer += SENSE_MS + FLOOR_SENSE_MS + HEAD_MS + peer_ctl_air(c) + TAIL_MS;
    /* A floor round: the answer is a pattern, a poll, or nothing -- and
     * nothing means "keep going" there too.  A handover round included: the
     * control mode may never get through, and a pattern confirms it. */
    c->floor_waiting = c->tx_level == 0 && c->io.pattern;
    /* Above the floor a pattern means "that round came whole: the same again".
     * Silence still means nothing there: only a poll or a pattern moves me. */
    c->pat_waiting = c->tx_level > 0 && c->io.pattern;
    if (c->floor_waiting) {
        /* The learned pattern delay may shorten the wait, but never below the
         * time a control frame takes to be heard: the answer to a floor round
         * can be the receiver's handover instead of a pattern.  On air the
         * wait had learned 4.2 s, the handover decodes 4.4 s after my unkey,
         * and carrier sense -- the gateway's control decoder, just after its
         * own 27 s keydown -- did not see it: I keyed 3.3 s into it. */
        uint64_t ctl_answer = answer;
        answer += FLOOR_ANSWER_MS;
        c->floor_tx_end = tx_end;
        if (c->floor_delay_ms && tx_end + c->floor_delay_ms + FLOOR_DELAY_MARGIN_MS < answer)
            answer = tx_end + c->floor_delay_ms + FLOOR_DELAY_MARGIN_MS;
        /* Only when that control frame could reach me at all: below the
         * control mode's floor both ways (sim, cliff:-9 bidir) the longer wait
         * bought nothing, and shifted this timer into lockstep with the
         * peer's handover repeats. */
        bool ctl_audible = c->snr_valid && c->snr_ema >= ARQ_SNR_MIN_DATAC15_DB;
        if (ctl_audible && answer < ctl_answer)
            answer = ctl_answer;
        /* Below it the peer's control is a 13.5 s MFSK frame, and the floor
         * wait was left to the learned pattern delay -- waiting out every one
         * shifted this timer into step with the peer's handover repeats (above).
         * But the receiver polls a floor stream every FLOOR_POLL_EVERY rounds or
         * more, so after a run of patterns the next answer may be that frame:
         * then cover it.  Continuing at the pattern delay, I keyed 3 s into it
         * (sim, fade:-9 one way: 7 collisions in 20 runs). */
        /* Not when the peer has data of its own: its handover is covered
         * below, and this wait on top fell into step with its repeats (sim,
         * fade:-9 both ways: 0 -> 4 collisions). */
        if (!ctl_audible && peer_ctl_on_floor(c) && !c->peer_has_data &&
            c->floor_pats_heard >= FLOOR_POLL_EVERY && answer < ctl_answer)
            answer = ctl_answer;
        /* But once the peer, with data of its own, has driven me about a turn
         * quantum, the answer may be its handover -- on MFSK below the control
         * mode, a frame found only as it ends, and in a fade not sensed
         * before.  Continuing at the pattern delay, I keyed into it (sim,
         * fade:-9 both ways: 187 collisions in 20 runs).  A pattern still
         * moves me at once. */
        if (c->peer_has_data && tx_end + HANDOVER_SOON_MS >= c->send_start_ms + TURN_QUANTUM_MS) {
            uint64_t ho = tx_end + GUARD_MS + HEAD_MS + peer_ctl_air(c) + TAIL_MS + FLOOR_ANSWER_MS;
            if (answer < ho) answer = ho;
        }
    }
    uint64_t t = c->handover_unconfirmed || c->floor_waiting ? answer : tx_end + SENDER_SILENCE_MS;
    arm(c, CAR_T_WAIT, t);
}

/* The longest a peer streaming the floor waits after its round, heard from
 * me, before sending the next one: arm_sender_wait's floor wait at its
 * longest (the learned delay only shortens it), its peer's control being
 * mine. */
static uint64_t peer_floor_wait_max(const car_t *c)
{
    uint64_t ctl = ctl_on_floor(c) ? level_air(0) : mode_air(ARQ_CONTROL_MODE);
    return GUARD_MS + HEAD_MS + ctl + TAIL_MS + WINDOW_MARGIN_MS +
           SENSE_MS + FLOOR_SENSE_MS + HEAD_MS + ctl + TAIL_MS + FLOOR_ANSWER_MS;
}

static void send_round(car_t *c)
{
    car_frame_t *fr = c->txbuf;
    c->floor_yielded = false;
    int nf = build_round(c, c->tx_level, c->tx_n, c->tx_poll_id, fr);
    if (!nf) {
        msg_t m;                                  /* polled with nothing to send */
        memset(&m, 0, sizeof(m));
        m.type = M_STATUS; m.poll_id = c->tx_poll_id;
        m.hi = (uint8_t)(c->next_blk_id - 1); m.snr_level = c->snr_level;
        m.ctl_deaf = c->ctl_deaf;
        put_ctl(c, &fr[0], &m);
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
#define PROBE_UP_DELIVERY 0.5   /* the best must deliver this well to probe above it */
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
static float level_min_db(int lv);
static bool level_marginal(const car_t *c, int lv);

/* A rung the SNR supports with room to spare gets the benefit of the doubt:
 * (lost + 1/2) / (sent + 2), so a clean link climbs on the first probe.  One
 * within SNR_GATE_DB of its threshold has to earn it: (sent - lost) / (sent +
 * 1).  With the optimistic prior everywhere, DATAC1 at 3 dB (threshold 3)
 * that had lost both its frames still outscored a DATAC3 delivering all of
 * its own, and every such choice was a round of airtime that delivered
 * nothing. */
static double level_delivery(const car_t *c, int lv)
{
    if (level_marginal(c, lv))
        return (c->lv_sent[lv] - c->lv_lost[lv]) / (c->lv_sent[lv] + 1.0);
    return 1.0 - (c->lv_lost[lv] + 0.5) / (c->lv_sent[lv] + 2.0);
}

/* Near its SNR threshold two lost frames in a row are enough: the SNR already
 * doubts the rung, and each loss costs a round. */
static bool level_dead(const car_t *c, int lv)
{
    return c->lv_dead_run[lv] >= (level_marginal(c, lv) ? 2 : DEAD_RUN);
}

static void measure_level(car_t *c, int lv, int frames, double loss, uint64_t now)
{
    c->lv_sent[lv] = LV_DECAY * c->lv_sent[lv] + frames;
    c->lv_lost[lv] = LV_DECAY * c->lv_lost[lv] + frames * loss;
    if (loss >= 1.0) c->lv_dead_run[lv] += frames;
    else             c->lv_dead_run[lv] = 0;
    if (loss >= 0.5) c->lv_probe_fails[lv]++;
    else             c->lv_probe_fails[lv] = 0;
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
/* The SNR a rung needs (the thresholds the stop-and-wait plane enters it at). */
static float level_min_db(int lv)
{
    switch (LADDER[lv]) {
    case FREEDV_MODE_QAM16C2: return ARQ_SNR_MIN_QAM16C2_DB;
    case FREEDV_MODE_DATAC17: return ARQ_SNR_MIN_DATAC17_DB;
    case FREEDV_MODE_DATAC1:  return ARQ_SNR_MIN_DATAC1_DB;
    case FREEDV_MODE_DATAC3:  return ARQ_SNR_MIN_DATAC3_DB;
    case FREEDV_MODE_DATAC4:  return ARQ_SNR_MIN_DATAC4_DB;
    case FREEDV_MODE_DATAC15: return ARQ_SNR_MIN_DATAC15_DB;
    default:                  return -99.0f;          /* the MFSK floor */
    }
}

/* A rung the SNR says cannot work is out, like a dead one, unless it has
 * delivered well itself.  Measured goodput alone kept choosing DATAC17 at
 * 3 dB (cliff 8 dB): a frame in ten still got through, so it was never
 * declared dead, and one lucky frame on a short memory made it outscore a
 * DATAC3 delivering 98 % -- minutes of rounds that delivered nothing, and
 * the application heard nothing.  The SNR only ever excludes: on NVIS it
 * reads 10 dB where fast rungs fail anyway, and measurement handles that. */
#define SNR_GATE_DB 3.0f
static bool level_marginal(const car_t *c, int lv)
{
    return c->snr_valid && c->snr_ema < level_min_db(lv) + SNR_GATE_DB;
}

static bool level_gated(const car_t *c, int lv)
{
    return c->snr_valid && c->snr_ema < level_min_db(lv) - SNR_GATE_DB &&
           !(c->lv_sent[lv] >= 3.0 && level_delivery(c, lv) >= 0.7);
}

static bool level_allowed(const car_t *c, int lv)
{
    if (level_dead(c, lv) || level_gated(c, lv)) return false;
    for (int b = 0; b < lv; b++)
        if (level_dead(c, b))
            return c->lv_sent[lv] - c->lv_lost[lv] >= 0.5;
    return true;
}

/* A dead or locked-out level comes back for one probe after its backoff --
 * counted in time, not rounds: slow rungs have 30 s rounds, and 64 of them
 * kept a false verdict in force for over half an hour. */
static bool level_gated(const car_t *c, int lv);
static bool reprobe_due(const car_t *c, int lv, uint64_t now)
{
    /* A rung the SNR has excluded and nobody has tried is not probed: its
     * probe clock starts at 0, and "due" at 60 s probed QAM16C2 at 8 dB
     * first thing in a session. */
    if (!c->lv_rounds[lv] && level_gated(c, lv)) return false;
    uint64_t wait = c->lv_backoff_ms[lv] ? c->lv_backoff_ms[lv] : DEAD_PROBE_MS;
    return now - c->lv_probe_at[lv] >= wait;
}

/* Nothing measured yet, the peer starts where the SNR measured here puts it.
 * Only a start: the first round there is a one-frame probe, and measured
 * goodput decides from then on, so a misleading SNR (ISI-limited NVIS reads
 * 10 dB) costs a few frames.  Climbing by probes from DATAC15 instead cost
 * 25-40 s of airtime per turn on a 15 dB fading channel. */
/* A round costs more than its frames: the poll that asks for it, and the
 * guards and turnarounds either side -- about 5.6 s, fixed.  Goodput per rung
 * is scaled by the airtime fraction of the round it gets.  Scored on raw
 * rate alone, a fast rung that delivered some frames in a fade kept winning
 * over a solid slower one, and ran one-frame rounds at 3.7 s of air for 9 s
 * of turnaround (in the sim: QAM16C2 at 8 dB, 7 rounds, 70 s for nothing). */
#define ROUND_OVERHEAD_MS (4400 + GUARD_MS + ISS_GUARD_MS + HEAD_MS + TAIL_MS)
/* A floor round answered by a pattern pays a 0.64 s pattern, not a poll.
 * Charged the poll's overhead, the floor lost to DATAC15, which it beats:
 * on the real modems at -5 dB the climb to DATAC15 took 201 s for what the
 * floor delivered in 108. */
#define FLOOR_OVERHEAD_MS (PATTERN_AIR_MS + GUARD_MS + ISS_GUARD_MS + HEAD_MS + TAIL_MS)
static uint64_t round_overhead(const car_t *c, int lv)
{
    return lv == 0 && c->io.pattern ? FLOOR_OVERHEAD_MS : ROUND_OVERHEAD_MS;
}
static int keydown_cap(int lv)
{
    int cap = (int)(MAX_KEYDOWN_MS / level_air(lv));
    bool slow = LADDER[lv] == FREEDV_MODE_DATAC15 || LADDER[lv] == FREEDV_MODE_DATAC4;
    if (slow && cap > SLOW_RUNG_FRAMES) cap = SLOW_RUNG_FRAMES;
    return cap > 15 ? 15 : cap < 1 ? 1 : cap;                          /* 4 bits */
}
static double air_fraction(const car_t *c, int lv, int frames)
{
    double air = (double)round_air(lv, frames);
    return air / (air + (double)round_overhead(c, lv));
}
/* What a measured rung delivers per second, in the rounds it has earned. */
static double level_goodput(const car_t *c, int lv)
{
    int earned = 1 + (int)(c->lv_sent[lv] - c->lv_lost[lv]);
    int frames = earned < keydown_cap(lv) ? earned : keydown_cap(lv);
    return level_rate(lv) * level_delivery(c, lv) * air_fraction(c, lv, frames);
}
/* The most a rung could deliver: no loss, full rounds. */
static double level_potential(const car_t *c, int lv) { return level_rate(lv) * air_fraction(c, lv, keydown_cap(lv)); }

/* A measured rung above the best is probed again every PROBE_EVERY polls,
 * twice as long after each round in a row on it that lost half its frames
 * while the SNR still says the rung is marginal: on the sim's cliffs and
 * fading DATAC1 was probed over DATAC3 and lost, every 8 polls, and each
 * probe cost a poll back down (NVIS 9 % faster without).  Once the SNR says
 * it should work, back to every PROBE_EVERY: backing off regardless cost a
 * link that rose from 3 to 10 dB 17 % (sim, step:3:10:200). */
static bool level_marginal(const car_t *c, int lv);
static double level_delivery(const car_t *c, int lv);
static int probe_every(const car_t *c, int lv)
{
    /* Nor for a rung that has been delivering: on air one faded DATAC3 round
     * sent a session down to DATAC4, whose frames read the SNR 3.5 dB lower
     * (marginal for DATAC3), and backed off it stayed on DATAC4 for 42-46
     * frames where the build without the backoff was back on DATAC3 after 8. */
    if (!level_marginal(c, lv) || level_delivery(c, lv) >= PROBE_UP_DELIVERY) return PROBE_EVERY;
    int f = c->lv_probe_fails[lv] < 3 ? c->lv_probe_fails[lv] : 3;
    return PROBE_EVERY << f;
}

static int choose_level(const car_t *c, uint64_t now)
{
    int best = -1;
    double best_gp = -1.0;
    for (int lv = 0; lv < CAR_NLEVELS; lv++) {
        if (!c->lv_rounds[lv] || !level_allowed(c, lv)) continue;
        double gp = level_goodput(c, lv);
        if (gp > best_gp) { best_gp = gp; best = lv; }
    }
    if (best < 0) {
        if (!c->lv_rounds[c->snr_level]) return c->snr_level;
        /* The SNR's rung was tried and is out: the next one down not tried
         * yet, not the floor.  At 12 dB two DATAC17 polls lost in a row
         * dropped the session to MFSK past four rungs with margin to spare,
         * and it took 220 s to climb back (sim, awgn:0.25 seed 6). */
        for (int lv = c->snr_level - 1; lv > 0; lv--)
            if (!c->lv_rounds[lv]) return lv;
        return 0;
    }
    /* Nothing measured gets through: go down to a rung not tried yet. */
    if (best_gp <= 0.0) {
        for (int lv = best - 1; lv >= 0; lv--)
            if (!c->lv_rounds[lv]) return lv;
    }
    /* A faster rung needs more SNR than the best one does, so probe upward
     * only while that one is delivering: after DATAC1 lost a frame at 8 dB,
     * probing DATAC17 and QAM16C2 cost a round each and delivered nothing.
     * Not stricter than half: flat 25 % loss holds every rung near 75 %, and
     * a 0.7 bar stopped the climb there. */
    if (level_delivery(c, best) < PROBE_UP_DELIVERY)
        return best;
    for (int up = best + 1; up < CAR_NLEVELS; up++) {
        if (level_potential(c, up) <= best_gp)
            continue;                   /* cannot win even with no loss */
        if (!level_allowed(c, up)) {
            if (!reprobe_due(c, up, now)) break;
        } else if (c->lv_rounds[up] && c->polls - c->lv_round_at[up] < probe_every(c, up)) {
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
 * rounds and a dead one never costs more than a frame), what fits a keydown
 * and the rest of the turn, and -- when every block of the sender is known
 * here -- what is still needed. */
static int poll_size(const car_t *c, int lv, uint64_t now)
{
    int cap = keydown_cap(lv);
    int earned = 1 + (int)(c->lv_sent[lv] - c->lv_lost[lv]);
    if (cap > earned) cap = earned;
    /* With our own data waiting, the peer's turn ends at TURN_CAP_MS: ask
     * only for what fits before it.  The cap is checked between rounds, and
     * a full keydown after it stretched a 45 s turn to 75 s. */
    if (has_data(c)) {
        uint64_t held = now - c->drive_start;
        int64_t left = (int64_t)TURN_CAP_MS - (int64_t)held;
        int fit = left > 0 ? (int)(left / (int64_t)(level_air(lv) + burst_gap_ms(lv))) : 1;
        if (fit < 1) fit = 1;
        if (cap > fit) cap = fit;
    }
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

/* Hand data to the application in order, sliding the window.  A decoded
 * block's data pieces are in piece[0..K-1].  The code is systematic, so the
 * block at the base is streamed as far as its data pieces have arrived
 * contiguously -- the repair pieces only fill the gaps.  Waiting for whole
 * blocks, a slow direction delivered nothing for minutes and NNCP took the
 * silent link for dead (two 60 s ping intervals without a byte) and hung up
 * with data still in flight. */
static void deliver_in_order(car_t *c)
{
    uint8_t buf[CAR_MAX_K * CAR_PIECE];
    for (;;) {
        car_rblock_t *r = &c->rb[c->rbase % CAR_WIN];
        if (!r->known) break;
        int upto;
        if (r->done) {
            upto = r->len;
        } else {
            int j = 0;
            while (j < r->K && r->got[j]) j++;
            upto = j * CAR_PIECE < r->len ? j * CAR_PIECE : r->len;
        }
        if (upto > r->delivered) {
            int n = 0;
            for (int off = r->delivered; off < upto; off++, n++)
                buf[n] = r->piece[off / CAR_PIECE][off % CAR_PIECE];
            if (c->io.deliver) c->io.deliver(c->io.ctx, buf, (size_t)n);
            r->delivered = upto;
        }
        if (!r->done) break;
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
    /* In-order delivery waits at the first data piece missing from the base
     * block; name it so the sender repeats it rather than the stream waiting
     * for the whole block to decode. */
    m->gap = -1;
    const car_rblock_t *h = &c->rb[c->rbase % CAR_WIN];
    if (h->known && !h->done)
        for (int i = 0; i < h->K; i++)
            if (!h->got[i]) { m->gap = i; break; }
    m->loss16 = (int)lround(c->loss_est * 15.0);
    m->has_data = has_data(c);
    m->snr_level = c->snr_level;
    m->ctl_deaf = c->ctl_deaf;
}

static void arm_poll(car_t *c, uint64_t end) { arm(c, CAR_T_POLL, end + GUARD_MS); }

static void start_driving(car_t *c, uint32_t poll_id, int lv, int n, uint64_t round_start, uint64_t now)
{
    c->sending = false;
    c->idle = false;
    c->handover_unconfirmed = false;
    c->poll_id = poll_id; c->poll_level = lv; c->poll_n = n;
    c->drove_peer = true;
    bind_rx(c, lv);
    c->round_seen = 0; c->round_frames = n; c->status_seen = false;
    c->round_heard = true;
    c->done_in_round = 0;
    c->drive_start = now;
    /* A BREAK speaks for the round just heard: a flag left from an earlier
     * turn retired a block the receiver never completed (sim, nvis bidir). */
    c->rx_break = false; c->floor_patterns = 0;
    c->floor_streaming = false;
    c->probe_off_floor = c->floor_fallback = c->floor_hold = false;
    c->floor_waiting = false; c->pat_waiting = false;
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
    /* The rung I send on now, as if polled there: the pattern that answers
     * this round is only taken on a rung I know.  Left at -1 until a poll,
     * a sender that took the turn at the floor ignored every pattern and
     * repeated its one-frame handover round, until a poll came (sim,
     * cliff:-11 bidir: 360-6120 of 8192 bytes back in 2 h). */
    c->tx_level = lv; c->tx_n = n;
    int nd = build_round(c, lv, n, c->tx_poll_id, fr + 1);
    m.h_level = lv; m.h_n = nd; m.h_id = c->tx_poll_id;
    m.hi = (uint8_t)(c->next_blk_id - 1);
    m.unopened = unopened(c);
    /* Name the last poll I heard, so a peer can tell "handed over after my
     * poll" from "never heard my poll". */
    m.poll_id = c->heard_poll_id;
    put_ctl(c, &fr[0], &m);
    if (nd) fr[1].gap_ms = CHAIN_GAP_MS;
    c->handover_unconfirmed = true;
    disarm(c, CAR_T_POLL); disarm(c, CAR_T_SENSE);
    keydown(c, fr, 1 + nd, AFTER_ROUND);
}

static void send_poll_as(car_t *c, int lv, int n, bool repeat)
{
    car_frame_t *fr = c->txbuf;
    msg_t m;
    fill_poll(c, &m);
    m.type = M_POLL;
    /* A repeat asks for the same round, so it keeps its id: frames of the
     * round the sender did key, answering the first ask, are that round's.
     * With a new id, on air, a re-poll over a round that had started threw
     * away the five of its seven frames that came, and marked DATAC3 dead. */
    if (!repeat) c->poll_id = (c->poll_id + 1) & 0x0F;
    m.poll_id = c->poll_id;
    m.level = lv; m.n = n;
    /* Asking a sender streaming the floor for another rung (see on_poll_timer:
     * if this is lost, it carries on at the floor).  A repeat keeps it. */
    if (!repeat) {
        c->probe_off_floor = n && lv > 0 && c->poll_level == 0 && c->floor_streaming && c->io.pattern;
        c->floor_fallback = false;
        if (c->probe_off_floor) { c->probe_lv = lv; c->probe_n = n; c->probe_after_ms = c->last_carrier_ms; }
    }
    c->rx_break = false; c->floor_patterns = 0;
    c->floor_streaming = false;
    if (n) { c->poll_level = lv; c->poll_n = n; c->drove_peer = true; bind_rx(c, lv); }
    if (n) { c->polled_since_handover = true; c->my_poll_id = (uint8_t)c->poll_id; }
    c->round_seen = 0; c->round_frames = n; c->status_seen = false; c->done_in_round = 0;
    c->round_heard = false;
    c->floor_hold = false;
    put_ctl(c, &fr[0], &m);
    disarm(c, CAR_T_POLL); disarm(c, CAR_T_SENSE);
    keydown(c, fr, 1, n ? AFTER_POLL : AFTER_NONE);
}

static void send_poll(car_t *c, int lv, int n) { send_poll_as(c, lv, n, false); }

/* At the floor: answer the round with a pattern.  expect_round: another round
 * of the same size comes next (a BREAK with nothing left expects none). */
static int floor_poll_every(const car_t *c)
{
    /* Not with data of my own: the poll's has_data is what makes the sender
     * yield the turn, and polled rarely it held it (sim, cliff:-11 bidir:
     * 40 KB delivered of 64 against 54 KB). */
    bool deep = c->snr_valid && c->snr_ema < ARQ_SNR_MIN_DATAC15_DB - FLOOR_DEEP_DB && !has_data(c);
    return deep ? FLOOR_POLL_EVERY_DEEP : FLOOR_POLL_EVERY;
}

static void send_pattern(car_t *c, int kind, bool expect_round)
{
    c->rx_break = false;
    if (expect_round) {
        if (c->poll_level == 0)
            c->poll_n = keydown_cap(0);     /* what a sender streaming the floor sends */
        c->round_seen = 0; c->round_frames = c->poll_n; c->status_seen = false;
        c->done_in_round = 0; c->round_heard = false;
    }
    c->floor_hold = false;
    disarm(c, CAR_T_POLL); disarm(c, CAR_T_SENSE);
    c->tx_busy = true;
    c->after_tx = expect_round ? AFTER_PATTERN : AFTER_NONE;
    c->io.pattern(c->io.ctx, kind);
}

/* A sender above the floor that hears no poll repeats its last round
 * SENDER_SILENCE_MS after it, on its rung, whatever I am bound to.  After
 * polls lost in a fade that repeat met my next one on the air, unsensed
 * either way (sim, fade:-5 seeds 1, 3, 7 and nvis seed 5).  I know when it
 * is due, where, and how long: if what I would key now could run into it,
 * listen for it on its rung instead, and act once it is over.  True when this
 * timer was moved for it. */
static bool clear_of_peer_nudge(car_t *c, uint64_t now)
{
    if (c->sending || c->peer_round_lv <= 0 || !c->peer_round_end) return false;
    /* Anything heard from the peer since that round means it heard a poll
     * after it: its repeat timer started over, on whatever it was told. */
    if (c->last_carrier_ms > c->peer_round_end + GUARD_MS) return false;
    uint64_t start = c->peer_round_end + SENDER_SILENCE_MS;
    uint64_t end   = start + HEAD_MS + round_air(c->peer_round_lv, c->peer_round_n) + TAIL_MS;
    uint64_t ctl   = ctl_on_floor(c) ? level_air(0) : mode_air(ARQ_CONTROL_MODE);
    uint64_t mine  = now + GUARD_MS + HEAD_MS + ctl + TAIL_MS;   /* a poll: what I would key */
    if (now >= end + GUARD_MS || mine + GUARD_MS <= start) return false;
    car_trace(c, "rx: the peer's repeat of its lv=%d round is due -- after it", c->peer_round_lv);
    bind_rx(c, c->peer_round_lv);
    c->peer_round_end = end;            /* and the next one, if this is not heard */
    arm(c, CAR_T_POLL, end + GUARD_MS);
    return true;
}

/* The poll timer: the round is over (or never came).  Measure it, then poll,
 * take the turn, or go quiet. */
static void on_poll_timer(car_t *c, uint64_t now)
{
    if (c->sending || c->idle) return;
    if ((c->floor_fallback || c->floor_hold) && peer_keyed(c, now)) c->round_heard = true;
    if (defer_if_busy(c, CAR_T_POLL, now)) return;
    if (!c->round_seen && !c->status_seen && !c->round_heard && defer_to_slot(c, CAR_T_POLL, now)) return;
    /* After the peer's handover, its repeat has the first blind keydown: my
     * poll answered by nothing may be a poll it missed, and its repeat comes
     * when its own wait for my poll runs out -- about when this window does,
     * both being sized from the same exchange.  Keying then, the two met
     * (sim, fade:-3 both ways, after NAV).  Waiting past it, its repeat is
     * heard first, and answered. */
    if (g_repoll_extra_ms && c->polled_since_handover && !c->round_seen && !c->status_seen &&
        !c->round_heard && !c->repoll_held) {
        c->repoll_held = true;
        arm(c, CAR_T_POLL, now + g_repoll_extra_ms);
        return;
    }
    c->repoll_held = false;
    if (clear_of_peer_nudge(c, now)) return;
    /* My poll off a floor stream brought nothing -- no frame, no status.  If
     * it was lost, the sender carries on at the floor a floor wait after its
     * round, and my decoder, bound to the rung I asked for, cannot hear that,
     * not even as carrier: polling again keyed into it (on air, car23 at 3 %:
     * 3.7 s into the gateway's 27 s MFSK, after a poll for DATAC4 it missed;
     * sim, asym:-6:14 bidir).  So listen at the floor first.  A floor frame
     * already on the air when I rebind ends within a frame; one that starts
     * after is sensed from its preamble; and the round starts by the end of
     * the sender's longest floor wait.  Silence past both, plus the time to
     * sense MFSK, means no such round: the sender heard the poll, and the
     * rung I asked for is what failed.  A round that does come re-arms this
     * timer for its own end. */
    /* Not when its carrier was heard: then the sender did answer, and the
     * rung asked for is what failed.  Waiting at the floor for a round that
     * was not coming, the receiver's floor polls and the sender's silence
     * nudge met on the air (sim, nvis seed 5). */
    if (c->probe_off_floor && !c->floor_fallback && !c->round_seen && !c->status_seen &&
        !c->round_heard) {
        uint64_t start_max = c->probe_after_ms + peer_floor_wait_max(c) + HEAD_MS;
        uint64_t in_frame = now + level_air(0) + burst_gap_ms(0);
        car_trace(c, "rx probe lv=%d unanswered: listening at the floor", c->probe_lv);
        c->floor_fallback = true;
        c->poll_level = 0; c->poll_n = keydown_cap(0);
        bind_rx(c, 0);
        c->round_seen = 0; c->round_frames = c->poll_n; c->round_heard = false; c->done_in_round = 0;
        disarm(c, CAR_T_SENSE);
        arm(c, CAR_T_POLL, (in_frame > start_max ? in_frame : start_max) + FLOOR_SENSE_MS);
        return;
    }
    /* An empty floor window.  The sender, hearing no answer, continues on its
     * own a floor wait after its round -- and with the control mode audible
     * that is 9.75 s, where the window runs 10.7 s past the round and MFSK is
     * sensed only a second or more in: the answer went out into the next
     * round (sim, awgn:0.25 seeds 6 and 17, after a slide to the floor).  So
     * hold until that round, if it comes, has been sensed; its frames re-arm
     * this timer for its end, as any round's do.  Silence past it means it is
     * not coming.  Cheap at the floor: silence there already means "keep
     * going". */
    /* Only while the sender hears my control mode: below it its wait takes an
     * MFSK control frame's air and outlasts the window by far. */
    if (c->poll_level == 0 && c->io.pattern && !c->floor_hold && !c->floor_fallback &&
        !c->peer_ctl_deaf && !c->round_seen && !c->status_seen && !c->round_heard) {
        uint64_t until = c->floor_round_end + peer_floor_wait_max(c) + FLOOR_SENSE_MS;
        if (until > now) {
            c->floor_hold = true;
            arm(c, CAR_T_POLL, until);
            return;
        }
    }
    c->floor_hold = false;
    /* Nothing at all of the round, and nothing sensed: the poll (or pattern)
     * was lost, or the rung is dead.  The quick re-poll told these apart by
     * asking again at once and scoring only a second silence; now the window
     * waits, and on a rung that has delivered asks again here, unscored --
     * the round, had it started, is over.  Scored at the first silence, two
     * lost polls in a row (sim, awgn:0.25) threw DATAC17 away at 12 dB and
     * slid the session to the floor: 169 s -> 719 s.  A rung that has not
     * delivered is scored at once, as before: asking twice cost cliff:3
     * probes 15 %. */
    if (!c->floor_fallback && c->poll_level > 0 && !c->round_seen && !c->status_seen &&
        !c->round_heard && c->round_frames > 0 && c->silent_polls == 0 &&
        c->lv_sent[c->poll_level] >= 2.0 && level_delivery(c, c->poll_level) >= 0.5) {
        c->silent_polls = 1;
        car_trace(c, "rx round lv=%d unheard: asking again", c->poll_level);
        send_poll_as(c, c->poll_level, c->poll_n, true);
        return;
    }
    bool probe_failed = c->floor_fallback && !c->round_seen && !c->status_seen && !c->round_heard;
    c->probe_off_floor = c->floor_fallback = false;
    if (probe_failed) {
        c->loss_est = 0.5 * c->loss_est + 0.5;
        measure_level(c, c->probe_lv, c->probe_n, 1.0, now);
    } else if (!c->status_seen) {
        int frames = c->round_frames > 0 ? c->round_frames : 1;
        double loss = 1.0 - (double)c->round_seen / frames;
        if (loss < 0) loss = 0;
        c->loss_est = 0.5 * c->loss_est + 0.5 * loss;
        measure_level(c, c->poll_level, frames, loss, now);
    }
    deliver_in_order(c);

    if (c->io.trace) {
        char gp[120]; int p = 0;
        for (int l = 0; l < CAR_NLEVELS && p < (int)sizeof gp - 24; l++)
            if (c->lv_rounds[l])
                p += snprintf(gp + p, sizeof gp - (size_t)p, " %d:%.0f/%.2f%s", l, 1000.0 * level_goodput(c, l),
                              level_delivery(c, l), level_allowed(c, l) ? "" : "x");
        car_trace(c, "rx round lv=%d seen=%d/%d status=%d snr=%.1f%s gp[lv:B/s/deliv]%s",
                  c->poll_level, c->round_seen, c->round_frames, c->status_seen, c->snr_ema,
                  c->snr_valid ? "" : "?", gp);
    }
    bool done = peer_direction_done(c);
    uint64_t held = now - c->drive_start;
    bool quantum = (c->done_in_round && held >= TURN_QUANTUM_MS) || held >= TURN_CAP_MS;
    if (has_data(c) && (done || quantum)) {
        /* When the peer still has data its open blocks are only suspended:
         * both sides keep their state and carry on when the turn comes back. */
        car_trace(c, "rx -> handover (%s)", done ? "peer done" : "turn quantum");
        c->send_start_ms = now;
        send_handover(c);
        return;
    }
    bool floor = c->poll_level == 0 && c->io.pattern;
    if (done) {
        car_trace(c, "rx -> done");
        c->idle = true;
        if (floor) send_pattern(c, CAR_PATTERN_BREAK, false);   /* the last block is in */
        else       send_poll(c, 0, 0);       /* ack only: nothing left either way */
        return;
    }
    int lv = choose_level(c, now);
    /* Leave the floor only when a poll can reach the sender: asking for
     * another rung re-binds my decoder to it at once, and a sender that never
     * heard the poll kept streaming MFSK I no longer listened to -- five
     * minutes of lost rounds at -11 dB until a poll got through. */
    if (floor && lv > 0 && !(c->snr_valid && c->snr_ema >= ARQ_SNR_MIN_DATAC15_DB))
        lv = 0;
    /* A pattern answers a sender that is streaming the floor: one of its
     * floor rounds has come since my last poll.  "Keep going" means nothing to
     * a sender that never heard it was at the floor -- patterns sent into that
     * silence left it on DATAC15 while I kept answering rounds that never
     * came.  But a lost round in a stream is answered all the same: polling
     * it instead gave back most of the floor's gain (26640 -> 14112 bytes at
     * -11 dB, carousel_bench). */
    if (c->round_seen > 0 && c->poll_level == 0) c->floor_streaming = true;
    if (floor && lv == 0 && c->floor_streaming && c->floor_patterns < floor_poll_every(c)) {
        car_trace(c, "rx -> floor pattern %s", c->rx_break ? "BREAK" : "ACK");
        c->floor_patterns++;
        send_pattern(c, c->rx_break ? CAR_PATTERN_BREAK : CAR_PATTERN_ACK, true);
        return;
    }
    int n = poll_size(c, lv, now);
    /* Above the floor a round that came whole, to be followed by the same
     * again, is answered by a pattern: 0.64 s where a poll takes a DATAC16
     * frame, and the sender takes it as "all of it is in".  Anything else --
     * a lost frame, a new rung or round size, the sender out of data -- needs
     * what only a poll carries, and a poll comes every FLOOR_POLL_EVERY rounds
     * all the same, for the repairs and the in-order gap. */
    /* A sender that is not hearing my polls -- it keeps handing over again
     * instead of sending the round I asked for -- gets the pattern even when
     * I would rather grow the round: a poll would only be lost again.  On air
     * (car18, estacao2 -> gateway at 2 %) the gateway asked for four DATAC17
     * frames over a DATAC16 return estacao2 could not decode, estacao2 re-sent
     * its one-frame handover every 25 s, and the UUCP hangup never completed
     * in 4 min. */
    bool polls_lost = c->polls_unheard > 0;
    if (c->io.pattern && lv > 0 && lv == c->poll_level && (n == c->poll_n || polls_lost) && !c->status_seen &&
        c->round_seen > 0 && c->round_seen >= c->round_frames && c->floor_patterns < FLOOR_POLL_EVERY) {
        car_trace(c, "rx -> pattern ACK (lv=%d n=%d)%s", lv, n, polls_lost ? " for lost polls" : "");
        c->floor_patterns++;
        c->pattern_for_lost_polls = polls_lost;
        send_pattern(c, CAR_PATTERN_ACK, true);
        return;
    }
    car_trace(c, "rx -> poll lv=%d n=%d", lv, n);
    send_poll(c, lv, n);
}

/* The sense timer: by now the sender should be keyed.  While it is on the air
 * with nothing decoded yet, keep watching, and poll when its carrier drops,
 * not at the window computed for the round we asked for -- the keydown may be
 * something else, a repeated handover, and a window timer ran into the
 * sender's repeat timer.
 *
 * Silence here is left to the window (on_poll_timer), above the floor too.
 * It used to be taken for a lost poll and polled again at once; but carrier
 * sense is a decoder syncing, and a round in a fade is not synced on.  In the
 * sim, with sensing as on air, 20 seeds: fade:3 226 collisions one way and
 * 376 both ways, nvis 178 and 295, fade:-3 494 and 1159 -- about none without
 * the quick re-poll, and fade:3 12 % faster, fade:8:1.0 23 %, nvis 9 %.  Earning
 * it back per rung where sensing proved sure did not help even pure random
 * loss (awgn:0.25), and brought collisions back under fading. */
static void on_sense_timer(car_t *c, uint64_t now)
{
    if (c->sending || c->idle || c->round_seen) return;
    /* At the floor a sender that missed the poll continues on its own, a
     * floor wait after its round: polling again before that keyed over it
     * (sim, cliff:-9 bidir). */
    if (c->poll_level == 0 && c->io.pattern) return;
    if (peer_keyed(c, now)) {
        c->round_heard = true; c->silent_polls = 0;
        arm(c, CAR_T_SENSE, now + CARRIER_CHECK_MS);
        return;
    }
    if (c->round_heard) arm_poll(c, now);   /* its carrier dropped */
}

static void take_pieces(car_t *c, const msg_t *m)
{
    for (int g = 0; g < m->nseg; g++) {
        const seg_t *sg = &m->seg[g];
        int off = (sg->block - c->rbase) & 0x0F;
        if (off >= CAR_WIN) { c->rx_break = true; continue; }   /* delivered already */
        car_rblock_t *r = &c->rb[(uint8_t)(c->rbase + off) % CAR_WIN];
        if (!r->known) {
            r->known = true;
            r->K = sg->K; r->len = sg->K * CAR_PIECE - sg->pad;
        }
        if (r->done) { c->rx_break = true; continue; }   /* delivered: still sent */
        for (int p = 0; p < sg->count; p++) {
            int i = (sg->first + p) % RS_MAX_PIECES;
            if (!r->got[i] && r->have < r->K) {
                r->got[i] = true;
                memcpy(r->piece[i], sg->pieces + p * CAR_PIECE, CAR_PIECE);
                r->have++;
            }
        }
        if (r->have >= r->K) { decode_block(c, r); c->rx_break = true; }
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
    /* At the floor the sender streams on patterns and silence, not on polls,
     * so its frames keep the id of the last poll it heard.  Counting only
     * frames that match my latest poll took a lost poll for a stream that had
     * stopped: I went back to polling, the ids never met again, and the BREAK
     * for a block I had completed never went out -- five minutes of pieces of
     * a delivered block at -11 dB. */
    bool floor_frame = lv == 0 && c->poll_level == 0 && c->io.pattern;
    if (floor_frame) c->floor_streaming = true;
    if (m->poll_id == c->my_poll_id && c->polled_since_handover)
        c->polls_unheard = 0;              /* it answered my poll: it hears them */
    if (m->poll_id == c->poll_id || floor_frame) {
        c->round_seen++;
        c->round_frames = c->round_seen + m->left;   /* the frames say how many there are */
    }
    deliver_in_order(c);
    if (lv > 0) {
        c->peer_round_end = now + (uint64_t)m->left * (level_air(lv) + burst_gap_ms(lv));
        c->peer_round_lv = lv;
        c->peer_round_n = c->round_frames > 0 ? c->round_frames : 1;
    }
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
        /* The peer takes the turn; its round follows in this keydown.  It
         * names the last poll it heard: if that is not the one I have out,
         * my poll was lost. */
        bool heard_mine = (m->poll_id & 0x0F) == (c->my_poll_id & 0x0F);
        c->polls_unheard = c->polled_since_handover && !heard_mine ? c->polls_unheard + 1 : 0;
        c->polled_since_handover = false;
        c->peer_has_data = m->has_data;   /* it will want the turn back */
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
    c->floor_waiting = false; c->floor_silent = 0; c->pat_waiting = false;
    apply_need(c, m);
    c->peer_has_data = m->has_data;
    c->floor_pats_heard = 0;
    if (!c->sending) c->send_start_ms = now;
    c->tx_poll_id = m->poll_id;
    c->heard_poll_id = (uint8_t)(m->poll_id & 0x0F);
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
    car_trace(c, "tx polled lv=%d n=%d loss=%.2f", m->level, m->n, c->tx_loss);
    /* While I send, what the payload decoder must catch is the peer's
     * handover round, which comes on the rung I last polled it on -- not on
     * mine.  Bound to mine, on air (DATAC1 one way, DATAC3 the other) it
     * found the handover's DATAC3 frame only after decoding the handover and
     * rebinding, 300 ms before that frame began, and lost it; the loss was
     * scored against DATAC3. */
    bind_rx(c, peer_ctl_on_floor(c) ? 0 : c->drove_peer ? c->poll_level : m->level);
    arm(c, CAR_T_SEND, now + ISS_GUARD_MS);
}

/* ---- entry points ---------------------------------------------------------- */
/* A start needs a margin over the rung's own threshold: the ARQ's 5 dB
 * gear-shift hysteresis for the fast rungs, which fading takes down (a 2 dB
 * margin started QAM16C2 at fading +15 dB and took 214 s against 98), but
 * only DATAC3_START_MARGIN_DB for DATAC3.  At 5 dB there, on the bench, a
 * session called at 1 dB -- where DATAC3 decodes every frame down to -2 dB
 * on AWGN -- started on DATAC15 every time and climbed by probes, and one
 * probe lost to a fade held it there for 40-odd frames.  In the sim the
 * smaller margin is 6 % faster at cliff +1 / +2 and 7-11 % on fading +1 / +3. */
#define DATAC3_START_MARGIN_DB 2.0f
static const struct { int mode; float min_db; } START[] = {
    { FREEDV_MODE_QAM16C2, ARQ_SNR_MIN_QAM16C2_DB }, { FREEDV_MODE_DATAC17, ARQ_SNR_MIN_DATAC17_DB },
    { FREEDV_MODE_DATAC1,  ARQ_SNR_MIN_DATAC1_DB },
    { FREEDV_MODE_DATAC3,  ARQ_SNR_MIN_DATAC3_DB - ARQ_SNR_HYST_DB + DATAC3_START_MARGIN_DB },
    /* DATAC4 is slower than DATAC15 here: never a start.  Below -3 dB the
     * floor starts faster than DATAC15 (7-9 % in the sim at -7..-5 dB, fixed
     * and fading); above it DATAC15 still wins in fading (a DATAC4 start cost
     * 9 % there at 0 dB). */
    { FREEDV_MODE_DATAC15, -3.0f - ARQ_SNR_HYST_DB },
};

int car_start_level(float snr_db)
{
    for (size_t i = 0; i < sizeof(START) / sizeof(START[0]); i++)
        if (snr_db >= START[i].min_db + ARQ_SNR_HYST_DB)
            return level_of_mode(START[i].mode);
    return 0;                  /* the MFSK floor */
}

void car_init(car_t *c, const car_io_t *io, int rx_level, int tx_level)
{
    memset(c, 0, sizeof(*c));
    c->io = *io;
    c->loss_est = c->tx_loss = 0.1;
    c->snr_level = rx_level;
    c->peer_snr_level = tx_level >= 0 ? tx_level : rx_level;
    c->tx_level = -1;
}

/* The caller: the callee's ACCEPT was the first poll, a one-frame probe on the
 * start rung.  Answer it now.  Until a poll comes back the round is treated as
 * an unconfirmed handover: the callee only knows it is connected once it hears
 * us, so a lost first round is repeated with a handover poll ahead of it,
 * which names the mode -- the callee never has to key blind. */
void car_start_sender(car_t *c, uint64_t now)
{
    c->sending = true;
    c->send_start_ms = now;
    c->tx_level = c->peer_snr_level; c->tx_n = 1; c->tx_poll_id = 1;
    c->handover_unconfirmed = true;
    arm(c, CAR_T_SEND, now);
}

/* The callee: the ACCEPT is on the air, as poll 1. */
void car_start_receiver(car_t *c, uint64_t now)
{
    start_driving(c, 1, c->snr_level, 1, now, now);
}

static void note_peer_ctl_deaf(car_t *c, bool deaf)
{
    if (deaf != c->peer_ctl_deaf) {
        c->peer_ctl_deaf = deaf;
        car_trace(c, "the peer hears me %s the control mode: control on %s",
                  deaf ? "below" : "above", deaf ? "MFSK" : "the control mode");
    }
}

bool car_peer_ctl_deaf(const car_t *c) { return c && ctl_on_floor(c); }

void car_seed_ctl_deaf(car_t *c, bool mine, bool peers)
{
    c->ctl_deaf = mine;
    c->peer_ctl_deaf = peers;
    car_trace(c, "at connect: I hear the peer %s the control mode, it hears me %s it",
              mine ? "below" : "above", peers ? "below" : "above");
}

void car_on_frame(car_t *c, uint64_t now, const uint8_t *bytes, size_t len, int mode, bool control,
                  float snr_db)
{
    msg_t m;
    c->last_carrier_ms = now;
    if (snr_db != 0.0f) {
        c->snr_ema = c->snr_valid ? 0.7f * c->snr_ema + 0.3f * snr_db : snr_db;
        c->snr_valid = true;
        bool deaf = c->ctl_deaf ? c->snr_ema < CTL_DEAF_OFF_DB : c->snr_ema < CTL_DEAF_ON_DB;
        if (deaf != c->ctl_deaf) {
            c->ctl_deaf = deaf;
            car_trace(c, "I hear the peer %s the control mode (%.1f dB)", deaf ? "below" : "above", c->snr_ema);
        }
    }
    if (control) {
        if (decode_ctl(bytes, len, &m)) { note_peer_ctl_deaf(c, m.ctl_deaf); on_ctl(c, now, &m); }
        return;
    }
    if (len >= 1 + CAR_POLL_BYTES && bytes[0] == CTL_FLOOR_MARK) {   /* control, on the floor */
        if (decode_ctl(bytes + 1, len - 1, &m)) { note_peer_ctl_deaf(c, m.ctl_deaf); on_ctl(c, now, &m); }
        return;
    }
    int lv = level_of_mode(mode);
    if (lv >= 0 && decode_data(bytes, len, &m)) { note_peer_ctl_deaf(c, m.ctl_deaf); on_data(c, now, &m, lv); }
}

/* Idle, with something to send: take the turn with a handover, after
 * listening (the T_WAIT path repeats an unconfirmed handover). */
static void take_turn_if_idle(car_t *c, uint64_t now)
{
    if (c->idle && !c->tx_busy && has_data(c)) {
        c->idle = false;
        c->sending = true;
        c->send_start_ms = now;
        c->handover_unconfirmed = true;
        disarm(c, CAR_T_POLL); disarm(c, CAR_T_SENSE); disarm(c, CAR_T_SEND);
        arm(c, CAR_T_WAIT, now);
    }
}

void car_on_tx_done(car_t *c, uint64_t now)
{
    c->tx_busy = false;
    int after = c->after_tx;
    c->after_tx = AFTER_NONE;
    /* Data the application gave us while our last poll (nothing left either
     * way) was on the air: that poll went idle without it, so take the turn
     * now -- a request answered at once, as UUCP does, arrives just then. */
    take_turn_if_idle(c, now);
    if (after == AFTER_ROUND) {
        arm_sender_wait(c, now);
    } else if (after == AFTER_PATTERN) {
        /* At the floor a sender that missed the pattern continues on silence,
         * later.  Above it, one that missed it waits for a poll: poll as soon
         * as its round has not started. */
        uint64_t start = now - TAIL_MS + ISS_GUARD_MS;
        bool floor = c->poll_level == 0;
        /* Not after a pattern that stood in for lost polls: that sender
         * repeats on its own timer, and a re-poll timed from the same round
         * end landed on its repeat (sim, awgn:0.25 bidir). */
        if (!floor && !c->pattern_for_lost_polls)
            arm(c, CAR_T_SENSE, start + SENSE_MS + PATTERN_SENSE_EXTRA_MS);
        c->floor_round_end = start + HEAD_MS + round_air(c->poll_level, c->poll_n) + TAIL_MS;
        arm_poll(c, c->floor_round_end + WINDOW_MARGIN_MS + (floor ? FLOOR_LATE_MS : 0));
    } else if (after == AFTER_POLL) {
        /* The peer heard the poll as its last frame ended, keys after its
         * guard, and should be heard by SENSE_MS after that. */
        uint64_t start = now - TAIL_MS + ISS_GUARD_MS;
        bool floor = c->poll_level == 0 && c->io.pattern;
        /* Only to follow the round's carrier (on_sense_timer): silence is no
         * longer answered with a re-poll -- on air (car14, car16, 3 %) a DATAC4
         * probe at -6.5 dB, and a DATAC1 one at 4.5 dB, unsensed 1.3 s in, had
         * the re-poll keyed into them. */
        arm(c, CAR_T_SENSE, start + SENSE_MS + (floor ? FLOOR_SENSE_MS : 0));
        c->floor_round_end = start + HEAD_MS + round_air(c->poll_level, c->poll_n) + TAIL_MS;
        arm_poll(c, c->floor_round_end + WINDOW_MARGIN_MS + (floor ? FLOOR_LATE_MS : 0));
    }
}

void car_on_pattern(car_t *c, uint64_t now, int kind)
{
    c->last_carrier_ms = now;
    c->floor_yielded = false;
    c->floor_pats_heard++;
    if (!c->sending || c->tx_busy || !(c->floor_waiting || c->pat_waiting)) return;
    car_trace(c, "tx pattern %s on lv=%d", kind == CAR_PATTERN_BREAK ? "BREAK" : "ACK", c->tx_level);
    if (c->pat_waiting) {
        /* The receiver answers a round above the floor with a pattern only
         * when every frame of it came: what I sent of each block is in.  The
         * blocks stay open until a poll retires them -- a wrong guess costs
         * pieces, never data -- but the next round moves on to what it still
         * needs, and to new blocks. */
        c->pat_waiting = false;
        c->handover_unconfirmed = false;
        disarm(c, CAR_T_WAIT);
        for (int b = 0; b < c->nsb; b++) {
            car_sblock_t *sb = &c->sb[b];
            sb->need = sb->need > sb->round_sent ? sb->need - sb->round_sent : 0;
            sb->round_sent = 0;
        }
        if (!has_data(c)) { c->sending = false; c->idle = true; return; }
        arm(c, CAR_T_SEND, now + ISS_GUARD_MS);
        return;
    }
    c->floor_waiting = false;
    c->floor_silent = 0;
    if (c->floor_tx_end && now > c->floor_tx_end) {
        uint32_t d = (uint32_t)(now - c->floor_tx_end);
        c->floor_delay_ms = c->floor_delay_ms ? (3 * c->floor_delay_ms + d) / 4 : d;
    }
    c->handover_unconfirmed = false;       /* the receiver hears me */
    c->tx_n = keydown_cap(0);              /* streaming: full floor rounds */
    disarm(c, CAR_T_WAIT);
    /* Only for the block my last round carried: a BREAK repeated after I
     * moved on must not retire the next one. */
    /* And never for a block the receiver cannot have decoded yet: fewer than
     * K distinct pieces of it have gone out.  A wrong BREAK is data lost. */
    if (kind == CAR_PATTERN_BREAK && c->nsb > 0 && c->sb[0].id == c->floor_blk &&
        c->sb[0].sent >= c->sb[0].K) {
        size_t len = (size_t)c->sb[0].len;
        memmove(&c->sb[0], &c->sb[1], (size_t)(c->nsb - 1) * sizeof(c->sb[0]));
        c->nsb--;
        if (c->io.tx_confirmed) c->io.tx_confirmed(c->io.ctx, len);
    }
    if (!has_data(c)) { c->sending = false; c->idle = true; return; }
    arm(c, CAR_T_SEND, now + ISS_GUARD_MS);
}

bool car_expect_pattern(const car_t *c)
{
    return (c->floor_waiting || c->pat_waiting) && !c->tx_busy;
}

void car_on_app_data(car_t *c, uint64_t now)
{
    take_turn_if_idle(c, now);
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
            if (defer_to_slot(c, CAR_T_WAIT, now)) break;
            if (c->handover_unconfirmed) { send_handover(c); break; }
            /* No answer, and the peer, with data of its own, is due to take
             * the turn: the likeliest answer is its handover, missed in a fade
             * -- a keydown that carries its first round after it.  Continuing on
             * silence I keyed into that round (sim, fade:-5 both ways: 25 of 35
             * collisions).  Hold instead, until the peer is heard: it repeats an
             * unconfirmed handover, and if it was not handing over its window
             * ends in a pattern or a poll, either of which moves me.  Not on a
             * timer of my own: one a floor round long ended just as the peer's
             * window did, and the two keyed together (sim, cliff:-5 both
             * ways).  The silence nudge stays the backstop. */
            if (c->floor_waiting && c->peer_has_data && !c->floor_yielded &&
                now + HANDOVER_SOON_MS >= c->send_start_ms + TURN_QUANTUM_MS) {
                c->floor_yielded = true;
                car_trace(c, "tx: no answer, the peer's turn is due -- holding for it");
                arm(c, CAR_T_WAIT, now + SENDER_SILENCE_MS);
                break;
            }
            if (c->floor_waiting && ++c->floor_silent > FLOOR_SILENT_MAX) {
                /* Nothing back for a while: stop streaming, wait to be polled. */
                c->floor_waiting = false;
                arm(c, CAR_T_WAIT, now + SENDER_SILENCE_MS);
                break;
            }
            send_round(c);
            break;
        }
    }
}

/* Three exchanges at the rung in use, either way: enough for a lost round or
 * handover to be repeated and answered.  A fixed 30 s drain dropped the last
 * UUCP reply at the floor, where one exchange takes 40-50 s (on air, car29 at
 * 2 %: the final 36 bytes behind a lost handover; the caller's uucico failed). */
uint64_t car_drain_budget_ms(const car_t *c)
{
    int lv = c->tx_level >= 0 ? c->tx_level : 0;
    if (c->drove_peer && c->poll_level < lv) lv = c->poll_level;   /* the slower way, once we poll */
    uint64_t ctl = level_air(0) > mode_air(ARQ_CONTROL_MODE) && (ctl_on_floor(c) || peer_ctl_on_floor(c))
                   ? level_air(0) : mode_air(ARQ_CONTROL_MODE);
    uint64_t exchange = GUARD_MS + HEAD_MS + ctl + CHAIN_GAP_MS + round_air(lv, keydown_cap(lv)) +
                        TAIL_MS + FLOOR_ANSWER_MS;
    return 3 * exchange;
}
