#include "nanorq_core.h"

#ifndef NANORQ_OPS_H
#define NANORQ_OPS_H

typedef struct {
  u8 u;
  u32 i;
  u32 j;
} sched_op;

typedef struct {
  sched_op *a;
  size_t n;
  size_t m;
} oplist;

typedef struct {
  u32 cp[2];
  u32 cpidx;
  bool overflowed;
  oplist ops;
} schedule;

/* initialise schedule with caller memory. buf needs
 * ops_estimate_schedule_bytes(k). */
bool schedule_init(schedule *S, void *buf, size_t buf_bytes);

void ops_push(void *arg, u32 i, u32 j, u8 u);
void ops_run(nanorq_core *rq, uint8_t *D, uint32_t stride, schedule *S);
void ops_mix(nanorq_core *rq, uint8_t *D, uint32_t stride, u32 esi, u8 *ptr);
size_t ops_estimate_schedule_bytes(uint32_t K);

/* mem reqs are defined in nanorq_core.h */

bool nanorq_core_encode_simple(uint8_t *src_data, uint32_t K, uint16_t T,
                               uint32_t num_repair, uint8_t *repair_out);

#endif
