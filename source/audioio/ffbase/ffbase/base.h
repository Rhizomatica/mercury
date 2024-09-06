/** ffbase: base types & memory functions
2020, Simon Zolin
*/

#ifndef _FFBASE_BASE_H
#define _FFBASE_BASE_H

/*
Detect CPU
	FF_AMD64 FF_X86 FF_ARM64 FF_ARM
	FF_LITTLE_ENDIAN FF_BIG_ENDIAN
	FF_64 FF_SSE2
Detect OS
	FF_UNIX FF_WIN FF_LINUX FF_ANDROID FF_BSD
Base types
	ffbyte ffushort ffint ffuint ffint64 ffuint64 ffsize ffssize
FF_ASSERT
FF_EXTERN FF_INLINE_EXTERN
ff_likely ff_unlikely
ff_printf
ffmin ffmin64
ffmax
ffint_abs
FFINT_JOIN64
FFINT_DIVSAFE
FF_SWAP
FF_COUNT FFS_LEN
FF_OFF FF_PTR
FF_CONTAINER
FF_STRUCTALIGN
Endian conversion:
	ffint_be_cpu16 ffint_be_cpu32 ffint_be_cpu64
	ffint_le_cpu16 ffint_le_cpu32 ffint_le_cpu64
	ffint_le_cpu16_ptr ffint_le_cpu24_ptr ffint_le_cpu32_ptr ffint_le_cpu64_ptr
	ffint_be_cpu16_ptr ffint_be_cpu24_ptr ffint_be_cpu32_ptr ffint_be_cpu64_ptr
Bits:
	ffbit_find32 ffbit_find64
	ffbit_rfind32 ffbit_rfind64
	ffbit_test32 ffbit_array_test
	ffbit_set32
	ffbit_reset32
Integer align:
	ffint_align_floor2 ffint_align_floor
	ffint_align_ceil2 ffint_align_ceil
	ffint_ispower2
	ffint_align_power2
ffsz_len ffwsz_len
Heap allocation
	ffmem_alloc ffmem_zalloc ffmem_new ffmem_realloc
	ffmem_free
	ffmem_align
	ffmem_alignfree
ffmem_stack
ffmem_cmp ffmem_fill ffmem_findbyte
ffmem_zero ffmem_zero_obj
ffmem_copy ffmem_move
*/

/* Detect CPU */
#if defined __amd64__ || defined _M_AMD64
	#define FF_AMD64
	#define FF_LITTLE_ENDIAN
	#define FF_64
	#define FF_SSE2

#elif defined __i386__ || defined _M_IX86
	#define FF_X86
	#define FF_LITTLE_ENDIAN
	#define FF_SSE2

#elif defined __aarch64__
	#define FF_ARM64
	#define FF_64

#elif defined __arm__ || defined _M_ARM
	#define FF_ARM

#else
	#warning "This CPU is not supported"
#endif

#if defined FF_ARM64 || defined FF_ARM
	#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		#define FF_LITTLE_ENDIAN
	#else
		#define FF_BIG_ENDIAN
	#endif
#endif


/* Detect OS */
#if defined FF_WIN || defined FF_UNIX
	// already defined

#elif defined _WIN32 || defined _WIN64 || defined __CYGWIN__
	#define FF_WIN

#elif defined __APPLE__ && defined __MACH__
	#define FF_UNIX
	#define FF_APPLE

#elif defined __linux__
	#define FF_LINUX
	#define FF_UNIX
	#ifdef ANDROID
		#define FF_ANDROID
	#endif

#elif defined __unix__
	#define FF_UNIX
	#include <sys/param.h>
	#if defined BSD
		#define FF_BSD
	#endif

#else
	#warning "This OS is not supported"
#endif

#if defined FF_WIN
	#include <winsock2.h>
	#include <windows.h>
	#include <stdlib.h>
#else
	#ifndef _POSIX_C_SOURCE
		#define _POSIX_C_SOURCE  200112L // for posix_memalign()
	#endif
	#ifndef __USE_XOPEN2K
		#define __USE_XOPEN2K
	#endif
	#include <alloca.h>
	#include <stdlib.h>
	#include <string.h>
	#include <unistd.h>
	#include <errno.h>
#endif


/* Base types */
#define ffbyte  unsigned char
#define ffushort  unsigned short
#define ffint  int
#define ffuint  unsigned int
#define ffint64  long long
#define ffuint64  unsigned long long
#define ffsize  size_t
#define ffssize  ssize_t


#ifdef FF_DEBUG
	#include <assert.h>

	/** Debug-mode assertion */
	#define FF_ASSERT(expr)  assert(expr)
#else
	#define FF_ASSERT(expr)
