/* Low-perturbation event trace for diagnosing connect-path races.
 *
 * The failure this exists for disappears under -v: any per-event logging shifts
 * the timing enough to hide it.  So nothing here does I/O on the hot path.  A
 * record is 16 bytes appended to a preallocated array under one relaxed atomic
 * increment; the whole trace is printed once, later, by arq_trace_dump().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ARQ_TRACE_H_
#define ARQ_TRACE_H_

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    ARQ_TR_RX_FRAME = 1,   /* a frame decoded off the air: a=packet_type b=subtype */
    ARQ_TR_EV_IN,          /* event entering the FSM:      a=event id   b=conn_state */
    ARQ_TR_EV_OUT,         /* FSM returned:                a=conn_state b=dflow_state */
    ARQ_TR_TX_START,       /* we keyed up:                 a=packet_type b=mode&0xff */
    ARQ_TR_TX_END,         /* we unkeyed */
    ARQ_TR_DROP,           /* a frame was discarded:       a=reason */
    ARQ_TR_DECODE,         /* decoder said something:      a=mode b=sync|status<<1 x=bytes */
} arq_trace_kind_t;

void arq_trace_add(uint8_t kind, uint8_t a, uint8_t b, uint16_t extra);
void arq_trace_dump(const char *why);

/* Compiled in only for diagnosis; the release build pays nothing. */
#ifdef ARQ_TRACE_ENABLED
#define ARQ_TRACE_ON 1
#define ARQ_TRACE(kind, a, b, extra) arq_trace_add((kind), (uint8_t)(a), (uint8_t)(b), (uint16_t)(extra))
#define ARQ_TRACE_DUMP(why)          arq_trace_dump((why))
#else
#define ARQ_TRACE_ON 0
#define ARQ_TRACE(kind, a, b, extra) ((void)0)
#define ARQ_TRACE_DUMP(why)          ((void)0)
#endif

#endif /* ARQ_TRACE_H_ */
