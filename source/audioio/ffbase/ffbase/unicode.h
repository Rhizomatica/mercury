/** ffbase: Unicode functions
2020, Simon Zolin
*/

/*
ffutf8_encode
ffutf8_decode
ffutf8_valid
ffutf8_from_utf8
ffutf8_from_utf16
ffutf8_to_utf16
ffutf8_from_cp
ffutf_bom
Windows-only:
	ffs_wtou ffs_wtouz ffsz_wtou ffsz_alloc_wtou
	ffsz_utow ffsz_utow_n ffsz_alloc_utow
*/

/*
UTF-8:
U+0000..U+007F      0xxxxxxx
U+0080..U+07FF      110xxxxx 10xxxxxx
U+0800..U+FFFF      1110xxxx 10xxxxxx*2
00010000..001FFFFF  11110xxx 10xxxxxx*3
00200000..03FFFFFF  111110xx 10xxxxxx*4
04000000..7FFFFFFF  1111110x 10xxxxxx*5

UTF-16:
U+0000..U+D7FF      XX XX
U+E000..U+FFFF      XX XX
010000..10FFFF     (D800..DBFF) (DC00..DFFF)
*/

#pragma once
#define _FFBASE_UNICODE_H

#ifndef _FFBASE_BASE_H
#include <ffbase/base.h>
#endif

enum FFUNICODE {
	FFUNICODE_UTF8,
	FFUNICODE_UTF16LE,
	FFUNICODE_UTF16BE,
};

enum FFUNICODE_CP {
	_FFUNICODE_CP_BEGIN = FFUNICODE_UTF16BE + 1,
	FFUNICODE_WIN866 = _FFUNICODE_CP_BEGIN,
	FFUNICODE_WIN1251,
	FFUNICODE_WIN1252,
};


/** Return the number of bytes needed to encode a character in UTF-8 */
static inline ffuint ffutf8_size(ffuint ch)
{
	ffuint n;

	if (ch < 0x80)
		n = 1;
	else if (ch < 0x0800)
		n = 2;
	else if (ch < 0x010000)
		n = 3;
	else if (ch < 0x200000)
		n = 4;
	else if (ch < 0x04000000)
		n = 5;
	else if (ch < 0x80000000)
		n = 6;
	else
		n = 0;
	return n;
}

/** Convert 31-bit number to UTF-8
Return N of bytes written */
static inline ffuint ffutf8_encode(char *dst, ffsize cap, ffuint i)
{
	ffuint n = ffutf8_size(i);
	if (n == 0 || cap < n)
		return 0;

	switch (n) {
	case 1:
		*dst++ = i; // 0xxxxxxx
		break;

	case 2:
		*dst++ = 0xc0 | (i >> 6); // 110xxxxx
		*dst++ = 0x80 | ((i >> 0) & 0x3f);
		break;

	case 3:
		*dst++ = 0xe0 | (i >> 12); // 1110xxxx
		*dst++ = 0x80 | ((i >> 6) & 0x3f);
		*dst++ = 0x80 | ((i >> 0) & 0x3f);
		break;

	case 4:
		*dst++ = 0xf0 | (i >> 18); // 11110xxx
		*dst++ = 0x80 | ((i >> 12) & 0x3f);
		*dst++ = 0x80 | ((i >> 6) & 0x3f);
		*dst++ = 0x80 | ((i >> 0) & 0x3f);
		break;

	case 5:
		*dst++ = 0xf8 | (i >> 24); // 111110xx
		*dst++ = 0x80 | ((i >> 18) & 0x3f);
		*dst++ = 0x80 | ((i >> 12) & 0x3f);
		*dst++ = 0x80 | ((i >> 6) & 0x3f);
		*dst++ = 0x80 | ((i >> 0) & 0x3f);
		break;

	case 6:
		*dst++ = 0xfc | (i >> 30); // 1111110x
		*dst++ = 0x80 | ((i >> 24) & 0x3f);
		*dst++ = 0x80 | ((i >> 18) & 0x3f);
		*dst++ = 0x80 | ((i >> 12) & 0x3f);
		*dst++ = 0x80 | ((i >> 6) & 0x3f);
		*dst++ = 0x80 | ((i >> 0) & 0x3f);
		break;
	}

	return n;
}

/** Decode a UTF-8 number
Return the number of bytes parsed;
 negative value if more data is needed;
 0 on error */
