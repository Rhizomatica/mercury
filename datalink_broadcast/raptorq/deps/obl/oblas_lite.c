#ifdef __cplusplus
#define register
#endif

#include "oblas_lite.h"
#ifndef NANORQ_NO_LIBC
#include <stdlib.h>
#endif
#include <string.h>

#if defined(OBLAS_ARCH_X86)
#include <immintrin.h>
#include <tmmintrin.h>
#include "gf2_8_affine_mat.h"
#endif

static uint8_t GF2_8_MUL[65536];
static int gf2_8_mul_initialized = 0;

static void oblas_lite_init(void)
{
    if (gf2_8_mul_initialized)
        return;
    for (int i = 0; i < 256; i++) {
        for (int j = 0; j < 256; j++) {
            if (i == 0 || j == 0) {
                GF2_8_MUL[(i << 8) + j] = 0;
            } else {
                GF2_8_MUL[(i << 8) + j] = GF2_8_EXP[GF2_8_LOG[i] + GF2_8_LOG[j]];
            }
        }
    }
    gf2_8_mul_initialized = 1;
}

#if defined(OBLAS_TINY)
static inline uint8_t gf2_8_mul(uint16_t a, uint16_t b)
{
    if (!a || !b) {
        return 0;
    }

    // Perform 8-bit, carry-less multiplication of |a| and |b|.
    return GF2_8_EXP[GF2_8_LOG[a] + GF2_8_LOG[b]];
}

static void obl_axpy_ref(u8 *a, u8 *b, u8 u, unsigned k)
{
    register u8 *ap = a, *ae = &a[k], *bp = b;
    for (; ap != ae; ap++, bp++)
        *ap ^= gf2_8_mul(u, *bp);
}

static void obl_axiy_ref(u8 *a, u8 *b, u8 u, unsigned k)
{
    register u8 *ap = a, *ae = &a[k], *bp = b;
    for (; ap != ae; ap++, bp++)
        *ap = gf2_8_mul(u, *bp);
}

#else
static void obl_axpy_ref(u8 *restrict a, u8 *restrict b, u8 u, unsigned k)
{
    register const u8 *u_row = &GF2_8_MUL[u << 8];
    register u8 *restrict ap = a;
    register u8 *ae = &a[k];
    register const u8 *restrict bp = b;
    for (; ap < ae; ap++, bp++)
        *ap ^= u_row[*bp];
}

static void obl_axiy_ref(u8 *a, u8 *b, u8 u, unsigned k)
{
    register const u8 *u_row = &GF2_8_MUL[u << 8];
    register u8 *ap = a, *ae = &a[k], *bp = b;
    for (; ap != ae; ap++, bp++)
        *ap = u_row[*bp];
}
#endif

static void obl_axpyb32_ref(u8 *a, u32 *b, u8 u, unsigned k)
{
    unsigned idx = 0, p = 0;
    unsigned k_fast = k & ~31;
    for (; idx < k_fast; idx += 32, p++) {
        u32 tmp = b[p];
        while (tmp > 0) {
            unsigned tz = __builtin_ctz(tmp);
            tmp = tmp & (tmp - 1);
            a[tz + idx] ^= u;
        }
    }
    if (idx < k) {
        u32 tmp = b[p];
        tmp &= (1U << (k - idx)) - 1;
        while (tmp > 0) {
            unsigned tz = __builtin_ctz(tmp);
            tmp = tmp & (tmp - 1);
            a[tz + idx] ^= u;
        }
    }
}

#define OBL_NOOP(a, b) (b)

