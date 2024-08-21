/** ffbase: dynamic hash table
2020, Simon Zolin
*/

#pragma once

#ifndef _FFBASE_BASE_H
#include <ffbase/base.h>
#endif
#ifdef __SSE4_2__
#include <ffbase/crc32.h>
#else
#include <ffbase/murmurhash3.h>
#endif

/*
FFMAP_WALK
ffmap_hash
ffmap_init
ffmap_alloc ffmap_grow
ffmap_free
ffmap_add ffmap_add_hash
ffmap_rm ffmap_rm_hash
ffmap_find ffmap_find_hash
_ffmap_stats
*/

/** When to resize the buffer (in percent) */
#define FFMAP_RESIZE_THRESHOLD  75

/** Return TRUE if 'key' matches 'val' */
typedef int (*ffmap_keyeq_func)(void *opaque, const void *key, ffsize keylen, void *val);

/** Hash function */
typedef ffuint (*ffmap_hash_func)(const void *key, ffsize keylen);

struct _ffmap_item;

/** Hash table container */
typedef struct ffmap {
	ffsize len;
	ffsize cap;
	ffsize mask;
	struct _ffmap_item *data;
	ffmap_keyeq_func key_eq;
	ffmap_hash_func hasher;
} ffmap;

struct _ffmap_item {
	ffuint flags; // 0:free  1:occupied
	ffuint hash;
	void *val;
};

static inline int _ffmap_item_occupied(const struct _ffmap_item *it)
{
	return it->flags != 0;
}

/** Walk through all elements
Example:

struct _ffmap_item *it;
FFMAP_WALK(&map, it) {
	if (!_ffmap_item_occupied(it))
		continue;
	void *val = it->val;
}
*/
#define FFMAP_WALK(m, it) \
	for (it = (m)->data;  it != &(m)->data[(m)->cap];  it++)

/** Get hash value */
static inline ffuint ffmap_hash(const void *key, ffsize keylen)
{
#ifdef __SSE4_2__
	return crc32c(0, key, keylen);
#else
	return murmurhash3(key, keylen, 0x12345678);
#endif
}

/** Initialize container before use */
static inline void ffmap_init(ffmap *m, ffmap_keyeq_func keyeq)
{
	m->len = m->cap = 0;
	m->data = NULL;
	m->key_eq = keyeq;
	m->hasher = &ffmap_hash;
}

/** Free buffer */
static inline void ffmap_free(ffmap *m)
{
	ffmem_alignfree(m->data);
	m->data = NULL;
	m->len = m->cap = 0;
}

/** Allocate hash table buffer
Return 0 on success */
static inline int ffmap_alloc(ffmap *m, ffsize n)
{
	n = ffint_align_power2(n);
	ffmap_free(m);
	void *p;
	if (NULL == (p = ffmem_align(n * sizeof(struct _ffmap_item), 64)))
		return -1;
	ffmem_zero(p, n * sizeof(struct _ffmap_item));
	m->data = (struct _ffmap_item*)p;
	m->cap = n;
	m->mask = n - 1;
	m->len = 0;
	return 0;
}

#define IDX_HASH(m, hash)  (hash & m->mask)

/** Add new element */
static inline void _ffmap_add(ffmap *m, ffuint hash, void *val)
{
	FF_ASSERT(m->len != m->cap);
	ffsize i = IDX_HASH(m, hash);

	for (;;) {
		struct _ffmap_item *it = &m->data[i];

		if (it->flags == 0) {
			it->flags = 1;
			it->hash = hash;
			it->val = val;
			m->len++;
			return;
		}

		i = (i + 1) & m->mask;
	}
}

/** Move all elements to a new buffer
Return 0 on success */
static inline int _ffmap_resize(ffmap *m, ffsize newsize)
{
	ffmap nm = {};
	if (0 != ffmap_alloc(&nm, newsize))
		return -1;

	for (ffsize i = 0;  i != m->cap;  i++) {
		const struct _ffmap_item *it = &m->data[i];

		if (it->flags != 0)
			_ffmap_add(&nm, it->hash, it->val);
	}

	ffmem_alignfree(m->data);
	m->len = nm.len;
	m->cap = nm.cap;
	m->mask = nm.mask;
	m->data = nm.data;
	return 0;
}

/** Grow buffer
Reallocate if capacity is less than total required size plus THRESHOLD
Return 0 on success */
static inline int ffmap_grow(ffmap *m, ffsize by)
{
	ffsize cap = (m->len + by) * 100 / FFMAP_RESIZE_THRESHOLD + 1;
	if (cap > m->cap)
		return _ffmap_resize(m, cap);
	return 0;
}

/** Reallocate buffer and add new element with hash
Reallocate if free space is less than THRESHOLD
Grow by twice the existing capacity
Return 0 on success */
static inline int ffmap_add_hash(ffmap *m, ffuint hash, void *val)
{
	if (m->cap == 0) {
		if (0 != ffmap_alloc(m, 16))
			return -1;

	} else if ((m->len + 1) * 100 / m->cap > FFMAP_RESIZE_THRESHOLD) {
		if (0 != _ffmap_resize(m, m->cap * 2))
			return -1;
	}

	_ffmap_add(m, hash, val);
	return 0;
}

