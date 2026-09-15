#ifndef NANORQ_SOPI_H
#define NANORQ_SOPI_H

#include <stdbool.h>
#include <stdint.h>

#define SOPI_N 2147483647ULL // 2^31 - 1 (Mersenne prime)

typedef struct {
  uint32_t a; // ESI start value
  uint32_t b; // ESI step value (must be > 0)
  uint32_t c; // SBN start value
  uint32_t d; // SBN step value (must be > 0)
} rq_sopi;

/* Computes (A + i * B) mod (2^31 - 1) using fast Mersenne prime modulo
 * arithmetic */
uint32_t sopi_mersenne_mod(uint64_t A, uint64_t i, uint64_t B);

/* Maps sequence position `i` to ESI for single-block objects (Z=1) */
uint32_t sopi_get_esi_single(const rq_sopi *sopi, uint64_t i);

/* Maps sequence position `i` to ESI and SBN for multi-block objects (Z > 1) */
void sopi_get_mapping_multi(const rq_sopi *sopi, uint64_t i, uint32_t Z,
                            uint32_t *out_sbn, uint32_t *out_esi);

#endif /* NANORQ_SOPI_H */
