/* arq_early_pending -- PENDING when a CALL starts arriving, not when it ends.
 *
 * A scanning host (BPQ32) holds its channel on PENDING and hops on its dwell
 * timer otherwise (about 10 s on air).  Mercury's PENDING used to wait for a
 * whole CALL addressed to us: a DATAC16 CALL that started in the last ~4 s of
 * a dwell was cut off by the hop, and an MFSK CALL (13.5 s, 27 s at BW500)
 * never fit in one.  VARA and ARDOP send PENDING as soon as they see a
 * connect request starting, before the callsign, and CANCELPENDING if the
 * frame is corrupt or not for them (G8BPQ, via K5DAT, November 2026).
 *
 * This is the decision logic only, a pure state machine on the caller's
 * clock: arq.c feeds it once per event-loop pass with whether one of the CALL
 * decoders is locked on a frame not yet decoded (modem_rx_frame_in_flight)
 * and sends what it returns.  A lock can be noise or another station's
 * traffic; each early PENDING holds a BPQ32 scanner for up to six scan
 * intervals, so a lock that yields no CALL for us is cancelled within
 * ARQ_EP_GRACE_MS of ending, a lock that never ends is cancelled after
 * ARQ_EP_MAX_LOCK_MS, and false alarms are budgeted.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARQ_EARLY_PENDING_H_
#define ARQ_EARLY_PENDING_H_

#include <stdbool.h>
#include <stdint.h>

/* A lock younger than this sends nothing: freedv and the MFSK listener both
 * trial-sync on noise for a moment.  Small against a 3.74 s DATAC16 CALL. */
#define ARQ_EP_MIN_LOCK_MS     500
/* The longest CALL on air is the BW500 MFSK one, 27 s; a lock held past this
 * is not a CALL. */
#define ARQ_EP_MAX_LOCK_MS     30000
/* After the lock ends, how long a decoded CALL has to reach the FSM (decode,
 * event queue, dispatch) before the PENDING is taken back. */
#define ARQ_EP_GRACE_MS        2000
/* At most this many false alarms in ARQ_EP_FALSE_WINDOW_MS; past it, locks
 * send nothing until the oldest ages out.  A channel carrying other traffic
 * would otherwise keep a scanner parked on it. */
#define ARQ_EP_FALSE_BUDGET    3
#define ARQ_EP_FALSE_WINDOW_MS 120000

typedef enum
{
    ARQ_EP_NONE = 0,
    ARQ_EP_SEND_PENDING,
    ARQ_EP_SEND_CANCEL,
} arq_ep_action_t;

typedef struct
{
    bool     active;          /* an early PENDING is outstanding            */
    bool     spent;           /* this lock was cancelled at the cap: wait for
                               * it to end (or a new one) before re-arming  */
    bool     suppressed;      /* over budget, already logged                */
    uint64_t clear_ms;        /* when the lock was first seen gone; 0: held */
    uint64_t last_age_ms;     /* the lock's age at the previous step        */
    uint64_t false_ms[ARQ_EP_FALSE_BUDGET]; /* recent false alarms, ring    */
    unsigned false_head;

    /* counters, for the log and for tuning on air */
    unsigned n_sent;
    unsigned n_promoted;      /* became a CALL for us                       */
    unsigned n_cancelled;     /* taken back: no CALL for us                 */
    unsigned n_suppressed;    /* locks that sent nothing, over budget       */
} arq_ep_state_t;

void arq_ep_init(arq_ep_state_t *s);

/* One step.  eligible: the feature is on and we are LISTENING with no
 * session; while it is false, an outstanding early PENDING is dropped
 * silently (the host started something itself, or the FSM took the call:
 * call arq_ep_promoted first in that case).  in_flight/age_ms: a CALL
 * decoder is locked on a frame not yet decoded, and for how long. */
arq_ep_action_t arq_ep_step(arq_ep_state_t *s, uint64_t now_ms, bool eligible,
                            bool in_flight, uint64_t age_ms);

/* The FSM took a CALL for us while an early PENDING was outstanding: the
 * FSM's own PENDING carries on from here, nothing to take back. */
void arq_ep_promoted(arq_ep_state_t *s);

#endif /* ARQ_EARLY_PENDING_H_ */
