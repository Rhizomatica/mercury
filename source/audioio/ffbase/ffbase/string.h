/** ffbase: string container
2020, Simon Zolin
*/

#pragma once
#define _FFBASE_STRING_H

#ifndef _FFBASE_BASE_H
#include <ffbase/base.h>
#endif
#include <ffbase/str-convert.h> // optional
#include <ffbase/slice.h> // optional


/** String container */
typedef struct ffstr {
	ffsize len; // Number of used bytes
	char *ptr; // Data pointer: can be dynamically allocated or static
} ffstr;


/* Contents:
GETTERS
SET
	FFSTR_INITZ FFSTR_INITN FFSTR_INITSTR
	FFSTR_Z
COMPARE
	ffstr_cmp ffstr_cmp2 ffstr_cmpz
	ffstr_eq ffstr_eq2 ffstr_eqz ffstr_eqcz
COMPARE (case insensitive)
	ffstr_icmp ffstr_icmp2
	ffstr_ieq ffstr_ieq2 ffstr_ieqz ffstr_ieqcz
FIND IN 'ffslice of ffstr'
	ffslicestr_find ffslicestr_findz
	ffslicestr_ifind ffslicestr_ifindz
MATCH
	ffstr_match ffstr_match2 ffstr_matchz ffstr_matchcz
	ffstr_imatch ffstr_imatch2 ffstr_imatchcz
REVERSE MATCH
	ffstr_irmatch ffstr_irmatch2 ffstr_irmatchcz
FIND CHARACTER
	ffstr_findchar ffstr_findany ffstr_findanyz
	ffstr_rfindchar ffstr_rfindany ffstr_rfindanyz
FIND SUBSTRING
	ffstr_find ffstr_find2 ffstr_findz
	ffstr_ifind ffstr_ifind2 ffstr_ifindstr ffstr_ifindz
	ffstr_rfind
SKIP
	ffstr_skipchar ffstr_skipany
	ffstr_rskipchar ffstr_rskipchar1 ffstr_rskipany
	ffstr_trimwhite
	ffstr_erase_left
SPLIT
	ffstr_split ffstr_splitby ffstr_rsplitby ffstr_splitbyany
CONVERT
	ffstr_toint ffstr_to_uint64 ffstr_to_uint32 ffstr_to_int64 ffstr_to_int32
	ffstr_fromint ffstr_from_uint ffstr_from_int
	ffstr_from_float
	ffstr_lower
ALLOCATE
	ffstr_alloc_stack
	ffstr_alloc ffstr_realloc ffstr_grow ffstr_growhalf ffstr_growtwice
	ffstr_free
COPY
	ffstr_copy ffstr_copystr
	ffstr_push
	ffstr_add ffstr_add2 ffstr_addcz ffstr_addz ffstr_addfill
	ffstr_addfmt ffstr_addfmtv
ALLOCATE+COPY
	ffstr_dup ffstr_dupstr ffstr_dupz
	ffstr_dup_lower ffstr_dupstr_lower
	ffstr_growaddfill ffstr_growadd ffstr_growadd2 ffstr_growaddz
	ffstr_growfmt
	ffstr_growadd_replace

ALGORITHMS
	_ffs_copy _ffs_copyz
	ffs_cmpz ffs_icmp ffs_icmpz ffs_matchz
	ffs_split
	ffs_findstr ffs_ifindstr ffs_findchar ffs_findany
	ffs_rfindchar ffws_rfindchar ffs_rfindstr ffs_rfindany
	ffs_skipchar ffs_skipany ffs_skip_ranges
	ffs_rskipany
	ffs_lower ffs_upper ffs_titlecase
	ffs_wildcard
*/

// ALGORITHMS

static inline ffsize _ffs_addchar(char *dst, ffsize cap, char c)
{
	if (cap == 0)
		return 0;
	*dst = c;
	return 1;
}

static inline ffsize _ffs_copy(char *dst, ffsize cap, const char *s, ffsize n)
{
	n = ffmin(n, cap);
	ffmem_copy(dst, s, n);
	return n;
}

#define _ffs_copycz(dst, cap, sz) \
	_ffs_copy(dst, cap, sz, ffsz_len(sz))

static inline ffsize _ffs_copyz(char *dst, ffsize cap, const char *sz)
{
	for (ffsize i = 0;  i != cap;  i++) {
		if (sz[i] == '\0')
			return i;
		dst[i] = sz[i];
	}
	return cap;
}

#include <ffbase/str-format.h> // optional

/** Compare buffer and NULL-terminated string: {s, len} <> sz
Return 0 if equal
 <0: s < sz;
 >0: s > sz */
static inline int ffs_cmpz(const char *s, ffsize len, const char *sz)
{
	for (ffsize i = 0;  i != len;  i++) {
		ffuint cl = (ffbyte)s[i];
		ffuint cr = (ffbyte)sz[i];

		if (cr == '\0')
			return 1; // s > sz

		if (cl != cr) {
			if (cl < cr)
				return -1; // s < sz
			return 1; // s > sz
		}
	}

	if (sz[len] != '\0')
		return -1; // s < sz
	return 0;
}

/** Compare (case-insensitive): {sl, n} <> {sr, n}
Return 0 if equal
 <0: sl < sr;
 >0: sl > sr */
static inline int ffs_icmp(const char *sl, const char *sr, ffsize n)
{
	for (ffsize i = 0;  i != n;  i++) {
		ffuint cl = (ffbyte)sl[i];
		ffuint cr = (ffbyte)sr[i];

		if (cl != cr) {
			if (cl >= 'A' && cl <= 'Z')
				cl |= 0x20;
			if (cr >= 'A' && cr <= 'Z')
				cr |= 0x20;
			if (cl != cr)
				return (cl < cr) ? -1 : 1;
		}
	}
	return 0; // sl == sr
}

/** Compare (case-insensitive): {s, len} <> sz
Return 0 if equal
 <0: s < sz;
 >0: s > sz */
static inline int ffs_icmpz(const char *s, ffsize len, const char *sz)
{
	for (ffsize i = 0;  i != len;  i++) {
		ffuint cl = (ffbyte)s[i];
		ffuint cr = (ffbyte)sz[i];

		if (cr == '\0')
			return 1; // s > sz

		if (cl != cr) {
			if (cl >= 'A' && cl <= 'Z')
				cl |= 0x20;
			if (cr >= 'A' && cr <= 'Z')
				cr |= 0x20;
			if (cl != cr) {
				if (cl < cr)
					return -1; // s < sz
				return 1; // s > sz
			}
		}
	}

	if (sz[len] != '\0')
		return -1; // s < sz
	return 0; // s == sz
}