#endif


#ifdef __cplusplus
	#define FF_EXTERN extern "C"
#else
	#define FF_EXTERN extern
#endif

#ifdef FFBASE_OPT_SIZE
	#define FF_INLINE_EXTERN  FF_EXTERN
#else
	#define FF_INLINE_EXTERN  static inline
#endif

#define FF_EXPORT  __attribute__((visibility("default")))


#define ff_likely(x)  __builtin_expect(!!(x), 1)

#define ff_unlikely(x)  __builtin_expect(!!(x), 0)


#include <stdio.h>
/** Formatted print to stdout
Supported: %p, %u, %s */
#define ff_printf(fmt, ...)  printf(fmt, ##__VA_ARGS__)


/** Get minimum value */
static inline ffsize ffmin(ffsize a, ffsize b)
{
	return (a < b) ? a : b;
}

static inline ffuint64 ffmin64(ffuint64 a, ffuint64 b)
{
	return (a < b) ? a : b;
}

/** Get maximum value */
#define ffmax(i0, i1) \
	(((i0) < (i1)) ? (i1) : (i0))

/** Get absolute value */
#define ffint_abs(n) \
({ \
	__typeof__(n) _n = (n); \
	(_n >= 0) ? _n : -_n; \
})

#define FFINT_JOIN64(hi, lo) \
	(((ffuint64)hi) << 32) | (lo)

/** Perform integer division and protect against division by zero.
Return 0 on error */
#define FFINT_DIVSAFE(val, by) \
	((by) != 0 ? (val) / (by) : 0)

/** Set new value and return the old value. */
#define FF_SWAP(ptr, val) \
({ \
	__typeof__(*ptr) __tmp = *ptr; \
	*ptr = val; \
	__tmp; \
})

/** Get N of elements in a static C array */
#define FF_COUNT(ar)  (sizeof(ar) / sizeof(ar[0]))

/** Loop for each element of a static C array */
#define FF_FOREACH(static_array, it) \
	for (it = static_array;  it != static_array + FF_COUNT(static_array);  it++)

/** Get N of characters in a static C string */
#define FFS_LEN(s)  (FF_COUNT(s) - 1)

/** Get offset of a field in structure */
#define FF_OFF(struct_type, field) \
	(((ffsize)&((struct_type *)0)->field))

/** Get struct field pointer by struct pointer and field offset */
#define FF_PTR(struct_ptr, field_off)  ((void*)((char*)(struct_ptr) + (field_off)))

/** Get struct pointer by its field pointer */
#define FF_CONTAINER(struct_type, field_name, field_ptr) \
	((struct_type*)((char*)field_ptr - FF_OFF(struct_type, field_name)))
#define FF_STRUCTPTR  FF_CONTAINER

/** Set alignment for a struct */
#define FF_STRUCTALIGN(n)  __attribute__((aligned(n)))


/** Swap bytes
e.g. "0x11223344" <-> "0x44332211" */
#define ffint_bswap16(i)  __builtin_bswap16(i)
#define ffint_bswap32(i)  __builtin_bswap32(i)
#define ffint_bswap64(i)  __builtin_bswap64(i)

#ifdef FF_LITTLE_ENDIAN
	/** Swap bytes: BE <-> CPU */
	#define ffint_be_cpu16(i)  __builtin_bswap16(i)
	#define ffint_be_cpu32(i)  __builtin_bswap32(i)
	#define ffint_be_cpu64(i)  __builtin_bswap64(i)

	/** Swap bytes: LE <-> CPU */
	#define ffint_le_cpu16(i)  (i)
	#define ffint_le_cpu32(i)  (i)
	#define ffint_le_cpu64(i)  (i)

#else // FF_BIG_ENDIAN:
	/** Swap bytes: BE <-> CPU */
	#define ffint_be_cpu16(i)  (i)
	#define ffint_be_cpu32(i)  (i)
	#define ffint_be_cpu64(i)  (i)

	/** Swap bytes: LE <-> CPU */
	#define ffint_le_cpu16(i)  __builtin_bswap16(i)
	#define ffint_le_cpu32(i)  __builtin_bswap32(i)
	#define ffint_le_cpu64(i)  __builtin_bswap64(i)
#endif

