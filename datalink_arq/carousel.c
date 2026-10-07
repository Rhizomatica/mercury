/* datalink_arq/carousel.c -- the carousel data plane (see carousel.h)
 *
 * The design was chosen on tests/sim/carousel_bench.c, which runs this module;
 * tests/sim/README.md has the measurements behind each choice.
 *
 * Wire formats (no compatibility with the stop-and-wait ARQ is kept):
 *
 *   DATA, in a payload mode, filling the frame:
 *     b0  frames left in the round (4) | data not yet cut into blocks (1) |
 *         the slot it went in (3: 0 for M's own rounds, docs/CAROUSEL-TURNS.md)
 *     b1  poll id (4) | highest block opened, mod 16 (4)
 *     then segments, one per block: 4 bytes
 *         block id mod 16 (4) | K - 1 (7) | piece count (6) | first piece
 *         index (8) | pad bytes in the block's last data piece (5) | in the
 *         FIRST segment header only: anchored on a pattern (1), control-deaf
 *         (1), else 0 0
 *       and count pieces with consecutive indices (mod 256).  A zero count
 *       ends the frame.  (The first header is always there: a payload mode
 *       frame holds at least FRAME_HDR + SEG_HDR bytes.)
 *   POLL / REQ / STATUS, CAR_POLL_BYTES, in the control mode -- or on
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
/* NAV headers (car_set_nav_ms) are not sent inside a session: the turn rules
 * (docs/CAROUSEL-TURNS.md) keep the two ends off each other without carrier
 * sense, and a header was 0.7 s of air on every keydown below 0 dB, and a
 * longer wait on every lost answer, for nothing.  The setters stay, for a
 * station that warns third parties. */
static uint32_t g_nav_ms;
void car_set_nav_ms(uint32_t ms) { g_nav_ms = ms; }
static float g_nav_below_db = 99.0f;
static double g_nav_loss = 0.3;
void car_set_nav_below_db(float db) { g_nav_below_db = db; }
void car_set_nav_loss(double loss) { g_nav_loss = loss; }
bool car_wants_nav(const car_t *c) { (void)c; return false; }
static uint32_t nav_lead(const car_t *c) { (void)c; return 0; }
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

enum { M_DATA, M_POLL, M_REQ, M_STATUS };

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
static bool has_data(const car_t *c);
static bool peer_direction_done(const car_t *c);
/* Nothing in flight either way.  The callee: no data of its own, and the
 * caller's all delivered -- its anchors say nothing about that. */
bool car_is_idle(const car_t *c)
{
    return c->master ? c->idle : !has_data(c) && peer_direction_done(c);
}

size_t car_tx_inflight(const car_t *c)
{
    size_t n = 0;
    for (int b = 0; b < c->nsb; b++) n += (size_t)c->sb[b].app_len;
    return n;
}

const char *car_failed(const car_t *c) { return c->failed; }

/* A block's check: CRC-32 (IEEE) of the session's key, the block's id and K,
 * and its bytes, in the block's last BLOCK_CHECK bytes.  The frame CRC16
 * passes about one bad frame in 65536, and a block is rebuilt from many: one
 * wrong piece -- a corrupt frame that passed, or a frame of an earlier
 * session with the same seed -- and the block fails its check instead of
 * reaching the application.  Room for a MAC once sessions are keyed. */
#define BLOCK_CHECK 4
static uint32_t crc32_step(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc;
}
static uint32_t block_check(const car_t *c, uint8_t id, int K, const uint8_t (*piece)[CAR_PIECE], int n)
{
    uint8_t h[6] = { (uint8_t)c->io.block_key, (uint8_t)(c->io.block_key >> 8),
                     (uint8_t)(c->io.block_key >> 16), (uint8_t)(c->io.block_key >> 24), id, (uint8_t)K };
    uint32_t crc = crc32_step(0xFFFFFFFFu, h, sizeof h);
    for (int i = 0; n > 0; i++, n -= CAR_PIECE)
        crc = crc32_step(crc, piece[i], (size_t)(n < CAR_PIECE ? n : CAR_PIECE));
    return ~crc;
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
static int pieces_per_frame(int lv) { return (mode_payload(LADDER[lv]) - FRAME_HDR - SEG_HDR) / CAR_PIECE; }
static uint64_t round_air(int lv, int n)
{
    return (uint64_t)n * level_air(lv) + (uint64_t)(n > 0 ? n - 1 : 0) * burst_gap_ms(lv);
}

/* A turn lasts TURN_QUANTUM_MS, but never less than two full rounds on the
 * rung in use: each change of direction costs a control exchange, and at
 * the floor, where a round alone is 28 s, a 30 s turn changed direction
 * every round (sim, fade:-5 both ways: 132 changes, a poll each). */
static int keydown_cap(int lv);
static int round_n(const car_t *c);
static uint64_t turn_quantum(int lv)
{
    uint64_t two = 2 * round_air(lv, keydown_cap(lv));
    return two > TURN_QUANTUM_MS ? two : TURN_QUANTUM_MS;
}
static uint64_t turn_cap(int lv) { return turn_quantum(lv) + (TURN_CAP_MS - TURN_QUANTUM_MS); }

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
    int      slot;                  /* DATA: the slot it went in */
    bool     pat_anchor;            /* DATA: ...of a pattern of M's */
} msg_t;

static void encode_ctl(const msg_t *m, uint8_t *b)
{
    memset(b, 0, CAR_POLL_BYTES);
    int type = m->type == M_POLL ? 0 : m->type == M_REQ ? 1 : 2;
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
    m->type = type == 0 ? M_POLL : type == 1 ? M_REQ : M_STATUS;
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
    /* Out-of-range fields mean a corrupt frame that passed its CRC: drop it
     * rather than act on it (spec risk R19). */
    for (int o = 0; o < CAR_WIN; o++)
        if (m->need[o] != FB_UNSEEN && m->need[o] > CAR_MAX_K) return false;
    if (m->gap >= CAR_MAX_K) return false;
    return true;
}