/** Compare strings.
Return 0 if equal;
  or position +1 at which the strings differ. */
static inline ffssize ffs_cmpz_n(const char *s, ffsize n, const char *sz)
{
	for (ffsize i = 0;  i != n;  i++) {
		if (sz[i] == '\0' || s[i] != sz[i])
			return ((ffbyte)s[i] < (ffbyte)sz[i]) ? -(ffssize)(i+1) : (ffssize)(i+1);
	}
	return (sz[n] != '\0') ? -(ffssize)(n+1) : 0;
}

/** Return TRUE if 's' starts with 'prefix' */
static inline int ffs_match(const char *s, ffsize len, const char *prefix, ffsize prefix_len)
{
	return len >= prefix_len
		&& 0 == ffmem_cmp(s, prefix, prefix_len);
}

/** Return TRUE if 's' starts with 'prefix' (case-insensitive) */
static inline int ffs_imatch(const char *s, ffsize len, const char *prefix, ffsize prefix_len)
{
	return len >= prefix_len
		&& 0 == ffs_icmp(s, prefix, prefix_len);
}

/** Return TRUE if 's' starts with 'sz' */
static inline int ffs_matchz(const char *s, ffsize n, const char *sz)
{
	for (ffsize i = 0;  i != n;  i++) {
		int cr = sz[i];

		if (cr == '\0')
			return 1; // s begins with sz

		if (s[i] != cr)
			return 0; // s != sz
	}
	if (sz[n] != '\0')
		return 0; // s < sz
	return 1; // s == sz
}

/** Return TRUE if 's' starts with 'sz' (case-insensitive) */
static inline int ffs_imatchz(const char *s, size_t len, const char *sz)
{
	ffsize n = ffsz_len(sz);
	return len >= n
		&& 0 == ffs_icmp(s, sz, n);
}

/** Find substring
Return -1 if not found */
static inline ffssize ffs_findstr(const char *s, ffsize len, const char *search, ffsize search_len)
{
	if (len < search_len)
		return -1; // too long search string

	if (search_len == 0)
		return (len != 0) ? 0 : -1;

	const char *p = s, *end = s + len;
	while (p != end) {
		p = (char*)ffmem_findbyte(p, end - p, search[0]);
		if (p == NULL || search_len > (ffsize)(end - p))
			break;
		if (0 == ffmem_cmp(p, search, search_len))
			return p - s;
		p++;
	}

	return -1;
}

static ffssize ffs_findany(const char *s, ffsize len, const char *anyof, ffsize anyof_len);

/** Find substring (case-insensitive)
Return -1 if not found */
static inline ffssize ffs_ifindstr(const char *s, ffsize len, const char *search, ffsize search_len)
{
	if (len < search_len)
		return -1; // too long search string

	if (search_len == 0)
		return (len != 0) ? 0 : -1;

	// prepare an array containing upper- and lower-case first character of the search string
	char chars[2] = { *search, *search };
	int c0 = *search;
	if (c0 >= 'A' && c0 <= 'Z') {
		chars[1] = c0 | 0x20;
	} else if (c0 >= 'a' && c0 <= 'z') {
		chars[0] = c0 & ~0x20;
	}

	const char *p = s, *end = s + len;
	while (p != end) {
		ffssize i = ffs_findany(p, end - p, chars, 2);
		p += i;
		if (i < 0 || search_len > (ffsize)(end - p))
			break;

		if (0 == ffs_icmp(p, search, search_len))
			return p - s;
		p++;
	}

	return -1;
}

/** Find character
Return -1 if not found */
static inline ffssize ffs_findchar(const char *s, ffsize len, int search)
{
	const char *pos = (char*)ffmem_findbyte(s, len, search);
	if (pos == NULL)
		return -1;
	return pos - s;
}

/** Find character from the end
Return the position from the beginning
  -1: not found */
static inline ffssize ffs_rfindchar(const char *s, ffsize len, int search)
{
	for (ffssize i = len - 1;  i >= 0;  i--) {
		if (search == s[i])
			return i;
	}
	return -1;
}

/** Find character from the end
Return the position from the beginning
  -1: not found */
static inline ffssize ffws_rfindchar(const wchar_t *s, ffsize len, wchar_t search)
{
	for (ffssize i = len - 1;  i >= 0;  i--) {
		if (search == s[i])
			return i;
	}
	return -1;
}

#ifdef __SSE4_2__
#include <ffbase/cpuid.h>
#include <nmmintrin.h>
#include <smmintrin.h>

static void ffmem_copy_static(void *dst, const void *src, ffsize n)
{
	switch (n) {
	case 2:
		*(ffushort*)dst = *(ffushort*)src; break;
	case 4:
		*(ffuint*)dst = *(ffuint*)src; break;
	case 6:
		*(ffuint*)dst = *(ffuint*)src;
		*(ffushort*)(((char*)dst)+4) = *(ffushort*)((char*)src+4);
		break;
	case 8:
		*(ffuint64*)dst = *(ffuint64*)src; break;
	case 10:
		*(ffuint64*)dst = *(ffuint64*)src;
		*(ffushort*)(((char*)dst)+8) = *(ffushort*)((char*)src+8);
		break;
	case 16:
		*(ffuint64*)dst = *(ffuint64*)src;
		*(ffuint64*)(((char*)dst)+8) = *(ffuint64*)((char*)src+8);
		break;
	}
}

