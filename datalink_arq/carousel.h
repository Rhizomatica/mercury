/* datalink_arq/carousel.h -- the carousel data plane: erasure-coded rounds
 *
 * One connected session moves data both ways.  Per direction the RECEIVER
 * chooses: what each block still needs, and the next round's mode and size
 * ("send me up to N frames in mode M").  The receiver chooses the mode because
 * it is the side whose payload decoder must be bound to it in advance, and the
 * side that sees what arrives.
 *
 * Who keys, and when (docs/CAROUSEL-TURNS.md, proved in specs/turns): the
 * caller is the session's only timing master (M).  It alone keys on timers of
 * its own, and only where the callee (S) cannot be on the air.  S keys only in
 * slots anchored to the end of an M keydown it decoded.  M carries both
 * directions: it sends its own rounds, which S answers with its poll; and it
 * polls S for S's rounds, which S sends in its slots.
 *
 * Data is cut into blocks of up to CAR_MAX_K pieces of CAR_PIECE bytes,
 * Reed-Solomon coded (rs_erasure.h): any K pieces rebuild a block, so a poll
 * says how many pieces a block needs, never which.  Pieces fit every mode, so
 * a block is never re-encoded when the mode changes.
 *
 * At the MFSK floor the control mode no longer gets through, so a round is
 * answered with a pattern instead (car_io_t.pattern): ACK "keep going", BREAK
 * "the block you are on is delivered".  There silence also means "keep going":
 * M continues once S's slot is over, S in its continuation slots.
 *
 * This module is the protocol only: no threads, no clock of its own, no I/O
 * but the callbacks.  tests/sim/carousel_bench.c runs two of them on the sim
 * channel; the ARQ runtime runs one against the modem.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CAROUSEL_H
#define CAROUSEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rs_erasure.h"

#define CAR_PIECE        24     /* bytes per piece: fits DATAC15 with the headers */
#define CAR_MAX_K        96     /* data pieces per block (see carousel.c)        */
#define CAR_WIN          8      /* open blocks at once                           */
#define CAR_NLEVELS      7      /* the payload mode ladder (3 bits on the wire) */
#define CAR_FRAME_MAX    1280   /* largest payload-mode frame, bytes             */
#define CAR_POLL_BYTES   14     /* a poll fills one control-mode (DATAC16) frame */
#define CAR_KEYDOWN_MAX  16     /* frames in one keydown (a round)               */
#define CAR_MLOG         16     /* M's keydowns whose slots may still be open    */

/* One frame of a keydown, as the modem sends it. */
typedef struct {
    int      mode;              /* FREEDV_MODE_* */
    size_t   len;
    uint8_t  bytes[CAR_FRAME_MAX];
    uint32_t gap_ms;            /* silence before this frame (not the first) */
} car_frame_t;

typedef struct {
    /* Key the radio for these frames, back to back with their gaps.  The
     * frames are only valid during the call.  The runtime calls
     * car_on_tx_done() when the keydown has ended. */
    void   (*keydown)(void *ctx, const car_frame_t *frames, int n);
    /* Bind the payload decoder to this mode (the control decoder always runs). */
    void   (*bind_rx)(void *ctx, int mode);
    /* Is the peer on the air?  A decoder in sync, held for the sync-loss
     * latency after it ends. */
    bool   (*peer_keyed)(void *ctx);
    /* The application's queue: take up to max bytes; how many are waiting. */
    size_t (*tx_read)(void *ctx, uint8_t *buf, size_t max);
    size_t (*tx_pending)(void *ctx);
    /* Bytes for the application, in order. */
    void   (*deliver)(void *ctx, const uint8_t *buf, size_t len);
    /* This many of our bytes have reached the peer. */
    void   (*tx_confirmed)(void *ctx, size_t len);
    /* Key a pattern: CAR_PATTERN_ACK or CAR_PATTERN_BREAK.  Optional: without
     * it the floor polls in the control mode like every other rung.  The
     * runtime calls car_on_tx_done() when it has ended. */
    void   (*pattern)(void *ctx, int kind);
    /* One line on each decision (optional): what the receiver measured and
     * chose, what the sender was told.  For debug logs. */
    void   (*trace)(void *ctx, const char *line);
    void    *ctx;
    /* Keys each block's check (the session's CRC seed): a block rebuilt from
     * another session's pieces fails it. */
    uint32_t block_key;
} car_io_t;

enum { CAR_PATTERN_ACK = 0, CAR_PATTERN_BREAK = 1 };

enum { CAR_T_M, CAR_T_S, CAR_T_F, CAR_NTIMERS };   /* M's next decision or keydown; S's next slot;
                                                    * S's fallback to the floor (listening only) */

