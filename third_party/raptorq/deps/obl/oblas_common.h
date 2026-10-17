#ifndef OBLAS_COMMON_H
#define OBLAS_COMMON_H

#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define OBLAS_ARCH_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__) || defined(_M_ARM)
#define OBLAS_ARCH_ARM 1
#elif defined(__riscv)
#define OBLAS_ARCH_RISCV 1
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#ifndef NANORQ_NO_LIBC
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#endif

#define __builtin_bswap64 _byteswap_uint64
#define __builtin_bswap32 _byteswap_ulong
#define __attribute__(x)
#define restrict __restrict

static inline int __builtin_popcount(unsigned int x)
{
#if defined(_M_X64) || defined(_M_IX86)
    return (int)__popcnt(x);
#else
    /* Portable SW fallback */
    x = x - ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    return (((x + (x >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
#endif
}

static inline int __builtin_ctz(unsigned int x)
{
    unsigned long index;
    if (_BitScanForward(&index, x)) {
        return (int)index;
    }
    return 32;
}

#if defined(OBLAS_ARCH_X86)
static inline int obl_msvc_xstate_enabled(uint64_t mask)
{
    int cpuInfo[4] = {0};
    __cpuid(cpuInfo, 1);
    if ((cpuInfo[2] & (1 << 27)) == 0) { /* OSXSAVE */
        return 0;
    }
    return (_xgetbv(0) & mask) == mask;
}

static inline int __builtin_cpu_supports(const char *feature)
{
    int cpuInfo[4] = {0};
    __cpuid(cpuInfo, 0);
    int max_leaf = cpuInfo[0];

    if (strcmp(feature, "ssse3") == 0) {
        if (max_leaf >= 1) {
            __cpuid(cpuInfo, 1);
            return (cpuInfo[2] & (1 << 9)) != 0;
        }
    } else if (strcmp(feature, "avx2") == 0) {
        if (max_leaf >= 7 && obl_msvc_xstate_enabled(0x06)) {
            __cpuidex(cpuInfo, 7, 0);
            return (cpuInfo[1] & (1 << 5)) != 0;
        }
    } else if (strcmp(feature, "avx512f") == 0) {
        if (max_leaf >= 7 && obl_msvc_xstate_enabled(0xE6)) {
            __cpuidex(cpuInfo, 7, 0);
            return (cpuInfo[1] & (1 << 16)) != 0;
        }
    } else if (strcmp(feature, "avx512dq") == 0) {
        if (max_leaf >= 7 && obl_msvc_xstate_enabled(0xE6)) {
            __cpuidex(cpuInfo, 7, 0);
            return (cpuInfo[1] & (1 << 17)) != 0;
        }
    } else if (strcmp(feature, "avx512bw") == 0) {
        if (max_leaf >= 7 && obl_msvc_xstate_enabled(0xE6)) {
            __cpuidex(cpuInfo, 7, 0);
            return (cpuInfo[1] & (1 << 30)) != 0;
        }
    } else if (strcmp(feature, "avx512vl") == 0) {
        if (max_leaf >= 7 && obl_msvc_xstate_enabled(0xE6)) {
            __cpuidex(cpuInfo, 7, 0);
            return (cpuInfo[1] & (1U << 31)) != 0;
        }
    } else if (strcmp(feature, "gfni") == 0) {
        if (max_leaf >= 7) {
            __cpuidex(cpuInfo, 7, 0);
            return (cpuInfo[2] & (1 << 8)) != 0;
        }
    }
    return 0;
}
#endif /* OBLAS_ARCH_X86 */
#endif /* defined(_MSC_VER) && !defined(__clang__) */

#ifdef __cplusplus
extern "C" {
#endif

void obl_swap(uint8_t *a, uint8_t *b, unsigned k);
void *obl_alloc(size_t num_rows, size_t row_size, size_t alignment);
void obl_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* OBLAS_COMMON_H */
