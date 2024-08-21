/** ffbase: fixed-size lockless ring buffer, multi-producer, multi-consumer
2022, Simon Zolin */

/*
ffring_alloc ffring_free
ffring_reset
ffring_write ffring_writestr ffring_write_all ffring_write_all_str
ffring_write_begin ffring_write_all_begin
ffring_write_finish
ffring_read_begin ffring_read_all_begin
ffring_read_finish_status ffring_read_finish
ffring_read_discard
*/

#pragma once
#include <ffbase/lock.h>
#include <ffbase/string.h>

typedef struct ffring {
	union {
		struct {
			ffsize cap;
			ffsize mask;
			ffuint flags;
		};
		char align0[64];
	};
	union {
		struct {
			ffsize whead, wtail;
		};
		char align1[64]; // try to gain performance by placing read-write cursors on separate cache lines
	};
	union {
		struct {
			ffsize rhead, rtail;
		};
		char align2[64];
	};
	char data[0];
} ffring;

typedef struct ffring_head {
	ffsize old, nu;
} ffring_head;

enum FFRING_FLAGS {
	FFRING_1_READER = 1, // optimize for single reader
	FFRING_1_WRITER = 2, // optimize for single writer
};

/** Allocate buffer
cap: max size; automatically aligned to the power of 2
flags: enum FFRING_FLAGS
Return NULL on error */
static inline ffring* ffring_alloc(ffsize cap, ffuint flags)
{
	cap = ffint_align_power2(cap);
	ffring *b = (ffring*)ffmem_align(sizeof(ffring) + cap, 64);
	if (b == NULL)
		return NULL;

	b->flags = flags;
	b->whead = b->wtail = 0;
	b->rhead = b->rtail = 0;
	b->cap = cap;
	b->mask = cap - 1;
	return b;
}

static inline void ffring_free(ffring *b)
{
	ffmem_alignfree(b);
}

static inline void ffring_reset(ffring *b)
{
	b->whead = b->wtail = 0;
	b->rhead = b->rtail = 0;
}

/** Reserve contiguous free space region with the maximum size of 'n' bytes.
free: (output) amount of free space after the operation
Return value for ffring_write_finish() */
static inline ffring_head ffring_write_begin(ffring *b, ffsize n, ffstr *dst, ffsize *free)
{
	ffring_head wh;
	ffsize i, _free, nc;

	// Reserve space for new data
	for (;;) {
		wh.old = FFINT_READONCE(b->whead);
		ffcpu_fence_acquire(); // read 'whead' before 'rtail'
		_free = b->cap + FFINT_READONCE(b->rtail) - wh.old;
		if (ff_unlikely(_free == 0)) {
			// Not enough space
			dst->len = 0;
			nc = 0;
			wh.nu = wh.old; // allow ffring_write_finish()
			goto end;
		}

		i = wh.old & b->mask;
		nc = n;
		if (nc > _free)
			nc = _free;
		if (i + nc > b->cap)
			nc = b->cap - i;

		wh.nu = wh.old + nc;
		if (b->flags & FFRING_1_WRITER) {
			b->whead = wh.nu;
			break;
		}
		if (ff_likely(wh.old == ffint_cmpxchg(&b->whead, wh.old, wh.nu)))
			break;
		// Another writer has just reserved this space
	}

	dst->ptr = b->data + i;
	dst->len = nc;

end:
	if (free != NULL)
		*free = _free - nc;
	return wh;
}

/** Reserve free space for 'n' bytes.
free: (output) amount of free space after the operation
Return value for ffring_write_finish() */
static inline ffring_head ffring_write_all_begin(ffring *b, ffsize n, ffstr *d1, ffstr *d2, ffsize *free)
{
	ffring_head wh;
	ffsize i, _free;

	// Reserve space for new data
	for (;;) {
		wh.old = FFINT_READONCE(b->whead);
		ffcpu_fence_acquire(); // read 'whead' before 'rtail'
		_free = b->cap + FFINT_READONCE(b->rtail) - wh.old;
		if (ff_unlikely(n > _free)) {
			// Not enough space
			d1->len = d2->len = 0;
			n = 0;
			wh.nu = wh.old; // allow ffring_write_finish()
			goto end;
		}

		wh.nu = wh.old + n;
		if (b->flags & FFRING_1_WRITER) {
			b->whead = wh.nu;
			break;
		}
		if (ff_likely(wh.old == ffint_cmpxchg(&b->whead, wh.old, wh.nu)))
			break;
		// Another writer has just reserved this space
	}

	i = wh.old & b->mask;
	d1->ptr = b->data + i;
	d2->ptr = b->data;
	d1->len = n;
	d2->len = 0;
	if (i + n > b->cap) {
		d1->len = b->cap - i;
		d2->len = i + n - b->cap;
	}

end:
	if (free != NULL)
		*free = _free - n;
	return wh;
}

/** Commit reserved data.
used_previously: (Optional) N of bytes used before the operation
wh: return value from ffring_write*_begin() */
static inline void ffring_write_finish(ffring *b, ffring_head wh, ffsize *used_previously)
{
	// wait until previous writers finish their work
	if (!(b->flags & FFRING_1_WRITER))
		ffintz_wait_until_equal(&b->wtail, wh.old);

	ffcpu_fence_release(); // write data before 'wtail'
	FFINT_WRITEONCE(b->wtail, wh.nu);

	if (used_previously != NULL) {
		ffcpu_fence_acquire(); // write 'wtail' before reading 'rtail'
		*used_previously = wh.old - FFINT_READONCE(b->rtail);
	}
}

