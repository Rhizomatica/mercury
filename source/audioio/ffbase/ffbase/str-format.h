/** ffbase: string format functions
2020, Simon Zolin
*/

/*
ffs_format ffs_formatv ffs_format_r0
ffsz_format ffsz_formatv
ffsz_allocfmt ffsz_allocfmtv
ffstr_matchfmt ffstr_matchfmtv
*/

#pragma once
#define _FFBASE_STRFORMAT_H

#ifdef FF_UNIX
#include <stdarg.h>
#endif

#include <ffbase/unicode.h> // optional

/** Formatted output into string.

Standard-compliant:
% [[0]width] d                 int
% [[0]width] u                 uint
%s                             char*
%%                             '%'
%c                             int
% [[0]width] [.prec] f         double

Non-standard:

Unlike standard format string where flags may change the size of an input argument
 (e.g. "%d" is int32 but "%lld" is int64)
 this format string has a consistent type which defines the *size* taken from stack,
 and flags change the *behaviour* - not the size.

Integer:
% [[0]width] [x|X|,] d         int
% [[0]width] [x|X|,] u         ffuint
% [[0]width] [x|X|,] D         ffint64
% [[0]width] [x|X|,] U         ffuint64
% [[0]width] [x|X|,] L         ffsize

Floating-point:
% [[0]width] [.prec] f         double (default precision:6)

Binary data:
% width x|X b                  byte*
% * x|X b                      ffsize, byte*

String:
%s                             char* (NULL-terminated string)
% width s                      char*
%*s                            ffsize, char*
% [width] S                    ffstr*
%*S                            ffsize, ffstr*

System string:
%q                             char* on UNIX, wchar_t* on Windows (NULL-terminated string)
% width q                      char*|wchar_t*
%*q                            ffsize, char*|wchar_t*

Char:
% [width] c                    int
%*c                            ffsize, int

%p                             void*  (Pointer)
%Z                             '\0'  (NULL byte)
%%                             '%'  (Percent sign)

%E                             int  (System error message) (build with -DFFBASE_HAVE_FFERR_STR and provide fferr_str())

Examples:
("%s", NULL)  =>  "(null)"
("%2xb", "AB")  =>  "4142"
("%*xb", (ffsize)2, "AB")  =>  "4142"
("%*c", (ffsize)3, 'A')  =>  "AAA"
("%E", errno)  =>  "(22) Invalid argument"

Algorithm:

Note: if there's not enough space, we just count the total size needed
 and then return it as a negative value.
This allows the caller to effectively get the N of bytes he needs to allocate,
 while also do the job (in 1 call) if the space is enough.

. Process flags:
 . '0'
 . width: [0-9]*
 . 'x|X|,'
 . '.':
  . precision: [0-9]*
 . '*':
  . pop ffsize from va
. Process type:
 . pop TYPE from va
 . process
 . if not integer: done
. Convert from integer


Return N of bytes written;
 0 on error (bad format string);
 <0 if not enough space: "-RESULT" is the total number of bytes needed */
FF_INLINE_EXTERN ffssize ffs_formatv(char *dst, ffsize cap, const char *fmt, va_list va);

static inline ffssize ffs_format(char *buf, ffsize cap, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ffssize r = ffs_formatv(buf, cap, fmt, args);
	va_end(args);
	return r;
}

/**
More convenient variant in case we don't need the exact data size if there was not enough space.
Return 0 on error */
static inline ffsize ffs_format_r0(char *buf, ffsize cap, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ffssize r = ffs_formatv(buf, cap, fmt, args);
	va_end(args);
	return (r >= 0) ? r : 0;
}

static inline ffssize ffsz_formatv(char *buf, ffsize cap, const char *fmt, va_list va)
{
	va_list args;
	va_copy(args, va);
	ffssize r = ffs_formatv(buf, (cap) ? cap - 1 : 0, fmt, args);
	va_end(args);
	if (cap != 0) {
		if (r >= 0)
			buf[r] = '\0';
		else
			buf[0] = '\0';
	}
	return r;
}

static inline ffssize ffsz_format(char *buf, ffsize cap, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ffssize r = ffsz_formatv(buf, cap, fmt, args);
	va_end(args);
	return r;
}

/** Allocate buffer and add %-formatted data to a NULL-terminated string.
Return a newly allocated string.  Must free with ffmem_free() */
static inline char* ffsz_allocfmtv(const char *fmt, va_list va)
{
	ffsize cap = 80;
	char *d;
	if (NULL == (d = (char*)ffmem_alloc(cap)))
		return NULL;

	va_list args;
	va_copy(args, va);
	ffssize n = ffs_formatv(d, cap - 1, fmt, args);
	va_end(args);

	if (n < 0) {
		FF_ASSERT(cap - 1 < (ffsize)-n);
		cap = -n + 1;
		ffmem_free(d);
		if (NULL == (d = (char*)ffmem_alloc(cap)))
			goto fail;

		va_copy(args, va);
		n = ffs_formatv(d, cap - 1, fmt, args);
		va_end(args);

		if (n < 0)
			goto fail;
	}

	d[n] = '\0';
	return d;

fail:
	ffmem_free(d);
	return NULL;
}

static inline char* ffsz_allocfmt(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	char *r = ffsz_allocfmtv(fmt, args);
	va_end(args);
	return r;
}


/** Match string by a format and extract data to output variables

Format:
% [width] S      ffstr*
% [width] [x] u  ffuint*
% [width] [x] U  ffuint64*
% [width] [x] d  int*
% [width] [x] D  ffint64*
%%               %

Algorithm:
. compare bytes until '%'
. get optional width from '%width...'
. get string chunk
. convert string to integer if needed

Return 0 if matched;
 >0: match: return stop index +1 within input string;
 <0: no match or format error */
FF_INLINE_EXTERN ffssize ffstr_matchfmtv(const ffstr *s, const char *fmt, va_list args);

static inline ffssize ffstr_matchfmt(const ffstr *s, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ffssize r = ffstr_matchfmtv(s, fmt, args);
	va_end(args);
	return r;
}

#ifndef FFBASE_OPT_SIZE
#include <ffbase/str-format.c>
#endif