#define ffint_le_cpu16_ptr(ptr)  ffint_le_cpu16(*(ffushort*)(ptr))
#define ffint_le_cpu32_ptr(ptr)  ffint_le_cpu32(*(ffuint*)(ptr))
#define ffint_le_cpu64_ptr(ptr)  ffint_le_cpu64(*(ffuint64*)(ptr))
static inline ffuint ffint_le_cpu24_ptr(const void *p)
{
	const ffbyte *b = (ffbyte*)p;
	return ((ffuint)b[2] << 16) | ((ffuint)b[1] << 8) | b[0];
}
#define ffint_be_cpu16_ptr(ptr)  ffint_be_cpu16(*(ffushort*)(ptr))
#define ffint_be_cpu32_ptr(ptr)  ffint_be_cpu32(*(ffuint*)(ptr))
#define ffint_be_cpu64_ptr(ptr)  ffint_be_cpu64(*(ffuint64*)(ptr))
static inline ffuint ffint_be_cpu24_ptr(const void *p)
{
	const ffbyte *b = (ffbyte*)p;
	return ((ffuint)b[0] << 16) | ((ffuint)b[1] << 8) | (ffuint)b[2];
}


/** Find the most significant 1-bit
--> 0xABCD
Return bit position +1;
 0 if not found */
static inline ffuint ffbit_find32(ffuint n)
{
	return (n != 0) ? __builtin_clz(n) + 1 : 0;
}

static inline ffuint ffbit_find64(ffuint64 n)
{
	return (n != 0) ? __builtin_clzll(n) + 1 : 0;
}

/** Find the least significant 1-bit
0xABCD <--
Return position +1
  0 if not found */
static inline ffuint ffbit_rfind32(ffuint n)
{
	return __builtin_ffs(n);
}

static inline ffuint ffbit_rfind64(ffuint64 n)
{
	return __builtin_ffsll(n);
}

/** Return TRUE if bit is set */
static inline int ffbit_test32(const ffuint *p, ffuint bit)
{
	FF_ASSERT(bit < 32);
	return ((*p & (1U << bit)) != 0);
}

/** Set bit or return TRUE if it's set already */
static inline int ffbit_set32(ffuint *p, ffuint bit)
{
	FF_ASSERT(bit < 32);
	if ((*p & (1U << bit)) == 0) {
		*p |= (1U << bit);
		return 0;
	}
	return 1;
}

/** Reset bit and return TRUE if it was set */
static inline int ffbit_reset32(ffuint *p, ffuint bit)
{
	FF_ASSERT(bit < 32);
	if ((*p & (1U << bit)) != 0) {
		*p &= ~(1U << bit);
		return 1;
	}
	return 0;
}

/** Return TRUE if a bit is set in bit-array */
static inline int ffbit_array_test(const void *d, ffsize bit)
{
	const ffbyte *b = (ffbyte*)d + bit / 8;
	bit = 7 - (bit % 8);
	return !!(*b & (1U << bit));
}

/** Set bit and return its previous value */
static inline int ffbit_array_set(const void *d, ffsize bit)
{
	ffbyte *b = (ffbyte*)d + bit / 8;
	bit = 7 - (bit % 8);
	if (!!(*b & (1U << bit)))
		return 1;
	*b |= (1U << bit);
	return 0;
}


/** Align number to lower/upper boundary
align: must be a power of 2 */
#define ffint_align_floor2(n, align) \
	((n) & ~(ffuint64)((align) - 1))
#define ffint_align_ceil2(n, align) \
	ffint_align_floor2((n) + (align) - 1, align)

/** Align number to lower/upper boundary */
#define ffint_align_floor(n, align) \
	((n) / (align) * (align))
#define ffint_align_ceil(n, align) \
	ffint_align_floor((n) + (align) - 1, align)

/** Return TRUE if number is a power of 2.
Example:
	4   (0100) &
	4-1 (0011) =
	0   (0000) => TRUE
Example:
	3   (0011) &
	3-1 (0010) =
	2   (0010) => FALSE
*/
#define ffint_ispower2(n)  ((n) >= 2 && (((n) - 1) & (n)) == 0)

/** Align number to the next power of 2.
Example:
	6    (0110)
	6-1  (0101) -> MSB=#3
	1<<3 (1000) = 8
Example:
	4    (0100)
	4-1  (0011) -> MSB=#2
	1<<2 (0100) = 4
Note: value n > 2^63 is not supported */
static inline ffuint64 ffint_align_power2(ffuint64 n)
{
	if (n <= 2)
		return 2;
	ffuint one = ffbit_find64(n - 1);
	return 1ULL << (64 - one + 1);
}


#define ffsz_len(sz)  strlen(sz)
#define ffwsz_len(sz)  wcslen(sz)

#define FFMEM_ASSERT(ptr)  (ptr)
#ifdef FFBASE_MEM_ASSERT
	#include <assert.h>
	#undef FFMEM_ASSERT
	/** Call assert() on memory allocation failure */
	#define FFMEM_ASSERT(ptr) \
	({ \
		void *p = ptr; \
		assert(p != NULL); \
		p; \
	})