static inline int ffutf8_decode(const char *utf8, ffsize len, ffuint *val)
{
	if (len == 0)
		return -1;

	ffuint d = (ffbyte)utf8[0];
	if ((d & 0x80) == 0) {
		*val = d;
		return 1;
	}

	ffuint n = ffbit_find32(~(d << 24) & 0xfe000000); // e.g. 110xxxxx -> 001xxxxx
	if (!(n >= 3 && n <= 7))
		return 0; // invalid first byte
	n--;
	if (len < n)
		return -(int)n; // need more data

	static const ffbyte b1_mask[] = { 0, 0, 0x1f, 0x0f, 0x07, 0x03, 0x01 };
	ffuint r = d & b1_mask[n];

	for (ffuint i = 1;  i != n;  i++) {
		d = (ffbyte)utf8[i];
		if ((d & 0xc0) != 0x80)
			return 0; // invalid

		r = (r << 6) | (d & ~0xc0);
	}

	*val = r;
	return n;
}

/** Return TRUE if UTF-8 data is valid */
static inline int ffutf8_valid(const char *utf8, ffsize len)
{
	int r;
	ffuint val;
	for (ffsize i = 0;  i < len;  i += r) {
		r = ffutf8_decode(utf8 + i, len - i, &val);
		if (r <= 0)
			return 0;
	}
	return 1;
}

#define ffutf8_valid_str(str)  ffutf8_valid((str).ptr, (str).len)


/** Return TRUE if UTF-16 code unit is in Basic Multilingual Plane
(0..0xd7ff) and (0xe000..0xffff) */
#define ffutf16_basic(i)  (((i) & 0xf800) != 0xd800)

/** Return TRUE if UTF-16 code unit is a high surrogate value for a character in Supplementary Plane
(0xd800..0xdbff) */
#define ffutf16_highsurr(i)  (((i) & 0xfc00) == 0xd800)

/** Return TRUE if UTF-16 code unit is a low surrogate value for a character in Supplementary Plane
(0xdc00..0xdfff) */
#define ffutf16_lowsurr(i)  (((i) & 0xfc00) == 0xdc00)

/** Get character in Supplementary Plane from UTF-16 high and low surrogates */
static inline ffuint ffutf16_suppl(ffuint hi, ffuint lo)
{
	return 0x10000 + ((hi - 0xd800) << 10) + (lo - 0xdc00);
}

#define _FFUTF8_REPLCHAR  "\xEF\xBF\xBD"

/** Convert UTF-8 to UTF-8.
Replace bad/incomplete codes with REPLACEMENT CHARACTER U+FFFD (UTF-8: 0xEF 0xBF 0xBD).
dst: NULL: return required capacity
Return
  N of bytes written;
  <0 if not enough space. */
static inline ffssize ffutf8_from_utf8(char *dst, ffsize cap, const char *src, ffsize len, ffuint flags)
{
	(void)flags;
	ffsize n = 0;
	const char *end = src + len;
	ffuint ch;

	if (dst == NULL) {
		while (src < end) {
			int r = ffutf8_decode(src, end - src, &ch);
			if (r < 0) {
				n += 3;
				break;
			} else if (r == 0) {
				src++;
				n += 3;
			} else {
				src += r;
				n += r;
			}
		}

		return n;
	}

	while (src < end) {
		int r = ffutf8_decode(src, end - src, &ch);
		if (r <= 0) {
			if (n + 3 > cap)
				return -1;
			ffmem_copy(dst + n, _FFUTF8_REPLCHAR, 3);
			n += 3;
			if (r < 0)
				break;
			src++;
		} else {
			ffmem_copy(dst + n, src, r);
			src += r;
			n += r;
		}
	}

	return n;
}

/** Convert UTF-16 to UTF-8
Replace bad/incomplete codes with REPLACEMENT CHARACTER U+FFFD (UTF-8: 0xEF 0xBF 0xBD).
dst: NULL: return capacity
flags: FFUNICODE_UTF16LE FFUNICODE_UTF16BE
Return N of bytes written;  <0 if not enough space */
static inline ffssize ffutf8_from_utf16(char *dst, ffsize cap, const char *src, ffsize len, ffuint flags)
{
	ffsize n = 0;
	ffuint hs = 0;
	const ffushort *us = (ffushort*)src;

	if (dst == NULL) {
		for (ffsize i = 0;  i < len / 2;  i++) {
			ffuint ch = (flags == FFUNICODE_UTF16LE) ? ffint_le_cpu16(us[i]) : ffint_be_cpu16(us[i]);

			if (hs) {
				hs = 0;
				if (ffutf16_lowsurr(ch)) {
					n += 4; // valid surrogate pair
					continue;
				}
				n += 3; // bad pair
			}

			if (ffutf16_basic(ch))
				n += ffutf8_size(ch);
			else if (ffutf16_highsurr(ch))
				hs = 1;
			else
				n += 3; // unexpected low surrogate value
		}

		if (hs || (len % 2) != 0) // incomplete surrogate pair OR incomplete code
			n += 3;

		return n;
	}

	for (ffsize i = 0;  i < len / 2;  i++) {
		ffuint ch = (flags == FFUNICODE_UTF16LE) ? ffint_le_cpu16(us[i]) : ffint_be_cpu16(us[i]);

		if (hs != 0) {

			if (ffutf16_lowsurr(ch)) {
				if (n + 4 > cap)
					return -1;
				ch = ffutf16_suppl(hs, ch);
				n += ffutf8_encode(dst + n, cap - n, ch);
				hs = 0;
				continue;
			}

			hs = 0;
			if (n + 3 > cap)
				return -1;
			ffmem_copy(dst + n, _FFUTF8_REPLCHAR, 3);
			n += 3; // bad pair
		}

		if (ffutf16_basic(ch)) {
			ffuint r = ffutf8_encode(dst + n, cap - n, ch);
			if (r == 0)
				return -1;
			n += r;

		} else if (ffutf16_highsurr(ch)) {
			hs = ch;

		} else {
			if (n + 3 > cap)
				return -1;
			ffmem_copy(dst + n, _FFUTF8_REPLCHAR, 3);
			n += 3; // unexpected low surrogate value
		}
	}

	if (hs // incomplete surrogate pair
		|| (len % 2) != 0) { // incomplete code

		if (n + 3 > cap)
			return -1;
		ffmem_copy(dst + n, _FFUTF8_REPLCHAR, 3);
		n += 3;
	}

	return n;
}

