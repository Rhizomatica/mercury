#include "nanorq_ops.h"

static void ops_apply_schedule(uint8_t *D, uint32_t stride, schedule *S) {
  sched_op *ops = S->ops.a;
  void (*axpy)(uint8_t *, uint8_t *, uint8_t, uint32_t) = nanorq_oblas.axpy;
  void (*scal)(uint8_t *, uint8_t, uint32_t) = nanorq_oblas.scal;

  /* forward ge */
  for (u32 idx = 0; idx < S->cp[1]; idx++) {
    sched_op op = ops[idx];
    if (op.u)
      axpy(D + op.i * stride, D + op.j * stride, op.u, stride);
    else
      scal(D + op.i * stride, op.j, stride);
  }

  /* undo phase 1 row additions */
  if (S->cp[0] > 0) {
    for (u32 idx = S->cp[0] - 1; idx < S->cp[0]; idx--) {
      sched_op op = ops[idx];
      if (op.u)
        axpy(D + op.i * stride, D + op.j * stride, op.u, stride);
      else
        scal(D + op.i * stride, op.j, stride);
    }
  }

  /* backsolve */
  for (u32 idx = S->cp[1]; idx < S->ops.n; idx++) {
    sched_op op = ops[idx];
    if (op.u)
      axpy(D + op.i * stride, D + op.j * stride, op.u, stride);
    else
      scal(D + op.i * stride, op.j, stride);
  }

  /* reapply phase 1 row additions */
  for (u32 idx = 0; idx < S->cp[0]; idx++) {
    sched_op op = ops[idx];
    if (op.u)
      axpy(D + op.i * stride, D + op.j * stride, op.u, stride);
    else
      scal(D + op.i * stride, op.j, stride);
  }
}

static void ops_permute(uint8_t *D, uint32_t stride, u32 P[], u32 n) {
  for (size_t i = 0; i < n; i++) {
    u32 at = i;
    while (!(P[at] & 0x80000000)) {
      u32 next = P[at];
      obl_swap((D + i * stride), (D + next * stride), stride);
      P[at] |= 0x80000000;
      at = next;
    }
  }
  for (size_t i = 0; i < n; i++) {
    P[i] &= 0x7FFFFFFF;
  }
}

void ops_push(void *arg, u32 i, u32 j, u8 u) {
  schedule *S = (schedule *)arg;
  if (!S || !S->ops.a)
    return;
  sched_op op = {u, i, j};
  if (i == 0 && j == 0 && u == 0) {
    if (S->cpidx < 2) {
      S->cp[S->cpidx++] = S->ops.n;
    } else {
      S->overflowed = true;
    }
  } else {
    if (S->ops.n < S->ops.m) {
      S->ops.a[S->ops.n++] = op;
    } else {
      S->overflowed = true;
    }
  }
}

void ops_run(nanorq_core *rq, uint8_t *D, uint32_t stride, schedule *S) {
  if (!rq || !D || stride == 0 || !S || !S->ops.a || S->cpidx != 2 ||
      S->overflowed)
    return;
  ops_apply_schedule(D, stride, S);
  u32 rows = nanorq_core_get_pc_rows(rq);
  u32 cols = nanorq_core_get_pc_cols(rq);
  /* permute using arena-backed pivot vectors, marking visited entries in-place.
   */
  ops_permute(D, stride, rq->W.di.a, rows);
  ops_permute(D, stride, rq->W.c.a, cols);
}

void ops_mix(nanorq_core *rq, uint8_t *D, uint32_t stride, u32 esi, u8 *ptr) {
  if (!rq || !D || stride == 0 || !ptr)
    return;
  uint32_t mix[GENC_MAX];
  uint32_t num = nanorq_core_get_packet_mix(rq, esi, mix, GENC_MAX);

  for (u32 i = 0; i < stride; i++)
    ptr[i] = 0;
  for (u32 it = 0; it < num; it++) {
    u32 row = mix[it];
    nanorq_oblas.axpy(ptr, (D + row * stride), 1, stride);
  }
}

