/** ffbase: array container consisting of 2 fields: data pointer and the number of elements.
2020, Simon Zolin
*/

#pragma once
#define _FFBASE_SLICE_H

#ifndef _FFBASE_BASE_H
#include <ffbase/base.h>
#endif
#include <ffbase/sort.h> // optional


/** Simple array container */
typedef struct ffslice {
	ffsize len; // Number of used elements
	void *ptr; // Data pointer: can be dynamically allocated or static
} ffslice;


/*
GETTERS
SET
WALK
	FFSLICE_WALK FFSLICE_WALK_T FFSLICE_RWALK FFSLICE_RWALK_T
	FFSLICE_FOREACH_T FFSLICE_FOREACH_PTR_T
COMPARE
	ffslice_eq ffslice_eqT ffslice_eq2T
FIND
	ffarrint8_find ffarrint16_find ffarrint32_find
	ffarrint8_binfind ffarrint32_binfind
	ffslice_find ffslice_findT
	ffarr_binfind ffslice_binfind ffslice_binfindT
ALLOCATE
	ffslice_alloc_stack
	ffslice_alloc ffslice_allocT ffslice_zalloc ffslice_zallocT
	ffslice_realloc ffslice_reallocT ffslice_growT
	ffslice_free
COPY
	ffslice_pushT ffslice_add ffslice_addT ffslice_add2T
ALLOCATE+COPY
	ffslice_dup ffslice_dupT ffslice_dup2T
REMOVE
	ffslice_rmswap ffslice_rmswapT
	ffslice_rm ffslice_rmT
SORT
	ffslice_sort ffslice_sortT
	ffarrint32_sort
*/

// GETTERS

/** Get the pointer to the element at index: &a[idx] */
#define ffslice_itemT(a, idx, T) \
	(&((T*)(a)->ptr)[idx])

/** Get the pointer to the last element: &a[len - 1] */
#define ffslice_lastT(a, T) \
	(&((T*)(a)->ptr)[(a)->len - 1])

/** Get array's tail: &a[len] */
#define ffslice_end(a, elsize) \
	((char*)(a)->ptr + (a)->len * elsize)

#define ffslice_endT(a, T) \
	(&((T*)(a)->ptr)[(a)->len])


// SET
// Note: don't use these functions for the array with allocated buffer.

/** Set data pointer and length: a = {data, length} */
#define ffslice_set(a, data, n) \
do { \
	(a)->ptr = (void*)data; \
	(a)->len = n; \
} while (0)

/** Set data pointer and length: a = src */
#define ffslice_set2(a, src)  ffslice_set(a, (src)->ptr, (src)->len)

/** Set empty array: a = {} */
#define ffslice_null(a)  ffslice_set(a, NULL, 0)

/** Shift data pointer */
static inline void ffslice_shift(ffslice *a, ffsize by, ffsize elsize)
{
	FF_ASSERT(by <= a->len);
	a->ptr = (char*)a->ptr + by * elsize;
	a->len -= by;
}

#define ffslice_shiftT(a, by, T)  ffslice_shift(a, by, sizeof(T))



// WALK

#define FFSLICE_FOR_T(a, it, T) \
	for (it = (T*)(a)->ptr;  it != ffslice_endT(a, T);  )
#define FFSLICE_FOR(a, it) \
	FFSLICE_FOR_T(a, it, __typeof__(*it))

/** Walk through array's elements */
#define FFSLICE_WALK_T(a, it, T) \
	for (it = (T*)(a)->ptr;  it != ffslice_endT(a, T);  it++)
#define FFSLICE_WALK(a, it) \
	FFSLICE_WALK_T(a, it, __typeof__(*it))

/** Reverse walk through array's elements */
#define FFSLICE_RWALK_T(a, it, T) \
	for (it = (T*)ffslice_lastT(a, T);  (ffssize)it >= (ffssize)(a)->ptr;  it--)
#define FFSLICE_RWALK(a, it) \
	FFSLICE_RWALK_T(a, it, __typeof__(*it))