#define OBL_SHUF_TEMPLATE(op, a, b, f, VEC_TYPE, VEC_LOAD, VEC_STORE, VEC_INIT, VEC_CORE, VEC_XOR)                                 \
    do {                                                                                                                           \
        VEC_INIT();                                                                                                                \
        VEC_TYPE *ap = (VEC_TYPE *)a;                                                                                              \
        VEC_TYPE *ae = (VEC_TYPE *)(a + k - (k % (4 * sizeof(VEC_TYPE))));                                                         \
        const VEC_TYPE *bp = (const VEC_TYPE *)b;                                                                                  \
        for (; ap < ae; ap += 4, bp += 4) {                                                                                        \
            VEC_TYPE bx0 = VEC_LOAD(bp + 0);                                                                                       \
            VEC_TYPE bx1 = VEC_LOAD(bp + 1);                                                                                       \
            VEC_TYPE bx2 = VEC_LOAD(bp + 2);                                                                                       \
            VEC_TYPE bx3 = VEC_LOAD(bp + 3);                                                                                       \
            VEC_CORE(bx0, prod0);                                                                                                  \
            VEC_CORE(bx1, prod1);                                                                                                  \
            VEC_CORE(bx2, prod2);                                                                                                  \
            VEC_CORE(bx3, prod3);                                                                                                  \
            VEC_STORE(ap + 0, f(VEC_LOAD(ap + 0), prod0));                                                                         \
            VEC_STORE(ap + 1, f(VEC_LOAD(ap + 1), prod1));                                                                         \
            VEC_STORE(ap + 2, f(VEC_LOAD(ap + 2), prod2));                                                                         \
            VEC_STORE(ap + 3, f(VEC_LOAD(ap + 3), prod3));                                                                         \
        }                                                                                                                          \
        VEC_TYPE *ae2 = (VEC_TYPE *)(a + k - (k % sizeof(VEC_TYPE)));                                                              \
        for (; ap < ae2; ap++, bp++) {                                                                                             \
            VEC_TYPE bx = VEC_LOAD(bp);                                                                                            \
            VEC_CORE(bx, prod);                                                                                                    \
            VEC_STORE(ap, f(VEC_LOAD(ap), prod));                                                                                  \
        }                                                                                                                          \
        op##_ref((u8 *)ap, (u8 *)bp, u, k % sizeof(VEC_TYPE));                                                                     \
    } while (0)

#define GENERATE_IMPL(suffix, attr, VEC_TYPE, VEC_LOAD, VEC_STORE, VEC_INIT, VEC_CORE, VEC_XOR)                                    \
    attr static void obl_axpy_##suffix(u8 *restrict a, u8 *restrict b, u8 u, unsigned k)                                           \
    {                                                                                                                              \
        if (u == 1) {                                                                                                              \
            u8 *restrict ap = a;                                                                                                   \
            u8 *ae = a + k;                                                                                                        \
            const u8 *restrict bp = b;                                                                                             \
            for (; ap < ae; ap++, bp++)                                                                                            \
                *ap ^= *bp;                                                                                                        \
        } else {                                                                                                                   \
            OBL_SHUF_TEMPLATE(obl_axpy, a, b, VEC_XOR, VEC_TYPE, VEC_LOAD, VEC_STORE, VEC_INIT, VEC_CORE, VEC_XOR);                \
        }                                                                                                                          \
    }                                                                                                                              \
    attr static void obl_axiy_##suffix(u8 *a, u8 *b, u8 u, unsigned k)                                                             \
    {                                                                                                                              \
        OBL_SHUF_TEMPLATE(obl_axiy, a, b, OBL_NOOP, VEC_TYPE, VEC_LOAD, VEC_STORE, VEC_INIT, VEC_CORE, VEC_XOR);                   \
    }                                                                                                                              \
    attr static void obl_scal_##suffix(u8 *a, u8 u, unsigned k)                                                                    \
    {                                                                                                                              \
        obl_axiy_##suffix(a, a, u, k);                                                                                             \
    }

#if defined(OBLAS_ARCH_X86)

/* avx512 gfni */
#define VEC_INIT_avx512_gfni() const __m512i u_mat = _mm512_set1_epi64(GF2_8_AFFINE_MAT[u])
#define VEC_CORE_avx512_gfni(bx, res) __m512i res = _mm512_gf2p8affine_epi64_epi8(bx, u_mat, 0)
GENERATE_IMPL(avx512_gfni, __attribute__((target("avx512f,avx512bw,avx512dq,avx512vl,gfni"))), __m512i, _mm512_loadu_si512,
              _mm512_storeu_si512, VEC_INIT_avx512_gfni, VEC_CORE_avx512_gfni, _mm512_xor_si512)

/* avx-512 */
#define VEC_INIT_avx512_std()                                                                                                      \
    const u8 *u_lo = GF2_8_SHUF_LO + u * 16;                                                                                       \
    const u8 *u_hi = GF2_8_SHUF_HI + u * 16;                                                                                       \
    const __m512i mask = _mm512_set1_epi8(0x0f);                                                                                   \
    const __m512i urow_lo = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i *)u_lo));                                        \
    const __m512i urow_hi = _mm512_broadcast_i32x4(_mm_loadu_si128((const __m128i *)u_hi))