typedef struct {
    uint8_t  id;
    int      K, len, next, need;          /* next: the next piece index to send */
    int      app_len;                     /* of len, the application's bytes (the rest: the check) */
    int      resend;                      /* data piece the receiver waits on, -1 */
    int      sent;                        /* distinct pieces sent so far (capped) */
    int      round_sent;                  /* fresh pieces of it in my last round */
    bool     stopped;                     /* a floor BREAK said it is in; a poll retires it */
    uint8_t  data[CAR_MAX_K][CAR_PIECE];
} car_sblock_t;

typedef struct {
    bool     known, done;
    uint8_t  id;
    int      K, len, have;          /* len: with the check */
    int      delivered;             /* bytes already handed to the application */
    bool     got[RS_MAX_PIECES];
    uint8_t  piece[RS_MAX_PIECES][CAR_PIECE];
} car_rblock_t;

typedef struct {
    uint64_t e;                     /* when it ended (exact: my own keydown) */
    uint32_t r, d;                  /* the longest S keydown it allows; S's lateness */
    uint32_t p;                     /* the period of its continuation slots */
    uint8_t  k;                     /* continuation slots after the response slot */
    uint8_t  done;                  /* slots I heard S's keydown in, end to end */
    uint8_t  mk;                    /* its id */
    bool     pat;                   /* a pattern: S's answers name no id */
} car_mlog_t;

typedef struct {
    car_io_t io;
    bool     master;                /* the caller: the session's only timing master */
    bool     idle;                  /* nothing in flight either way */
    int      rx_level;              /* what the payload decoder is bound to */
    int      snr_level;             /* where the SNR measured here starts the peer */
    bool     ctl_deaf;              /* I hear the peer below the control mode: its
                                     * control must reach me on MFSK (reported) */
    bool     peer_ctl_deaf;         /* ...and the peer, of me, as it reports */
    float    snr_ema;               /* SNR of what the peer sends us, smoothed */
    bool     snr_valid;
    uint64_t deadline[CAR_NTIMERS]; /* 0 = disarmed */
    uint64_t last_carrier_ms;       /* the peer was last heard on the air */
    bool     tx_busy;               /* our keydown is on the air */

    /* M: the turn */
    int      phase;                 /* sending my data, polling the peer's, or idle */
    uint64_t phase_start;
    uint8_t  mk;                    /* id of my next keydown, mod 16 */
    car_mlog_t mlog[CAR_MLOG];
    int      nmlog;
    int      m_kind;                /* my keydown on the air, its id, and the slots it opens: */
    uint8_t  m_id;
    uint32_t m_r, m_d, m_p;
    uint8_t  m_k;
    int      m_state;               /* waiting for S's answer, or for a free time */
    int      plan, plan_lv, plan_n; /* the keydown decided */
    bool     m_heard;               /* S keyed since my last keydown, and I heard it end */
    bool     s_answered;            /* ...with a poll or a pattern for my round */
    int      last_lv, last_nf;      /* my last round, for a REQ */
    uint8_t  last_mk;
    bool     idle_poll;             /* my poll on the air only asks whether S has data */
    uint32_t idle_backoff_ms;
    int      reqs;                  /* REQs in a row unanswered */

    /* S: the slots */
    struct {
        bool     on;
        uint8_t  mk;                /* the M keydown (its id; patterns carry none) */
        int      kind;
        uint64_t eh;                /* its end, as I learned it */
        uint32_t p;                 /* slot period */
        uint8_t  k, i;              /* continuation slots; the next slot */
        bool     sent_round;        /* I sent a round in this anchor's slots */
    } anc;
    int      req_lv, req_n;         /* a REQ: M's round I may not have heard */
    uint8_t  req_mk;
    uint8_t  rx_mk;                 /* the M round whose frames I am counting */
    uint64_t s_tx_end;              /* when my last keydown ended */

    /* as sender */
    car_sblock_t sb[CAR_WIN];
    int      nsb;
    uint8_t  next_blk_id;
    int      tx_level, tx_n;        /* as the last poll directed (-1: never polled) */
    uint8_t  tx_poll_id;            /* the poll my rounds answer (its keydown id) */
    int      tx_slot;               /* ...in this slot (S) */
    bool     tx_pat_anchor;         /* ...anchored on a pattern (S) */
    double   tx_loss;               /* the receiver's loss estimate, for the margin */
    bool     floor_waiting;         /* my floor round is out: a pattern may come */
    bool     pat_waiting;           /* my round above the floor is out: a pattern
                                     * ("it came whole, the same again") may come */
    int      floor_silent;          /* floor rounds in a row with no answer */
    uint8_t  floor_blk;             /* the block my last floor round carried */
    int      peer_snr_level;        /* where the SNR the peer measures starts me */
    bool     peer_has_data;         /* the peer said it has data for me */

    /* as receiver: the peer's direction, measured here */
    double   lv_sent[CAR_NLEVELS], lv_lost[CAR_NLEVELS];
    int      lv_rounds[CAR_NLEVELS], lv_dead_run[CAR_NLEVELS];
    int      lv_probe_fails[CAR_NLEVELS]; /* its rounds in a row that lost half */
    uint32_t lv_round_at[CAR_NLEVELS];
    uint64_t lv_probe_at[CAR_NLEVELS], lv_backoff_ms[CAR_NLEVELS];
    uint32_t polls;
    double   loss_est;
    uint8_t  poll_id;               /* my last poll's keydown id (M), or the one I answer (S) */
    int      poll_level, poll_n;    /* what I asked the peer for */
    int      round_seen, round_frames;
    int      round_lv;              /* the rung the frames came on */
    bool     status_seen;
    int      silent_polls;
    bool     peer_unopened;
    uint8_t  peer_hi;
    uint8_t  polled_base;           /* the window base my last poll reported */
    bool     round_fresh;           /* this round carried a block I do not have yet */
    int      stale_rounds;          /* floor rounds in a row that carried nothing new */
    const char *failed;             /* car_failed() */
    bool     peer_hi_known;
    car_rblock_t rb[CAR_WIN];
    uint8_t  rbase;
    int      done_in_round;
    bool     rx_break;              /* the last round delivered the block it carried */
    int      floor_patterns;        /* patterns since my last poll */
    bool     floor_streaming;       /* a floor round came since my last poll */
    car_frame_t txbuf[CAR_KEYDOWN_MAX];
} car_t;

