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
#define CAR_NLEVELS      6      /* the payload mode ladder                       */
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
    void    *ctx;
} car_io_t;

enum { CAR_T_POLL, CAR_T_SEND, CAR_T_WAIT, CAR_T_SENSE, CAR_NTIMERS };

typedef struct {
    uint8_t  id;
    int      K, len, next, need;          /* next: the next piece index to send */
    uint8_t  data[CAR_MAX_K][CAR_PIECE];
} car_sblock_t;

typedef struct {
    bool     known, done;
    int      K, len, have;
    bool     got[RS_MAX_PIECES];
    uint8_t  piece[RS_MAX_PIECES][CAR_PIECE];
} car_rblock_t;

typedef struct {
    car_io_t io;
    bool     sending;               /* holds the turn */
    bool     idle;                  /* nothing in flight either way */
    int      rx_level;              /* what the payload decoder is bound to */
    int      snr_level;             /* where the SNR measured here starts the peer */
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
    int      peer_snr_level;        /* where the SNR the peer measures starts me */
    size_t   confirmed_pending;     /* retired block bytes not yet reported */

    /* as receiver: the peer's direction, measured here */
    double   lv_sent[CAR_NLEVELS], lv_lost[CAR_NLEVELS];
    int      lv_rounds[CAR_NLEVELS], lv_dead_run[CAR_NLEVELS];
    uint32_t lv_round_at[CAR_NLEVELS];
    uint64_t lv_probe_at[CAR_NLEVELS], lv_backoff_ms[CAR_NLEVELS];
    uint32_t polls;
    double   loss_est;
    uint32_t poll_id;
    int      poll_level, poll_n;
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
    car_frame_t txbuf[CAR_KEYDOWN_MAX];
} car_t;

/* Start a connected session.  rx_level: the rung the SNR measured here gives
 * the peer's direction; tx_level: the rung the peer gave ours (-1: unknown,
 * then rx_level until the peer's frames say).  The caller is polled first --
 * the callee's ACCEPT is that poll, naming tx_level -- so car_start_sender()
 * on the caller and car_start_receiver() on the callee. */
void car_init(car_t *c, const car_io_t *io, int rx_level, int tx_level);
void car_start_sender(car_t *c, uint64_t now);
void car_start_receiver(car_t *c, uint64_t now);

/* A frame came in.  control: the control decoder (DATAC16) produced it;
 * otherwise mode is the payload decoder's. */
void car_on_frame(car_t *c, uint64_t now, const uint8_t *bytes, size_t len, int mode, bool control);
void car_on_tx_done(car_t *c, uint64_t now);
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

#endif
