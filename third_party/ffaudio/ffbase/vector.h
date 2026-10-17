/** ffbase: dynamic array container
2020, Simon Zolin
*/

#pragma once
#define _FFBASE_VECTOR_H

#include <ffbase/slice.h>
#include <ffbase/string.h> // optional

/*
GETTERS
	ffvec_isfull
	ffvec_unused
SET
	ffvec_null ffvec_shift
COMPARE
	ffvec_eq ffvec_eqT ffvec_eq2T
FIND
	ffvec_find ffvec_findT
	ffvec_binfind ffvec_binfindT
ALLOCATE
	ffvec_alloc ffvec_allocT ffvec_zalloc ffvec_zallocT
	ffvec_realloc ffvec_reallocT ffvec_grow ffvec_growT ffvec_growhalf ffvec_growtwiceT
	ffvec_free
COPY
	ffvec_push ffvec_pushT ffvec_add ffvec_addT ffvec_add2T
	ffvec_addchar
	ffvec_addstr ffvec_addsz
	ffvec_addfmt
*/

/** Dynamic array container
Can be casted to ffslice */
typedef struct ffvec {
	ffsize len; // Number of used elements
	void *ptr; // Data pointer: can be dynamically allocated or static
	ffsize cap; // Maximum number of allocated elements
} ffvec;

// GETTERS

/** Return TRUE for static data or when there's no free space left in buffer */
#define ffvec_isfull(v)  ((v)->cap == 0 || (v)->len == (v)->cap)

/** The number of free elements */
#define ffvec_unused(v)  (((v)->cap != 0) ? (v)->cap - (v)->len : 0)


// SET

static inline void ffvec_free(ffvec *v);

/** Set data pointer and length: v = {data, length} */
static inline void ffvec_set(ffvec *v, const void *data, ffsize n)
{
	ffvec_free(v);
	v->ptr = (void*)data;
	v->len = n;
}

/** Set data pointer and length: v = src */
#define ffvec_set2(v, src)  ffvec_set(v, (src)->ptr, (src)->len)

/** Set data pointer, length, capacity: v = {data, length, cap} */
static inline void ffvec_set3(ffvec *v, const void *data, ffsize n, ffsize cap)
{
	ffvec_free(v);
	v->ptr = (void*)data;
	v->len = n;
	v->cap = cap;
}

static inline void ffvec_null(ffvec *v)
{
	v->ptr = NULL;
	v->len = v->cap = 0;
}

static inline void ffvec_shift(ffvec *v, ffsize by, ffsize elsize)
{
	FF_ASSERT(v->cap == 0);
	FF_ASSERT(by <= v->len);
	v->ptr = (char*)v->ptr + by * elsize;
	v->len -= by;
}

#define ffvec_shiftT(v, by, T)  ffvec_shift(v, by, sizeof(T))


// COMPARE

#define ffvec_eq(v, cmp, n, elsize)  ffslice_eq((ffslice*)(v), cmp, n, elsize)
#define ffvec_eqT(v, cmp, n, T)  ffvec_eq(v, cmp, n, sizeof(T))
#define ffvec_eq2T(v, cmp, T)  ffvec_eq(v, cmp, sizeof(T))


// FIND

#define ffvec_find(v, search, cmp, udata, elsize)  ffslice_find((ffslice*)(v), search, cmp, udata, elsize)
#define ffvec_findT(v, search, cmp, udata, T)  ffvec_find(v, search, cmp, udata, sizeof(T))
#define ffvec_binfind(v, search, cmp, udata, elsize)  ffslice_binfind((ffslice*)(v), search, cmp, udata, elsize)
#define ffvec_binfindT(v, search, cmp, udata, T)  ffvec_binfind(v, search, cmp, udata, sizeof(T))


// ALLOCATE

/** Allocate memory for 'n' elements: byte[n][elsize]
Return NULL on error */
static inline void* ffvec_alloc(ffvec *v, ffsize n, ffsize elsize)
{
	ffvec_free(v);

	if (n == 0)
		n = 1;
	ffsize bytes;
	if (__builtin_mul_overflow(n, elsize, &bytes)) {
		return NULL;
	}

	if (NULL == (v->ptr = ffmem_alloc(bytes)))
		return NULL;

	v->cap = n;
	return v->ptr;
}

#define ffvec_allocT(v, n, T)  ((T*)ffvec_alloc(v, n, sizeof(T)))

/** Allocate and zero data in buffer */
static inline void* ffvec_zalloc(ffvec *v, ffsize n, ffsize elsize)
{
	ffvec_free(v);

	if (n == 0)
		n = 1;
	ffsize bytes;
	if (__builtin_mul_overflow(n, elsize, &bytes))
		return NULL;

	if (NULL == (v->ptr = ffmem_zalloc(bytes)))
		return NULL;

	v->cap = n;
	return v->ptr;
}

#define ffvec_zallocT(v, n, T)  ffvec_zalloc(v, n, sizeof(T))