/* Similar to SSE-optimized strpbrk() except it doesn't stop at NUL character */
static void* _ffmem_findany_sse42(const void *d, ffsize n, const char *anyof, ffuint anyof_len)
{
	__m128i mask;
	ffmem_copy_static(&mask, anyof, anyof_len);

	const __m128i *p = (__m128i*)d, *end = (__m128i*)((char*)d+n);
	while (p+1 <= end) {
		__m128i r = _mm_loadu_si128(p);
		ffuint i = _mm_cmpestri(mask, anyof_len, r, 16, _SIDD_CMP_EQUAL_ANY);
		if (i != 16)
			return (char*)p + i;

		p++;
	}

	// check trailer
	if (p != end) {
		n = (char*)end - (char*)p;
		__m128i r;
#ifdef FFBASE_MEM_ASAN
		if (1)
#else
		if (((ffsize)(p+1) & ~0x0f) == ((ffsize)(p+1) & ~0x0fff)) // avoid the possibility of crashing if the 16byte region crosses 4k boundary
#endif
			ffmem_copy(&r, p, n);
		else
			r = _mm_loadu_si128(p);
		ffuint i = _mm_cmpestri(mask, anyof_len, r, n, _SIDD_CMP_EQUAL_ANY);
		if (i != 16)
			return (char*)p + i;
	}

	return NULL;
}

static int _ffs_skip_ranges_sse42(const void *d, ffsize n, const char *ranges, ffuint ranges_len)
{
	__m128i mask;
	ffmem_copy_static(&mask, ranges, ranges_len);

	const __m128i *p = (__m128i*)d, *end = (__m128i*)((char*)d+n);
	while (p+1 <= end) {
		__m128i r = _mm_loadu_si128(p);
		ffuint i = _mm_cmpestri(mask, ranges_len, r, 16, _SIDD_CMP_RANGES | _SIDD_NEGATIVE_POLARITY);
		if (i != 16)
			return (char*)p+i - (char*)d;
		p++;
	}

	// check trailer
	if (p != end) {
		n = (char*)end - (char*)p;
		__m128i r;
#ifdef FFBASE_MEM_ASAN
		if (1)
#else
		if (((ffsize)(p+1) & ~0x0f) == ((ffsize)(p+1) & ~0x0fff)) // avoid the possibility of crashing if the 16byte region crosses 4k boundary
#endif
			ffmem_copy(&r, p, n);
		else
			r = _mm_loadu_si128(p);
		ffuint i = _mm_cmpestri(mask, ranges_len, r, n, _SIDD_CMP_RANGES | _SIDD_NEGATIVE_POLARITY);
		if (i != n)
			return (char*)p+i - (char*)d;
	}

	return -1;
}

#endif // __SSE4_2__

/** Find the position of any byte from 'anyof' in 's'
Return -1 if not found */
static inline ffssize ffs_findany(const char *s, ffsize len, const char *anyof, ffsize anyof_len)
{
#ifdef __SSE4_2__
	if (_ffcpu_features == 0) {
		struct ffcpuid i = {};
		ffcpuid(&i, FFCPUID_FEATURES);
		_ffcpu_features = i.features[0];
	}

	if ((anyof_len == 2 || anyof_len == 4 || anyof_len == 6 || anyof_len == 8) && (_ffcpu_features & (1<<20))) {
		void *r = _ffmem_findany_sse42(s, len, anyof, anyof_len);
		return (r != NULL) ? (char*)r - (char*)s : -1;
	}
#endif

	for (ffsize i = 0;  i != len;  i++) {
		for (ffsize j = 0;  j != anyof_len;  j++) {
			if (s[i] == anyof[j])
				return i;
		}
	}
	return -1;
}

/** Find (from the end) the position of any byte from 'anyof' in 's'
Return the position from the beginning
  -1: not found */
static inline ffssize ffs_rfindany(const char *s, ffsize len, const char *anyof, ffsize anyof_len)
{
	for (ffssize i = len - 1;  i >= 0;  i--) {
		for (ffsize j = 0;  j != anyof_len;  j++) {
			if (s[i] == anyof[j])
				return i;
		}
	}
	return -1;
}

/** Find substring from the end
Return the position from the beginning
  -1: not found */
static inline ffssize ffs_rfindstr(const char *s, ffsize len, const char *search, ffsize search_len)
{
	if (len < search_len)
		return -1; // too long search string

	if (search_len == 0)
		return (len != 0) ? 0 : -1;

	search_len--;
	int clast = search[search_len];

	for (ffsize i = len - 1;  i >= search_len;  i--) {
		if (s[i] == clast
			&& 0 == ffmem_cmp(&s[i - search_len], search, search_len))
			return i - search_len;
	}
	return -1;
}

/** Skip bytes from the beginning */
static inline ffsize ffs_skipchar(const char *s, ffsize len, int skip_char)
{
	ffsize i;
	for (i = 0;  i != len;  i++) {
		if (s[i] != skip_char)
			break;
	}
	return i;
}

/** Remove any of matching bytes from the beginning.
Return N of bytes skipped */
static inline ffsize ffs_skipany(const char *s, ffsize len, const char *skip_chars, ffsize skip_chars_len)
{
	for (ffsize i = 0;  i != len;  i++) {
		for (ffsize j = 0;  ;  j++) {
			if (j == skip_chars_len)
				return i;
			if (s[i] == skip_chars[j])
				break;
		}
	}
	return len;
}

/** Remove any of matching bytes from the end.
Return N of bytes skipped */
static inline ffsize ffs_rskipany(const char *s, ffsize len, const char *skip_chars, ffsize skip_chars_len)
{
	for (ffssize i = len - 1;  i >= 0;  i--) {
		for (ffsize j = 0;  ;  j++) {
			if (j == skip_chars_len)
				return len - (i + 1);
			if (s[i] == skip_chars[j])
				break;
		}
	}
	return len;
}

/** Skip bytes while they are within the specified range
Return offset of the first out-of-range byte
 <0: all bytes were skipped */
static inline int ffs_skip_ranges(const char *s, ffsize len, const char *ranges, ffsize ranges_len)
{
	FF_ASSERT(ranges_len%2 == 0);

#ifdef __SSE4_2__
	if (_ffcpu_features == 0) {
		struct ffcpuid i = {};
		ffcpuid(&i, FFCPUID_FEATURES);
		_ffcpu_features = i.features[0];
	}

	if ((ranges_len == 2 || ranges_len == 4 || ranges_len == 6 || ranges_len == 8 || ranges_len == 10 || ranges_len == 16)
		&& (_ffcpu_features & (1<<20))) {
		return _ffs_skip_ranges_sse42(s, len, ranges, ranges_len);
	}
#endif

	for (ffsize i = 0;  i != len;  i++) {
		for (ffsize j = 0;  ;  j += 2) {
			if (j == ranges_len)
				return i;
			if (ranges[j] <= s[i] && s[i] <= ranges[j+1])
				break;
		}
	}
	return -1;
}

