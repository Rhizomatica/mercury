#include "sopi.h"

uint32_t sopi_mersenne_mod(uint64_t A, uint64_t i, uint64_t B) {
  uint64_t D = i * B;
  uint64_t D0 = D & 0x7FFFFFFF;
  uint64_t D1 = D >> 31;
  uint64_t C = A + D0 + D1;
  if (C >= SOPI_N)
    C -= SOPI_N;
  if (C >= SOPI_N)
    C -= SOPI_N;
  return (uint32_t)C;
}

uint32_t sopi_get_esi_single(const rq_sopi *sopi, uint64_t i) {
  return sopi_mersenne_mod(sopi->a, i, sopi->b);
}

void sopi_get_mapping_multi(const rq_sopi *sopi, uint64_t i, uint32_t Z,
                            uint32_t *out_sbn, uint32_t *out_esi) {
  uint64_t r = i / Z;
  if (out_esi) {
    *out_esi = sopi_mersenne_mod(sopi->a, r, sopi->b);
  }
  if (out_sbn) {
    *out_sbn = (uint32_t)((i + sopi->c + r * (uint64_t)sopi->d) % Z);
  }
}