/** Grow/allocate memory buffer
If the object's data is static, allocate new buffer and copy that data.
Allocate new buffer if length is 0.
Reallocate buffer only if new capacity is larger.
Return NULL on error (existing memory buffer is preserved if length is not 0) */
static inline void* ffvec_realloc(ffvec *v, ffsize n, ffsize elsize)
{
	void *p;
	if (v->cap == 0) {
		p = NULL;

	} else if (n > v->cap) {
		if (v->len != 0) {
			p = v->ptr;
		} else {
			ffmem_free(v->ptr);
			v->ptr = NULL;
			v->cap = 0;
			p = NULL;
		}

	} else {
		if (v->len > n)
			v->len = n;
		return v->ptr;
	}

	ffsize ncap = (n != 0) ? n : 1;
	ffsize bytes;
	if (__builtin_mul_overflow(ncap, elsize, &bytes)) {
		return NULL;
	}
	if (NULL == (p = ffmem_realloc(p, bytes)))
		return NULL;

	if (v->len > n)
		v->len = n;

	if (v->cap == 0 && v->len != 0)
		ffmem_copy(p, v->ptr, v->len * elsize);

	v->ptr = p;
	v->cap = ncap;
	return v->ptr;
}

#define ffvec_reallocT(v, n, T)  ffvec_realloc(v, n, sizeof(T))

/** Grow memory buffer.
Return NULL on error (existing memory buffer is preserved if length is not 0) */
static inline void* ffvec_growhalf(ffvec *v, ffsize by, ffsize elsize)
{
	if (v->len + by <= v->cap)
		return v->ptr;
	return ffvec_realloc(v, v->len + ffmax(by, v->len/2), elsize);
}

static inline void* ffvec_growtwice(ffvec *v, ffsize by, ffsize elsize)
{
	if (v->len + by <= v->cap)
		return v->ptr;
	return ffvec_realloc(v, v->len + ffmax(by, v->len), elsize);
}
#define ffvec_growtwiceT(v, by, T)  ffvec_growtwice(v, by, sizeof(T))

#define ffvec_grow(v, by, elsize)  ffvec_growhalf(v, by, elsize)
#define ffvec_growT(v, by, T)  ffvec_growhalf(v, by, sizeof(T))

/** Free array's buffer */
static inline void ffvec_free(ffvec *v)
{
	if (v->cap != 0) {
		FF_ASSERT(v->ptr != NULL);
		FF_ASSERT(v->len <= v->cap);
		ffmem_free(v->ptr);
		v->cap = 0;
	}
	v->ptr = NULL;
	v->len = 0;
}


// COPY

/** Add a new (uninitialized) element and return its pointer: &a[len++]
Return NULL on error, the program will crash when accessing the pointer */
static inline void* ffvec_push(ffvec *v, ffsize elsize)
{
	if (NULL == ffvec_grow(v, 1, elsize))
		return NULL;

	return (char*)v->ptr + v->len++ * elsize;
}

/** Add a new empty (zeroed) element */
static inline void* ffvec_zpush(ffvec *v, ffsize elsize)
{
	if (NULL == ffvec_grow(v, 1, elsize))
		return NULL;

	char *p = (char*)v->ptr + v->len++ * elsize;
	ffmem_zero(p, elsize);
	return p;
}

#define ffvec_pushT(v, T)  ((T*)ffvec_push(v, sizeof(T)))
#define ffvec_zpushT(v, T)  ((T*)ffvec_zpush(v, sizeof(T)))

static inline void* _ffvec_push(ffvec *v, ffsize elsize)
{
	return (char*)v->ptr + v->len++ * elsize;
}

/** Add data into array's tail: a[len..] = src[]
Return N of elements copied */
static inline ffsize ffvec_add(ffvec *v, const void *src, ffsize n, ffsize elsize)
{
	if (NULL == ffvec_grow(v, n, elsize))
		return 0;

	ffmem_copy(ffslice_end(v, elsize), src, n * elsize);
	v->len += n;
	return n;
}

#define ffvec_add2(v, vsrc, elsize)  ffvec_add(v, (vsrc)->ptr, (vsrc)->len, elsize)
#define ffvec_addT(v, src, n, T)  ffvec_add(v, src, n, sizeof(T))
#define ffvec_add2T(v, vsrc, T)  ffvec_add(v, (vsrc)->ptr, (vsrc)->len, sizeof(T))

static inline ffsize ffvec_addchar(ffvec *v, char ch)
{
	return ffvec_add(v, &ch, 1, 1);
}

#define ffvec_addstr(v, str)  ffvec_add(v, (str)->ptr, (str)->len, 1)
#define ffvec_addsz(v, sz)  ffvec_add(v, sz, ffsz_len(sz), 1)

#ifdef _FFBASE_STRING_H
#define ffvec_addfmt(v, fmt, ...)  ffstr_growfmt((ffstr*)(v), &(v)->cap, fmt, ##__VA_ARGS__)
#define ffvec_addfmtv(v, fmt, va)  ffstr_growfmtv((ffstr*)(v), &(v)->cap, fmt, va)
#endif