size_t ops_estimate_schedule_bytes(uint32_t K) {
  /* allocate 40 * k + 1000 to conservatively bound schedule length. */
  if ((size_t)K > (SIZE_MAX - 1000) / 40)
    return 0;
  size_t estimated_ops = 40 * (size_t)K + 1000;
  if (estimated_ops > SIZE_MAX / sizeof(sched_op))
    return 0;
  return estimated_ops * sizeof(sched_op);
}

bool schedule_init(schedule *S, void *buf, size_t buf_bytes) {
  if (!S || !buf || buf_bytes < sizeof(sched_op))
    return false;
  *S = (schedule){0};
  S->ops.a = (sched_op *)buf;
  S->ops.m = buf_bytes / sizeof(sched_op);
  return true;
}

#ifndef NANORQ_NO_LIBC
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#endif

void nanorq_core_get_memory_reqs(uint32_t K, uint32_t overhead, uint32_t stride,
                                 struct nanorq_core_mem_reqs *reqs) {
  if (!reqs)
    return;
  *reqs = (struct nanorq_core_mem_reqs){0};
  nanorq_core rq = {0};
  if (!nanorq_core_encoder_new(K, overhead, &rq) || stride == 0)
    return;
  reqs->prepare_bytes = nanorq_core_calculate_prepare_memory(&rq);
  reqs->work_bytes = nanorq_core_calculate_work_memory(&rq);
  size_t rows = nanorq_core_get_pc_rows(&rq);
  if (rows > SIZE_MAX / stride) {
    *reqs = (struct nanorq_core_mem_reqs){0};
    return;
  }
  reqs->matrix_bytes = rows * stride;
  reqs->schedule_bytes = ops_estimate_schedule_bytes(K);
}

#ifndef NANORQ_NO_LIBC

bool nanorq_core_encode_simple(uint8_t *src_data, uint32_t K, uint16_t T,
                               uint32_t num_repair, uint8_t *repair_out) {
  if (!src_data || !repair_out || T == 0)
    return false;
  nanorq_core rq = {0};
  if (!nanorq_core_encoder_new(K, 0, &rq))
    return false;

  struct nanorq_core_mem_reqs reqs;
  nanorq_core_get_memory_reqs(K, 0, T, &reqs);
  if (!reqs.prepare_bytes || !reqs.work_bytes || !reqs.matrix_bytes ||
      !reqs.schedule_bytes)
    return false;

  uint8_t *prep_mem = (uint8_t *)malloc(reqs.prepare_bytes);
  if (!prep_mem)
    return false;
  if (!nanorq_core_prepare(&rq, prep_mem, reqs.prepare_bytes)) {
    free(prep_mem);
    return false;
  }

  uint8_t *D = (uint8_t *)calloc(1, reqs.matrix_bytes);
  if (!D) {
    free(prep_mem);
    return false;
  }
  uint32_t SH = nanorq_core_get_pc_genc_offset(&rq);

  for (uint32_t i = 0; i < K; i++) {
    memcpy(D + (SH + i) * T, src_data + i * T, T);
  }

  uint8_t *work_mem = (uint8_t *)malloc(reqs.work_bytes);
  schedule S_enc = {0};
  void *sched_mem = malloc(reqs.schedule_bytes);
  if (!work_mem || !schedule_init(&S_enc, sched_mem, reqs.schedule_bytes)) {
    free(prep_mem);
    free(D);
    free(work_mem);
    free(sched_mem);
    return false;
  }

  nanorq_core_set_op_callback(&rq, &S_enc, ops_push);

  if (!nanorq_core_precalculate(&rq, work_mem, reqs.work_bytes) ||
      S_enc.overflowed || S_enc.cpidx != 2) {
    free(prep_mem);
    free(D);
    free(work_mem);
    free(S_enc.ops.a);
    return false;
  }

  ops_run(&rq, D, T, &S_enc);

  for (uint32_t i = 0; i < num_repair; i++) {
    ops_mix(&rq, D, T, K + i, repair_out + i * T);
  }

  free(prep_mem);
  free(D);
  free(work_mem);
  free(S_enc.ops.a);
  return true;
}

#endif /* NANORQ_NO_LIBC */