#endif

/* Heap allocation */
#if !defined _FFBASE_MEM_ALLOC
#define _FFBASE_MEM_ALLOC

#ifdef FF_WIN

static inline void* ffmem_alloc(ffsize size)
{
	return FFMEM_ASSERT(HeapAlloc(GetProcessHeap(), 0, size));
}

static inline void* ffmem_calloc(ffsize n, ffsize elsize)
{
	return FFMEM_ASSERT(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * elsize));
}

static inline void* ffmem_realloc(void *ptr, ffsize new_size)
{
	if (ptr == NULL)
		return FFMEM_ASSERT(HeapAlloc(GetProcessHeap(), 0, new_size));
	return FFMEM_ASSERT(HeapReAlloc(GetProcessHeap(), 0, ptr, new_size));
}

static inline void ffmem_free(void *ptr)
{
	HeapFree(GetProcessHeap(), 0, ptr);
}

static inline void* ffmem_align(ffsize size, ffsize align)
{
	return _aligned_malloc(size, align);
}

static inline void ffmem_alignfree(void *ptr)
{
	_aligned_free(ptr);
}

#else // UNIX:

static inline void* ffmem_alloc(ffsize size)
{
	return FFMEM_ASSERT(malloc(size));
}

static inline void* ffmem_calloc(ffsize n, ffsize elsize)
{
	return FFMEM_ASSERT(calloc(n, elsize));
}

static inline void* ffmem_realloc(void *ptr, ffsize new_size)
{
	return FFMEM_ASSERT(realloc(ptr, new_size));
}

static inline void ffmem_free(void *ptr)
{
	free(ptr);
}

static inline void* ffmem_align(ffsize size, ffsize align)
{
	void *buf;
	int e = posix_memalign(&buf, align, size);
	if (e != 0) {
		errno = e;
		return NULL;
	}
	return buf;
}

static inline void ffmem_alignfree(void *ptr)
{
	free(ptr);
}

#endif

/** Allocate heap memory region
Return NULL on error */
static void* ffmem_alloc(ffsize size);

#define ffmem_zalloc(size)  ffmem_calloc(1, size)

/** Allocate heap memory zero-filled region
Return NULL on error */
static void* ffmem_calloc(ffsize n, ffsize elsize);

/** Reallocate heap memory region
Return NULL on error */
static void* ffmem_realloc(void *ptr, ffsize new_size);

/** Allocate an object of type T */
#define ffmem_new(T)  ((T*)ffmem_calloc(1, sizeof(T)))

/** Deallocate heap memory region */
static void ffmem_free(void *ptr);


/** Allocate aligned memory
align:
  Windows, Android: must be a multiple of sizeof(void*)
Return NULL on error */
static void* ffmem_align(ffsize size, ffsize align);

/** Deallocate aligned memory */
static void ffmem_alignfree(void *ptr);


/** Reserve stack buffer */
#define ffmem_stack(size)  __builtin_alloca(size)

#define FFMEM_STACK_THRESHOLD  4096

/** Reserve stack or allocate a heap buffer */
#define _ffmem_alloc_stackorheap(size) \
	((size) < FFMEM_STACK_THRESHOLD) ? __builtin_alloca(size) : ffmem_alloc(size)

#define _ffmem_free_stackorheap(ptr, size) \
({ \
	if ((size) >= FFMEM_STACK_THRESHOLD) \
		ffmem_free(ptr); \
})

#endif


/** Compare buffers */
#define ffmem_cmp(a, b, len)  memcmp(a, b, len)

/** Find byte in buffer
Return pointer to the byte or NULL */
#define ffmem_findbyte(p, len, ch)  memchr(p, ch, len)

/** Fill the buffer with copies of byte */
#define ffmem_fill(p, ch, len)  memset(p, ch, len)

/** Fill the buffer with zeros */
#define ffmem_zero(ptr, len)  memset(ptr, 0, len)

/** Fill the buffer with zeros */
#define ffmem_zero_obj(ptr)  memset(ptr, 0, sizeof(*ptr))

/** Copy data
Return tail pointer */
static inline void* ffmem_copy(void *dst, const void *src, ffsize len)
{
	memcpy(dst, src, len);
	return (char*)dst + len;
}

/** Return N of bytes copied */
static inline ffsize ffmem_ncopy(void *dst, ffsize cap, const void *src, ffsize len)
{
	len = ffmin(len, cap);
	memcpy(dst, src, len);
	return len;
}

/** Safely copy data (overlapping regions) */
#define ffmem_move(dst, src, len)  (void) memmove(dst, src, len)

FF_EXTERN int _ffcpu_features;

#endif
