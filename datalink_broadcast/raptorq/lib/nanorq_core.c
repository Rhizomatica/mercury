#include "nanorq_core.h"
#include "precode.h"
#include "tuple.h"
#include "util.h"

#include <stdatomic.h>

#define ALIGN_VAL(x, align) (((x) + (align) - 1) & ~((align) - 1))

struct oblas_impl nanorq_oblas = {.align_size = 32};
static atomic_uint nanorq_oblas_state = ATOMIC_VAR_INIT(0);

static bool add_bytes(size_t *total, size_t count, size_t unit) {
  if (count != 0 && unit > SIZE_MAX / count)
    return false;
  size_t bytes = count * unit;
  if (*total > SIZE_MAX - bytes)
    return false;
  *total += bytes;
  return true;
}

static bool add_padded_bytes(size_t *total, size_t count, size_t unit,
                             size_t repeats) {
  size_t align = get_align_size();
  if (align == 0 || (align & (align - 1)) != 0 ||
      (count != 0 && unit > SIZE_MAX / count))
    return false;
  size_t bytes = count * unit;
  if (bytes > SIZE_MAX - (align - 1))
    return false;
  size_t padded = ALIGN_VAL(bytes, align);
  return add_bytes(total, repeats, padded);
}

void nanorq_core_init(void) {
  unsigned expected = 0;
  if (atomic_compare_exchange_strong_explicit(&nanorq_oblas_state, &expected, 1,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
    struct oblas_impl impl = {.align_size = 32};
    oblas_get_impl(&impl);
    nanorq_oblas = impl;
    atomic_store_explicit(&nanorq_oblas_state, 2, memory_order_release);
    return;
  }
  while (atomic_load_explicit(&nanorq_oblas_state, memory_order_acquire) != 2) {
  }
}

/*
 * T: size of each symbol in bytes (should be aligned to Al)
 * K: number of symbols per block
 */
bool nanorq_core_encoder_new(u32 K, u32 overhead, nanorq_core *rq) {
  if (!rq)
    return false;
  if (K < 1 || K > K_max)
    return false;
  rq->P = params_init(K);
  if (overhead > UINT32_MAX - rq->P.L)
    return false;
  uint64_t rows = (uint64_t)rq->P.L + overhead;
  uint64_t u_stride = DC(rq->P.L, 32);
  if (rows * u_stride > UINT32_MAX)
    return false;
  rq->overhead = overhead;
  nanorq_core_init();
  return true;
}

uint32_t nanorq_core_recommended_stride(uint32_t T) {
  nanorq_core_init();
  uint32_t align = (uint32_t)get_align_size();
  if (align == 0 || T > UINT32_MAX - (align - 1))
    return 0;
  uint32_t padded = ALIGN_VAL(T, align);
  /* avoid cache-line aliasing: if padded is an exact multiple of 64, add one
   * alignment unit */
  if (padded % 64 == 0) {
    if (padded > UINT32_MAX - align)
      return 0;
    padded += align;
  }
  return padded;
}
void nanorq_core_place_symbol(nanorq_core *rq, uint8_t *D, uint32_t stride,
                              uint32_t esi, const uint8_t *src, uint32_t T) {
  if (!rq || !D || !src || T > stride)
    return;
  uint32_t SH = nanorq_core_get_pc_genc_offset(rq);
  if (esi > UINT32_MAX - SH || SH + esi >= nanorq_core_get_pc_rows(rq))
    return;
  uint8_t *dst = D + (SH + esi) * stride;
  memcpy(dst, src, T);
}
size_t nanorq_core_calculate_prepare_memory(nanorq_core *rq) {
  if (!rq)
    return 0;
  params *P = &rq->P;
  size_t mem = 0;
  u32 snz = 3 * DC(P->B, P->S) + 3;

  /* c/ci, d/di & nz/cnz */
  if (!add_padded_bytes(&mem, P->L, sizeof(u32), 3) ||
      !add_padded_bytes(&mem, (size_t)P->L + rq->overhead, sizeof(u32), 3))
    return 0;
  /* NZT (one empty vec) */
  if (!add_padded_bytes(&mem, 1, sizeof(u32_vec), 3) ||
      !add_padded_bytes(&mem, (size_t)P->L + rq->overhead, sizeof(u32), 2))
    return 0;
  /* A */
  if (!add_bytes(&mem, (size_t)P->L + rq->overhead, sizeof(u32_vec)))
    return 0;
  size_t memb4a = mem;
  if (!add_padded_bytes(&mem, snz, sizeof(u32), P->S) ||
      !add_padded_bytes(&mem, GENC_MAX, sizeof(u32),
                        (size_t)P->Kprime + rq->overhead))
    return 0;
  /* AT */
  if (!add_bytes(&mem, P->L, sizeof(u32_vec)))
    return 0;
  size_t mirrored = mem - memb4a;
  if (!add_bytes(&mem, 1, mirrored) ||
      !add_padded_bytes(&mem, 1, sizeof(u32), P->L))
    return 0;

  return (size_t)mem;
}

static bool assign_prepare_memory(nanorq_core *rq, u8 *mem, size_t len) {
  if (!rq || !mem || len == 0)
    return false;
  params *P = &rq->P;
  pc *W = &rq->W;
  u32 snz = 3 * DC(P->B, P->S) + 3;

  W->rows = P->L + rq->overhead;
  W->cols = P->L;

  arena *a = &W->prep_mem;
  a->beg = mem;
  a->end = mem + len;
  if (!u32_vec_init(&W->c, a, W->cols, W->cols, 0))
    return false;
  if (!u32_vec_init(&W->ci, a, W->cols, W->cols, 0))
    return false;
  if (!u32_vec_init(&W->d, a, W->rows, W->rows, 0))
    return false;
  if (!u32_vec_init(&W->di, a, W->rows, W->rows, 0))
    return false;
  if (!u32_vec_init(&W->cnz, a, W->cols, W->cols, 0))
    return false;
  if (!u32_vec_init(&W->nz, a, W->rows, W->rows, 0))
    return false;

  W->NZT = alloc_array(a, u32_vec, 3);
  if (!W->NZT)
    return false;
  for (u32 i = 1; i < 3; i++)
    if (!u32_vec_init(&W->NZT[i], a, 0, W->rows, 0))
      return false;

  W->A = alloc_array(a, u32_vec, W->rows);
  if (!W->A)
    return false;
  for (u32 i = 0; i < P->S; i++)
    if (!u32_vec_init(&W->A[i], a, 0, snz, 0))
      return false;
  for (u32 i = P->S + P->H, esi = 0; i < W->rows; i++, esi++)
    if (!u32_vec_init(&W->A[i], a, 0, GENC_MAX, 0))
      return false;

  W->AT = alloc_array(a, u32_vec, W->cols);
  if (!W->AT)
    return false;

  W->cb.on_choose_arg = 0x0;
  W->cb.on_choose = precode_matrix_choose;
  W->cb.on_op_arg = 0x0;
  W->cb.on_op = precode_matrix_on_op;

  W->AT_mem_beg = NULL;
  return true;
}

bool nanorq_core_prepare(nanorq_core *rq, uint8_t *prep_mem, size_t pm_len) {
  if (!rq || !prep_mem || pm_len == 0)
    return false;
  if (!assign_prepare_memory(rq, prep_mem, pm_len))
    return false;
  precode_matrix_gen(&rq->P, &rq->W);
  return precode_matrix_prepare(&rq->P, &rq->W);
}

uint32_t nanorq_core_get_packet_mix(nanorq_core *rq, u32 esi,
                                    uint32_t *mix_idxs, uint32_t mix_max) {
  if (!rq || !mix_idxs || mix_max < GENC_MAX)
    return 0;
  params *P = &rq->P;
  u32 X = esi;
  if (esi >= P->K) {
    if (esi > UINT32_MAX - (P->Kprime - P->K))
      return 0;
    X += (P->Kprime - P->K);
  }
  u32_vec mix = {.m = mix_max, .n = 0, .s = 0, .a = mix_idxs};
  params_set_idxs(P, X, &mix);
  return mix.n;
}

void nanorq_core_replace_symbol(nanorq_core *rq, u32 row, u32 esi) {
  if (!rq)
    return;
  params *P = &rq->P;
  pc *W = &rq->W;
  if (!W->A)
    return;
  row += P->H + P->S;
  if (row >= W->rows)
    return;
  u32 X = esi;
  if (esi >= P->K) {
    if (esi > UINT32_MAX - (P->Kprime - P->K))
      return;
    X += (P->Kprime - P->K);
  }
  uv_clear(W->A[row]);
  params_set_idxs(P, X, &W->A[row]);
}

bool nanorq_core_patch_matrix(nanorq_core *rq) {
  if (!rq)
    return false;
  pc *W = &rq->W;
  if (!W->cnz.a || !W->nz.a || !W->NZT)
    return false;

  for (u32 i = 0; i < W->cols; i++) {
    uv_A(W->cnz, i) = 0;
  }
  for (u32 i = 0; i < W->rows; i++) {
    uv_A(W->nz, i) = 0;
  }
  uv_clear(W->NZT[1]);
  uv_clear(W->NZT[2]);

  return precode_matrix_prepare(&rq->P, W);
}

size_t nanorq_core_calculate_work_memory(nanorq_core *rq) {
  if (!rq)
    return 0;
  params *P = &rq->P;
  size_t mem = 0;
  u32 max_u = P->L;
  u32 rows = P->L + rq->overhead;
  /* U */
  u32 u_stride = DC(max_u, 32);
  if (!add_padded_bytes(&mem, (size_t)rows * u_stride, sizeof(u32), 1))
    return 0;
  /* field maps */
  if (!add_padded_bytes(&mem, rows, sizeof(u32), 2))
    return 0;
  /* UL */
  if (!add_padded_bytes(&mem, 2U * P->H, (size_t)u_stride * 32, 1))
    return 0;
  /* HDPC */
  if (!add_padded_bytes(&mem, P->H, (P->Kprime + P->S + 31U) & ~31U, 1))
    return 0;
  /* add w->a to bump allocator */
  if (!add_padded_bytes(&mem, P->Kprime + P->S, 1, 1))
    return 0;
  return (size_t)mem;
}

static bool assign_work_memory(nanorq_core *rq, u8 *mem, size_t len) {
  if (!rq || !mem || len == 0)
    return false;
  params *P = &rq->P;
  pc *W = &rq->W;
  u32 tmp = 0;

  arena *a = &W->work_mem;
  a->beg = mem;
  a->end = mem + len;

  u32 u_stride = DC(W->u, 32);
  tmp = W->rows * u_stride;
  if (!u32_vec_init(&W->U, a, tmp, tmp, u_stride))
    return false;
  if (!u32_vec_init(&W->F.rowmap, a, W->rows, W->rows, 0))
    return false;
  if (!u32_vec_init(&W->F.type, a, W->rows, W->rows, 0))
    return false;

  u32 u_aligned = u_stride * 32;
  tmp = 2 * P->H * u_aligned;
  if (!u8_vec_init(&W->UL, a, tmp, tmp, u_aligned))
    return false;

  u32 k_aligned = (P->Kprime + P->S + 31) & ~31;
  tmp = P->H * k_aligned;
  if (!u8_vec_init(&W->HDPC, a, tmp, tmp, k_aligned))
    return false;
  return true;
}

bool nanorq_core_precalculate(nanorq_core *rq, u8 *work_mem, size_t wm_len) {
  if (!rq || !work_mem || wm_len == 0)
    return false;
  if (!assign_work_memory(rq, work_mem, wm_len))
    return false;
  return precode_matrix_invert(&rq->P, &rq->W);
}

void nanorq_core_set_op_callback(nanorq_core *rq, void *arg,
                                 void (*on_op)(void *, u32, u32, u8)) {
  if (!rq)
    return;
  pc *W = &rq->W;
  W->cb.on_op_arg = arg;
  W->cb.on_op = on_op;
}

void nanorq_core_set_choose_callback(nanorq_core *rq, void *arg,
                                     u32 (*on_choose)(void *, pc *, u32, u32,
                                                      u32, u32)) {
  if (!rq)
    return;
  pc *W = &rq->W;
  W->cb.on_choose_arg = arg;
  W->cb.on_choose = on_choose;
}

void nanorq_core_init_matrix(nanorq_core *rq, uint8_t *D, uint32_t stride) {
  if (!rq || !D || stride == 0)
    return;
  uint32_t rows = nanorq_core_get_pc_rows(rq);
  memset(D, 0, (size_t)rows * stride);
}
