#ifndef ARENA_H
#define ARENA_H

#ifdef __cplusplus
#define _Alignof alignof
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
  uint8_t *beg;
  uint8_t *end;
} arena;

static inline void *alloc(arena *a, ptrdiff_t size, ptrdiff_t align,
                          ptrdiff_t count) {
  if (!a || !a->beg || !a->end || size <= 0 || count < 0)
    return 0;
  if (align < 1)
    align = 1;
  ptrdiff_t padding = -(uintptr_t)a->beg & (align - 1);
  ptrdiff_t available = a->end - a->beg - padding;
  if (available < 0 || count > available / size)
    return 0;
  void *p = a->beg + padding;
  a->beg += padding + count * size;
  return memset(p, 0, count * size);
}

#define alloc_array(a, type, count)                                            \
  (type *)alloc((a), sizeof(type), _Alignof(type), (count))

#endif
