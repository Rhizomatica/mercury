#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "oblas_common.h"

#ifndef NANORQ_NO_LIBC
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER) || defined(__MINGW32__)
#include <malloc.h>
#endif
#endif

void obl_swap(uint8_t *a, uint8_t *b, unsigned k)
{
    register uint8_t *ap = a, *ae = &a[k], *bp = b;
    for (; ap < ae; ap++, bp++) {
        uint8_t tmp = *ap;
        *ap = *bp;
        *bp = tmp;
    }
}

#ifndef NANORQ_NO_LIBC
void *obl_alloc(size_t num_rows, size_t row_size, size_t alignment)
{
    if (num_rows == 0 || row_size == 0) {
        return NULL;
    }
    if (alignment > 1 && (alignment & (alignment - 1)) != 0) {
        return NULL;
    }
    size_t stride = row_size;
    if (alignment > 1) {
        if (row_size > SIZE_MAX - (alignment - 1)) {
            return NULL;
        }
        stride = (row_size + alignment - 1) & ~(alignment - 1);
    }
    if (num_rows > SIZE_MAX / stride) {
        return NULL;
    }
    size_t total_size = num_rows * stride;

    void *ptr = NULL;
    if (alignment <= 1) {
        ptr = calloc(num_rows, stride);
    } else {
#if defined(_MSC_VER) || defined(__MINGW32__)
        ptr = _aligned_malloc(total_size, alignment);
        if (ptr) {
            memset(ptr, 0, total_size);
        }
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__) || defined(__posix__)
        if (posix_memalign(&ptr, alignment, total_size) == 0) {
            memset(ptr, 0, total_size);
        } else {
            ptr = NULL;
        }
#else
        size_t aligned_size = (total_size + alignment - 1) & ~(alignment - 1);
        ptr = aligned_alloc(alignment, aligned_size);
        if (ptr) {
            memset(ptr, 0, aligned_size);
        }
#endif
    }
    return ptr;
}

void obl_free(void *ptr)
{
    if (!ptr) {
        return;
    }
#if defined(_MSC_VER) || defined(__MINGW32__)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}
#endif