/** Convert case (ANSI).
Return the number of bytes written */
static inline ffsize ffs_lower(char *dst, ffsize cap, const char *src, ffsize len)
{
	int inplace = (dst == src);
	len = ffmin(len, cap);

	for (ffsize i = 0;  i < len;  i++) {
		int ch = src[i];
		if (ch >= 'A' && ch <= 'Z')
			dst[i] = ch | 0x20;
		else if (!inplace)
			dst[i] = ch;
	}

	return len;
}

/** Convert case (ANSI)
Return the number of bytes written */
static inline ffsize ffs_upper(char *dst, ffsize cap, const char *src, ffsize len)
{
	int inplace = (dst == src);
	len = ffmin(len, cap);

	for (ffsize i = 0;  i < len;  i++) {
		int ch = src[i];
		if (ch >= 'a' && ch <= 'z')
			dst[i] = ch & ~0x20;
		else if (!inplace)
			dst[i] = ch;
	}

	return len;
}

/** Convert case (ANSI)
Return the number of bytes written */
static inline ffsize ffs_titlecase(char *dst, ffsize cap, const char *src, ffsize len)
{
	int inplace = (dst == src), capital = 1;
	len = ffmin(len, cap);

	for (ffsize i = 0;  i < len;  i++) {
		int ch = src[i];

		if (capital) {
			if (ch != ' ') {
				capital = 0;

				if (ch >= 'a' && ch <= 'z') {
					dst[i] = ch & ~0x20;
					continue;
				}
			}

		} else {
			if (ch == ' ')
				capital = 1;

			else if (ch >= 'A' && ch <= 'Z') {
				dst[i] = ch | 0x20;
				continue;
			}
		}

		if (!inplace)
			dst[i] = ch;
	}

	return len;
}

enum FFS_WILDCARD {
	FFS_WC_ICASE = 1
};

/** Match string by a wildcard pattern
'*': >=0 any characters match
'?': 1 any character matches
flags: enum FFS_WILDCARD
Return 0 if match */
static inline int ffs_wildcard(const char *pattern, ffsize pattern_len, const char *s, ffsize len, ffuint flags)
{
	ffsize i, is = 0, astk = 0, astk_is = -1;
	int c, cs;

	for (i = 0;  i != pattern_len;  ) {

		c = pattern[i];

		if (c == '*') {
			if (++i == pattern_len)
				return 0; // the rest of input string matches

			astk = i;
			astk_is = is;
			continue;
		}

		if (is == len)
			return 1; // too short input string

		if (c == '?') {
			i++;
			is++;
			continue;
		}

		cs = s[is];
		if (flags & FFS_WC_ICASE) {
			if (c >= 'A' && c <= 'Z')
				c |= 0x20;
			if (cs >= 'A' && cs <= 'Z')
				cs |= 0x20;
		}

		if (c == cs) {
			i++;
			is++;

		} else {
			if (astk == 0)
				return 1; // no match

			i = astk;
			is = ++astk_is;
		}
	}

	return (is == len) ? 0 : 1;
}

static inline ffsize _ffsz_nlen(const char *s, ffsize maxlen)
{
	const char *pos = (char*)ffmem_findbyte(s, maxlen, '\0');
	if (pos == NULL)
		return maxlen;
	return pos - s;
}


// GETTERS

/** Get the pointer to the last element: &a[len - 1] */
#define ffstr_last(s) \
	(&((char*)(s)->ptr)[(s)->len - 1])

/** Get array's tail: &a[len] */
#define ffstr_end(s) \
	((char*)(s)->ptr + (s)->len)


// SET
// Note: don't use these functions for the string with allocated buffer.

/** ffstr s = FFSTR_INITZ("string") */
#define FFSTR_INITZ(sz) \
	{ ffsz_len(sz), (char*)(sz) }
#define FFSTR_INIT(s) \
	{ ffsz_len(s), (char*)(s) }

/** ffstr s = FFSTR_INITN(ptr, n) */
#define FFSTR_INITN(s, n) \
	{ n, (char*)(s) }

/** ffstr s = FFSTR_INITSTR(ffstr) */
#define FFSTR_INITSTR(str) \
	{ (str)->len, (char*)(str)->ptr }

/**
void a(ffstr s) { ... }
void b()
{
	a(FFSTR_Z("string"));
}
*/
#define FFSTR_Z(sz) \
	({ ffstr __s = { ffsz_len(sz), (char*)(sz) }; __s; })

/** Set data pointer and length: s = {data, length} */
#define ffstr_set(s, data, n) \
do { \
	(s)->ptr = (char*)data; \
	(s)->len = n; \
} while(0)

/** Set string: s = ffstr */
#define ffstr_set2(s, src)  ffstr_set(s, (src)->ptr, (src)->len)
#define ffstr_setstr(s, src)  ffstr_set(s, (src)->ptr, (src)->len)

/** Set NULL-terminated string: s = sz */
#define ffstr_setz(s, sz)  ffstr_set(s, sz, ffsz_len(sz))

/** Set constant NULL-terminated string: s = "string" */
#define ffstr_setcz(s, csz)  ffstr_set(s, csz, FFS_LEN(csz))

/** Set NULL-terminated string, but limit the length: s = sz */
#define ffstr_setnz(s, sz, maxlen)  ffstr_set(s, sz, _ffsz_nlen(sz, maxlen))

/** Set ffstr from ffiovec: s = iovec */
#define ffstr_setiovec(s, iov)  ffstr_set(s, (iov)->iov_base, (iov)->iov_len)

/** Set empty array: s = {} */
#define ffstr_null(s)  ffstr_set(s, NULL, 0)

/** Shift data pointer */
static inline void ffstr_shift(ffstr *s, ffsize by)
{
	FF_ASSERT(by <= s->len);
	s->ptr += by;
	s->len -= by;
}


static inline int ffstr_popfront(ffstr *s)
{
	FF_ASSERT(s->len != 0);
	s->len--;
	return *(s->ptr++);
}


// COMPARE

/** Compare: ffstr <> {data, n}
Return 0 if equal;
 <0: s < data;
 >0: s > data */
static inline int ffstr_cmp(const ffstr *s, const char *data, ffsize n)
{
	int r = ffmem_cmp(s->ptr, data, ffmin(s->len, n));
	if (r == 0)
		return s->len - n;
	return r;
}