#define VEC_CORE_avx512_std(bx, res)                                                                                               \
    __m512i lo_##res = _mm512_and_si512(bx, mask);                                                                                 \
    __m512i hi_##res = _mm512_and_si512(_mm512_srli_epi64(bx, 4), mask);                                                           \
    lo_##res = _mm512_shuffle_epi8(urow_lo, lo_##res);                                                                             \
    hi_##res = _mm512_shuffle_epi8(urow_hi, hi_##res);                                                                             \
    __m512i res = _mm512_xor_si512(lo_##res, hi_##res)
GENERATE_IMPL(avx512_std, __attribute__((target("avx512f,avx512bw,avx512dq,avx512vl"))), __m512i, _mm512_loadu_si512,
              _mm512_storeu_si512, VEC_INIT_avx512_std, VEC_CORE_avx512_std, _mm512_xor_si512)

/* avx2 gnfi */
#define VEC_INIT_avx2_gfni() const __m256i u_mat = _mm256_set1_epi64x(GF2_8_AFFINE_MAT[u])
#define VEC_CORE_avx2_gfni(bx, res) __m256i res = _mm256_gf2p8affine_epi64_epi8(bx, u_mat, 0)
GENERATE_IMPL(avx2_gfni, __attribute__((target("avx2,gfni"))), __m256i, _mm256_loadu_si256, _mm256_storeu_si256, VEC_INIT_avx2_gfni,
              VEC_CORE_avx2_gfni, _mm256_xor_si256)

/* avx2 */
#define VEC_INIT_avx2_std()                                                                                                        \
    const u8 *u_lo = GF2_8_SHUF_LO + u * 16;                                                                                       \
    const u8 *u_hi = GF2_8_SHUF_HI + u * 16;                                                                                       \
    const __m256i mask = _mm256_set1_epi8(0x0f);                                                                                   \
    const __m256i urow_lo = _mm256_loadu2_m128i((const __m128i *)u_lo, (const __m128i *)u_lo);                                     \
    const __m256i urow_hi = _mm256_loadu2_m128i((const __m128i *)u_hi, (const __m128i *)u_hi)
#define VEC_CORE_avx2_std(bx, res)                                                                                                 \
    __m256i lo_##res = _mm256_and_si256(bx, mask);                                                                                 \
    __m256i hi_##res = _mm256_and_si256(_mm256_srli_epi64(bx, 4), mask);                                                           \
    lo_##res = _mm256_shuffle_epi8(urow_lo, lo_##res);                                                                             \
    hi_##res = _mm256_shuffle_epi8(urow_hi, hi_##res);                                                                             \
    __m256i res = _mm256_xor_si256(lo_##res, hi_##res)
GENERATE_IMPL(avx2_std, __attribute__((target("avx2"))), __m256i, _mm256_loadu_si256, _mm256_storeu_si256, VEC_INIT_avx2_std,
              VEC_CORE_avx2_std, _mm256_xor_si256)

/* sse3 gfni */
#define VEC_INIT_ssse3_gfni() const __m128i u_mat = _mm_set1_epi64x(GF2_8_AFFINE_MAT[u])
#define VEC_CORE_ssse3_gfni(bx, res) __m128i res = _mm_gf2p8affine_epi64_epi8(bx, u_mat, 0)
GENERATE_IMPL(ssse3_gfni, __attribute__((target("ssse3,gfni"))), __m128i, _mm_loadu_si128, _mm_storeu_si128, VEC_INIT_ssse3_gfni,
              VEC_CORE_ssse3_gfni, _mm_xor_si128)

/* sse3 */
#define VEC_INIT_ssse3_std()                                                                                                       \
    const u8 *u_lo = GF2_8_SHUF_LO + u * 16;                                                                                       \
    const u8 *u_hi = GF2_8_SHUF_HI + u * 16;                                                                                       \
    const __m128i mask = _mm_set1_epi8(0x0f);                                                                                      \
    const __m128i urow_lo = _mm_loadu_si128((const __m128i *)u_lo);                                                                \
    const __m128i urow_hi = _mm_loadu_si128((const __m128i *)u_hi)
#define VEC_CORE_ssse3_std(bx, res)                                                                                                \
    __m128i lo_##res = _mm_and_si128(bx, mask);                                                                                    \
    __m128i hi_##res = _mm_and_si128(_mm_srli_epi64(bx, 4), mask);                                                                 \
    lo_##res = _mm_shuffle_epi8(urow_lo, lo_##res);                                                                                \
    hi_##res = _mm_shuffle_epi8(urow_hi, hi_##res);                                                                                \
    __m128i res = _mm_xor_si128(lo_##res, hi_##res)
GENERATE_IMPL(ssse3_std, __attribute__((target("ssse3"))), __m128i, _mm_loadu_si128, _mm_storeu_si128, VEC_INIT_ssse3_std,
              VEC_CORE_ssse3_std, _mm_xor_si128)

__attribute__((target("avx512f,avx512bw,avx512dq,avx512vl"))) static void obl_axpyb32_avx512(u8 *a, u32 *b, u8 u, unsigned k)
{
    __m512i *ap = (__m512i *)a;
    __m512i *ae = (__m512i *)(a + (k & ~63));
    __m512i scatter =
        _mm512_set_epi32(0x07070707, 0x07070707, 0x06060606, 0x06060606, 0x05050505, 0x05050505, 0x04040404, 0x04040404, 0x03030303,
                         0x03030303, 0x02020202, 0x02020202, 0x01010101, 0x01010101, 0x00000000, 0x00000000);
    __m512i cmpmask =
        _mm512_set_epi32(0x80402010, 0x08040201, 0x80402010, 0x08040201, 0x80402010, 0x08040201, 0x80402010, 0x08040201, 0x80402010,
                         0x08040201, 0x80402010, 0x08040201, 0x80402010, 0x08040201, 0x80402010, 0x08040201);
    __m512i up = _mm512_set1_epi8(u);
    unsigned p = 0;
    for (; ap < ae; p += 2, ap++) {
        __m128i low = _mm_loadl_epi64((const __m128i *)(b + p));
        __m512i bcast = _mm512_broadcastq_epi64(low);
        __m512i ret = _mm512_shuffle_epi8(bcast, scatter);
        ret = _mm512_andnot_si512(ret, cmpmask);
        __mmask64 tmp = _mm512_cmpeq_epi8_mask(ret, _mm512_setzero_si512());
        ret = _mm512_mask_blend_epi8(tmp, _mm512_setzero_si512(), up);
        _mm512_storeu_si512(ap, _mm512_xor_si512(_mm512_loadu_si512(ap), ret));
    }
    obl_axpyb32_ref((u8 *)ap, b + p, u, k & 63);
}

__attribute__((target("avx2"))) static void obl_axpyb32_avx2(u8 *a, u32 *b, u8 u, unsigned k)
{
    __m256i *ap = (__m256i *)a;
    __m256i *ae = (__m256i *)(a + (k & ~31));
    __m256i scatter =
        _mm256_set_epi32(0x03030303, 0x03030303, 0x02020202, 0x02020202, 0x01010101, 0x01010101, 0x00000000, 0x00000000);
    __m256i cmpmask =
        _mm256_set_epi32(0x80402010, 0x08040201, 0x80402010, 0x08040201, 0x80402010, 0x08040201, 0x80402010, 0x08040201);
    __m256i up = _mm256_set1_epi8(u);
    unsigned p = 0;
    for (; ap < ae; p++, ap++) {
        __m256i bcast = _mm256_set1_epi32(b[p]);
        __m256i ret = _mm256_shuffle_epi8(bcast, scatter);
        ret = _mm256_andnot_si256(ret, cmpmask);
        ret = _mm256_and_si256(_mm256_cmpeq_epi8(ret, _mm256_setzero_si256()), up);
        _mm256_storeu_si256(ap, _mm256_xor_si256(_mm256_loadu_si256(ap), ret));
    }
    obl_axpyb32_ref((u8 *)ap, b + p, u, k & 31);
}

__attribute__((target("ssse3"))) static void obl_axpyb32_ssse3(u8 *a, u32 *b, u8 u, unsigned k)
{
    __m128i *ap = (__m128i *)a;
    __m128i *ae = (__m128i *)(a + (k & ~31));
    __m128i scatter_hi = _mm_set_epi32(0x03030303, 0x03030303, 0x02020202, 0x02020202);
    __m128i scatter_lo = _mm_set_epi32(0x01010101, 0x01010101, 0x00000000, 0x00000000);
    __m128i cmpmask = _mm_set_epi32(0x80402010, 0x08040201, 0x80402010, 0x08040201);
    __m128i up = _mm_set1_epi8(u);
    unsigned p = 0;
    for (; ap < ae; p++, ap++) {
        __m128i bcast = _mm_set1_epi32(b[p]);
        __m128i ret_lo = _mm_shuffle_epi8(bcast, scatter_lo);
        __m128i ret_hi = _mm_shuffle_epi8(bcast, scatter_hi);
        ret_lo = _mm_andnot_si128(ret_lo, cmpmask);
        ret_hi = _mm_andnot_si128(ret_hi, cmpmask);
        ret_lo = _mm_and_si128(_mm_cmpeq_epi8(ret_lo, _mm_setzero_si128()), up);
        ret_hi = _mm_and_si128(_mm_cmpeq_epi8(ret_hi, _mm_setzero_si128()), up);
        _mm_storeu_si128(ap, _mm_xor_si128(_mm_loadu_si128(ap), ret_lo));
        ap++;
        _mm_storeu_si128(ap, _mm_xor_si128(_mm_loadu_si128(ap), ret_hi));
    }
    obl_axpyb32_ref((u8 *)ap, b + p, u, k & 31);
}

#endif

#if defined(OBLAS_ARCH_ARM) && (defined(__ARM_NEON) || defined(_MSC_VER))
#include <arm_neon.h>

#if !defined(__aarch64__) && !defined(_M_ARM64)
static inline uint8x16_t vqtbl1q_u8(uint8x16_t tbl, uint8x16_t idx)
{
    uint8x8x2_t tbl2;
    tbl2.val[0] = vget_low_u8(tbl);
    tbl2.val[1] = vget_high_u8(tbl);
    return vcombine_u8(vtbl2_u8(tbl2, vget_low_u8(idx)), vtbl2_u8(tbl2, vget_high_u8(idx)));
}
#endif

#define VEC_INIT_neon()                                                                                                            \
    const u8 *u_lo = GF2_8_SHUF_LO + u * 16;                                                                                       \
    const u8 *u_hi = GF2_8_SHUF_HI + u * 16;                                                                                       \
    const uint8x16_t mask = vdupq_n_u8(0x0f);                                                                                      \
    const uint8x16_t urow_lo = vld1q_u8(u_lo);                                                                                     \
    const uint8x16_t urow_hi = vld1q_u8(u_hi)
#define VEC_CORE_neon(bx, res)                                                                                                     \
    uint8x16_t lo_##res = vandq_u8(bx, mask);                                                                                      \
    uint8x16_t hi_##res = vshrq_n_u8(bx, 4);                                                                                       \
    lo_##res = vqtbl1q_u8(urow_lo, lo_##res);                                                                                      \
    hi_##res = vqtbl1q_u8(urow_hi, hi_##res);                                                                                      \
    uint8x16_t res = veorq_u8(lo_##res, hi_##res)
#define VEC_LOAD_neon(ptr) vld1q_u8((const uint8_t *)(ptr))
#define VEC_STORE_neon(ptr, val) vst1q_u8((uint8_t *)(ptr), val)
GENERATE_IMPL(neon, , uint8x16_t, VEC_LOAD_neon, VEC_STORE_neon, VEC_INIT_neon, VEC_CORE_neon, veorq_u8)

static void obl_axpyb32_neon(u8 *a, u32 *b, u8 u, unsigned k)
{
    uint8_t *ap = (uint8_t *)a;
    uint8_t *ae = (uint8_t *)(a + (k & ~31));
#ifdef _MSC_VER
    const uint8x16_t scatter_hi = {.n128_u8 ={ 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3}};
    const uint8x16_t scatter_lo = {.n128_u8 ={0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1}};
    const uint8x16_t cmpmask = {.n128_u8 = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80}};
#else
    const uint8x16_t scatter_hi = {2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3};
    const uint8x16_t scatter_lo = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1};
    const uint8x16_t cmpmask = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
#endif
    const uint8x16_t up = vdupq_n_u8(u);
    unsigned p = 0;
    for (; ap < ae; p++) {
        uint8x16_t bcast = vreinterpretq_u8_u32(vdupq_n_u32(b[p]));
        uint8x16_t ret_lo = vceqq_u8(vbicq_u8(cmpmask, vqtbl1q_u8(bcast, scatter_lo)), vdupq_n_u8(0));
        uint8x16_t ret_hi = vceqq_u8(vbicq_u8(cmpmask, vqtbl1q_u8(bcast, scatter_hi)), vdupq_n_u8(0));
        ret_lo = vandq_u8(ret_lo, up);
        ret_hi = vandq_u8(ret_hi, up);
        vst1q_u8(ap, veorq_u8(vld1q_u8(ap), ret_lo));
        vst1q_u8(ap + 16, veorq_u8(vld1q_u8(ap + 16), ret_hi));
        ap += 32;
    }
    obl_axpyb32_ref((u8 *)ap, b + p, u, k & 31);
}
#endif

#if defined(OBLAS_ARCH_RISCV) && defined(__riscv_vector)
#include <riscv_vector.h>

static void obl_axpy_rvv(u8 *a, u8 *b, u8 u, unsigned k)
{
    if (u == 0) {
        return;
    }
    if (u == 1) {
        size_t vl;
        for (unsigned i = 0; i < k; i += vl) {
            vl = __riscv_vsetvl_e8m1(k - i);
            vuint8m1_t va = __riscv_vle8_v_u8m1(a + i, vl);
            vuint8m1_t vb = __riscv_vle8_v_u8m1(b + i, vl);
            vuint8m1_t res = __riscv_vxor_vv_u8m1(va, vb, vl);
            __riscv_vse8_v_u8m1(a + i, res, vl);
        }
        return;
    }
    const u8 *u_lo = GF2_8_SHUF_LO + u * 16;
    const u8 *u_hi = GF2_8_SHUF_HI + u * 16;
    vuint8m1_t urow_lo = __riscv_vle8_v_u8m1(u_lo, 16);
    vuint8m1_t urow_hi = __riscv_vle8_v_u8m1(u_hi, 16);
    size_t vl;
    for (unsigned i = 0; i < k; i += vl) {
        vl = __riscv_vsetvl_e8m1(k - i);
        vuint8m1_t va = __riscv_vle8_v_u8m1(a + i, vl);
        vuint8m1_t vb = __riscv_vle8_v_u8m1(b + i, vl);
        vuint8m1_t indices_lo = __riscv_vand_vx_u8m1(vb, 0x0F, vl);
        vuint8m1_t indices_hi = __riscv_vsrl_vx_u8m1(vb, 4, vl);
        vuint8m1_t prod_lo = __riscv_vrgather_vv_u8m1(urow_lo, indices_lo, vl);
        vuint8m1_t prod_hi = __riscv_vrgather_vv_u8m1(urow_hi, indices_hi, vl);
        vuint8m1_t prod = __riscv_vxor_vv_u8m1(prod_lo, prod_hi, vl);
        vuint8m1_t res = __riscv_vxor_vv_u8m1(va, prod, vl);
        __riscv_vse8_v_u8m1(a + i, res, vl);
    }
}

static void obl_axiy_rvv(u8 *a, u8 *b, u8 u, unsigned k)
{
    if (u == 0) {
        size_t vl;
        for (unsigned i = 0; i < k; i += vl) {
            vl = __riscv_vsetvl_e8m1(k - i);
            vuint8m1_t zero = __riscv_vmv_v_x_u8m1(0, vl);
            __riscv_vse8_v_u8m1(a + i, zero, vl);
        }
        return;
    }
    if (u == 1) {
        if (a != b) {
            size_t vl;
            for (unsigned i = 0; i < k; i += vl) {
                vl = __riscv_vsetvl_e8m1(k - i);
                vuint8m1_t vb = __riscv_vle8_v_u8m1(b + i, vl);
                __riscv_vse8_v_u8m1(a + i, vb, vl);
            }
        }
        return;
    }
    const u8 *u_lo = GF2_8_SHUF_LO + u * 16;
    const u8 *u_hi = GF2_8_SHUF_HI + u * 16;
    vuint8m1_t urow_lo = __riscv_vle8_v_u8m1(u_lo, 16);
    vuint8m1_t urow_hi = __riscv_vle8_v_u8m1(u_hi, 16);
    size_t vl;
    for (unsigned i = 0; i < k; i += vl) {
        vl = __riscv_vsetvl_e8m1(k - i);
        vuint8m1_t vb = __riscv_vle8_v_u8m1(b + i, vl);
        vuint8m1_t indices_lo = __riscv_vand_vx_u8m1(vb, 0x0F, vl);
        vuint8m1_t indices_hi = __riscv_vsrl_vx_u8m1(vb, 4, vl);
        vuint8m1_t prod_lo = __riscv_vrgather_vv_u8m1(urow_lo, indices_lo, vl);
        vuint8m1_t prod_hi = __riscv_vrgather_vv_u8m1(urow_hi, indices_hi, vl);
        vuint8m1_t prod = __riscv_vxor_vv_u8m1(prod_lo, prod_hi, vl);
        __riscv_vse8_v_u8m1(a + i, prod, vl);
    }
}

static void obl_scal_rvv(u8 *a, u8 u, unsigned k)
{
    obl_axiy_rvv(a, a, u, k);
}
#endif

static void obl_scal_ref_wrapper(u8 *a, u8 u, unsigned k)
{
    obl_axiy_ref(a, a, u, k);
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4113)
#endif
void oblas_get_impl(struct oblas_impl *impl)
{
    oblas_lite_init();
#if defined(OBLAS_ARCH_X86) && !defined(_MSC_VER)
    __builtin_cpu_init();
#endif
    /* fallback */
    impl->axpy = obl_axpy_ref;
    impl->scal = obl_scal_ref_wrapper;
    impl->axiy = obl_axiy_ref;
    impl->axpyb32 = obl_axpyb32_ref;
    impl->align_size = sizeof(void *);

#if defined(OBLAS_ARCH_X86)
    int has_avx512 = __builtin_cpu_supports("avx512f") &&
                     __builtin_cpu_supports("avx512bw") &&
                     __builtin_cpu_supports("avx512dq") &&
                     __builtin_cpu_supports("avx512vl");
    if (has_avx512 && __builtin_cpu_supports("gfni")) {
        impl->axpy = obl_axpy_avx512_gfni;
        impl->scal = obl_scal_avx512_gfni;
        impl->axiy = obl_axiy_avx512_gfni;
        impl->axpyb32 = obl_axpyb32_avx512;
        impl->align_size = 64;
    } else if (has_avx512) {
        impl->axpy = obl_axpy_avx512_std;
        impl->scal = obl_scal_avx512_std;
        impl->axiy = obl_axiy_avx512_std;
        impl->axpyb32 = obl_axpyb32_avx512;
        impl->align_size = 64;
    } else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("gfni")) {
        impl->axpy = obl_axpy_avx2_gfni;
        impl->scal = obl_scal_avx2_gfni;
        impl->axiy = obl_axiy_avx2_gfni;
        impl->axpyb32 = obl_axpyb32_avx2;
        impl->align_size = 32;
    } else if (__builtin_cpu_supports("avx2")) {
        impl->axpy = obl_axpy_avx2_std;
        impl->scal = obl_scal_avx2_std;
        impl->axiy = obl_axiy_avx2_std;
        impl->axpyb32 = obl_axpyb32_avx2;
        impl->align_size = 32;
    } else if (__builtin_cpu_supports("ssse3") && __builtin_cpu_supports("gfni")) {
        impl->axpy = obl_axpy_ssse3_gfni;
        impl->scal = obl_scal_ssse3_gfni;
        impl->axiy = obl_axiy_ssse3_gfni;
        impl->axpyb32 = obl_axpyb32_ssse3;
        impl->align_size = 16;
    } else if (__builtin_cpu_supports("ssse3")) {
        impl->axpy = obl_axpy_ssse3_std;
        impl->scal = obl_scal_ssse3_std;
        impl->axiy = obl_axiy_ssse3_std;
        impl->axpyb32 = obl_axpyb32_ssse3;
        impl->align_size = 16;
    }
#elif defined(OBLAS_ARCH_ARM) && (defined(__ARM_NEON) || defined(_MSC_VER))
    impl->axpy = obl_axpy_neon;
    impl->scal = obl_scal_neon;
    impl->axiy = obl_axiy_neon;
    impl->axpyb32 = obl_axpyb32_neon;
    impl->align_size = 16;
#elif defined(OBLAS_ARCH_RISCV) && defined(__riscv_vector)
    impl->axpy = obl_axpy_rvv;
    impl->scal = obl_scal_rvv;
    impl->axiy = obl_axiy_rvv;
    impl->axpyb32 = obl_axpyb32_ref;
    impl->align_size = 16;
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}