/** Write some data
Return N of bytes written */
static inline ffsize ffring_write(ffring *b, const void *src, ffsize n)
{
	ffstr d;
	ffring_head wh = ffring_write_begin(b, n, &d, NULL);
	if (d.len == 0)
		return 0;

	ffmem_copy(d.ptr, src, d.len);
	ffring_write_finish(b, wh, NULL);
	return d.len;
}

static inline ffsize ffring_writestr(ffring *b, ffstr data)
{
	return ffring_write(b, data.ptr, data.len);
}

/** Write whole data
Return N of bytes written */
static inline ffsize ffring_write_all(ffring *b, const void *src, ffsize n)
{
	ffstr d1, d2;
	ffring_head wh = ffring_write_all_begin(b, n, &d1, &d2, NULL);
	if (d1.len + d2.len == 0)
		return 0;

	ffmem_copy(d1.ptr, src, d1.len);
	if (d2.len != 0)
		ffmem_copy(d2.ptr, (char*)src + d1.len, d2.len);
	ffring_write_finish(b, wh, NULL);
	return n;
}

static inline ffsize ffring_write_all_str(ffring *b, ffstr data)
{
	return ffring_write_all(b, data.ptr, data.len);
}

/** Lock contiguous data region with the maximum size of 'n' bytes.
used: (output) amount of used space after the operation
Return value for ffring_read_finish() */
static inline ffring_head ffring_read_begin(ffring *b, ffsize n, ffstr *dst, ffsize *used)
{
	ffring_head rh;
	ffsize i, _used, nc;

	// Reserve data region
	for (;;) {
		rh.old = FFINT_READONCE(b->rhead);
		ffcpu_fence_acquire(); // read 'rhead' before 'wtail'
		_used = FFINT_READONCE(b->wtail) - rh.old;
		if (_used == 0) {
			// Not enough data
			dst->len = 0;
			nc = 0;
			rh.nu = rh.old; // allow ffring_read_finish()
			goto end;
		}

		i = rh.old & b->mask;
		nc = n;
		if (nc > _used)
			nc = _used;
		if (i + nc > b->cap)
			nc = b->cap - i;

		rh.nu = rh.old + nc;
		if (b->flags & FFRING_1_READER) {
			b->rhead = rh.nu;
			ffcpu_fence_acquire(); // read 'wtail' before data
			break;
		}
		if (ff_likely(rh.old == ffint_cmpxchg(&b->rhead, rh.old, rh.nu)))
			break;
		// Another reader has just reserved this data region
	}

	dst->ptr = b->data + i;
	dst->len = nc;

end:
	if (used != NULL)
		*used = _used - nc;
	return rh;
}

/** Lock data region of exactly 'n' bytes.
used: (output) amount of used space after the operation
Return value for ffring_read_finish() */
static inline ffring_head ffring_read_all_begin(ffring *b, ffsize n, ffstr *d1, ffstr *d2, ffsize *used)
{
	ffring_head rh;
	ffsize i, _used;

	// Reserve data region
	for (;;) {
		rh.old = FFINT_READONCE(b->rhead);
		ffcpu_fence_acquire(); // read 'rhead' before 'wtail'
		_used = FFINT_READONCE(b->wtail) - rh.old;
		if (n > _used) {
			// Not enough data
			d1->len = d2->len = 0;
			n = 0;
			rh.nu = rh.old; // allow ffring_read_finish()
			goto end;
		}

		rh.nu = rh.old + n;
		if (b->flags & FFRING_1_READER) {
			b->rhead = rh.nu;
			ffcpu_fence_acquire(); // read 'wtail' before data
			break;
		}
		if (ff_likely(rh.old == ffint_cmpxchg(&b->rhead, rh.old, rh.nu)))
			break;
		// Another reader has just reserved this data region
	}

	i = rh.old & b->mask;
	d1->ptr = b->data + i;
	d2->ptr = b->data;
	d1->len = n;
	d2->len = 0;
	if (i + n > b->cap) {
		d1->len = b->cap - i;
		d2->len = i + n - b->cap;
	}

end:
	if (used != NULL)
		*used = _used - n;
	return rh;
}

/** Discard the locked data region.
used_previously: (Optional) N of bytes used before the operation
rh: return value from ffring_read*_begin() */
static inline void ffring_read_finish_status(ffring *b, ffring_head rh, ffsize *used_previously)
{
	// wait until previous readers finish their work
	if (!(b->flags & FFRING_1_READER))
		ffintz_wait_until_equal(&b->rtail, rh.old);

	FFINT_WRITEONCE(b->rtail, rh.nu);

	if (used_previously != NULL) {
		ffcpu_fence_acquire(); // write 'rtail' before reading 'wtail'
		*used_previously = FFINT_READONCE(b->wtail) - rh.old;
	}
}

static inline void ffring_read_finish(ffring *b, ffring_head rh)
{
	ffring_read_finish_status(b, rh, NULL);
}

/** Read and discard all current data without breaking data integrity.
Return N of bytes read. */
static inline ffsize ffring_read_discard(ffring *b)
{
	ffsize used = b->cap;
	for (;;) {
		ffstr d1, d2;
		ffring_head rh = ffring_read_all_begin(b, used, &d1, &d2, &used);
		if (d1.len > 0) {
			ffring_read_finish(b, rh);
			return d1.len + d2.len;
		}
		if (used == 0)
			return 0;
	}
}