/** Compare 2 ffstr objects: ffstr <> ffstr
Return 0 if equal;
 <0: s < data;
 >0: s > data */
static inline int ffstr_cmp2(const ffstr *s, const ffstr *s2)
{
	int r = ffmem_cmp(s->ptr, s2->ptr, ffmin(s->len, s2->len));
	if (r == 0)
		return s->len - s2->len;
	return r;
}

/** Compare ffstr and C string: ffstr <> sz
Return 0 if equal;
 <0: s < data;
 >0: s > data */
static inline int ffstr_cmpz(const ffstr *s, const char *sz)
{
	return ffs_cmpz(s->ptr, s->len, sz);
}

/** Is equal: ffstr == {data, length} */
static inline int ffstr_eq(const void *ss, const void *data, ffsize n)
{
	const ffstr *s = (ffstr*)ss;
	return s->len == n
		&& 0 == ffmem_cmp(s->ptr, data, n);
}

/** Is equal: ffstr == ffstr */
static inline int ffstr_eq2(const void *ss, const void *ss2)
{
	const ffstr *s = (ffstr*)ss;
	const ffstr *s2 = (ffstr*)ss2;
	return s->len == s2->len
		&& 0 == ffmem_cmp(s->ptr, s2->ptr, s2->len);
}

/** Is equal: ffstr == sz */
static inline int ffstr_eqz(const void *ss, const char *sz)
{
	const ffstr *s = (ffstr*)ss;
	return 0 == ffs_cmpz(s->ptr, s->len, sz);
}

/** Is equal: ffstr == "string" */
#define ffstr_eqcz(s, csz)  ffstr_eq(s, csz, FFS_LEN(csz))


// COMPARE (case insensitive)

/** Compare ANSI strings (case-insensitive)
Return 0 if equal;
 <0: s < data;
 >0: s > data */
static inline int ffstr_icmp(const ffstr *s, const char *cmp, ffsize n)
{
	int r = ffs_icmp(s->ptr, cmp, ffmin(s->len, n));
	if (r == 0 && s->len != n)
		return (s->len < n) ? -1 : 1;
	return r;
}

static inline int ffstr_icmp2(const ffstr *s, const ffstr *cmp)
{
	return ffstr_icmp(s, cmp->ptr, cmp->len);
}

static inline int ffstr_icmpz(const ffstr *s, const char *sz)
{
	return ffs_icmpz(s->ptr, s->len, sz);
}

/** Is equal: ffstr == {data, length} */
static inline int ffstr_ieq(const ffstr *s, const char *cmp, ffsize n)
{
	return (s->len == n
		&& 0 == ffs_icmp(s->ptr, cmp, n));
}

/** Is equal (case-insensitive): ffstr == ffstr */
#define ffstr_ieq2(s, cmp)  ffstr_ieq(s, (cmp)->ptr, (cmp)->len)

/** Is equal (case-insensitive): ffstr == sz */
#define ffstr_ieqz(s, sz)  (0 == ffs_icmpz((s)->ptr, (s)->len, sz))

/** Is equal (case-insensitive): ffstr == "string" */
#define ffstr_ieqcz(s, csz)  ffstr_ieq(s, csz, FFS_LEN(csz))


// FIND IN 'ffslice of ffstr'

#ifdef _FFBASE_SLICE_H
static inline ffssize ffslicestr_find(const ffslice *a, const ffstr *search)
{
	return ffslice_findT(a, search, (ffslice_cmp_func)ffstr_cmp2, ffstr);
}

static inline ffssize ffslicestr_findz(const ffslice *a, const char *search)
{
	return ffslice_findT(a, search, (ffslice_cmp_func)ffstr_cmpz, ffstr);
}

static inline ffssize ffslicestr_ifind(const ffslice *a, const ffstr *search)
{
	return ffslice_findT(a, search, (ffslice_cmp_func)ffstr_icmp2, ffstr);
}

static inline ffssize ffslicestr_ifindz(const ffslice *a, const char *search)
{
	return ffslice_findT(a, search, (ffslice_cmp_func)ffstr_icmpz, ffstr);
}
#endif


// MATCH

/** Return TRUE if n characters are equal in both strings */
static inline int ffstr_match(const ffstr *s, const char *prefix, ffsize n)
{
	return s->len >= n
		&& 0 == ffmem_cmp(s->ptr, prefix, n);
}

#define ffstr_match2(s, prefix)  ffstr_match(s, (prefix)->ptr, (prefix)->len)

#define ffstr_matchz(s, sz)  ffs_matchz((s)->ptr, (s)->len, sz)

#define ffstr_matchcz(s, csz)  ffstr_match(s, csz, ffsz_len(csz))

static inline int ffstr_imatch(const ffstr *s, const char *prefix, ffsize n)
{
	return s->len >= n
		&& 0 == ffs_icmp(s->ptr, prefix, n);
}

#define ffstr_imatch2(s, prefix)  ffstr_imatch(s, (prefix)->ptr, (prefix)->len)
#define ffstr_imatchz(s, sz)  ffs_imatchz((s)->ptr, (s)->len, sz)
#define ffstr_imatchcz(s, csz)  ffstr_imatch(s, csz, ffsz_len(csz))


// REVERSE MATCH - match ending bytes of both strings

static inline int ffstr_irmatch(const ffstr *s, const char *suffix, ffsize n)
{
	return s->len >= n
		&& 0 == ffs_icmp(s->ptr + s->len - n, suffix, n);
}

#define ffstr_irmatch2(s, suffix)  ffstr_irmatch(s, (suffix)->ptr, (suffix)->len)
#define ffstr_irmatchcz(s, csz)  ffstr_irmatch(s, csz, ffsz_len(csz))


// FIND CHARACTER

/** Find character
Return -1 if not found */
static inline ffssize ffstr_findchar(const ffstr *s, int search)
{
	return ffs_findchar(s->ptr, s->len, search);
}

/** Find character from the end
Return the position from the beginning
  -1: not found */
static inline ffssize ffstr_rfindchar(const ffstr *s, int search)
{
	return ffs_rfindchar(s->ptr, s->len, search);
}

/** Find the position of any byte from 'anyof' in 's'
Return -1 if not found */
static inline ffssize ffstr_findany(const ffstr *s, const char *anyof, ffsize n)
{
	return ffs_findany(s->ptr, s->len, anyof, n);
}

