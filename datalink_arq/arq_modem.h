/* HERMES Modem — ARQ Modem interface: action queue and PTT callbacks
 *
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ARQ_MODEM_H_
#define ARQ_MODEM_H_

#include <stdbool.h>
#include <stddef.h>

#include "arq.h"      /* arq_action_t, arq_action_type_t */
#include "arq_fsm.h"  /* arq_session_t                   */

/* ======================================================================
 * Action queue (replaces arq_action_queue in old arq.c)
 *
 * The FSM enqueues TX actions; the modem worker dequeues them.
 * Thread-safe: enqueue from event-loop thread, dequeue from modem thread.
 * ====================================================================== */

/**
 * @brief Initialise the action queue.
 * @param capacity Maximum number of pending actions.
 * @return 0 on success, -1 on allocation failure.
 */
int  arq_modem_queue_init(size_t capacity);

/** @brief Flush and destroy the action queue. */
void arq_modem_queue_shutdown(void);

/**
 * @brief Enqueue a modem action (called from FSM action callbacks).
 * @return 0 on success, -1 if queue full.
 */
int  arq_modem_enqueue(const arq_action_t *action);

/**
 * @brief Dequeue the next modem action (called from modem TX worker).
 *
 * Blocks up to timeout_ms milliseconds waiting for an action.
 *
 * @param action      Output action.
 * @param timeout_ms  Wait timeout in milliseconds.
 * @return true if an action was dequeued, false on timeout.
 */
bool arq_modem_dequeue(arq_action_t *action, int timeout_ms);

/* ======================================================================
 * PTT notifications (modem → ARQ)
 *
 * Called from the modem TX worker when PTT changes state.
 * These generate ARQ_EV_TX_STARTED / ARQ_EV_TX_COMPLETE events and
 * trigger the timing recorder.
 * ====================================================================== */

/**
 * @brief Register the PTT event injection function (called from arq.c init).
 * @param fn  Callback: mode >= 0 and ptt_on=true → TX_STARTED; ptt_on=false → TX_COMPLETE.
 */
void arq_modem_set_event_fn(void (*fn)(int mode, bool ptt_on));

/**
 * @brief Register the station's channel-occupancy probe.
 *
 * Set by the modem at init.  Lets the ARQ layer ask "is there energy on the
 * channel right now" without including modem.h, keeping this header the single
 * ARQ->modem boundary.  Unset (or a detector that is switched off) simply
 * reports not-busy, so the FSM falls back to decoder sync alone.
 */
void arq_modem_set_channel_busy_fn(bool (*fn)(void));

/** @brief Is the channel occupied?  false when no probe is registered. */
bool arq_modem_channel_busy(void);

/**
 * @brief Register the modem's session-seed setter (carousel data plane).
 *
 * The carousel's frames carry the session in their CRC16: with a seed set,
 * the payload decoders accept only seeded frames, the control decoder seeded
 * and plain ones (CALL, ACCEPT, DISCONNECT), and HARQ combining is off --
 * consecutive carousel frames are different codewords.  0 restores plain
 * CRCs and HARQ.
 */
void arq_modem_set_crc_seed_fn(void (*fn)(uint16_t seed));
void arq_modem_crc_seed(uint16_t seed);

/**
 * @brief Notify ARQ that PTT has gone ON.
 * @param mode       FreeDV mode of the frame now on air.
 * @param frame_size Frame size in bytes.
 */
void arq_modem_ptt_on(int mode, size_t frame_size);

/**
 * @brief Notify ARQ that PTT has gone OFF.
 */
void arq_modem_ptt_off(void);

/* ======================================================================
 * Mode selection helpers
 *
 * Used by the modem worker to query preferred RX/TX modes.
 * Delegated to arq_modem.c so mode-selection logic is co-located with
 * the rest of the modem interface.
 * ====================================================================== */

/**
 * @brief Return the preferred RX FreeDV mode for the current session state.
 * @param sess Current session (read-only).
 * @return FREEDV_MODE_* constant.
 */
int arq_modem_preferred_rx_mode(const arq_session_t *sess);

/**
 * @brief Return the preferred TX FreeDV mode for the current session state.
 * @param sess Current session (read-only).
 * @return FREEDV_MODE_* constant.
 */
int arq_modem_preferred_tx_mode(const arq_session_t *sess);

#endif /* ARQ_MODEM_H_ */