/** Call the function for each element.
Example:

struct s {...};
void foo(struct s *s) { ... }

ffslice a = {}; // struct s[]
FFSLICE_FOREACH_T(&a, foo, struct s);
*/
#define FFSLICE_FOREACH_T(a, func, T) \
do { \
	T *ffsliceiter; \
	FFSLICE_WALK_T(a, ffsliceiter, T) { \
		func(ffsliceiter); \
	} \
} while (0)

/** Call the function for each pointer element.
Example:

void foo(char *s) { ... }

ffslice a = {}; // char*[]
FFSLICE_FOREACH_PTR_T(&a, foo, char*);
*/
#define FFSLICE_FOREACH_PTR_T(a, func, T) \
do { \
	T *ffsliceiter; \
	FFSLICE_WALK_T(a, ffsliceiter, T) { \
		func(*ffsliceiter); \
	} \
} while (0)


// COMPARE

/** Compare array data: a == cmp[] */
static inline int ffslice_eq(ffslice *a, const void *cmp, ffsize n, ffsize elsize)
{
	return a->len == n
		&& !ffmem_cmp(a->ptr, cmp, n * elsize);
}

#define ffslice_eqT(a, cmp, n, T)  ffslice_eq(a, cmp, n, sizeof(T))

/** Compare 2 arrays: a == a_cmp */
#define ffslice_eq2T(a, a_cmp, T)  ffslice_eq(a, (a_cmp)->ptr, (a_cmp)->len, sizeof(T))


// FIND

static inline ffssize ffarrint8_find(const ffbyte *a, ffsize len, ffuint search)
{
	for (ffsize i = 0;  i != len;  i++) {
		if (a[i] == search)
			return i;
	}
	return -1;
}

static inline ffssize ffarrint16_find(const ffushort *a, ffsize len, ffuint search)
{
	for (ffsize i = 0;  i != len;  i++) {
		if (a[i] == search)
			return i;
	}
	return -1;
}

static inline ffssize ffarrint32_find(const ffuint *a, ffsize len, ffuint search)
{
	for (ffsize i = 0;  i != len;  i++) {
		if (a[i] == search)
			return i;
	}
	return -1;
}

static inline ffssize ffarrint8_binfind(const ffbyte *a, ffsize len, ffuint search)
{
	ffsize start = 0;
	while (start != len) {
		ffsize i = start + (len - start) / 2;
		if (a[i] == search)
			return i;
		else if (a[i] > search)
			len = i;
		else
			start = i + 1;
	}
	return -1;
}

static inline ffssize ffarrint16_binfind(const ffushort *a, ffsize len, ffuint search)
{
	ffsize start = 0;
	while (start != len) {
		ffsize i = start + (len - start) / 2;
		if (a[i] == search)
			return i;
		else if (a[i] > search)
			len = i;
		else
			start = i + 1;
	}
	return -1;
}

static inline ffssize ffarrint32_binfind(const ffuint *a, ffsize len, ffuint search)
{
	ffsize start = 0;
	while (start != len) {
		ffsize i = start + (len - start) / 2;
		if (a[i] == search)
			return i;
		else if (a[i] > search)
			len = i;
		else
			start = i + 1;
	}
	return -1;
}

/** Return 0: (a == b);
 -1: (a < b);
  1: (a > b) */
typedef int (*ffslice_cmp_func)(const void *a, const void *b);

/** Find element 'search' of size 'elsize' in array.
eq: compare function
Return -1 if not found */
static inline ffssize ffslice_find(const ffslice *a, const void *search, ffsize elsize, ffslice_cmp_func cmp)
{
	for (ffsize i = 0;  i != a->len;  i++) {
		if (0 == cmp((char*)a->ptr + i * elsize, search))
			return i;
	}
	return -1;
}

#define ffslice_findT(a, search, cmp, T)  ffslice_find(a, search, sizeof(T), cmp)