/** Find (from the end) the position of any byte from 'anyof' in 's'
Return the position from the beginning
  -1: not found */
static inline ffssize ffstr_rfindany(const ffstr *s, const char *anyof, ffsize n)
{
	return ffs_rfindany(s->ptr, s->len, anyof, n);
}

#define ffstr_findanyz(s, anyof_sz)  ffstr_findany(s, anyof_sz, ffsz_len(anyof_sz))
#define ffstr_rfindanyz(s, anyof_sz)  ffstr_rfindany(s, anyof_sz, ffsz_len(anyof_sz))


// FIND SUBSTRING

/** Find substring
Return -1 if not found */
#define ffstr_find(s, search, search_len)  ffs_findstr((s)->ptr, (s)->len, search, search_len)
#define ffstr_find2(s, search)  ffs_findstr((s)->ptr, (s)->len, (search)->ptr, (search)->len)
#define ffstr_findstr(s, search)  ffs_findstr((s)->ptr, (s)->len, (search)->ptr, (search)->len)
#define ffstr_findz(s, sz)  ffs_findstr((s)->ptr, (s)->len, sz, ffsz_len(sz))

/** Find substring (case-insensitive)
Return -1 if not found */
#define ffstr_ifind(s, search, search_len)  ffs_ifindstr((s)->ptr, (s)->len, search, search_len)
#define ffstr_ifind2(s, search)  ffs_ifindstr((s)->ptr, (s)->len, (search)->ptr, (search)->len)
#define ffstr_ifindstr(s, search)  ffs_ifindstr((s)->ptr, (s)->len, (search)->ptr, (search)->len)
#define ffstr_ifindz(s, sz)  ffs_ifindstr((s)->ptr, (s)->len, sz, ffsz_len(sz))

/** Find substring from the end
Return the position from the beginning
  -1: not found */
#define ffstr_rfind(s, search, search_len)  ffs_rfindstr((s)->ptr, (s)->len, search, search_len)


// SKIP
// Note: don't use these functions for the string with allocated buffer.

/** Remove bytes from the beginning */
static inline ffsize ffstr_skipchar(ffstr *s, int skip_char)
{
	ffsize i = ffs_skipchar(s->ptr, s->len, skip_char);
	s->ptr += i;
	s->len -= i;
	return i;
}

/** Remove any of matching bytes from the beginning */
static inline void ffstr_skipany(ffstr *s, const ffstr *skip_chars)
{
	ffsize i = ffs_skipany(s->ptr, s->len, skip_chars->ptr, skip_chars->len);
	s->ptr += i;
	s->len -= i;
}

/** Remove bytes from the end */
static inline void ffstr_rskipchar(ffstr *s, int skip_char)
{
	ffssize i;
	for (i = s->len - 1;  i >= 0;  i--) {
		if (s->ptr[i] != skip_char)
			break;
	}
	s->len = i + 1;
}

/** Remove 1 byte from the end */
static inline void ffstr_rskipchar1(ffstr *s, int skip_char)
{
	if (s->len != 0 && s->ptr[s->len - 1] == skip_char)
		s->len--;
}

/** Remove any of matching bytes from the end */
static inline void ffstr_rskipany(ffstr *s, const ffstr *skip_chars)
{
	ffsize i = ffs_rskipany(s->ptr, s->len, skip_chars->ptr, skip_chars->len);
	s->len -= i;
}

/** Remove whitespace from beginning and end */
static inline void ffstr_trimwhite(ffstr *s)
{
	ffstr ws = FFSTR_INIT(" \t\r\n");
	ffstr_skipany(s, &ws);
	ffstr_rskipany(s, &ws);
}

/** Delete data at the beginning.  Move remaining data to the left. */
static inline void ffstr_erase_left(ffstr *s, ffsize n)
{
	FF_ASSERT(n <= s->len);
	s->len -= n;
	ffmem_move(s->ptr, &s->ptr[n], s->len);
}


// SPLIT

static inline ffssize ffs_split(const char *s, ffsize len, ffssize index, ffstr *first, ffstr *second)
{
	if (index == -1) {
		if (first != NULL)
			ffstr_set(first, s, len);
		if (second != NULL)
			ffstr_null(second);
		return -1;
	}

	FF_ASSERT((ffsize)index < len);
	if (first != NULL)
		ffstr_set(first, s, index);
	if (second != NULL)
		ffstr_set(second, s + index + 1, len - (index + 1));
	return index;
}

/** Split string into 2 pieces, excluding the separator
index: if -1, the second string will be empty
first: optional
second: optional
Return index */
static inline ffssize ffstr_split(const ffstr *s, ffssize index, ffstr *first, ffstr *second)
{
	return ffs_split(s->ptr, s->len, index, first, second);
}

#define ffstr_splitby(s, by_char, first, second) \
	ffstr_split(s, ffstr_findchar(s, by_char), first, second)

#define ffstr_rsplitby(s, by_char, first, second) \
	ffstr_split(s, ffstr_rfindchar(s, by_char), first, second)

#define ffstr_splitbyany(s, by_chars_sz, first, second) \
	ffstr_split(s, ffstr_findanyz(s, by_chars_sz), first, second)


// CONVERT
#if defined _FFBASE_STRCONVERT_H

/** Convert to integer.
flags: enum FFS_INT
Return TRUE on success */
#define ffstr_toint(s, intXX_dst, flags) \
	((s)->len != 0 && (s)->len == ffs_toint((s)->ptr, (s)->len, intXX_dst, flags))
#define ffstr_to_uint64(s, dst) \
	((s)->len != 0 && (s)->len == ffs_toint((s)->ptr, (s)->len, dst, FFS_INT64))
#define ffstr_to_uint32(s, dst) \
	((s)->len != 0 && (s)->len == ffs_toint((s)->ptr, (s)->len, dst, FFS_INT32))
#define ffstr_to_int64(s, dst) \
	((s)->len != 0 && (s)->len == ffs_toint((s)->ptr, (s)->len, dst, FFS_INT64 | FFS_INTSIGN))
#define ffstr_to_int32(s, dst) \
	((s)->len != 0 && (s)->len == ffs_toint((s)->ptr, (s)->len, dst, FFS_INT32 | FFS_INTSIGN))