/** Convert UTF-8 to UTF-16
Note: doesn't support surrogate pairs
dst: NULL: return capacity
flags: FFUNICODE_UTF16LE FFUNICODE_UTF16BE
Return N of bytes written;
 <0 on error */
static inline ffssize ffutf8_to_utf16(char *dst, ffsize cap, const char *src, ffsize len, ffuint flags)
{
	const ffushort U_REPL = 0xFFFD;
	ffsize n = 0;
	ffuint ch;

	if (dst == NULL) {
		for (ffsize i = 0;  i < len;  ) {
			int r = ffutf8_decode(src + i, len - i, &ch);
			if (r <= 0) {
				n += 2;
				continue;
			}
			i += r;

			if (ch > 0xffff
				|| !ffutf16_basic(ch)) {
				n += 2;
				continue;
			}
			n++;
		}

		return n * 2;
	}

	ffushort *us = (ffushort*)dst;
	cap /= 2;
	for (ffsize i = 0;  i < len;  ) {
		int r = ffutf8_decode(src + i, len - i, &ch);
		if (r <= 0) {
			// bad UTF-8 char
			ch = U_REPL;
			i++;
		} else {
			i += r;
		}

		if (ch > 0xffff
			|| !ffutf16_basic(ch)) {
			// too large value or need surrogate pair
			ch = U_REPL;
		}

		if (n >= cap)
			return -1;

		us[n] = (flags == FFUNICODE_UTF16LE) ? ffint_le_cpu16(ch) : ffint_be_cpu16(ch);
		n++;
	}

	return n * 2;
}

/** Find a character in UTF-16 data
@len: length in bytes
Return -1 if not found */
static inline ffssize ffutf16_findchar(const char *s, size_t len, int ch)
{
	const short *p = (short*)s;
	len /= 2;
	for (ffuint i = 0;  i != len;  i++) {
		if (p[i] == ch)
			return i * 2;
	}
	return -1;
}


/** Detect BOM
Return enum FFUNICODE or -1 on error */
static inline int ffutf_bom(const void *src, ffsize *len)
{
	const ffbyte *s = (ffbyte*)src;

	switch (s[0]) {
	case 0xff:
		if (*len >= 2 && s[1] == 0xfe) {
			*len = 2;
			return FFUNICODE_UTF16LE;
		}
		break;

	case 0xfe:
		if (*len >= 2 && s[1] == 0xff) {
			*len = 2;
			return FFUNICODE_UTF16BE;
		}
		break;

	case 0xef:
		if (*len >= 3 && s[1] == 0xbb && s[2] == 0xbf) {
			*len = 3;
			return FFUNICODE_UTF8;
		}
	}

	return -1;
}