/* Start a connected session.  rx_level: the rung the SNR measured here gives
 * the peer's direction; tx_level: the rung the peer gave ours (-1: unknown,
 * then rx_level until the peer's frames say).  car_start_sender() on the
 * caller, the session's timing master; car_start_receiver() on the callee. */
void car_init(car_t *c, const car_io_t *io, int rx_level, int tx_level);
/* Seed, from the connect exchange, whether each end hears the other below the
 * control mode (see ctl_on_floor in carousel.c).  Frames keep both current. */
void car_seed_ctl_deaf(car_t *c, bool mine, bool peers);
/* The peer hears us below the control mode, as it last reported: our control
 * frames go on MFSK.  The session's own control frames follow the same rule. */
bool car_peer_ctl_deaf(const car_t *c);
void car_start_sender(car_t *c, uint64_t now);
void car_start_receiver(car_t *c, uint64_t now);

/* A frame came in.  control: the control decoder (DATAC16) produced it;
 * otherwise mode is the payload decoder's.  snr_db: the decoder's estimate
 * for it, 0 when unknown. */
void car_on_frame(car_t *c, uint64_t now, const uint8_t *bytes, size_t len, int mode, bool control,
                  float snr_db);
void car_on_tx_done(car_t *c, uint64_t now);
/* A pattern was heard: CAR_PATTERN_ACK or CAR_PATTERN_BREAK. */
void car_on_pattern(car_t *c, uint64_t now, int kind);
/* Whether a pattern can arrive now (the runtime runs its detector only then). */
bool car_expect_pattern(const car_t *c);
/* The application queued data. */
void car_on_app_data(car_t *c, uint64_t now);

/* The earliest armed deadline (UINT64_MAX: none), and firing what is due. */
uint64_t car_next_deadline(const car_t *c);
void     car_on_time(car_t *c, uint64_t now);

/* The ladder rung for an SNR, mapped the way trunk enters a mode from DATAC15. */
int  car_start_level(float snr_db);
int  car_level_mode(int level);
bool car_is_idle(const car_t *c);
/* The session can no longer be trusted: a block failed its check, or did not
 * decode.  What it carried may already be partly delivered (the stream runs
 * ahead of the check), so the session must end -- never carry on.  NULL: fine. */
const char *car_failed(const car_t *c);
/* Every keydown of mine opens with a NAV header this long (0: none), where
 * car_wants_nav() says the peer may not sense it otherwise (car_set_nav_below_db). */
void car_set_nav_ms(uint32_t ms);
void car_set_nav_below_db(float db);
void car_set_nav_loss(double loss);   /* ...or at this loss estimate (default 0.3) */
bool car_wants_nav(const car_t *c);
/* A rung's geometry (bytes of data per frame, frames per full keydown, its
 * airtime, a round's fixed overhead): for computing bounds. */
void car_rung_geometry(int lv, bool floor_patterns, int *bytes_per_frame, int *frames,
                       uint64_t *round_air_ms, uint64_t *overhead_ms);

/* How long delivering what is left may take on the rung the session is on:
 * a few exchanges -- my round, the peer's control answer -- there.  Seconds
 * on a fast rung; minutes at the MFSK floor. */
uint64_t car_drain_budget_ms(const car_t *c);
/* Bytes taken from the application that the peer has not confirmed yet. */
size_t car_tx_inflight(const car_t *c);

#endif
