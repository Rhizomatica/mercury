#ifndef OBLAS_LITE_H
#define OBLAS_LITE_H

#include <stdint.h>
#include <stddef.h>

#include "gf2_8_tables.h"
#include "oblas_common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct oblas_impl {
    void (*axpy)(uint8_t *a, uint8_t *b, uint8_t u, unsigned k);
    void (*scal)(uint8_t *a, uint8_t u, unsigned k);
    void (*axiy)(uint8_t *a, uint8_t *b, uint8_t u, unsigned k);
    void (*axpyb32)(uint8_t *a, uint32_t *b, uint8_t u, unsigned k);
    size_t align_size;
};

void oblas_get_impl(struct oblas_impl *impl);

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#ifdef __cplusplus
}
#endif

#endif /* OBLAS_LITE_H */