static inline ffssize ffarr_binfind(const void *a, ffsize len, const void *search, ffsize elsize, ffslice_cmp_func cmp)
{
	ffsize start = 0;
	while (start != len) {
		ffsize i = start + (len - start) / 2;
		int r = cmp((char*)a + i * elsize, search);
		if (r == 0)
			return i;
		else if (r > 0)
			len = i;
		else
			start = i + 1;
	}
	return -1;
}

/** Find element 'search' of size 'elsize' in a sorted array.
cmp: compare function
Return -1 if not found */
static inline ffssize ffslice_binfind(const ffslice *a, const void *search, ffsize elsize, ffslice_cmp_func cmp)
{
	return ffarr_binfind(a->ptr, a->len, search, elsize, cmp);
}

#define ffslice_binfindT(a, search, cmp, T)  ffslice_binfind(a, search, sizeof(T), cmp)


// ALLOCATE

/** Reserve stack for 'n' elements: byte[n][elsize]
Don't free this buffer!
Return NULL on error */
static inline void* ffslice_alloc_stack(ffslice *a, ffsize n, ffsize elsize)
{
	FF_ASSERT(a->ptr == NULL);
	a->len = 0;
	return (a->ptr = ffmem_stack(n * elsize));
}

/** Allocate memory for 'n' elements: byte[n][elsize]
Return NULL on error */
static inline void* ffslice_alloc(ffslice *a, ffsize n, ffsize elsize)
{
	FF_ASSERT(a->ptr == NULL);

	ffsize bytes;
	if (__builtin_mul_overflow(n, elsize, &bytes)) {
		return NULL;
	}

	a->len = 0;
	return (a->ptr = ffmem_alloc(bytes));
}

/** Allocate memory for 'n' elements: T[n]
Return NULL on error */
#define ffslice_allocT(a, n, T)  ffslice_alloc(a, n, sizeof(T))

/** Allocate zeroed memory for 'n' elements: byte[n][elsize]
Return NULL on error */
static inline void* ffslice_zalloc(ffslice *a, ffsize n, ffsize elsize)
{
	FF_ASSERT(a->ptr == NULL);

	ffsize bytes;
	if (__builtin_mul_overflow(n, elsize, &bytes)) {
		return NULL;
	}

	a->len = 0;
	return (a->ptr = ffmem_zalloc(bytes));
}

/** Allocate zeroed memory for 'n' elements: T[n]
Return NULL on error */
#define ffslice_zallocT(a, n, T)  ffslice_zalloc(a, n, sizeof(T))

/** Grow/shrink/allocate memory buffer.
Return NULL on error (existing memory buffer is preserved) */
static inline void* ffslice_realloc(ffslice *a, ffsize n, ffsize elsize)
{
	ffsize bytes;
	if (__builtin_mul_overflow(n, elsize, &bytes)) {
		return NULL;
	}

	void *p;
	if (NULL == (p = ffmem_realloc(a->ptr, bytes)))
		return NULL;
	a->ptr = p;
	if (a->len > n)
		a->len = n;
	return a->ptr;
}

#define ffslice_reallocT(a, n, T)  ffslice_realloc(a, n, sizeof(T))

/** Grow memory buffer.
Return NULL on error (existing memory buffer is preserved) */
#define ffslice_growT(a, by, T)  ffslice_realloc(a, (a)->len + by, sizeof(T))

/** Free array's buffer */
#define ffslice_free(a) \
do { \
	ffmem_free((a)->ptr); \
	(a)->ptr = NULL; \
	(a)->len = 0; \
} while (0)


// COPY

/** Add a new (uninitialized) element and return its pointer: &a[len++]
Return NULL on error, the program will crash when accessing the pointer */
#define ffslice_pushT(a, cap, T) \
	(((a)->len < cap) ? &((T*)(a)->ptr)[(a)->len++] : NULL)

/** Add data into array's tail (the buffer must be allocated): a[len..] = src[]
Return N of elements copied */
static inline ffsize ffslice_add(ffslice *a, ffsize cap, const void *src, ffsize n, ffsize elsize)
{
	n = ffmin(n, cap - a->len);
	ffmem_copy(ffslice_end(a, elsize), src, n * elsize);
	a->len += n;
	return n;
}

