#ifndef NANORQ_PRECODE_H
#define NANORQ_PRECODE_H

#include "params.h"
#include "rand.h"

#include "arena.h"

typedef struct {
  u32 used;
  u32 max;
  u32_vec rowmap;
  u32_vec type;
} field_map;

struct _pc;

typedef struct {
  void *on_op_arg;
  void (*on_op)(void *arg, u32 i, u32 j, u8 u);
  void *on_choose_arg;
  u32 (*on_choose)(void *arg, struct _pc *, u32 V0, u32 Vrows, u32 Srows,
                   u32 Vcols);
} cbset;

typedef struct _pc {
  u32 rows;
  u32 cols;
  u32 i; /* dim of X submatrix */
  u32 u; /* remaining cols */

  u32_vec nz;  /* non zero count per row */
  u32_vec cnz; /* non zero count per col */
  u32_vec c;   /* column permutation */
  u32_vec d;   /* row permutation */
  u32_vec ci;  /* inverse map of c */
  u32_vec di;  /* inverse map of d */
  u32_vec U;   /* dense gf2 mat */
  u8_vec UL;   /* dense gf256 mat */
  u8_vec HDPC; /* dense hdpc mat */

  field_map F; /* map between gf2/gf256 rows */

  arena prep_mem;
  uint8_t *AT_mem_beg;
  arena work_mem;

  u32_vec *NZT;
  u32_vec *A;
  u32_vec *AT;

  cbset cb;
} pc;

void precode_matrix_gen(params *P, pc *W);
bool precode_matrix_prepare(params *P, pc *W);
int precode_matrix_invert(params *P, pc *W);
void precode_matrix_make_HDPC(params *P, pc *W);
void precode_matrix_on_op(void *arg, u32 i, u32 j, u8 u);
u32 precode_matrix_choose(void *arg, pc *W, u32 V0, u32 Vrows, u32 Srows,
                          u32 Vcols);

#endif
