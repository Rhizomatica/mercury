/* datalink_arq/carousel.h -- the carousel data plane: erasure-coded rounds,
 * driven by the receiver
 *
 * One connected session moves data both ways.  Per direction the RECEIVER
 * drives: it polls in the control mode ("send me up to N frames in mode M,
 * and here is what each block still needs"), the sender answers with a round
 * -- one keydown of up to N bursts -- and keys only when polled.  The
 * receiver chooses the mode because it is the side whose payload decoder must
 * be bound to it in advance, and the side that sees what arrives.
 *
 * Data is cut into blocks of up to CAR_MAX_K pieces of CAR_PIECE bytes,
 * Reed-Solomon coded (rs_erasure.h): any K pieces rebuild a block, so a poll
 * says how many pieces a block needs, never which.  Pieces fit every mode, so
 * a block is never re-encoded when the mode changes.
 *
 * The receiver takes the turn with a HANDOVER poll that announces its own
 * first round, sent in the same keydown.
 *
 * At the MFSK floor the control mode no longer gets through, so the receiver
 * answers a round with a pattern instead (car_io_t.pattern): ACK "keep
 * going", BREAK "the block you are on is delivered".  Any piece is useful
 * there, so silence also means "keep going", and the sender sends only its
 * oldest block, so a BREAK always names it.
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
#define CAR_KEYDOWN_MAX  (1 + 16) /* a handover poll and its round               */

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
} car_io_t;

enum { CAR_PATTERN_ACK = 0, CAR_PATTERN_BREAK = 1 };

enum { CAR_T_POLL, CAR_T_SEND, CAR_T_WAIT, CAR_T_SENSE, CAR_NTIMERS };

typedef struct {
    uint8_t  id;
    int      K, len, next, need;          /* next: the next piece index to send */
    int      resend;                      /* data piece the receiver waits on, -1 */
    int      sent;                        /* distinct pieces sent so far (capped) */
    int      round_sent;                  /* fresh pieces of it in my last round */
    uint8_t  data[CAR_MAX_K][CAR_PIECE];
} car_sblock_t;

typedef struct {
    bool     known, done;
    int      K, len, have;
    int      delivered;             /* bytes already handed to the application */
    bool     got[RS_MAX_PIECES];
    uint8_t  piece[RS_MAX_PIECES][CAR_PIECE];
} car_rblock_t;

typedef struct {
    car_io_t io;
    bool     sending;               /* holds the turn */
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
    int      after_tx;              /* what to arm when it ends */

    /* as sender */
    car_sblock_t sb[CAR_WIN];
    int      nsb;
    uint8_t  next_blk_id;
    int      tx_level, tx_n;        /* as the last poll directed (-1: never polled) */
    uint32_t tx_poll_id;
    double   tx_loss;               /* the receiver's loss estimate, for the margin */
    bool     handover_unconfirmed;  /* my handover round is out, no poll yet */
    bool     floor_waiting;         /* a floor round is out: a pattern may come */
    bool     pat_waiting;           /* a round above the floor is out: a pattern
                                     * ("it came whole, the same again") may come */
    int      floor_silent;          /* floor rounds in a row with no answer */
    uint8_t  floor_blk;             /* the block my last floor round carried */
    uint64_t floor_tx_end;          /* when my last floor round ended */
    uint32_t floor_delay_ms;        /* how long answers take after it (average) */
    int      peer_snr_level;        /* where the SNR the peer measures starts me */
    size_t   confirmed_pending;     /* retired block bytes not yet reported */

    /* as receiver: the peer's direction, measured here */
    double   lv_sent[CAR_NLEVELS], lv_lost[CAR_NLEVELS];
    int      lv_rounds[CAR_NLEVELS], lv_dead_run[CAR_NLEVELS];
    int      lv_probe_fails[CAR_NLEVELS]; /* its rounds in a row that lost half */
    uint32_t lv_round_at[CAR_NLEVELS];
    uint64_t lv_probe_at[CAR_NLEVELS], lv_backoff_ms[CAR_NLEVELS];
    uint32_t polls;
    double   loss_est;
    uint32_t poll_id;
    int      poll_level, poll_n;
    bool     drove_peer;            /* poll_level is a rung I have polled the peer on */
    int      round_seen, round_frames;
    bool     round_heard, status_seen;
    int      silent_polls;
    bool     peer_unopened;
    uint8_t  peer_hi;
    bool     peer_hi_known;
    uint64_t drive_start;
    car_rblock_t rb[CAR_WIN];
    uint8_t  rbase;
    int      done_in_round;
    bool     rx_break;              /* the last round delivered the block it carried */
    bool     last_was_pattern;      /* my last answer was a pattern, not a poll */
    int      floor_patterns;        /* patterns since my last poll */
    bool     polled_since_handover; /* a poll of mine is out since the peer's last handover */
    int      polls_unheard;         /* ...answered by another handover instead, in a row */
    uint8_t  my_poll_id;            /* the id of the last poll I sent */
    uint8_t  heard_poll_id;         /* the id of the last poll I heard (sent back in my handovers) */
    bool     pattern_for_lost_polls;/* my last pattern stood in for polls the peer was not hearing */
    bool     floor_streaming;       /* a floor round came since my last poll */
    bool     probe_off_floor;       /* my last poll asked a floor stream for another rung */
    bool     floor_fallback;        /* ...went unanswered: I listen for the floor round it kept sending */
    bool     floor_hold;            /* an empty floor window: waiting out its continuation */
    uint64_t floor_round_end;       /* when the floor round I wait for should have ended */
    /* The peer's last round heard above the floor: a sender that hears no
     * poll repeats it SENDER_SILENCE_MS after it (see on_poll_timer). */
    uint64_t peer_round_end;
    int      peer_round_lv, peer_round_n;
    bool     peer_has_data;         /* the peer's last poll said it has data for me */
    uint64_t send_start_ms;         /* when this sending turn of mine began */
    bool     floor_yielded;         /* silence near the peer's turn: I stopped for its handover */
    int      probe_lv, probe_n;     /* what that poll asked for */
    uint64_t probe_after_ms;        /* the stream's last frame I heard before it */
    car_frame_t txbuf[CAR_KEYDOWN_MAX];
} car_t;

/* Start a connected session.  rx_level: the rung the SNR measured here gives
 * the peer's direction; tx_level: the rung the peer gave ours (-1: unknown,
 * then rx_level until the peer's frames say).  The caller is polled first --
 * the callee's ACCEPT is that poll, naming tx_level -- so car_start_sender()
 * on the caller and car_start_receiver() on the callee. */
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
bool car_is_sending(const car_t *c);
bool car_is_idle(const car_t *c);
/* Bytes taken from the application that the peer has not confirmed yet. */
size_t car_tx_inflight(const car_t *c);

#endif