#define ffslice_addT(a, cap, src, n, T)  ffslice_add(a, cap, src, n, sizeof(T))
#define ffslice_add2T(a, cap, asrc, T)  ffslice_add(a, cap, (asrc)->ptr, (asrc)->len, sizeof(T))

/** Move elements (overwriting existing data)
 so that the data within region [from..from+n) is copied into region [to..to+n).
Return pointer to the element at index `from`. */
static inline void* ffslice_move(ffslice *a, ffsize from, ffsize to, ffsize n, ffsize elsize)
{
	FF_ASSERT(ffmax(from, to) <= a->len);
	FF_ASSERT(ffmax(from, to) + n <= a->len);
	const char *src = (char*)a->ptr + from * elsize;
	char *dst = (char*)a->ptr + to * elsize;
	ffmem_move(dst, src, n * elsize);
	return (void*)src;
}

#define ffslice_moveT(a, from, to, n, T)  (T*)ffslice_move(a, from, to, n, sizeof(T))


// ALLOCATE+COPY

/** Allocate and copy data: a = copy(src[])
Return NULL on error */
static inline void* ffslice_dup(ffslice *a, const void *src, ffsize n, ffsize elsize)
{
	if (NULL == ffslice_alloc(a, n, elsize))
		return NULL;
	ffmem_copy(a->ptr, src, n * elsize);
	a->len = n;
	return a->ptr;
}

#define ffslice_dupT(a, src, n, T)  ffslice_dup(a, src, n, sizeof(T))

/** Allocate and copy data: a = copy(src) */
#define ffslice_dup2T(a, src, T)  ffslice_dup(a, (src)->ptr, (src)->len, sizeof(T))


// REMOVE

/** Remove elements moving the last elements into the hole:
A[0]...  ( A[i]... )  A[i+n]...  A[...]
A[0]...  A[...]  A[i+n]...
*/
static inline void ffslice_rmswap(ffslice *a, ffsize i, ffsize n, ffsize elsize)
{
	FF_ASSERT(i + n <= a->len);
	char *dst = (char*)a->ptr + i * elsize;
	ffsize off = ffmax(a->len - n, i + n);
	const char *src = (char*)a->ptr + off * elsize;
	ffmem_move(dst, src, (a->len - off) * elsize);
	a->len -= n;
}

#define ffslice_rmswapT(a, index, n, T)  ffslice_rmswap(a, index, n, sizeof(T))

/** Remove elements and shift next elements to the left:
A[0]...  ( A[i]... )  A[i+n]...
*/
static inline void ffslice_rm(ffslice *a, ffsize i, ffsize n, ffsize elsize)
{
	FF_ASSERT(i + n <= a->len);
	char *dst = (char*)a->ptr + i * elsize;
	const char *src = (char*)a->ptr + (i + n) * elsize;
	ffmem_move(dst, src, (a->len - (i + n)) * elsize);
	a->len -= n;
}

#define ffslice_rmT(a, index, n, T)  ffslice_rm(a, index, n, sizeof(T))


// SORT
#ifdef _FFBASE_SORT_H

static inline int _ffintcmp(const void *_a, const void *_b, void *udata)
{
	(void) udata;
	const ffuint *a = (ffuint*)_a,  *b = (ffuint*)_b;
	return (*a == *b) ? 0
		: (*a < *b) ? -1
		: 1;
}

static inline void ffarrint32_sort(ffuint *a, ffsize len)
{
	ffsort(a, len, 4, _ffintcmp, NULL);
}

static inline void ffslice_sort(ffslice *a, ffsortcmp cmp, void *udata, ffsize elsize)
{
	ffsort(a->ptr, a->len, elsize, cmp, udata);
}

/**
cmp: compare function */
#define ffslice_sortT(a, cmp, udata, T) \
	ffslice_sort(a, cmp, udata, sizeof(T))

#endif // _FFBASE_SORT_H