#define ffstr_to_float(s, dst) \
	((s)->len != 0 && (s)->len == ffs_tofloat((s)->ptr, (s)->len, dst, 0))

/** Convert from integer.
Return N of bytes written;  0 on error */
#define ffstr_fromint(sdst, cap, i, flags) \
	((sdst)->len = ffs_fromint(i, (sdst)->ptr, cap, flags))
#define ffstr_from_uint(sdst, cap, i) \
	((sdst)->len = ffs_fromint(i, (sdst)->ptr, cap, 0))
#define ffstr_from_int(sdst, cap, i) \
	((sdst)->len = ffs_fromint(i, (sdst)->ptr, cap, FFS_INTSIGN))

/** Convert from float.
Return N of bytes written;  0 on error */
#define ffstr_from_float(sdst, cap, f, prec) \
	((sdst)->len = ffs_fromfloat(f, (sdst)->ptr, cap, prec))

#endif // _FFBASE_STRCONVERT_H

/** Convert to lower case in-place */
static inline void ffstr_lower(ffstr *s)
{
	ffs_lower(s->ptr, s->len, s->ptr, s->len);
}


// ALLOCATE

/** Reserve stack for 'n' elements: char[n]
Don't free this buffer!
Return NULL on error */
static inline char* ffstr_alloc_stack(ffstr *s, ffsize n)
{
	FF_ASSERT(s->ptr == NULL);
	s->len = 0;
	return (s->ptr = (char*)ffmem_stack(n));
}

/** Allocate memory for 'n' elements: char[n]
Return NULL on error */
static inline char* ffstr_alloc(ffstr *s, ffsize n)
{
	FF_ASSERT(s->ptr == NULL);
	s->len = 0;
	return (s->ptr = (char*)ffmem_alloc(n));
}

/** Grow/shrink/allocate memory buffer.
Return NULL on error (existing memory buffer is preserved) */
static inline char* ffstr_realloc(ffstr *s, ffsize n)
{
	char *p;
	if (NULL == (p = (char*)ffmem_realloc(s->ptr, n)))
		return NULL;
	s->ptr = p;
	if (s->len > n)
		s->len = n;
	return s->ptr;
}

/** Grow by 'by' bytes, but not less than the half of the currently used size */
static inline char* ffstr_growhalf(ffstr *s, ffsize *cap, ffsize by)
{
	if (s->len + by <= *cap)
		return s->ptr;
	ffsize newcap = s->len + ffmax(by, s->len / 2);
	if (NULL == ffstr_realloc(s, newcap))
		return NULL;
	*cap = newcap;
	return s->ptr;
}

/** Grow by 'by' bytes, but not less than the currently used size */
static inline char* ffstr_growtwice(ffstr *s, ffsize *cap, ffsize by)
{
	if (s->len + by <= *cap)
		return s->ptr;
	ffsize newcap = s->len + ffmax(by, s->len);
	if (NULL == ffstr_realloc(s, newcap))
		return NULL;
	*cap = newcap;
	return s->ptr;
}

/** Grow memory buffer.
Return NULL on error (existing memory buffer is preserved) */
#define ffstr_grow(s, cap_ptr, by)  ffstr_growhalf(s, cap_ptr, by)

/** Free string buffer */
static inline void ffstr_free(ffstr *s)
{
	ffmem_free(s->ptr);
	s->ptr = NULL;
	s->len = 0;
}


// COPY

/** Copy data
Return N of bytes copied */
static inline ffsize ffstr_copy(ffstr *s, ffsize cap, const void *src, ffsize n)
{
	n = ffmin(n, cap);
	ffmem_copy(s->ptr, src, n);
	s->len = n;
	return n;
}

#define ffstr_copystr(s, cap, ssrc)  ffstr_copy(s, cap, (ssrc)->ptr, (ssrc)->len)

/** Add a new element and return its pointer: &s[len++]
ffstr_addchar() is a safe variant */
#define ffstr_push(s) \
	(&((char*)(s)->ptr)[(s)->len++])

/** Add data into array's tail (the buffer must be allocated): s[len..] = src[]
Return N of bytes copied */
static inline ffsize ffstr_add(ffstr *s, ffsize cap, const void *src, ffsize n)
{
	n = ffmin(n, cap - s->len);
	ffmem_copy(s->ptr + s->len, src, n);
	s->len += n;
	return n;
}

#define ffstr_add2(s, cap, ssrc)  ffstr_add(s, cap, (ssrc)->ptr, (ssrc)->len)
#define ffstr_addcz(s, cap, sz)  ffstr_add(s, cap, sz, ffsz_len(sz))

static inline ffsize ffstr_addz(ffstr *s, ffsize cap, const char *sz)
{
	ffsize n = _ffs_copyz(s->ptr + s->len, cap - s->len, sz);
	s->len += n;
	return n;
}

static inline ffsize ffstr_addchar(ffstr *s, ffsize cap, int ch)
{
	if (s->len == cap)
		return 0;
	s->ptr[s->len++] = ch;
	return 1;
}

/** Add data filling the buffer with copies of 1 byte.
Return N of bytes copied */
static inline ffsize ffstr_addfill(ffstr *s, ffsize cap, int ch, ffsize n)
{
	n = ffmin(n, cap - s->len);
	ffmem_fill(s->ptr + s->len, ch, n);
	s->len += n;
	return n;
}

#if defined _FFBASE_STRFORMAT_H

/** Add %-formatted data into array's tail (the buffer must be allocated) */
static inline ffsize ffstr_addfmt(ffstr *s, ffsize cap, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ffssize n = ffs_formatv(s->ptr + s->len, cap - s->len, fmt, args);
	va_end(args);
	if (n <= 0)
		return 0;
	s->len += n;
	return n;
}

static inline ffsize ffstr_addfmtv(ffstr *s, ffsize cap, const char *fmt, va_list args)
{
	va_list va;
	va_copy(va, args);
	ffssize n = ffs_formatv(s->ptr + s->len, cap - s->len, fmt, va);
	va_end(va);
	if (n <= 0)
		return 0;
	s->len += n;
	return n;
}

#endif // _FFBASE_STRFORMAT_H


// ALLOCATE+COPY