/** Reallocate buffer and add new element */
static inline int ffmap_add(ffmap *m, const void *key, ffsize keylen, void *val)
{
	ffuint hash = ffmap_hash(key, keylen);
	return ffmap_add_hash(m, hash, val);
}

/* Move elements (in the same chunk) that collide with the removed element:
(removed) #1    (occupied)#3
(occupied)#2    (occupied)#2
(coll#1)  #3 -> (free)
(free)          (free)
*/
static inline void _ffmap_normalize(ffmap *m, ffsize end, ffsize irm)
{
	ffsize i = end;
	ffsize begin = (end + 1) & m->mask;
	struct _ffmap_item *it;
	for (;;) {
		i = (i - 1) & m->mask;
		it = &m->data[i];
		if (i == irm)
			break; // none of the next elements can be moved

		ffsize ni = IDX_HASH(m, it->hash);
		if (((ni - begin) & m->mask) <= ((irm - begin) & m->mask)) {
			m->data[irm] = *it;
			irm = i; // repeat the same procedure for this element
			i = end;
		}
	}

	it->flags = 0;
	it->hash = 0;
	it->val = NULL;
	m->len--;
}

/** Remove the element matching 'hash' and 'val'
Normalize the next elements
Return 0 on success */
static inline int ffmap_rm_hash(ffmap *m, ffuint hash, void *val)
{
	if (m->len == 0)
		return -1;

	ffsize i = IDX_HASH(m, hash);
	ffsize sentl = i;
	ffssize irm = -1;
	struct _ffmap_item *it;

	for (;;) {
		it = &m->data[i];
		if (it->flags == 0)
			break;

		if (irm < 0
			&& it->hash == hash
			&& it->val == val)
			irm = i; // mark the element to remove and continue iterating until the end of the chunk

		i = (i + 1) & m->mask;
		if (i == sentl)
			return -1;
	}

	if (irm < 0)
		return -1;

	_ffmap_normalize(m, i, irm);
	return 0;
}

static inline int ffmap_rm(ffmap *m, const void *key, ffsize keylen, void *opaque)
{
	if (m->len == 0)
		return -1;

	ffuint hash = ffmap_hash(key, keylen);
	ffsize i = IDX_HASH(m, hash);
	ffsize sentl = i;
	ffssize irm = -1;
	struct _ffmap_item *it;

	for (;;) {
		it = &m->data[i];
		if (it->flags == 0)
			break;

		if (irm < 0
			&& it->hash == hash
			&& m->key_eq(opaque, key, keylen, it->val))
			irm = i;

		i = (i + 1) & m->mask;
		if (i == sentl)
			return -1;
	}

	if (irm < 0)
		return -1;

	_ffmap_normalize(m, i, irm);
	return 0;
}

/** Find element
Return user's value or NULL on error */
static inline void* ffmap_find_hash(const ffmap *m, ffuint hash, const void *key, ffsize keylen, void *opaque)
{
	if (m->len == 0)
		return NULL;

	ffsize i = IDX_HASH(m, hash);
	ffsize sentl = i;

	for (;;) {
		const struct _ffmap_item *it = &m->data[i];
		if (it->flags == 0)
			break;

		if (it->hash == hash
			&& m->key_eq(opaque, key, keylen, it->val))
			return it->val;

		i = (i + 1) & m->mask;
		if (i == sentl)
			break;
	}

	return NULL;
}

static inline void* ffmap_find(const ffmap *m, const void *key, ffsize keylen, void *opaque)
{
	ffuint hash = ffmap_hash(key, keylen);
	return ffmap_find_hash(m, hash, key, keylen, opaque);
}

/** Print statistics info */
static inline void _ffmap_stats(const ffmap *m, ffuint flags)
{
	ffsize nlen = 0, ncoll = 0;
	ffsize max_coll = 0, max_coll_n = 0;
	ffsize max_free = 0, max_free_n = 0;

	for (ffsize i = 0;  i != m->cap;  i++) {
		const struct _ffmap_item *it = &m->data[i];

		const char *status = "";
		if (it->flags == 0) {
			status = "free";
			max_free_n++;
			if (max_free < max_free_n)
				max_free = max_free_n;

		} else {
			max_free_n = 0;

			if (IDX_HASH(m, it->hash) != i) {
				ncoll++;
				max_coll_n++;
				if (max_coll < max_coll_n)
					max_coll = max_coll_n;
				status = "coll";
			} else {
				max_coll_n = 0;
			}

			nlen++;
		}

		if (flags & 1) {
			ff_printf("map %p:  %u: %s %p\n"
				, m, (int)i
				, status, it->val);
		}
	}

	ff_printf("map %p:  cap:%u  len:%u/%u(%u%%)  ncoll:%u(%u%%)  longest-coll:%u  longest-free:%u\n"
		, m
		, (int)m->cap
		, (int)nlen, (int)m->len, (int)((m->cap != 0) ? (m->len * 100 / m->cap) : 0)
		, (int)ncoll, (int)((m->len != 0) ? (ncoll * 100 / m->len) : 0)
		, (int)max_coll
		, (int)max_free
		);
}

#undef IDX_HASH
