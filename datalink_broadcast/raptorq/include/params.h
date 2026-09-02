#ifndef NANORQ_PARAMS_H
#define NANORQ_PARAMS_H

#include "rqtables.h"
#include "util.h"

#define GENC_MAX 32

typedef struct {
  u32 K;
  u32 Kprime;
  u32 S;
  u32 H;
  u32 W;
  u32 L;
  u32 P;
  u32 P1;
  u32 U;
  u32 B;
  u32 J;
} params;

params params_init(u32 symbols);
u32 params_set_idxs(params *P, u32 X, u32_vec *v);

#endif