/** Allocate and copy data: s = copy(src[])
Return NULL on error */
static inline char* ffstr_dup(ffstr *s, const char *src, ffsize n)
{
	if (NULL == ffstr_alloc(s, n))
		return NULL;
	ffmem_copy(s->ptr, src, n);
	s->len = n;
	return s->ptr;
}

/** Allocate and copy data: s = copy(ffstr) */
#define ffstr_dup2(s, src)  ffstr_dup(s, (src)->ptr, (src)->len)
#define ffstr_dupstr(s, src)  ffstr_dup(s, (src)->ptr, (src)->len)

/** Allocate and copy data: s = copy(sz) */
#define ffstr_dupz(s, sz)  ffstr_dup(s, sz, ffsz_len(sz))

/** Allocate and copy data in lower case: s = copy(lower(src[])) */
static inline char* ffstr_dup_lower(ffstr *s, const char *src, ffsize n)
{
	if (NULL == ffstr_alloc(s, n))
		return NULL;
	ffs_lower(s->ptr, n, src, n);
	s->len = n;
	return s->ptr;
}

#define ffstr_dupstr_lower(s, src)  ffstr_dup_lower(s, (src)->ptr, (src)->len)

/** Reallocate buffer (if necessary) and append data
Return N of bytes copied, set 'cap' to the new capacity of reallocated buffer;
 0 on error */
static inline ffsize ffstr_growadd(ffstr *s, ffsize *cap, const void *src, ffsize n)
{
	if (NULL == ffstr_grow(s, cap, n))
		return 0;

	ffmem_copy(s->ptr + s->len, src, n);
	s->len += n;
	return n;
}

#define ffstr_growadd2(s, cap_ptr, ssrc)  ffstr_growadd(s, cap_ptr, (ssrc)->ptr, (ssrc)->len)
#define ffstr_growaddz(s, cap_ptr, sz)  ffstr_growadd(s, cap_ptr, sz, ffsz_len(sz))

static inline ffsize ffstr_growaddchar(ffstr *s, ffsize *cap, int ch)
{
	if (NULL == ffstr_grow(s, cap, 1))
		return 0;
	s->ptr[s->len++] = ch;
	return 1;
}

static inline ffsize ffstr_growaddfill(ffstr *s, ffsize *cap, int ch, ffsize n)
{
	if (NULL == ffstr_grow(s, cap, n))
		return 0;

	ffmem_fill(s->ptr + s->len, ch, n);
	s->len += n;
	return n;
}

#if defined _FFBASE_STRFORMAT_H

/** Reallocate buffer (if necessary) and append %-formatted data
Return N of bytes copied, set 'cap' to the new capacity of reallocated buffer;
 0 on error */
static inline ffsize ffstr_growfmtv(ffstr *s, ffsize *cap, const char *fmt, va_list va)
{
	ffsize newcap = (*cap > s->len) ? *cap - s->len : 0;
	va_list args;
	va_copy(args, va);
	ffssize n = ffs_formatv(s->ptr + s->len, newcap, fmt, args);
	va_end(args);

	if (n < 0) {
		FF_ASSERT(newcap < (ffsize)-n);
		if (NULL == ffstr_grow(s, cap, -n))
			return 0;

		va_copy(args, va);
		n = ffs_formatv(s->ptr + s->len, *cap - s->len, fmt, args);
		va_end(args);
	}

	if (n <= 0)
		return 0;

	s->len += n;
	return n;
}

static inline ffsize ffstr_growfmt(ffstr *s, ffsize *cap, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ffsize r = ffstr_growfmtv(s, cap, fmt, args);
	va_end(args);
	return r;
}

#endif // _FFBASE_STRFORMAT_H

#ifdef _FFBASE_UNICODE_H

/** Add (convert to UTF-8) data in specified code-page; reallocate buffer if necessary
flags: enum FFUNICODE_CP
Return N of bytes copied, set 'cap' to the new capacity of reallocated buffer;
 0 on error */
static inline ffsize ffstr_growadd_codepage(ffstr *s, ffsize *cap, const char *src, ffsize len, ffuint flags)
{
	ffssize r = ffutf8_from_cp(NULL, 0, src, len, flags);
	if (r <= 0)
		return 0;

	if (NULL == ffstr_growhalf(s, cap, r))
		return 0;

	r = ffutf8_from_cp(s->ptr + s->len, *cap - s->len, src, len, flags);
	s->len += r;
	return r;
}

#endif // _FFBASE_UNICODE_H

enum FFSTR_REPLACE {
	FFSTR_REPLACE_ICASE = 1,
	FFSTR_REPLACE_ALL = 2,
};

/** Grow buffer and append data while replacing search text
flags: enum FFSTR_REPLACE
Return N of replacements */
static inline ffsize ffstr_growadd_replace(ffstr *output, ffsize *cap, const ffstr *input, const ffstr *search, const ffstr *replace, ffuint flags)
{
	ffstr in = *input;
	ffssize n = 0, pos;
	for (;;) {

		if (flags & FFSTR_REPLACE_ICASE)
			pos = ffstr_ifindstr(&in, search);
		else
			pos = ffstr_findstr(&in, search);
		if (pos < 0)
			break;

		if ((ffsize)pos != ffstr_growadd(output, cap, in.ptr, pos)
			|| replace->len != ffstr_growadd2(output, cap, replace))
			return 0;

		n++;
		ffstr_shift(&in, pos + search->len);

		if (!(flags & FFSTR_REPLACE_ALL))
			break;
	}

	if (in.len != ffstr_growadd2(output, cap, &in))
		return 0;
	return n;
}

/** Gather contiguous data of the specified size
If the required data is already available (from the container or from input data), don't copy any data
Reallocate buffer (if necessary) and append data until the total length limit is reached
Return N of bytes copied or processed;
 <0 on error */
static inline ffssize ffstr_gather(ffstr *s, ffsize *cap, const char *d, ffsize n, ffsize total, ffstr *out)
{
	if (total <= n && s->len == 0) {
		ffstr_set(out, d, total);
		return total;
	}

	if (total <= s->len) {
		ffstr_set(out, s->ptr, total);
		return 0;
	}

	if (total > *cap) {
		if (NULL == ffstr_realloc(s, total))
			return -1;
		*cap = total;
	}

	n = ffmin(n, total - s->len);
	ffmem_copy(s->ptr + s->len, d, n);
	s->len += n;
	if (s->len == total)
		*out = *s;
	else
		out->len = 0;
	return n;
}
