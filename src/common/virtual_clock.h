/* HERMES Modem — Virtual clock: time injection for the framed-socket bench
 * transport (-x sock), so ARQ timing can run in lockstep with signal time.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ARQ FSM is sans-io: arq_fsm_timeout_ms(sess, now) takes `now` from the
 * caller and never reads a clock itself. On wall time (the default) time_now_ms()
 * returns hermes_uptime_ms(). In virtual mode (the -x sock lockstep transport),
 * the transport owns time: it reads virtual_now_ms off each sim frame and calls
 * virtual_clock_set(), so time_now_ms() -- and therefore every FSM timer --
 * advances with signal time instead of wall time. The FSM needs no changes.
 *
 * virtual_clock_enable() is one-way for the process lifetime: mixing wall and
 * virtual time in one run would tear the timer base.
 */
#ifndef VIRTUAL_CLOCK_H_
#define VIRTUAL_CLOCK_H_

#include <stdbool.h>
#include <stdint.h>

/* Virtual-time origin for the -x sock transport: the sim sends
 * virtual_now_ms relative to run start; the transport publishes
 * EPOCH + virtual_now_ms.  Any large constant works (timers only ever
 * difference two reads); this one matches the armstrong bench so both
 * modems log the same virtual timestamps under one sim. */
#define VIRTUAL_CLOCK_EPOCH_MS 1700000000000ULL

/* Switch the process to virtual time, starting at epoch_ms. Call before the ARQ
 * threads start so every time_now_ms() read sees one time base. One-way. */
void virtual_clock_enable(uint64_t epoch_ms);

/* Advance the virtual clock (called by the -x sock transport once per block,
 * from the frame's virtual_now_ms). */
void virtual_clock_set(uint64_t now_ms);

/* True once virtual_clock_enable() has been called. */
bool virtual_clock_enabled(void);

/* Process time in ms: virtual-now when enabled, else hermes_uptime_ms(). The
 * single time source the ARQ event loop / FSM read for timeout decisions. */
uint64_t time_now_ms(void);

#endif /* VIRTUAL_CLOCK_H_ */
