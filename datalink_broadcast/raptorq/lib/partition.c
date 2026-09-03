#include "partition.h"

static inline size_t div_ceil(size_t a, size_t b) { return (a + b - 1) / b; }

static inline size_t div_floor(size_t a, size_t b) { return a / b; }

struct partition partition_fill(size_t I, uint16_t J) {
  struct partition p = {0, 0, 0, 0};
  if (J == 0)
    return p;
  p.IL = (size_t)(div_ceil(I, J));
  p.IS = (size_t)(div_floor(I, J));
  p.JL = (size_t)(I - p.IS * J);
  p.JS = J - p.JL;

  if (p.JL == 0)
    p.IL = 0;
  return p;
}
