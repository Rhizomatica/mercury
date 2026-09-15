#include "precode.h"
/* shortcuts are taken here
 *  - component / original degree tracking is skipped for speed
 *  - it might result in decoding failures until more symbols are added
 *  - different choose implementations can be added here
 */
u32 precode_matrix_choose(void *arg, pc *W, u32 V0, u32 Vrows, u32 Srows,
                          u32 Vcols) {
  (void)arg;
  (void)Vcols;
  u32 chosen = Vrows;
  for (u32 b = 1; b < 3; b++) {
    while (uv_size(W->NZT[b]) > 0) {
      chosen = uv_pop(W->NZT[b]);
      if (uv_A(W->di, chosen) >= V0 && uv_A(W->di, chosen) < Srows &&
          uv_A(W->nz, chosen) == b)
        return uv_A(W->di, chosen);
    }
  }
  return Srows;
}
