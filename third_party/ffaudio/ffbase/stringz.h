/** ffbase: NULL-terminated string functions
2020, Simon Zolin
*/

#pragma once

#ifndef _FFBASE_BASE_H
#include <ffbase/base.h>
#endif
#include <ffbase/string.h>

/*
ffsz_copyz ffsz_copyn ffsz_copystr
ffsz_dupn ffsz_dup ffsz_dupstr
ffsz_findchar
ffsz_cmp ffsz_icmp ffsz_eq
ffsz_match ffsz_matchz
ffszarr_find ffszarr_ifind
ffszarr_findsorted ffszarr_ifindsorted
ffcharr_findsorted
*/

/** Copy string
Return number of bytes written (including NULL byte)
  ==cap: usually means that there was not enough space */
static inline ffsize ffsz_copyz(char *dst, ffsize cap, const char *sz)
{
	if (cap == 0)
		return 0;

	ffsize i;
	for (i = 0;  i != cap - 1;  i++) {
		if (sz[i] == '\0')
			break;
		dst[i] = sz[i];
	}
	dst[i] = '\0';
	return i + 1;
}

static inline ffsize ffsz_copyn(char *dst, ffsize cap, const char *src, ffsize n)
{
	if (cap == 0)
		return 0;
	n = ffmin(n, cap - 1);
	ffmem_copy(dst, src, n);
	dst[n] = '\0';
	return n + 1;
}

#define ffsz_copystr(dst, cap, str)  ffsz_copyn(dst, cap, (str)->ptr, (str)->len)

/** Allocate memory and copy string */
static inline char* ffsz_dupn(const char *src, ffsize n)
{
	char *s = (char*)ffmem_alloc(n + 1);
	if (s == NULL)
		return NULL;
	ffmem_copy(s, src, n);
	s[n] = '\0';
	return s;
}

static inline char* ffsz_dupstr(const ffstr *src)
{
	return ffsz_dupn(src->ptr, src->len);
}

/** Allocate memory and copy string */
static inline char* ffsz_dup(const char *sz)
{
	return ffsz_dupn(sz, ffsz_len(sz));
}

/** Compare
Return 0 if equal
 <0: sz < cmpz;
 >0: sz > cmpz */
static inline int ffsz_cmp(const char *sz, const char *cmpz)
{
	ffsize i = 0;
	do {
		if (sz[i] != cmpz[i])
			return (ffbyte)sz[i] - (ffbyte)cmpz[i];
	} while (sz[i++] != '\0');
	return 0;
}

/** Compare (case-insensitive)
Return 0 if equal
 <0: sz < cmpz;
 >0: sz > cmpz */
static inline int ffsz_icmp(const char *sz, const char *cmpz)
{
	for (ffsize i = 0;  ;  i++) {
		ffuint cl = (ffbyte)sz[i];
		ffuint cr = (ffbyte)cmpz[i];

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

		} else if (cl == '\0') {
			return 0; // s == sz
		}
	}
}

#define ffsz_ieq(sz, cmpz)  (!ffsz_icmp(sz, cmpz))

static inline int ffsz_eq(const char *sz, const char *cmpz)
{
	return !ffsz_cmp(sz, cmpz);
}

/** Find a position of byte in a NULL-terminated string
Return <0 if not found */
static inline ffssize ffsz_findchar(const char *sz, int ch)
{
	for (ffsize i = 0;  sz[i] != '\0';  i++) {
		if (sz[i] == ch)
			return i;
	}
	return -1;
}

/** Return TRUE if 'n' characters are equal in both strings */
static inline int ffsz_match(const char *sz, const char *s, ffsize n)
{
	ffsize i;
	for (i = 0;  sz[i] != '\0';  i++) {
		if (i == n)
			return 1; // sz begins with s
		if (sz[i] != s[i])
			return 0; // s != sz
	}
	if (i != n)
		return 0; // sz < s
	return 1; // s == sz
}

static inline int ffsz_matchz(const char *sz, const char *sz2)
{
	for (ffsize i = 0;  ;  i++) {
		if (sz2[i] == '\0')
			return 1; // sz begins with sz2
		if (sz[i] != sz2[i])
			return 0; // sz != sz2
	}
}

/** Search a string in array of pointers to NULL-terminated strings.
Return -1 if not found */
static inline ffssize ffszarr_find(const char *const *ar, ffsize n, const char *search, ffsize search_len)
{
	for (ffsize i = 0;  i < n;  i++) {
		if (0 == ffs_cmpz(search, search_len, ar[i]))
			return i;
	}
	return -1;
}

/** Search (case-insensitive) a string in array of pointers to NULL-terminated strings.
Return -1 if not found */
static inline ffssize ffszarr_ifind(const char *const *ar, ffsize n, const char *search, ffsize search_len)
{
	for (ffsize i = 0;  i != n;  i++) {
		if (0 == ffs_icmpz(search, search_len, ar[i]))
			return i;
	}
	return -1;
}

/** Search a string in array of pointers to NULL-terminated strings.
Return -1 if not found */
static inline ffssize ffszarr_findsorted(const char *const *ar, ffsize n, const char *search, ffsize search_len)
{
	ffsize start = 0, end = n;
	while (start != end) {
		ffsize i = start + (end - start) / 2;
		int r = ffs_cmpz(search, search_len, ar[i]);
		if (r == 0)
			return i;
		else if (r < 0)
			end = i;
		else
			start = i + 1;
	}
	return -1;
}

/** Search (case-insensitive) a string in array of pointers to NULL-terminated strings.
Return -1 if not found */
static inline ffssize ffszarr_ifindsorted(const char *const *ar, ffsize n, const char *search, ffsize search_len)
{
	ffsize start = 0, end = n;
	while (start != end) {
		ffsize i = start + (end - start) / 2;
		int r = ffs_icmpz(search, search_len, ar[i]);
		if (r == 0)
			return i;
		else if (r < 0)
			end = i;
		else
			start = i + 1;
	}
	return -1;
}


/** Search a string in char[n][el_size+padding] array.
Return -1 if not found */
static inline ffssize ffcharr_find_sorted_padding(const void *ar, ffsize n, ffsize el_size, ffsize padding, const char *search, ffsize search_len)
{
	if (search_len > el_size)
		return -1; // the string's too large for this array

	ffsize start = 0;
	while (start != n) {
		ffsize i = start + (n - start) / 2;
		const char *ptr = (char*)ar + i * (el_size + padding);
		int r = ffmem_cmp(search, ptr, search_len);

		if (r == 0
			&& search_len != el_size
			&& ptr[search_len] != '\0')
			r = -1; // found "01" in {0,1,2}

		if (r == 0)
			return i;
		else if (r < 0)
			n = i;
		else
			start = i + 1;
	}
	return -1;
}

#define ffcharr_findsorted(ar, n, el_size, search, search_len) \
	ffcharr_find_sorted_padding(ar, n, el_size, 0, search, search_len)
