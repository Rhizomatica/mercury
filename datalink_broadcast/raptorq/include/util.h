#ifndef NANORQ_UTIL_H
#define NANORQ_UTIL_H

#include "obl/oblas_lite.h"
#include "uvec.h"
#ifndef NANORQ_NO_LIBC
#include <assert.h>
#else
/* embedded: assert is unreachable hint for no libc */
#define assert(x) ((void)(x))
#endif

extern struct oblas_impl nanorq_oblas;

#define OBLAS_DEFAULT_ALIGN 32

static inline size_t get_align_size(void) {
  return nanorq_oblas.align_size > 0 ? nanorq_oblas.align_size
                                     : OBLAS_DEFAULT_ALIGN;
}

#define OCT_EXP GF2_8_EXP
#define OCT_LOG GF2_8_LOG
#define OCT_INV GF2_8_INV

#define TMPSWAP(t, a, b)                                                       \
  do {                                                                         \
    t __tmp = a;                                                               \
    a = b;                                                                     \
    b = __tmp;                                                                 \
  } while (0)

#endif