static bool decode_data(const uint8_t *b, size_t len, msg_t *m)
{
    if (len < FRAME_HDR) return false;
    memset(m, 0, sizeof(*m));
    m->type = M_DATA;
    m->left = b[0] >> 4;
    m->unopened = (b[0] >> 3) & 1;
    m->slot = b[0] & 7;
    m->poll_id = b[1] >> 4;
    m->hi = b[1] & 0x0F;
    if (len >= FRAME_HDR + SEG_HDR) {
        m->ctl_deaf = b[FRAME_HDR + SEG_HDR - 1] & 1;
        m->pat_anchor = (b[FRAME_HDR + SEG_HDR - 1] >> 1) & 1;
    }
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

static void keydown(car_t *c, car_frame_t *fr, int n)
{
    c->tx_busy = true;
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
    size_t len = c->io.tx_read(c->io.ctx, buf, (size_t)max_k * CAR_PIECE - BLOCK_CHECK);
    if (!len) return;
    car_sblock_t *s = &c->sb[c->nsb++];
    s->id = c->next_blk_id++;
    s->app_len = (int)len;
    s->len = (int)len + BLOCK_CHECK;
    s->K = (s->len + CAR_PIECE - 1) / CAR_PIECE;
    s->next = 0;
    s->need = s->K;
    s->resend = -1;
    s->sent = 0;
    s->stopped = false;
    memset(s->data, 0, sizeof(s->data));
    memcpy(s->data, buf, len);
    uint32_t chk = block_check(c, s->id, s->K, (const uint8_t (*)[CAR_PIECE])s->data, (int)len);
    for (int i = 0; i < BLOCK_CHECK; i++, len++)
        s->data[len / CAR_PIECE][len % CAR_PIECE] = (uint8_t)(chk >> (8 * i));
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
        if (need == 0) { retired += (size_t)c->sb[b].app_len; continue; }
        c->sb[b].need = need;
        c->sb[b].stopped = false;             /* a BREAK it contradicts was not the receiver's */
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
    int live = 0;                             /* the oldest block no BREAK stopped */
    while (live < c->nsb && c->sb[live].stopped) live++;
    for (int b = 0; b < c->nsb; b++) queued += (int)ceil(c->sb[b].need * (1.0 + margin));
    while (queued < n * ppf && c->nsb < CAR_WIN && unopened(c) && !(floor && live < c->nsb) &&
           (c->nsb == 0 || (uint8_t)(c->next_blk_id - c->sb[0].id) < CAR_WIN))
    {
        open_block(c, block_k_for(lv));
        queued += (int)ceil(c->sb[c->nsb - 1].need * (1.0 + margin));
    }
    if (!c->nsb || n < 1) return 0;
    for (int b = 0; b < c->nsb; b++) c->sb[b].round_sent = 0;
    int nsb_all = c->nsb;
    /* A floor round carries one block: the oldest a BREAK has not stopped,
     * or, every one stopped, repair for the oldest until a poll retires them
     * -- or says one is still needed after all.  It goes in slot 0 for the
     * round and back after. */
    int fb = 0;
    if (floor) {
        live = 0;
        while (live < c->nsb && c->sb[live].stopped) live++;
        fb = live < c->nsb ? live : 0;
        if (fb) { car_sblock_t t = c->sb[0]; c->sb[0] = c->sb[fb]; c->sb[fb] = t; }
        c->nsb = 1; c->floor_blk = c->sb[0].id;
    }
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
    if (fb) { car_sblock_t t = c->sb[0]; c->sb[0] = c->sb[fb]; c->sb[fb] = t; }
    for (int i = 0; i < nf; i++) {
        fr[i].bytes[0] = (uint8_t)((nf - 1 - i) << 4 | (unopened(c) ? 1 : 0) << 3 | (c->tx_slot & 7));
        fr[i].bytes[1] = (uint8_t)((poll_id & 0x0F) << 4 | ((c->next_blk_id - 1) & 0x0F));
        if (c->ctl_deaf) fr[i].bytes[FRAME_HDR + SEG_HDR - 1] |= 1;
        if (c->tx_pat_anchor) fr[i].bytes[FRAME_HDR + SEG_HDR - 1] |= 2;
    }
    return nf;
}

static int keydown_cap(int lv);

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
 * doubts the rung, and each loss costs a round.  But never fewer than a run
 * that would be unlikely (DEAD_P) at the delivery the rung showed while it
 * delivered: on NVIS DATAC3 delivers ~30 % of frames, a whole 7-frame round
 * of it is lost 6 % of the time, and each such round declared it dead and
 * sent the session to DATAC4 and the floor (sim, nvis seed 2: 2975 s, where
 * staying on DATAC3 takes ~1800). */
#define DEAD_P 0.02
static bool level_dead(const car_t *c, int lv)
{
    int run = level_marginal(c, lv) ? 2 : DEAD_RUN;
    double p = c->lv_p_ok[lv];
    if (p > 0.0) {
        if (p > 0.95) p = 0.95;
        int need = (int)ceil(log(DEAD_P) / log(1.0 - p));
        if (need > run) run = need;
    }
    return c->lv_dead_run[lv] >= run;
}

static void measure_level(car_t *c, int lv, int frames, double loss, uint64_t now)
{
    c->lv_sent[lv] = LV_DECAY * c->lv_sent[lv] + frames;
    c->lv_lost[lv] = LV_DECAY * c->lv_lost[lv] + frames * loss;
    if (loss >= 1.0) c->lv_dead_run[lv] += frames;
    else {
        c->lv_dead_run[lv] = 0;
        c->lv_p_ok[lv] = 1.0 - c->lv_lost[lv] / c->lv_sent[lv];   /* what it delivers */
    }
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
static int keydown_cap(int lv);
/* For bounds (tests/sim/carousel_bound.c): a rung's geometry as the carousel
 * itself sees it -- data bytes per frame, frames per full keydown, that
 * keydown's airtime and the fixed cost of a round around it. */
void car_rung_geometry(int lv, bool floor_patterns, int *bytes_per_frame, int *frames,
                       uint64_t *round_air_ms, uint64_t *overhead_ms)
{
    static const car_t z;   /* the overhead macros read c: no session, no NAV lead */
    const car_t *c = &z;
    int n = keydown_cap(lv);
    if (bytes_per_frame) *bytes_per_frame = pieces_per_frame(lv) * CAR_PIECE;
    if (frames) *frames = n;
    if (round_air_ms) *round_air_ms = round_air(lv, n);
    if (overhead_ms) *overhead_ms = lv == 0 && floor_patterns ? FLOOR_OVERHEAD_MS : ROUND_OVERHEAD_MS;
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
/* The round a rung has earned: one frame more than it recently delivered,
 * twice that where the SNR has room to spare -- the round size then doubles
 * from a probe as TCP's window does, where adding one frame took an 8 KB
 * transfer at 20 dB four rounds (1, 2, 3, 2) and a poll's overhead each. */
static bool level_marginal(const car_t *c, int lv);
static int level_earned(const car_t *c, int lv)
{
    double got = c->lv_sent[lv] - c->lv_lost[lv];
    return 1 + (int)(level_marginal(c, lv) || !c->snr_valid ? got : 2.0 * got);
}
/* What a measured rung delivers per second, in the rounds it has earned. */
static double level_goodput(const car_t *c, int lv)
{
    int earned = level_earned(c, lv);
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
    /* A probe that came whole is followed by a second round there before
     * the argmax judges the rung: near its threshold one frame scores 1/2,
     * under the rung below, and the session went straight back down.  On air
     * (20 %, both ways, every run) DATAC17 at 6.5 dB delivered its probe and
     * the session stayed on DATAC1 the rest of the transfer, and QAM16C2 at
     * 13 dB went back to DATAC17 for a 4-frame round; in the sim at 12 dB a
     * clean QAM16C2 was left after one frame.  The second round is the one
     * the probe earned (two frames); two lost there declare a marginal rung
     * dead.  Not in the lower half of the SNR's doubt: there a frame that
     * came is more likely luck than the rung working (sim, cliff:10: a
     * QAM16C2 frame at 10 dB, then two lost, +5 %). */
    for (int up = best + 1; up < CAR_NLEVELS; up++)
        if (c->lv_rounds[up] && c->lv_round_at[up] == c->polls && c->lv_sent[up] < 3.0 &&
            c->lv_lost[up] < 0.05 && level_allowed(c, up) && level_potential(c, up) > best_gp &&
            !(c->snr_valid && c->snr_ema < level_min_db(up) - SNR_GATE_DB / 2)) {
            car_trace(c, "rx confirms lv=%d", up);
            return up;
        }
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
    int earned = level_earned(c, lv);
    if (cap > earned) cap = earned;
    /* With my own data waiting, the peer's turn ends at TURN_CAP_MS: ask
     * only for what fits before it.  The cap is checked between rounds, and
     * a full keydown after it stretched a 45 s turn to 75 s.  (M keeps the
     * turns; S asks only for M's rounds.) */
    if (c->master && has_data(c)) {
        uint64_t held = now - c->phase_start;
        int64_t left = (int64_t)turn_cap(lv) - (int64_t)held;
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
    r->done = true;
    c->done_in_round++;
    if (rs_decode(r->K, CAR_PIECE, idx, pcs, out) != 0) {
        if (!c->failed) c->failed = "a block did not decode";
        return;
    }
    for (int i = 0; i < r->K; i++) memcpy(r->piece[i], out[i], CAR_PIECE);
    int len = r->len - BLOCK_CHECK;
    uint32_t want = 0;
    for (int i = 0; i < BLOCK_CHECK; i++)
        want |= (uint32_t)r->piece[(len + i) / CAR_PIECE][(len + i) % CAR_PIECE] << (8 * i);
    if (len < 0 || block_check(c, r->id, r->K, (const uint8_t (*)[CAR_PIECE])r->piece, len) != want) {
        car_trace(c, "rx block %d failed its check", r->id);
        if (!c->failed) c->failed = "a block failed its check";
    }
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
    while (!c->failed) {
        car_rblock_t *r = &c->rb[c->rbase % CAR_WIN];
        if (!r->known) break;
        int len = r->len - BLOCK_CHECK, upto;   /* the check is not the application's */
        if (r->done) {
            upto = len;
        } else {
            int j = 0;
            while (j < r->K && r->got[j]) j++;
            upto = j * CAR_PIECE < len ? j * CAR_PIECE : len;
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

static void fill_poll(car_t *c, msg_t *m)
{
    memset(m, 0, sizeof(*m));
    m->base = c->rbase;
    c->polled_base = c->rbase;
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

static void take_pieces(car_t *c, const msg_t *m)
{
    for (int g = 0; g < m->nseg; g++) {
        const seg_t *sg = &m->seg[g];
        int off = (sg->block - c->rbase) & 0x0F;
        if (off >= CAR_WIN) { c->rx_break = true; continue; }   /* delivered already */
        car_rblock_t *r = &c->rb[(uint8_t)(c->rbase + off) % CAR_WIN];
        if (!r->known) {
            r->known = true;
            r->id = (uint8_t)(c->rbase + off);
            r->K = sg->K; r->len = sg->K * CAR_PIECE - sg->pad;
        } else if (sg->K != r->K || sg->K * CAR_PIECE - sg->pad != r->len) {
            /* Two frames disagree on what the block is: one was corrupt and
             * passed its CRC, or came from another session -- and the first
             * one seen may be it, so neither can be kept. */
            car_trace(c, "rx segment of block %d: K/len %d/%d against %d/%d",
                      r->id, sg->K, sg->K * CAR_PIECE - sg->pad, r->K, r->len);
            if (!c->failed) c->failed = "two frames disagree on a block";
            continue;
        }
        if (r->done) { c->rx_break = true; continue; }   /* delivered: still sent */
        c->round_fresh = true;
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
}

/* ---- turns: who keys, and when (docs/CAROUSEL-TURNS.md) ------------------- *
 * The caller (M) is the session's only timing master: it alone keys on timers
 * of its own, and only where the callee (S) cannot be on the air.  S keys only
 * in slots anchored to the end of an M keydown it decoded: the response slot,
 * TG_MS after that end, and, for a keydown that asked for S's rounds, up to k
 * continuation slots one period apart -- the floor's "silence means keep
 * going".  Both ends compute every bound from the anchoring keydown alone;
 * specs/turns proves the rules keep the two off the air together under any
 * loss, with no carrier sense.  Hearing S only changes what M sends, and lets
 * it reuse a slot it heard S finish in.
 *
 * Every bound counts the longest lead a keydown may carry (LEAD_MAX_MS). */
#define TG_MS        ISS_GUARD_MS     /* S's slot opens this long after the end */
#define TURN_G_MS    GUARD_MS         /* M's guard either side of a slot */
#define D_OFDM_MS    1000             /* the latest S decodes an M keydown: OFDM, */
#define D_MFSK_MS    4500             /* MFSK (on air ~3.7 s after the unkey), */
#define D_PAT_MS     800              /* a pattern (detected ~0.24 s after it) */
#define LEAD_MAX_MS  (110 + 300)      /* tx delay and head silence, with a margin */
#define K_FLOOR      2                /* continuations of a floor stream */
#define K_ABOVE      0                /* ...above the floor none: M re-polls */
/* S hears M only on the mode its payload decoder is bound to (the control
 * decoder aside), and binds it where its own last poll asked M to send.
 * That poll may be lost: M then keys on, on the rung or control mode it last
 * knew -- MFSK, say, while S listens on DATAC4 -- and S, which may not key
 * without an anchor, never hears it (sim, fade:-5 both ways seed 23: stuck
 * from 453 s on).  So after S_FALLBACK_MS with nothing decoded from M, S
 * listens at the floor, where M's floor rounds and MFSK control both come
 * through.  Listening only: it costs nothing to safety. */
#define S_FALLBACK_MS 90000
/* Idle, M asks whether S has data -- first soon after the last traffic,
 * since the callee's application answers what it just got (UUCP's request
 * and reply), then backing off.  At 10 s and doubling across idle spells,
 * on air (20 %, both ways) each UUCP turnaround waited 10-20 s for the ask
 * the callee may no longer start itself. */
#define IDLE_POLL_MIN_MS 3000
#define IDLE_POLL_MAX_MS 60000        /* doubling up to this */

enum { KD_NONE, KD_POLL, KD_ROUND, KD_PATTERN_ACK, KD_PATTERN_BREAK, KD_REQ, KD_DONE, KD_BOTH };
enum { PH_IDLE, PH_SEND, PH_RECV };
enum { MS_WAIT, MS_PLAN, MS_IDLE };
enum { ANC_POLL, ANC_DONE, ANC_ROUND, ANC_REQ, ANC_PATTERN, ANC_BOTH };

static uint32_t ctl_air_max(const car_t *c)
{
    uint64_t a = mode_air(ARQ_CONTROL_MODE);
    if (c->io.pattern && level_air(0) > a) a = level_air(0);
    return (uint32_t)a;
}
/* The longest S keydown of each kind.  S's control goes on MFSK when the
 * keydown it answers said its sender is control-deaf (the bit in every frame,
 * which S takes before it answers): s_ctl_for(c, my ctl_deaf) is exact for
 * an answer to my keydown, s_ctl_ms the most it can be. */
static uint32_t s_ctl_for(const car_t *c, bool deaf)
{
    return LEAD_MAX_MS + (uint32_t)(deaf && c->io.pattern ? level_air(0) : mode_air(ARQ_CONTROL_MODE)) + TAIL_MS;
}
static uint32_t s_ctl_ms(const car_t *c) { return LEAD_MAX_MS + ctl_air_max(c) + TAIL_MS; }
static uint32_t s_round_ms(int lv, int n) { return LEAD_MAX_MS + (uint32_t)round_air(lv, n) + TAIL_MS; }
static uint32_t s_pat_ms(void) { return LEAD_MAX_MS + PATTERN_AIR_MS + TAIL_MS; }
static uint32_t r_cap_ms(void)
{
    uint32_t m = 0;
    for (int lv = 0; lv < CAR_NLEVELS; lv++)
        if (s_round_ms(lv, keydown_cap(lv)) > m) m = s_round_ms(lv, keydown_cap(lv));
    return m;
}
static uint32_t d_of_mode(int mode) { return mode == MERCURY_MODE_MFSK ? D_MFSK_MS : D_OFDM_MS; }
/* The slot period of an anchor S learned up to d late, whose rounds are at
 * most r: the round, then room for M to hear it end (however late it began)
 * and answer with control, guarded.  A continuation slot opens only after
 * that.  r is what both ends know of the anchor: the round a poll asked
 * for, or a full floor round after a pattern (continuations exist only at
 * the floor). */
static uint32_t slot_period(const car_t *c, uint32_t d, uint32_t r)
{
    return r + d + (c->io.pattern ? D_MFSK_MS : D_OFDM_MS) + 2 * TURN_G_MS + s_ctl_ms(c);
}
static uint32_t r_floor_ms(void) { return s_round_ms(0, keydown_cap(0)); }

/* ---- M: where S may be ---- */
static uint64_t mslot_lo(const car_t *c, const car_mlog_t *l, int i)
{
    (void)c;
    return l->e + l->tg + (uint64_t)i * l->p;
}
static uint64_t mslot_hi(const car_t *c, const car_mlog_t *l, int i) { return mslot_lo(c, l, i) + l->d + l->r; }

static void m_log_add(car_t *c, uint64_t e)
{
    int keep = 0;
    for (int j = 0; j < c->nmlog; j++)
        if (mslot_hi(c, &c->mlog[j], c->mlog[j].k) + TURN_G_MS > e) c->mlog[keep++] = c->mlog[j];
    c->nmlog = keep;
    if (c->nmlog == CAR_MLOG) {                  /* cannot happen: slots expire faster */
        memmove(c->mlog, c->mlog + 1, sizeof(c->mlog[0]) * (CAR_MLOG - 1));
        c->nmlog--;
    }
    car_mlog_t *l = &c->mlog[c->nmlog++];
    l->e = e; l->r = c->m_r; l->d = c->m_d; l->k = c->m_k; l->p = c->m_p; l->done = 0; l->tg = TG_MS;
    l->mk = c->m_id; l->pat = c->m_kind == KD_PATTERN_ACK || c->m_kind == KD_PATTERN_BREAK;
}

/* The earliest time from s at which M may key for len ms: clear, with a guard
 * either side, of every slot S could be on the air in. */
static uint64_t m_free_from(const car_t *c, uint64_t s, uint64_t len)
{
    for (bool moved = true; moved; ) {
        moved = false;
        for (int j = 0; j < c->nmlog; j++)
            for (int i = 0; i <= c->mlog[j].k; i++) {
                if (c->mlog[j].done & (1u << i)) continue;
                uint64_t lo = mslot_lo(c, &c->mlog[j], i), hi = mslot_hi(c, &c->mlog[j], i);
                if (s < hi + TURN_G_MS && lo < s + len + TURN_G_MS) { s = hi + TURN_G_MS; moved = true; }
            }
    }
    return s;
}

/* S keys once per slot: a keydown of S's heard ending at te closes the slot
 * it went in.  Its frames say which: the M keydown it answers (by id, or
 * "a pattern"), and the slot.  And S re-anchors on every keydown of mine it
 * decodes, so once it answers one, it has abandoned the slots of every
 * keydown before it: those close too.  Unidentified (two candidates, or
 * none), only a slot that te can fall in alone is closed. */
static void m_close_from(car_t *c, int j, int i)
{
    c->mlog[j].done |= (uint8_t)(1u << i);
    for (int o = 0; o < j; o++) c->mlog[o].done = 0xFF;
}
static void m_close_slot(car_t *c, uint64_t te, int mk, bool pat, int slot)
{
    uint32_t ds = c->io.pattern ? D_MFSK_MS : D_OFDM_MS;
    int hj = -1, hits = 0;
    if (slot >= 0) {
        for (int j = 0; j < c->nmlog; j++) {
            const car_mlog_t *l = &c->mlog[j];
            if (slot > l->k || (l->done & (1u << slot))) continue;
            if (pat ? !l->pat : (l->pat || l->mk != mk)) continue;
            if (mslot_lo(c, l, slot) <= te && te <= mslot_hi(c, l, slot) + ds) { hits++; hj = j; }
        }
        if (hits == 1) { m_close_from(c, hj, slot); return; }
    }
    int hi_ = -1;
    hits = 0;
    for (int j = 0; j < c->nmlog; j++)
        for (int i = 0; i <= c->mlog[j].k; i++) {
            if (c->mlog[j].done & (1u << i)) continue;
            if (mslot_lo(c, &c->mlog[j], i) <= te && te <= mslot_hi(c, &c->mlog[j], i) + ds) {
                hits++; hj = j; hi_ = i;
            }
        }
    if (hits == 1) c->mlog[hj].done |= (uint8_t)(1u << hi_);
}

/* ---- keydowns, either end ---- */
/* A poll: what I still need of the peer's blocks, and its next round --
 * with my own round after it, in the same keydown, when with_round and I have
 * one: both directions in one exchange (the poll names the round's rung and
 * size, so a receiver that decodes only the poll knows when the keydown
 * ends).  Returns the frames of my round. */
static int send_poll_ex(car_t *c, int lv, int n, uint8_t id, bool with_round)
{
    car_frame_t *fr = c->txbuf;
    msg_t m;
    fill_poll(c, &m);
    m.type = M_POLL;
    m.poll_id = id;
    m.level = lv; m.n = n;
    c->poll_id = id;
    c->polled_loss = m.loss16 / 15.0;
    c->rx_break = false; c->floor_patterns = 0;
    c->floor_streaming = false;
    if (n) { c->poll_level = lv; c->poll_n = n; bind_rx(c, lv); }
    c->round_seen = 0; c->round_frames = n; c->status_seen = false;
    c->done_in_round = 0; c->round_fresh = false; c->rx_tail = false;
    int nd = 0;
    if (with_round && c->tx_level >= 0) {
        nd = build_round(c, c->tx_level, round_n(c), id, fr + 1);
        if (nd) {
            fr[1].gap_ms = CHAIN_GAP_MS;
            c->last_lv = c->tx_level; c->last_nf = nd; c->last_mk = id;
            c->traffic = true;
            m.h_level = c->tx_level; m.h_n = nd; m.h_id = id;
            m.hi = (uint8_t)(c->next_blk_id - 1);
            m.unopened = unopened(c);
        }
    }
    put_ctl(c, &fr[0], &m);
    keydown(c, fr, 1 + nd);
    return nd;
}
static void send_poll(car_t *c, int lv, int n, uint8_t id) { send_poll_ex(c, lv, n, id, false); }

/* A round answered with a pattern: another of the same size comes next. */
static void send_pattern(car_t *c, int kind)
{
    c->rx_break = false;
    if (c->poll_level == 0)
        c->poll_n = keydown_cap(0);     /* what a sender streaming the floor sends */
    c->round_seen = 0; c->round_frames = c->poll_n; c->status_seen = false;
    c->done_in_round = 0; c->round_fresh = false; c->rx_tail = false;
    c->tx_busy = true;
    c->io.pattern(c->io.ctx, kind);
}

static void send_status(car_t *c, uint8_t id)
{
    car_frame_t *fr = c->txbuf;
    msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = M_STATUS; m.poll_id = id;
    m.hi = (uint8_t)(c->next_blk_id - 1); m.snr_level = c->snr_level;
    m.ctl_deaf = c->ctl_deaf;
    m.has_data = has_data(c); m.unopened = unopened(c);
    put_ctl(c, &fr[0], &m);
    keydown(c, fr, 1);
}

/* My round as the peer last directed, its frames naming keydown id.  With
 * nothing to send, a status instead: false. */
/* The frames my next round takes.  S's keydowns are held to what M allowed,
 * but M's rounds are its own: when one frame more than the receiver asked
 * for finishes what I have to send, M sends it.  Otherwise the last few
 * bytes cost a poll and a round of their own (sim, awgn:0.1: an 8 KB
 * transfer's last 116 bytes took 10 s of 58).  Above the floor only: a floor
 * round carries one block. */
static int round_n(const car_t *c)
{
    int n = c->tx_n;
    if (!c->master || c->tx_level <= 0 || n >= keydown_cap(c->tx_level)) return n;
    double margin = c->tx_loss * 1.3 + 0.05;
    int pieces = 0;
    for (int b = 0; b < c->nsb; b++) pieces += (int)ceil(c->sb[b].need * (1.0 + margin));
    size_t pend = c->io.tx_pending ? c->io.tx_pending(c->io.ctx) : 0;
    if (pend) pieces += (int)ceil((double)(pend + BLOCK_CHECK) / CAR_PIECE * (1.0 + margin)) + 1;
    int ppf = pieces_per_frame(c->tx_level);
    return pieces > n * ppf && pieces <= (n + 1) * ppf ? n + 1 : n;
}

static bool send_round(car_t *c, uint8_t id)
{
    car_frame_t *fr = c->txbuf;
    int nf = build_round(c, c->tx_level, round_n(c), id, fr);
    if (!nf) { send_status(c, id); return false; }
    c->floor_waiting = c->tx_level == 0 && c->io.pattern;
    c->pat_waiting = c->tx_level > 0 && c->io.pattern;
    c->last_lv = c->tx_level; c->last_nf = nf; c->last_mk = id;
    c->traffic = true;
    keydown(c, fr, nf);
    return true;
}

/* M: no answer to my round.  Its rung and size go with the ask, so a receiver
 * that heard none of it can score the loss. */
static void send_req(car_t *c, uint8_t id)
{
    car_frame_t *fr = c->txbuf;
    msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = M_REQ; m.poll_id = id;
    m.level = c->last_lv >= 0 ? c->last_lv : 0; m.n = c->last_nf; m.h_id = c->last_mk;
    m.has_data = has_data(c); m.unopened = unopened(c);
    m.hi = (uint8_t)(c->next_blk_id - 1); m.snr_level = c->snr_level; m.ctl_deaf = c->ctl_deaf;
    put_ctl(c, &fr[0], &m);
    keydown(c, fr, 1);
}

/* My round came back answered by a pattern.  False if no round of mine was
 * out to answer. */
static bool tx_on_pattern(car_t *c, int kind)
{
    if (!(c->floor_waiting || c->pat_waiting)) return false;
    car_trace(c, "tx pattern %s on lv=%d", kind == CAR_PATTERN_BREAK ? "BREAK" : "ACK", c->tx_level);
    if (c->pat_waiting) {
        /* Above the floor an ACK says the round came whole: what I sent of
         * each block is in.  A BREAK, that it lost no more than the
         * receiver's last poll said: what I sent less that loss.  The blocks
         * stay open until a poll retires them -- a wrong guess costs pieces,
         * never data. */
        c->pat_waiting = false;
        for (int b = 0; b < c->nsb; b++) {
            car_sblock_t *sb = &c->sb[b];
            int got = kind == CAR_PATTERN_ACK ? sb->round_sent : (int)floor(sb->round_sent * (1.0 - c->tx_loss));
            sb->need = sb->need > got ? sb->need - got : 0;
            sb->round_sent = 0;
        }
        return true;
    }
    c->floor_waiting = false;
    c->floor_silent = 0;
    c->tx_n = keydown_cap(0);              /* streaming: full floor rounds */
    /* "The block you are on is in": only for the block my last round carried,
     * and never for one the receiver cannot have decoded yet.  It stops the
     * block; only a poll retires it (spec risk R5, specs/carousel). */
    for (int b = 0; kind == CAR_PATTERN_BREAK && b < c->nsb; b++)
        if (c->sb[b].id == c->floor_blk && c->sb[b].sent >= c->sb[b].K) {
            c->sb[b].stopped = true;
            c->sb[b].need = 0;
            c->sb[b].resend = -1;
        }
    return true;
}

/* ---- the receiver's answer to a round, either end ---- */
/* At the floor a round is answered by a pattern, with a real poll every
 * FLOOR_POLL_EVERY rounds -- or every FLOOR_POLL_EVERY_DEEP deep below the
 * floor's exit, where a poll cannot take the session off it.  Not with data
 * of my own: the poll's has_data is what tells the peer. */
static int floor_poll_every(const car_t *c)
{
    bool deep = c->snr_valid && c->snr_ema < ARQ_SNR_MIN_DATAC15_DB - FLOOR_DEEP_DB && !has_data(c);
    return deep ? FLOOR_POLL_EVERY_DEEP : FLOOR_POLL_EVERY;
}

static void rx_trace(car_t *c)
{
    if (!c->io.trace) return;
    char gp[120]; int p = 0;
    for (int l = 0; l < CAR_NLEVELS && p < (int)sizeof gp - 24; l++)
        if (c->lv_rounds[l])
            p += snprintf(gp + p, sizeof gp - (size_t)p, " %d:%.0f/%.2f%s", l, 1000.0 * level_goodput(c, l),
                          level_delivery(c, l), level_allowed(c, l) ? "" : "x");
    car_trace(c, "rx round lv=%d seen=%d/%d status=%d snr=%.1f%s gp[lv:B/s/deliv]%s",
              c->master ? c->poll_level : c->round_lv, c->round_seen, c->round_frames, c->status_seen, c->snr_ema,
              c->snr_valid ? "" : "?", gp);
}

/* A round's size is what its frames say -- but only its last frame says it
 * all: with the tail lost, seen + left counted the lost frames as never sent,
 * and a NVIS round of 7 that brought 3 read as "3 of 3, whole" (sim, nvis
 * seed 3: a "same again" pattern, and the loss unscored).  Without the tail,
 * the round is taken to be what I asked for. */
static void rx_measure(car_t *c, int lv, uint64_t now)
{
    int frames = c->round_frames > 0 ? c->round_frames : 1;
    if (!c->rx_tail && c->poll_n > frames) frames = c->poll_n;
    double loss = 1.0 - (double)c->round_seen / frames;
    if (loss < 0) loss = 0;
    c->loss_est = 0.5 * c->loss_est + 0.5 * loss;
    measure_level(c, lv, frames, loss, now);
}

/* What to ask the sender next: KD_POLL (lv, n), a pattern, or KD_DONE. */
static int rx_next(car_t *c, uint64_t now, int *plv, int *pn)
{
    if (peer_direction_done(c)) { car_trace(c, "rx -> done"); return KD_DONE; }
    bool floor = c->poll_level == 0 && c->io.pattern;
    int lv = choose_level(c, now);
    /* Leave the floor only when a poll can reach the sender: asking for
     * another rung re-binds my decoder to it at once.  And a receiver that
     * hears the peer's control only on MFSK stays there. */
    if (floor && lv > 0 && !(c->snr_valid && c->snr_ema >= ARQ_SNR_MIN_DATAC15_DB)) lv = 0;
    if (c->ctl_deaf && c->io.pattern) lv = 0;
    /* A pattern answers a sender that is streaming the floor: one of its
     * floor rounds has come since my last poll. */
    if (c->round_seen > 0 && c->poll_level == 0) c->floor_streaming = true;
    /* A BREAK only stops a block: the sender holds it until a poll, and opens
     * a new one only while its blocks span fewer than CAR_WIN ids. */
    bool window_full = c->peer_hi_known && (uint8_t)(c->peer_hi + 1 - c->polled_base) >= CAR_WIN - 2;
    /* A sender whose every block is stopped sends repair of the oldest until
     * a poll comes: two rounds in a row of nothing new. */
    if (c->round_seen > 0) c->stale_rounds = c->round_fresh ? 0 : c->stale_rounds + 1;
    bool stuck = c->stale_rounds >= 2;
    if (floor && lv == 0 && c->floor_streaming && c->floor_patterns < floor_poll_every(c) && !window_full &&
        !stuck) {
        car_trace(c, "rx -> floor pattern %s", c->rx_break ? "BREAK" : "ACK");
        c->floor_patterns++;
        return c->rx_break ? KD_PATTERN_BREAK : KD_PATTERN_ACK;
    }
    int n = poll_size(c, lv, now);
    /* Above the floor a round to be followed by the same again is answered
     * by a pattern: 0.64 s that reach the sender ~10 dB below a poll, where
     * a lost poll costs a REQ and another poll (NVIS: one poll in four, ~11 s
     * each).  An ACK says the round came whole; a BREAK (above the floor)
     * that it lost no more than my last poll told the sender, which credits
     * the round with that loss -- so a worse round is polled.  Not to a
     * sender whose window is full (only a poll retires its blocks). */
    int frames = c->round_frames > 0 ? c->round_frames : 1;
    if (!c->rx_tail && c->poll_n > frames) frames = c->poll_n;
    double loss = 1.0 - (double)c->round_seen / frames;
    if (c->io.pattern && lv > 0 && lv == c->poll_level && n == c->poll_n && !c->status_seen &&
        c->round_seen > 0 && loss <= c->polled_loss + 0.05 &&
        c->floor_patterns < FLOOR_POLL_EVERY && !window_full) {
        bool whole = c->round_seen >= c->round_frames && c->rx_tail;
        car_trace(c, "rx -> pattern %s (lv=%d n=%d)", whole ? "ACK" : "BREAK", lv, n);
        c->floor_patterns++;
        return whole ? KD_PATTERN_ACK : KD_PATTERN_BREAK;
    }
    car_trace(c, "rx -> poll lv=%d n=%d", lv, n);
    *plv = lv; *pn = n;
    return KD_POLL;
}

/* A frame of the peer's round: its pieces, and (counts) toward the round. */
static void rx_frame(car_t *c, const msg_t *m, int lv, bool counts)
{
    take_pieces(c, m);
    bool floor_frame = lv == 0 && c->poll_level == 0 && c->io.pattern;
    if (floor_frame) c->floor_streaming = true;
    if (counts || floor_frame) {
        c->round_seen++;
        c->round_frames = c->round_seen + m->left;   /* the frames say how many there are */
        c->round_lv = lv;
        if (!m->left) c->rx_tail = true;
    }
    deliver_in_order(c);
}

/* ---- M: deciding, and keying when it may ---- */
static void m_enter(car_t *c, int phase, uint64_t now)
{
    if (c->phase == phase) return;
    c->phase = phase;
    c->phase_start = now;
    static const char *P[] = { "idle", "sending", "polling the peer" };
    car_trace(c, "turn: %s", P[phase]);
}

static void m_plan(car_t *c, int kind, int lv, int n)
{
    c->plan = kind; c->plan_lv = lv; c->plan_n = n;
    c->m_state = MS_PLAN;
}

static uint64_t kind_len(const car_t *c, int kind)
{
    uint64_t head = 110 + nav_lead(c);
    switch (kind) {
    case KD_ROUND:         return head + round_air(c->tx_level, round_n(c)) + TAIL_MS;
    case KD_BOTH:          return head + (ctl_on_floor(c) ? level_air(0) : mode_air(ARQ_CONTROL_MODE)) +
                                  CHAIN_GAP_MS + round_air(c->tx_level, round_n(c)) + TAIL_MS;
    case KD_PATTERN_ACK:
    case KD_PATTERN_BREAK: return 110 + PATTERN_AIR_MS + TAIL_MS;
    default:               return head + (ctl_on_floor(c) ? level_air(0) : mode_air(ARQ_CONTROL_MODE)) + TAIL_MS;
    }
}
static uint64_t plan_len(const car_t *c) { return kind_len(c, c->plan); }

static void m_note(car_t *c, int kind, uint32_t r, uint32_t d, int k, uint32_t rp)
{
    c->m_kind = kind; c->m_r = r; c->m_d = d; c->m_k = (uint8_t)k;
    c->m_p = k ? slot_period(c, d, rp) : 0;
}
static uint32_t my_ctl_d(const car_t *c) { return ctl_on_floor(c) ? D_MFSK_MS : D_OFDM_MS; }

static void m_key_plan(car_t *c)
{
    int kind = c->plan;
    c->plan = KD_NONE;
    c->m_state = MS_WAIT;
    uint8_t id = c->mk;
    c->mk = (c->mk + 1) & 0x0F;
    c->m_id = id;
    c->m_heard = false; c->s_answered = false;
    c->floor_waiting = c->pat_waiting = false;
    uint32_t ctl = s_ctl_for(c, c->ctl_deaf);  /* S answers this keydown's bit */
    switch (kind) {
    case KD_POLL: {
        int lv = c->plan_lv, n = c->plan_n;
        uint32_t rr = n ? s_round_ms(lv, n) : 0;
        int k = n && lv == 0 && c->io.pattern ? K_FLOOR : 0;
        m_note(c, KD_POLL, rr > ctl ? rr : ctl, my_ctl_d(c), k, rr);
        send_poll(c, lv, n, id);
        break;
    }
    case KD_BOTH: {
        /* My poll and my round: S answers with its poll and its round, in
         * one keydown no longer than the two (no continuations: the next
         * exchange is mine to start). */
        int lv = c->plan_lv, n = c->plan_n;
        uint32_t rr = ctl + CHAIN_GAP_MS + s_round_ms(lv, n);
        uint32_t d = my_ctl_d(c), dr = d_of_mode(LADDER[c->tx_level >= 0 ? c->tx_level : 0]);
        m_note(c, KD_BOTH, rr, d > dr ? d : dr, 0, 0);
        if (peer_ctl_on_floor(c) && lv > 0) bind_rx(c, 0);
        if (!send_poll_ex(c, lv, n, id, true))            /* nothing of mine after all */
            m_note(c, KD_POLL, s_round_ms(lv, n) > ctl ? s_round_ms(lv, n) : ctl, my_ctl_d(c),
                   n && lv == 0 && c->io.pattern ? K_FLOOR : 0, s_round_ms(lv, n));
        break;
    }
    case KD_DONE:
        m_note(c, KD_DONE, ctl, my_ctl_d(c), 0, 0);
        send_poll(c, 0, 0, id);
        break;
    case KD_PATTERN_ACK:
    case KD_PATTERN_BREAK: {
        /* S answers with its next round, as it last understood my polls.
         * When I heard the round I answer, answering my latest poll, I know
         * which: above the floor the same again, with no continuations; at
         * the floor a full floor round, continued while it hears nothing.
         * Otherwise any round, and continuations if it is at the floor.  A
         * pattern carries no control-deaf bit: S's status may be either mode. */
        uint32_t cm = s_ctl_ms(c), r = r_cap_ms();
        bool known = c->round_seen > 0, floor = c->poll_level == 0 && c->io.pattern;
        if (known) r = floor ? r_floor_ms() : s_round_ms(c->poll_level, c->poll_n);
        int k = c->io.pattern && (!known || floor) ? K_FLOOR : 0;
        m_note(c, kind, r > cm ? r : cm, D_PAT_MS, k, r_floor_ms());
        send_pattern(c, kind == KD_PATTERN_BREAK ? CAR_PATTERN_BREAK : CAR_PATTERN_ACK);
        break;
    }
    case KD_ROUND:
        /* S answers my round with its poll or a pattern, which I listen for:
         * its control comes on MFSK when I hear it below the control mode. */
        m_note(c, KD_ROUND, ctl > s_pat_ms() ? ctl : s_pat_ms(), d_of_mode(LADDER[c->tx_level]), 0, 0);
        if (peer_ctl_on_floor(c)) bind_rx(c, 0);
        if (!send_round(c, id)) m_note(c, KD_REQ, ctl, my_ctl_d(c), 0, 0);  /* a status went */
        break;
    default:
        m_note(c, KD_REQ, ctl, my_ctl_d(c), 0, 0);
        if (peer_ctl_on_floor(c)) bind_rx(c, 0);
        send_req(c, id);
        break;
    }
}

/* Key the planned keydown now if no slot of S's is in the way; else when. */
static void m_try(car_t *c, uint64_t now)
{
    if (c->tx_busy || c->plan == KD_NONE) return;
    uint64_t at = m_free_from(c, now, plan_len(c));
    /* My round does not fit before S's next slot: S may be streaming on in
     * its continuation slots, and my round would wait them all out (300 s at
     * the floor, sim cliff:-7 bidir).  A REQ fits the gap, stops the stream
     * -- S re-anchors on any keydown of mine it decodes -- and its answer
     * says where S stands on my data. */
    /* Both directions do not fit before S's next slot: the poll alone. */
    if (at > now + TURN_G_MS && c->plan == KD_BOTH && m_free_from(c, now, kind_len(c, KD_POLL)) <= now) {
        c->plan = KD_POLL;
        at = now;
    }
    /* First the largest round that fits the gap: S decodes it and drops its
     * stream as surely as a REQ, and its answer sets the size again.  Else a
     * REQ.  Whichever can start first: the gaps between S's slots come and
     * go, and waiting for the full round passed by every one that fitted
     * something shorter (sim, fade:-5 both ways: 279 s). */
    if (at > now + TURN_G_MS && c->plan == KD_ROUND) {
        int full = c->tx_n, best_n = full;
        uint64_t best = at;
        for (int n = full - 1; n >= 1; n--) {
            c->tx_n = n;
            uint64_t t = m_free_from(c, now, plan_len(c));
            if (t + TURN_G_MS < best) { best = t; best_n = n; }
        }
        c->tx_n = full;
        uint64_t treq = m_free_from(c, now, kind_len(c, KD_REQ));
        if (treq + TURN_G_MS < best) {
            if (treq <= now) {
                car_trace(c, "tx: round blocked by the peer's slots -- asking first");
                c->plan = KD_REQ;
            }
            at = treq;
        } else if (best_n < full) {
            if (best <= now) {
                car_trace(c, "tx: round blocked by the peer's slots -- %d of %d frames first", best_n, full);
                c->tx_n = best_n;
            }
            at = best;
        }
    }
    if (at > now) { arm(c, CAR_T_M, at); return; }
    if (defer_if_busy(c, CAR_T_M, now)) return;     /* another station on the channel */
    m_key_plan(c);
}

/* Both directions in one exchange only above the floor, with the control
 * mode getting through both ways: at the floor the poll is a 13.5 s MFSK
 * frame on every keydown, and patterns and continuations are lost (sim,
 * cliff:-11 both ways 4900 -> 7678 s); there M takes turns instead. */
static bool both_ok(const car_t *c, int lv)
{
    /* ...and on a link that loses little: a lost exchange loses both ways
     * (sim, nvis both ways 3696 -> 4179 s). */
    return lv > 0 && c->tx_level > 0 && !ctl_on_floor(c) && !peer_ctl_on_floor(c) &&
           c->loss_est < 0.3 && c->tx_loss < 0.3;
}

static void m_start_recv(car_t *c, uint64_t now)
{
    m_enter(c, PH_RECV, now);
    c->silent_polls = 0;
    int lv = choose_level(c, now);
    if (c->ctl_deaf && c->io.pattern) lv = 0;
    m_plan(c, has_data(c) && both_ok(c, lv) ? KD_BOTH : KD_POLL, lv, poll_size(c, lv, now));
}

static void m_decide_idle(car_t *c, uint64_t now)
{
    c->idle_poll = false;
    if (has_data(c)) {
        c->idle = false; c->idle_backoff_ms = 0;
        if (c->tx_level < 0) { c->tx_level = c->peer_snr_level; c->tx_n = 1; }
        m_enter(c, PH_SEND, now);
        m_plan(c, KD_ROUND, 0, 0);
        return;
    }
    if (c->peer_has_data) {
        c->idle = false; c->idle_backoff_ms = 0;
        m_start_recv(c, now);
        return;
    }
    /* Nothing either way: S cannot key without me, so ask it now and then. */
    c->idle = true;
    if (c->traffic) { c->traffic = false; c->idle_backoff_ms = 0; }
    m_enter(c, PH_IDLE, now);
    c->idle_backoff_ms = !c->idle_backoff_ms ? IDLE_POLL_MIN_MS
                       : c->idle_backoff_ms * 2 > IDLE_POLL_MAX_MS ? IDLE_POLL_MAX_MS : c->idle_backoff_ms * 2;
    c->plan = KD_NONE;
    c->m_state = MS_IDLE;
    arm(c, CAR_T_M, now + c->idle_backoff_ms);
}

static void m_decide_send(car_t *c, uint64_t now)
{
    bool answered = c->s_answered;
    if (answered) c->reqs = 0;
    if (c->peer_has_data && (!has_data(c) || now - c->phase_start >= turn_quantum(c->tx_level > 0 ? c->tx_level : 0))) {
        car_trace(c, "turn: the peer's (%s)", has_data(c) ? "quantum" : "I am done");
        m_start_recv(c, now);
        return;
    }
    if (!has_data(c)) { m_decide_idle(c, now); return; }
    if (!answered) {
        /* At the floor silence means "keep going"; above it, or after too
         * many, ask the receiver where it stands. */
        if (c->m_kind == KD_ROUND && c->last_lv == 0 && c->io.pattern && ++c->floor_silent <= FLOOR_SILENT_MAX) {
            car_trace(c, "tx: no answer -- continuing (%d)", c->floor_silent);
            m_plan(c, KD_ROUND, 0, 0);
            return;
        }
        /* A round on a rung S had not had my rounds on yet: S, hearing
         * nothing, went back to listening on the last one it had.  Go there
         * once before asking. */
        if (c->m_kind == KD_ROUND && c->ok_lv >= 0 && c->last_lv != c->ok_lv && !c->fell_back) {
            car_trace(c, "tx: no answer on lv=%d -- back to lv=%d", c->last_lv, c->ok_lv);
            c->fell_back = true;
            c->tx_level = c->ok_lv; c->tx_n = 1;
            m_plan(c, KD_ROUND, 0, 0);
            return;
        }
        c->reqs++;
        car_trace(c, "tx: no answer -- asking (%d)", c->reqs);
        m_plan(c, KD_REQ, 0, 0);
        return;
    }
    c->floor_silent = 0;
    m_plan(c, KD_ROUND, 0, 0);
}

static void m_decide_recv(car_t *c, uint64_t now)
{
    if (c->idle_poll) {
        /* My poll only asked whether S has data. */
        c->idle_poll = false;
        if (!c->m_heard || (c->round_seen == 0 && !c->peer_has_data)) { m_decide_idle(c, now); return; }
        c->idle = false; c->idle_backoff_ms = 0;
    }
    if (c->m_kind == KD_DONE) { m_decide_idle(c, now); return; }
    /* Nothing at all of the round: the poll (or pattern) was lost, or the
     * rung is dead.  On a rung that has delivered, ask again once, unscored:
     * two lost polls scored in a row threw DATAC17 away at 12 dB (sim,
     * awgn:0.25: 169 s -> 719 s). */
    if (!c->m_heard && c->poll_level > 0 && c->round_frames > 0 && c->silent_polls == 0 &&
        c->lv_sent[c->poll_level] >= 2.0 && level_delivery(c, c->poll_level) >= 0.5) {
        c->silent_polls = 1;
        car_trace(c, "rx round lv=%d unheard: asking again", c->poll_level);
        m_plan(c, KD_POLL, c->poll_level, c->poll_n);
        return;
    }
    c->silent_polls = 0;
    if (!c->status_seen) rx_measure(c, c->poll_level, now);
    deliver_in_order(c);
    rx_trace(c);
    bool done = peer_direction_done(c);
    uint64_t held = now - c->phase_start;
    bool quantum = !both_ok(c, c->poll_level) &&
                   ((c->done_in_round && held >= turn_quantum(c->poll_level)) || held >= turn_cap(c->poll_level));
    if (has_data(c) && (done || quantum)) {
        car_trace(c, "turn: mine (%s)", done ? "peer done" : "quantum");
        if (c->tx_level < 0) { c->tx_level = c->peer_snr_level; c->tx_n = 1; }
        m_enter(c, PH_SEND, now);
        m_plan(c, KD_ROUND, 0, 0);
        return;
    }
    int lv = 0, n = 0;
    int kind = rx_next(c, now, &lv, &n);
    if (kind == KD_DONE) m_enter(c, PH_IDLE, now);
    /* With data of my own, the answer carries my round: a poll, then it (a
     * pattern carries nothing; it becomes the same poll). */
    if (has_data(c) && kind != KD_DONE && both_ok(c, kind == KD_POLL ? lv : c->poll_level)) {
        if (kind != KD_POLL) { lv = c->poll_level; n = c->poll_n; }
        kind = KD_BOTH;
    }
    m_plan(c, kind, lv, n);
}

static void m_decide(car_t *c, uint64_t now)
{
    switch (c->phase) {
    case PH_SEND: m_decide_send(c, now); break;
    case PH_RECV: m_decide_recv(c, now); break;
    default:      m_decide_idle(c, now); break;
    }
    m_try(c, now);
}

static void m_timer(car_t *c, uint64_t now)
{
    if (c->tx_busy) return;                      /* car_on_tx_done takes over */
    if (c->m_state == MS_PLAN) { m_try(c, now); return; }
    if (c->m_state == MS_IDLE) {
        /* Idle: ask whether S has data, for one frame on its rung. */
        m_enter(c, PH_RECV, now);
        c->idle_poll = true;
        m_plan(c, KD_POLL, c->poll_level, 1);
        m_try(c, now);
        return;
    }
    m_decide(c, now);
}

/* M heard a keydown of S's end at te: decide then (or key, if planned). */
static void m_heard_end(car_t *c, uint64_t te, int mk, bool pat, int slot)
{
    c->m_heard = true;
    m_close_slot(c, te, mk, pat, slot);
    if (c->m_state != MS_IDLE) arm(c, CAR_T_M, te);
}

static void m_on_data(car_t *c, uint64_t now, const msg_t *m, int lv)
{
    if (lv != c->rx_level) return;           /* the payload decoder is on another mode */
    c->traffic = true;
    rx_frame(c, m, lv, m->poll_id == c->poll_id);
    m_heard_end(c, now + (uint64_t)m->left * (level_air(lv) + burst_gap_ms(lv)) + TAIL_MS,
                (int)m->poll_id, m->pat_anchor, m->slot);
}

static void m_on_ctl(car_t *c, uint64_t now, const msg_t *m)
{
    c->peer_snr_level = m->snr_level;
    if (m->type == M_POLL) {                 /* the peer's poll for my rounds */
        apply_need(c, m);
        c->peer_has_data = m->has_data;
        c->tx_loss = m->loss16 / 15.0;
        c->floor_waiting = c->pat_waiting = false;
        c->ok_lv = c->last_lv; c->fell_back = false;
        if (m->n) { c->tx_level = m->level; c->tx_n = m->n; }
        car_trace(c, "tx polled lv=%d n=%d loss=%.2f", m->level, m->n, c->tx_loss);
        c->s_answered = true;
        /* Polled for its round as well, it answered with no round: it had
         * nothing to send, as a status says -- not frames lost. */
        if (!m->h_n && c->m_kind == KD_BOTH && (m->poll_id & 0x0F) == (c->poll_id & 0x0F) && !m->has_data)
            c->status_seen = true;
        if (m->h_n) {                        /* its round follows, in this keydown */
            c->peer_hi = resolve16(c->rbase, m->hi);
            c->peer_hi_known = true; c->peer_unopened = m->unopened;
            m_heard_end(c, now + CHAIN_GAP_MS + round_air(m->h_level, m->h_n) + TAIL_MS,
                        (int)m->poll_id, false, 0);
            return;
        }
    } else if (m->type == M_STATUS) {        /* the peer had nothing to send */
        if (m->poll_id == c->poll_id) c->status_seen = true;
        c->peer_has_data = m->has_data;
        c->peer_hi = resolve16(c->rbase, m->hi);
        c->peer_hi_known = true; c->peer_unopened = m->unopened;
    } else {
        return;                              /* a REQ: only M sends one */
    }
    m_heard_end(c, now + TAIL_MS, (int)m->poll_id, false, 0);
}

/* ---- S: slots ---- */
static void s_anchor(car_t *c, int kind, uint8_t mk, uint64_t eh, uint32_t d, int k, uint32_t rp)
{
    if (c->rx_late) return;                  /* decoded too late to answer (R4) */
    /* More frames of the keydown already anchored on. */
    if (c->anc.on && c->anc.kind == kind && kind != ANC_PATTERN && c->anc.mk == mk && c->anc.i == 0) {
        if (eh < c->anc.eh) { c->anc.eh = eh; arm(c, CAR_T_S, eh + TG_MS); }
        return;
    }
    c->anc.on = true; c->anc.kind = kind; c->anc.mk = mk; c->anc.eh = eh;
    c->anc.p = k ? slot_period(c, d, rp) : 0; c->anc.k = (uint8_t)k; c->anc.i = 0;
    c->anc.sent_round = false;
    c->idle = false;
    arm(c, CAR_T_S, eh + TG_MS);
}

static void s_heard_m(car_t *c, uint64_t now)
{
    if (!c->master && c->io.pattern) arm(c, CAR_T_F, now + S_FALLBACK_MS);
    c->deadline[CAR_T_W] = 0;
}

/* S answers M's round (or its REQ about one) as its receiver. */
static void s_answer(car_t *c, uint64_t now, bool req)
{
    if (req) {
        /* M's round had no answer from me.  None of it came: score it lost. */
        if (c->req_mk != c->rx_mk && c->req_n > 0) {
            c->round_seen = 0; c->round_frames = c->req_n;
            rx_measure(c, c->req_lv, now);
        }
    } else {
        rx_measure(c, c->round_lv, now);
    }
    deliver_in_order(c);
    rx_trace(c);
    int lv = 0, n = 0;
    int kind = rx_next(c, now, &lv, &n);
    switch (kind) {
    case KD_DONE:          send_poll(c, 0, 0, c->anc.mk); break;    /* ack only */
    case KD_PATTERN_ACK:   send_pattern(c, CAR_PATTERN_ACK); break;
    case KD_PATTERN_BREAK: send_pattern(c, CAR_PATTERN_BREAK); break;
    default:
        c->s_asked_new = c->s_prev_lv >= 0 && lv != c->s_prev_lv;
        send_poll(c, lv, n, c->anc.mk);
        break;
    }
}

/* S answers both ways: its poll for M's round, then its own round. */
static void s_answer_both(car_t *c, uint64_t now)
{
    if (c->round_frames > 0) rx_measure(c, c->round_lv, now);
    deliver_in_order(c);
    rx_trace(c);
    int lv = 0, n = 0;
    int kind = rx_next(c, now, &lv, &n);
    if (kind == KD_DONE) { lv = 0; n = 0; }
    else if (kind != KD_POLL) { lv = c->poll_level; n = c->poll_n; }   /* a pattern carries no round */
    if (n && c->ctl_deaf && c->io.pattern) lv = 0;
    c->s_asked_new = n && c->s_prev_lv >= 0 && lv != c->s_prev_lv;
    c->tx_slot = 0; c->tx_pat_anchor = false;
    send_poll_ex(c, lv, n, c->anc.mk, has_data(c));
}

static void s_slot(car_t *c, uint64_t now)
{
    if (!c->anc.on) return;
    if (c->end_req) {
        /* Ending: the session's DISCONNECT goes in this slot instead of an
         * answer -- and in the next, if M keys again without hearing it. */
        car_trace(c, "tx: ending the session in slot %d", c->anc.i);
        c->anc.on = false;
        if (!c->tx_busy && c->io.end) c->io.end(c->io.ctx);
        return;
    }
    if (!c->tx_busy) {                       /* slots are spaced past my keydowns */
        switch (c->anc.kind) {
        case ANC_POLL:
        case ANC_PATTERN:
            if (c->anc.i == 0 || (c->anc.sent_round && has_data(c))) {
                if (c->anc.i > 0) car_trace(c, "tx: no answer -- continuing in slot %d", c->anc.i);
                c->tx_slot = c->anc.i; c->tx_pat_anchor = c->anc.kind == ANC_PATTERN;
                bool r = send_round(c, c->tx_poll_id);
                c->tx_slot = 0; c->tx_pat_anchor = false;
                if (r) c->anc.sent_round = true;
            }
            break;
        case ANC_ROUND:
        case ANC_REQ:
            if (c->anc.i == 0) s_answer(c, now, c->anc.kind == ANC_REQ);
            break;
        case ANC_BOTH:
            if (c->anc.i == 0) s_answer_both(c, now);
            break;
        default:                             /* ANC_DONE */
            if (c->anc.i == 0 && has_data(c)) send_status(c, c->anc.mk);
            else if (!has_data(c)) c->idle = true;
            break;
        }
    }
    if (++c->anc.i > c->anc.k) { c->anc.on = false; return; }
    arm(c, CAR_T_S, c->anc.eh + TG_MS + (uint64_t)c->anc.i * c->anc.p);
}

static void s_on_data(car_t *c, uint64_t now, const msg_t *m, int lv)
{
    if (lv != c->rx_level) return;           /* the payload decoder is on another mode */
    s_heard_m(c, now);
    bool both = c->anc.on && c->anc.kind == ANC_BOTH && c->anc.mk == m->poll_id;
    if (both) {
        rx_frame(c, m, lv, true);
        c->s_prev_lv = lv;
        return;
    }
    if (m->poll_id != c->rx_mk || !(c->anc.on && c->anc.kind == ANC_ROUND && c->anc.mk == m->poll_id)) {
        c->rx_mk = (uint8_t)m->poll_id;      /* a new round of M's */
        c->round_seen = 0; c->round_frames = 0;
        c->done_in_round = 0; c->round_fresh = false; c->rx_tail = false;
    }
    rx_frame(c, m, lv, true);
    c->s_prev_lv = lv;
    s_anchor(c, ANC_ROUND, (uint8_t)m->poll_id,
             now + (uint64_t)m->left * (level_air(lv) + burst_gap_ms(lv)) + TAIL_MS, d_of_mode(LADDER[lv]), 0, 0);
}

static void s_on_ctl(car_t *c, uint64_t now, const msg_t *m, int mode)
{
    s_heard_m(c, now);
    c->peer_snr_level = m->snr_level;
    uint32_t d = d_of_mode(mode);
    uint64_t eh = now + TAIL_MS;
    if (m->type == M_POLL) {
        apply_need(c, m);
        c->peer_has_data = m->has_data;
        c->tx_poll_id = (uint8_t)m->poll_id;
        c->tx_loss = m->loss16 / 15.0;
        c->floor_waiting = c->pat_waiting = false;
        c->floor_silent = 0;
        if (m->h_n) {
            /* M's round follows in this keydown: both ways in one exchange. */
            if (m->n) { c->tx_level = m->level; c->tx_n = m->n; }
            c->rx_mk = (uint8_t)m->poll_id;
            c->round_seen = 0; c->round_frames = m->h_n; c->round_lv = m->h_level;
            c->done_in_round = 0; c->round_fresh = false; c->rx_tail = false;
            c->peer_hi = resolve16(c->rbase, m->hi);
            c->peer_hi_known = true; c->peer_unopened = m->unopened;
            uint32_t dr = d_of_mode(LADDER[m->h_level]);
            s_anchor(c, ANC_BOTH, (uint8_t)m->poll_id, eh + CHAIN_GAP_MS + round_air(m->h_level, m->h_n),
                     d > dr ? d : dr, 0, 0);
        } else if (m->n) {
            c->tx_level = m->level; c->tx_n = m->n;
            car_trace(c, "tx polled lv=%d n=%d loss=%.2f", m->level, m->n, c->tx_loss);
            bool fl = m->level == 0 && c->io.pattern;
            s_anchor(c, ANC_POLL, (uint8_t)m->poll_id, eh, d, fl ? K_FLOOR : 0, s_round_ms(m->level, m->n));
        } else {
            s_anchor(c, ANC_DONE, (uint8_t)m->poll_id, eh, d, 0, 0);
        }
    } else if (m->type == M_REQ) {
        c->req_lv = m->level; c->req_n = m->n; c->req_mk = (uint8_t)m->h_id;
        c->peer_has_data = m->has_data;
        s_anchor(c, ANC_REQ, (uint8_t)m->poll_id, eh, d, 0, 0);
    }
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
    c->s_prev_lv = c->ok_lv = -1;
}

/* The caller: the session's timing master.  The callee's ACCEPT named the
 * rung it starts my rounds on (tx_level); I start its on what I measured. */
void car_start_sender(car_t *c, uint64_t now)
{
    c->master = true;
    c->tx_level = c->peer_snr_level; c->tx_n = 1;
    c->poll_level = c->snr_level; c->poll_n = 1;
    c->phase = PH_IDLE;
    c->phase_start = now;
    c->m_state = MS_WAIT;
    arm(c, CAR_T_M, now);
}

/* The callee: it keys only in slots, so it starts by listening -- for the
 * caller's control, and for its rounds on the rung the ACCEPT gave them. */
void car_start_receiver(car_t *c, uint64_t now)
{
    c->master = false;
    c->poll_level = c->snr_level; c->poll_n = 1;
    bind_rx(c, c->ctl_deaf && c->io.pattern ? 0 : c->poll_level);
    s_heard_m(c, now);
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
                  float snr_db, uint32_t age_ms)
{
    msg_t m;
    c->last_carrier_ms = now;
    /* Times below are the frame's own: when it ended, not when it decoded. */
    uint64_t when = now > age_ms ? now - age_ms : now;
    c->rx_late = age_ms > d_of_mode(control ? ARQ_CONTROL_MODE : mode);
    if (c->rx_late) car_trace(c, "rx frame decoded %u ms late: data only", age_ms);
    now = when;
    if (snr_db != 0.0f) {
        c->snr_ema = c->snr_valid ? 0.7f * c->snr_ema + 0.3f * snr_db : snr_db;
        c->snr_valid = true;
        bool deaf = c->ctl_deaf ? c->snr_ema < CTL_DEAF_OFF_DB : c->snr_ema < CTL_DEAF_ON_DB;
        if (deaf != c->ctl_deaf) {
            c->ctl_deaf = deaf;
            car_trace(c, "I hear the peer %s the control mode (%.1f dB)", deaf ? "below" : "above", c->snr_ema);
        }
    }
    bool ctl = control && decode_ctl(bytes, len, &m);
    if (!control && len >= 1 + CAR_POLL_BYTES && bytes[0] == CTL_FLOOR_MARK)   /* control, on the floor */
        ctl = decode_ctl(bytes + 1, len - 1, &m);
    if (control || ctl) {
        if (!ctl) return;
        note_peer_ctl_deaf(c, m.ctl_deaf);
        if (c->master) m_on_ctl(c, now, &m);
        else           s_on_ctl(c, now, &m, control ? ARQ_CONTROL_MODE : mode);
        return;
    }
    int lv = level_of_mode(mode);
    if (lv < 0 || !decode_data(bytes, len, &m)) return;
    note_peer_ctl_deaf(c, m.ctl_deaf);
    if (c->master) m_on_data(c, now, &m, lv);
    else           s_on_data(c, now, &m, lv);
}

void car_on_tx_done(car_t *c, uint64_t now)
{
    c->tx_busy = false;
    if (!c->master) {                        /* S: its next slot is armed */
        c->s_tx_end = now;
        /* A poll moved my decoder to a rung M's rounds have not come on
         * yet.  If that round never comes, M goes back to the last rung it
         * came on: by then, listen there.  M waits out my answer slot first,
         * so this is in time. */
        if (c->s_asked_new) {
            c->s_asked_new = false;
            uint64_t round_end = now + D_OFDM_MS + TURN_G_MS + LEAD_MAX_MS +
                                 round_air(c->poll_level, c->poll_n) + TAIL_MS + d_of_mode(LADDER[c->poll_level]);
            arm(c, CAR_T_W, round_end);
        }
        return;
    }
    m_log_add(c, now);
    c->m_state = MS_WAIT;
    /* Decide when S's response slot is over, or once its answer is heard. */
    arm(c, CAR_T_M, mslot_hi(c, &c->mlog[c->nmlog - 1], 0) + TURN_G_MS);
}

/* A pattern carries no id and a detector can false-alarm, so one is taken
 * only when it could be the answer: no sooner than its sender could key after
 * my keydown (a turnaround, or M's guard) plus the pattern's own air, and on
 * M, within the slot my keydown opened.  A BREAK the explorer heard 1.2 s
 * after a keydown made S send a round into M's (car_explore, cliff:-5 and
 * cliff:-9 both ways). */
#define PAT_MIN_MS (110 + PATTERN_AIR_MS)
static bool pattern_in_time(const car_t *c, uint64_t now)
{
    if (c->master) {
        if (!c->nmlog) return false;
        const car_mlog_t *l = &c->mlog[c->nmlog - 1];
        return now >= l->e + TG_MS + PAT_MIN_MS && now <= mslot_hi(c, l, 0) + D_PAT_MS;
    }
    if (!c->s_tx_end || now < c->s_tx_end + TURN_G_MS + PAT_MIN_MS) return false;
    /* ...and before my next slot, where I would have gone on regardless */
    return !c->anc.on || now < c->anc.eh + TG_MS + (uint64_t)c->anc.i * c->anc.p;
}

void car_on_pattern(car_t *c, uint64_t now, int kind)
{
    c->last_carrier_ms = now;
    c->rx_late = false;
    if (c->tx_busy || !(c->floor_waiting || c->pat_waiting)) return;
    if (!pattern_in_time(c, now)) {
        car_trace(c, "pattern %s out of time: ignored", kind == CAR_PATTERN_BREAK ? "BREAK" : "ACK");
        return;
    }
    if (!tx_on_pattern(c, kind)) return;
    if (c->master) {
        c->s_answered = true;
        c->ok_lv = c->last_lv; c->fell_back = false;
        /* the answer to my round: my latest keydown */
        m_heard_end(c, now, c->nmlog ? c->mlog[c->nmlog - 1].mk : -1, false, 0);
    } else {
        /* M's answer to my round: my next round is its response.  A pattern
         * names no keydown; my rounds keep the poll id they follow. */
        s_heard_m(c, now);
        s_anchor(c, ANC_PATTERN, 0xFF, now, D_PAT_MS, c->tx_level == 0 && c->io.pattern ? K_FLOOR : 0, r_floor_ms());
    }
}

bool car_expect_pattern(const car_t *c)
{
    return (c->floor_waiting || c->pat_waiting) && !c->tx_busy;
}

void car_on_app_data(car_t *c, uint64_t now)
{
    c->idle = false;
    if (c->master && c->m_state == MS_IDLE && !c->tx_busy) {
        c->m_state = MS_WAIT;
        arm(c, CAR_T_M, now);
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
        if (due == CAR_T_M) m_timer(c, now);
        else if (due == CAR_T_S) s_slot(c, now);
        else if (due == CAR_T_W) {
            if (c->rx_level == c->poll_level && c->s_prev_lv >= 0 && c->poll_level != c->s_prev_lv) {
                car_trace(c, "rx: no round on lv=%d -- back to lv=%d", c->poll_level, c->s_prev_lv);
                c->round_seen = 0; c->round_frames = c->poll_n;
                rx_measure(c, c->poll_level, now);
                bind_rx(c, c->s_prev_lv);
            }
        }
        else if (c->rx_level != 0) {
            car_trace(c, "rx: nothing from the peer in %d s -- listening at the floor", S_FALLBACK_MS / 1000);
            bind_rx(c, 0);
        }
    }
}

/* Three exchanges at the rung in use, either way: enough for a lost round to
 * be asked about and answered.  A fixed 30 s drain dropped the last UUCP
 * reply at the floor, where one exchange takes 40-50 s (on air, car29 at
 * 2 %: the final 36 bytes; the caller's uucico failed). */
uint64_t car_drain_budget_ms(const car_t *c)
{
    int lv = c->tx_level >= 0 ? c->tx_level : 0;
    if (c->poll_level < lv) lv = c->poll_level;          /* the slower way */
    uint64_t exchange = (uint64_t)s_round_ms(lv, keydown_cap(lv)) + s_ctl_ms(c) + 2 * TG_MS +
                        D_MFSK_MS + 2 * TURN_G_MS;
    return 3 * exchange;
}

bool car_is_master(const car_t *c) { return c && c->master; }

uint64_t car_free_at(const car_t *c, uint64_t now, uint64_t len)
{
    return c->master ? m_free_from(c, now, len) : now;
}

/* My control keydown that ended at end; the peer may answer it tg later, as
 * the session's control (MFSK to a control-deaf end), up to D late. */
void car_log_ctl(car_t *c, uint64_t end, uint32_t tg)
{
    if (!c->master) return;
    m_note(c, KD_REQ, s_ctl_ms(c), my_ctl_d(c), 0, 0);
    m_log_add(c, end);
    c->mlog[c->nmlog - 1].tg = tg;
}

void car_request_end(car_t *c)
{
    if (c->master) return;
    c->end_req = true;
    car_trace(c, "ending: the DISCONNECT goes in my next slot");
}