static inline ffssize ffutf8_from_cp(char *dst, ffsize cap, const char *src, ffsize len, ffuint flags)
{
	const ffushort U_REPL = 0xFFFD;

	/* Unicode code-points for characters 0x80-0xff */
	static const ffushort codes[][128] = {
		// FFUNICODE_WIN866
		{
		0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
		0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
		0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
		0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
		0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
		0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
		0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
		0x0401, 0x0451, 0x0404, 0x0454, 0x0407, 0x0457, 0x040E, 0x045E, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x2116, 0x00A4, 0x25A0, 0x00A0,
		},

		// FFUNICODE_WIN1251
		{
		0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
		0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, U_REPL, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
		0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
		0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
		0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
		0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
		0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
		0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
		},

		// FFUNICODE_WIN1252
		{
		0x20AC, U_REPL, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, U_REPL, 0x017D, U_REPL,
		U_REPL, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, U_REPL, 0x017E, 0x0178,
		0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
		0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
		0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
		0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
		0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
		0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
		},
	};

	flags -= _FFUNICODE_CP_BEGIN;
	FF_ASSERT(flags < FF_COUNT(codes));

	const ffushort *cp = codes[flags];
	ffsize n = 0;

	if (dst == NULL) {
		for (ffsize i = 0;  i < len;  i++) {
			if (src[i] & 0x80) {
				ffuint ch = cp[src[i] & 0x7f];
				n += ffutf8_size(ch);
			} else {
				n++;
			}
		}

		return n;
	}

	for (ffsize i = 0;  i < len;  i++) {
		if (src[i] & 0x80) {
			ffuint ch = cp[src[i] & 0x7f];
			ffuint r = ffutf8_encode(dst + n, cap - n, ch);
			if (r == 0)
				return -1;
			n += r;
		} else {
			if (n >= cap)
				return -1;
			dst[n++] = src[i];
		}
	}

	return n;
}


#ifdef FF_WIN
static inline ffssize ffs_wtou(char *dst, ffsize cap, const wchar_t *wsrc, ffsize len_wchars)
{
	return ffutf8_from_utf16(dst, cap, (char*)wsrc, len_wchars * 2, FFUNICODE_UTF16LE);
}

static inline ffssize ffs_wtouz(char *dst, ffsize cap, const wchar_t *wsz)
{
	ffsize len = ffwsz_len(wsz);
	return ffutf8_from_utf16(dst, cap, (char*)wsz, len * 2, FFUNICODE_UTF16LE);
}

/** Convert NULL-terminated UTF-16LE string to NULL-terminated UTF-8 string
dst: NULL: return capacity (including terminating NULL character)
Return N of characters written (including terminating NULL character);
 <0 on error */
static inline ffssize ffsz_wtou(char *dst, ffsize cap, const wchar_t *wsz)
{
	ffsize len = ffwsz_len(wsz);
	ffssize r = ffutf8_from_utf16(dst, cap, (char*)wsz, (len + 1) * 2, FFUNICODE_UTF16LE);
	if (r <= 0) {
		if (cap != 0)
			dst[0] = '\0';
		return r;
	}
	return r;
}

static inline ffssize ffsz_utow_n(wchar_t *dst, ffsize cap_wchars, const char *s, ffsize len)
{
	ffsize cap = (cap_wchars != 0) ? cap_wchars-1 : 0;
	ffssize r = ffutf8_to_utf16((char*)dst, cap * 2, s, len, FFUNICODE_UTF16LE);
	if (dst == NULL)
		return (r < 0) ? r : (r/2)+1;

	if (cap_wchars == 0)
		return 0;

	if (r < 0) {
		dst[0] = '\0';
		return r;
	}

	r /= 2;
	dst[r++] = '\0';
	return r;
}

static inline ffssize ffsz_utow(wchar_t *dst, ffsize cap_wchars, const char *sz)
{
	return ffsz_utow_n(dst, cap_wchars, sz, ffsz_len(sz));
}

/** Allocate memory and convert NULL-terminated UTF-16LE string to a NULL-terminated UTF-8 string
Return UTF-8 string */
static inline char* ffsz_alloc_wtou(const wchar_t *wsz)
{
	ffsize len = ffwsz_len(wsz);
	ffssize r = ffutf8_from_utf16(NULL, 0, (char*)wsz, (len + 1) * 2, FFUNICODE_UTF16LE);
	if (r <= 0)
		return NULL;

	char *s;
	if (NULL == (s = (char*)ffmem_alloc(r)))
		return NULL;

	r = ffutf8_from_utf16(s, r, (char*)wsz, (len + 1) * 2, FFUNICODE_UTF16LE);
	if (r <= 0) {
		ffmem_free(s);
		return NULL;
	}
	return s;
}

/** Allocate memory and convert NULL-terminated UTF-8 string to a NULL-terminated UTF-16LE string
Return UTF-16LE string */
static inline wchar_t* ffsz_alloc_utow(const char *sz)
{
	ffsize len = ffsz_len(sz);
	ffssize r = ffutf8_to_utf16(NULL, 0, sz, len + 1, FFUNICODE_UTF16LE);
	if (r <= 0)
		return NULL;

	wchar_t *ws;
	if (NULL == (ws = (wchar_t*)ffmem_alloc(r)))
		return NULL;

	r = ffutf8_to_utf16((char*)ws, r, sz, len + 1, FFUNICODE_UTF16LE);
	if (r <= 0) {
		ffmem_free(ws);
		return NULL;
	}
	return ws;
}
#endif
