/** ffbase: fixed-size lockless ring queue, multi-producer, multi-consumer
2022, Simon Zolin */

/*
ffrq_alloc
ffrq_free
ffrq_add ffrq_add_sw
ffrq_fetch ffrq_fetch_sr
*/

#pragma once
#include <ffbase/lock.h>

typedef struct ffringqueue {
	union {
		struct {
			ffuint cap;
			ffuint mask;
		};
		char align0[64];
	};
	union {
		struct {
			ffuint whead, wtail;
		};
		char align1[64]; // try to gain performance by placing read-write cursors on separate cache lines
	};
	union {
		struct {
			ffuint rhead, rtail;
		};
		char align2[64];
	};
	void *data[0];
} ffringqueue;

/** Allocate buffer for a queue
cap: max N of elements; automatically aligned to the power of 2
Return NULL on error */
static inline ffringqueue* ffrq_alloc(ffsize cap)
{
	cap = ffint_align_power2(cap);
	ffringqueue *q = (ffringqueue*)ffmem_align(sizeof(ffringqueue) + cap * sizeof(void*), 64);
	if (q == NULL)
		return NULL;
	q->whead = q->wtail = 0;
	q->rhead = q->rtail = 0;
	q->cap = cap;
	q->mask = cap - 1;
	return q;
}

static inline void ffrq_free(ffringqueue *q)
{
	ffmem_alignfree(q);
}

static int _ffrq_add(ffringqueue *q, void *it, ffuint sw, ffuint *used)
{
	ffuint wh, nwh, unused, n = 1;

	// reserve space for new data
	for (;;) {
		wh = FFINT_READONCE(q->whead);
		ffcpu_fence_acquire(); // read 'whead' before 'rtail'
		unused = q->cap + FFINT_READONCE(q->rtail) - wh;
		if (ff_unlikely(n > unused))
			return -1;

		nwh = wh + n;
		if (sw) {
			q->whead = nwh;
			break;
		}
		if (ff_likely(wh == ffint_cmpxchg(&q->whead, wh, nwh)))
			break;
		// another writer has just added an element
	}

	ffuint i = wh & q->mask;
	q->data[i] = it;

	// wait until previous writers finish their work
	if (!sw)
		ffint_wait_until_equal(&q->wtail, wh);

	ffcpu_fence_release(); // write data before 'wtail'
	FFINT_WRITEONCE(q->wtail, nwh);
	if (used != NULL) {
		ffcpu_fence(); // write 'wtail' before reading 'rtail'
		*used = wh - FFINT_READONCE(q->rtail);
	}
	return 0;
}

static inline int _ffrq_fetch(ffringqueue *q, void **item, ffuint sr, ffuint *used_ptr)
{
	ffuint rh, nrh, used, n = 1;

	// reserve items
	for (;;) {
		rh = FFINT_READONCE(q->rhead);
		ffcpu_fence_acquire(); // read 'rhead' before 'wtail'
		used = FFINT_READONCE(q->wtail) - rh;
		if (n > used)
			return -1;

		nrh = rh + n;
		if (sr) {
			q->rhead = nrh;
			ffcpu_fence_acquire(); // read 'wtail' before data
			break;
		}
		if (ff_likely(rh == ffint_cmpxchg(&q->rhead, rh, nrh)))
			break;
		// another reader has just read this element
	}

	ffuint i = rh & q->mask;
	*item = q->data[i];

	// wait until previous readers finish their work
	if (!sr)
		ffint_wait_until_equal(&q->rtail, rh);

	ffcpu_fence_release(); // read data before writing 'rtail'
	FFINT_WRITEONCE(q->rtail, nrh);
	if (used_ptr != NULL) {
		ffcpu_fence(); // write 'rtail' before reading 'wtail'
		*used_ptr = FFINT_READONCE(q->wtail) - rh;
	}
	return 0;
}

/** Add an element
used: (optional) N of used elements before this operation
Return
  0: success
  <0: not enough free space */
static inline int ffrq_add(ffringqueue *q, void *it, ffuint *used)
{
	return _ffrq_add(q, it, 0, used);
}

/** Add an element (single writer) */
static inline int ffrq_add_sw(ffringqueue *q, void *it, ffuint *used)
{
	return _ffrq_add(q, it, 1, used);
}

/** Fetch and remove element
used: (optional) N of used elements before this operation
Return
  0: success
  <0: empty */
static inline int ffrq_fetch(ffringqueue *q, void **item, ffuint *used)
{
	return _ffrq_fetch(q, item, 0, used);
}

/** Fetch and remove element (single reader) */
static inline int ffrq_fetch_sr(ffringqueue *q, void **item, ffuint *used)
{
	return _ffrq_fetch(q, item, 1, used);
}
