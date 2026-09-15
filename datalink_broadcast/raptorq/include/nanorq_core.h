#include <stdbool.h>
#ifndef NANORQ_CORE_H
#define NANORQ_CORE_H

#include "params.h"
#include "precode.h"
#include <stddef.h>

typedef struct {
  pc W;
  params P;
  u32 overhead;
} nanorq_core;

/* Thread-safe one-time SIMD implementation selection. */
void nanorq_core_init(void);

/* returns a new encoder configured with given parameters */
bool nanorq_core_encoder_new(uint32_t K, uint32_t overhead, nanorq_core *rq);

/* return the amount of memory required for preconditioning */
size_t nanorq_core_calculate_prepare_memory(nanorq_core *rq);

/* prepare rq matrix for inversion */
bool nanorq_core_prepare(nanorq_core *rq, uint8_t *prep_mem, size_t pm_len);

/* fetch the codebook for the given packet */
uint32_t nanorq_core_get_packet_mix(nanorq_core *rq, uint32_t esi,
                                    uint32_t *mix_idxs, uint32_t mix_max);

/* replace symbols in rq matrix for given row */
void nanorq_core_replace_symbol(nanorq_core *rq, u32 row, u32 esi);

/* patch the matrix after symbol replacements */
bool nanorq_core_patch_matrix(nanorq_core *rq);

/* return the amount of memory required for inversion */
size_t nanorq_core_calculate_work_memory(nanorq_core *rq);

/* precalculate rq matrix inversion */
bool nanorq_core_precalculate(nanorq_core *rq, uint8_t *work_mem,
                              size_t wm_len);

/* recommended row stride for t, accounting for simd and cache-line aliasing. */
uint32_t nanorq_core_recommended_stride(uint32_t T);

/* memory requirements for k, overhead, and stride. */
struct nanorq_core_mem_reqs {
  size_t prepare_bytes;
  size_t work_bytes;
  size_t matrix_bytes; /* rows * stride */
  size_t schedule_bytes;
};
void nanorq_core_get_memory_reqs(uint32_t K, uint32_t overhead, uint32_t stride,
                                 struct nanorq_core_mem_reqs *reqs);

/* d matrix must be zero-initialised. use nanorq_core_init_matrix for
 * arenas/static buffers. */

/* zero-initialise d matrix. no libc dependency. */
void nanorq_core_init_matrix(nanorq_core *rq, uint8_t *D, uint32_t stride);

/* copy t bytes from src into d for source esi. */
void nanorq_core_place_symbol(nanorq_core *rq, uint8_t *D, uint32_t stride,
                              uint32_t esi, const uint8_t *src, uint32_t T);

/* set callback for choosing a row during preconditioning */
void nanorq_core_set_choose_callback(nanorq_core *rq, void *arg,
                                     u32 (*on_choose)(void *, pc *, u32, u32,
                                                      u32, u32));

/* set callback for when data matrix operations are computed */
void nanorq_core_set_op_callback(nanorq_core *rq, void *arg,
                                 void (*on_op)(void *, u32, u32, u8));

/* get the offset to the GENC rows in precode matrix */
static uint32_t nanorq_core_get_pc_genc_offset(nanorq_core *rq);

/* get the number of rows for the precode matrix */
static uint32_t nanorq_core_get_pc_rows(nanorq_core *rq);

/* get the number of columns for the precode matrix */
static uint32_t nanorq_core_get_pc_cols(nanorq_core *rq);

/* clone the precode matrix row pivot vector to supplied array */
static void nanorq_core_clone_pc_rows_pv(nanorq_core *rq, uint32_t *pv);

/* clone the precode matrix column pivot vector to supplied array */
static void nanorq_core_clone_pc_cols_pv(nanorq_core *rq, uint32_t *pv);

static inline uint32_t nanorq_core_get_pc_genc_offset(nanorq_core *rq) {
  return rq->P.S + rq->P.H;
}

inline uint32_t nanorq_core_get_pc_rows(nanorq_core *rq) {
  return rq->P.L + rq->overhead;
}
inline uint32_t nanorq_core_get_pc_cols(nanorq_core *rq) { return rq->P.L; }
inline void nanorq_core_clone_pc_rows_pv(nanorq_core *rq, uint32_t *pv) {
  for (u32 i = 0; i < rq->W.rows; i++)
    pv[i] = uv_A(rq->W.di, i);
}
inline void nanorq_core_clone_pc_cols_pv(nanorq_core *rq, uint32_t *pv) {
  for (u32 i = 0; i < rq->W.cols; i++)
    pv[i] = uv_A(rq->W.c, i);
}

/* returns pointer to slot in d matrix for symbol row. */
static inline uint8_t *nanorq_core_get_symbol_ptr(nanorq_core *rq, uint8_t *D,
                                                  uint32_t stride,
                                                  uint32_t row) {
  uint32_t SH = nanorq_core_get_pc_genc_offset(rq);
  return D + (SH + row) * stride;
}

#endif
