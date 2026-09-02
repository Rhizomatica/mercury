#ifndef NANORQ_TUPLE_H
#define NANORQ_TUPLE_H

#include "params.h"

typedef struct {
  u32 d;
  u32 a;
  u32 b;
  u32 d1;
  u32 a1;
  u32 b1;
} tuple;

tuple gen_tuple(u32 X, params *P);

extern const u32 degree_dist[];
extern const u16 degree_dist_size;

#endif
