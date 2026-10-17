/** ffbase: data stream
2022, Simon Zolin */

/*
ffstream_realloc ffstream_free
ffstream_reset
ffstream_used
ffstream_view
ffstream_gather
ffstream_consume
*/

#pragma once
#include <ffbase/string.h>

typedef struct ffstream ffstream;
struct ffstream {
	char *buf;
	const char *ptr;
	unsigned r, w;
	unsigned cap, mask;

#ifdef FF_DEBUG
	unsigned copied;
#endif
};

static inline int ffstream_realloc(ffstream *s, ffsize newcap)
{
	newcap = ffint_align_power2(newcap);
	if (newcap <= s->cap)
		return 0;

	char *p;
	if (!(p = (char*)ffmem_alloc(newcap)))
		return -1;

	if (!s->ptr) {
		unsigned used = s->w - s->r,  i = s->r & s->mask;
		ffmem_copy(p, s->buf + i, used);
		s->r = 0;
		s->w = used;
	}

	ffmem_free(s->buf);
	s->buf = p;
	s->cap = newcap;
	s->mask = newcap - 1;
	return 0;
}

static inline void ffstream_free(ffstream *s)
{
	ffmem_free(s->buf);
	s->buf = NULL;
}

static inline void ffstream_reset(ffstream *s)
{
	s->r = s->w = 0;
	s->ptr = NULL;
}

static inline unsigned ffstream_used(ffstream *s)
{
	return s->w - s->r;
}

static inline ffstr ffstream_view(ffstream *s)
{
	unsigned used = s->w - s->r,  i = s->r & s->mask;
	ffstr view = FFSTR_INITN(s->buf + i, used);
	if (s->ptr)
		view.ptr = (char*)s->ptr + s->r;
	return view;
}

/** Gather a contiguous region of at least 'need' bytes of data.
output: valid until the next call to this function
Return N of input bytes consumed */
static inline unsigned ffstream_gather(ffstream *s, ffstr input, ffsize need, ffstr *output)
{
	unsigned i, n = 0, used = s->w - s->r;

	if (need < input.len && used == 0) {
		// Set input view
		s->ptr = input.ptr;
		s->r = 0;
		s->w = input.len;
		*output = input;
		return input.len;
	}

	if (s->ptr) {
		if (need < used) {
			// Use existing input view
			output->ptr = (char*)s->ptr + s->r;
			output->len = used;
			return 0;
		}
	}

	FF_ASSERT(need <= s->cap);
	if (need > s->cap)
		return 0;

	if (s->ptr) {
		// Copy input data to the buffer
		ffmem_copy(s->buf, s->ptr + s->r, used);
		s->ptr = NULL;
		s->r = 0;
		s->w = used;
#ifdef FF_DEBUG
		s->copied += used;
#endif
	}

	if (used < need) {
		i = s->r & s->mask;
		if (i + need > s->cap) {
			// Not enough space in tail: move tail to front
			ffmem_move(s->buf, s->buf + i, used); // "...DD" -> "DD"
			s->r -= i;
			s->w -= i;
#ifdef FF_DEBUG
			s->copied += used;
#endif
		}

		// Append input data to tail
		i = s->w & s->mask;
		n = ffmin(need - used, input.len);
		ffmem_copy(s->buf + i, input.ptr, n);
		s->w += n;
		used = s->w - s->r;
#ifdef FF_DEBUG
		s->copied += n;
#endif
	}

	i = s->r & s->mask;
	ffstr_set(output, s->buf + i, used);
	return n;
}

static inline void ffstream_consume(ffstream *s, ffsize n)
{
	FF_ASSERT(s->w - s->r >= n);
	s->r += n;
}
