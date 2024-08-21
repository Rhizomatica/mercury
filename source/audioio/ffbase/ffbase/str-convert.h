/** ffbase: string conversion functions
2020, Simon Zolin
*/

/*
ffchar_tohex
ffs_toint ffs_fromint
ffs_tofloat ffs_fromfloat
ffs_tohex ffs_fromhex
*/

#pragma once
#define _FFBASE_STRCONVERT_H
#include <math.h>


/** Lowercase/uppercase hex alphabet */
static const char ffhex[] = "0123456789abcdef";
static const char ffHEX[] = "0123456789ABCDEF";

/** Convert character to hex 4-bit number
Return -1 if invalid hex char */
static inline int ffchar_tohex(int ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	int b = (ch | 0x20) - 'a' + 10;
	if ((ffuint)b <= 0x0f)
		return b;
	return -1;
}

enum FFS_INT {
	// 0000 000x - size
	FFS_INT8 = 1,
	FFS_INT16 = 2,
	FFS_INT32 = 4,
	FFS_INT64 = 8,

	// 0000 00x0 - base
	// DEC = 0
	FFS_INTHEX = 0x10,
	FFS_INTOCTAL = 0x20,

	// 0000 0x00 - flags
	FFS_INTSIGN = 0x0100,
};

/** Convert string to integer.
dst: int64|int|short|char
flags: enum FFS_INT
Return N of bytes processed;  0 on error */
static inline ffuint ffs_toint(const char *s, ffsize len, void *dst, ffuint flags)
{
	ffuint64 r;
	ffsize i;
	int minus;
	const char *ss = s;

	if (len == 0)
		return 0;

	minus = 0;
	if (flags & FFS_INTSIGN) {
		minus = (s[0] == '-');
		if (s[0] == '-' || s[0] == '+') {
			s++;
			len--;
		}
	}

	r = 0;
	switch (flags & (FFS_INTOCTAL | FFS_INTHEX)) {
	case FFS_INTOCTAL:
		for (i = 0;  i != len;  i++) {
			int b = s[i] - '0';
			if ((ffuint)b >= 8)
				break;
			if (r & 0xe000000000000000)
				return 0; // 64-bit overflow
			r = (r << 3) + b;
		}
		break;

	case 0: // decimal
		for (i = 0;  i != len;  i++) {
			int b = s[i] - '0';
			if ((ffuint)b >= 10)
				break;
			if (__builtin_mul_overflow(r, 10, &r))
				return 0; // 64-bit overflow
			if (__builtin_add_overflow(r, (ffuint)b, &r))
				return 0; // 64-bit overflow
		}
		break;

	case FFS_INTHEX:
		for (i = 0;  i != len;  i++) {
			int b = ffchar_tohex(s[i]);
			if (b < 0)
				break;
			if (r & 0xf000000000000000)
				return 0; // 64-bit overflow
			r = (r << 4) | b;
		}
		break;

	default:
		return 0; // invalid flags
	}

	if (i == 0)
		return 0; // expect at least 1 digit

	switch (flags & 0x0f) {

	case FFS_INT64:
		if (minus)
			r = -(ffint64)r;
		*(ffuint64*)dst = r;
		break;

	case FFS_INT32:
		if (r & 0xffffffff00000000)
			return 0; // 32-bit overflow
		if (minus)
			r = -(ffint64)r;
		*(ffuint*)dst = r;
		break;

	case FFS_INT16:
		if (r & 0xffffffffffff0000)
			return 0; // 16-bit overflow
		if (minus)
			r = -(ffint64)r;
		*(ffushort*)dst = r;
		break;

	case FFS_INT8:
		if (r & 0xffffffffffffff00)
			return 0; // 8-bit overflow
		if (minus)
			r = -(ffint64)r;
		*(ffbyte*)dst = r;
		break;

	default:
		return 0; // invalid type
	}

	return s + i - ss;
}

enum FFS_FROMINT {
	// 0000 00x0 - base
	// DEC = 0
	// FFS_INTHEX = 0x10,
	// FFS_INTOCTAL = 0x20,

	// 0000 xx00 - flags
	// FFS_INTSIGN = 0x0100,
	FFS_INTKEEPSIGN = 0x0200,
	FFS_INTHEXUP = 0x0400,
	FFS_INTZERO = 0x0800,
	FFS_INTSEP1000 = 0x1000, // use thousands separator, e.g. "1,000"

	// 00xx 0000 - width
};

#define FFS_INTWIDTH(w)  ((w) << 16)

/** Copy and add ',' after every 3rd digit */
static inline ffuint _ffs_fromint_sep1000(char *dst, ffsize cap, const char *src, ffuint len)
{
	ffuint n = len + len / 3 - !(len % 3); // 0 for "999"; 1 for "1999"
	if (cap < n)
		return 0; // not enough space

	ffuint i = n;
	ffuint j = len;
	while (j > 3) {
		dst[--i] = src[--j];
		dst[--i] = src[--j];
		dst[--i] = src[--j];
		dst[--i] = ',';
	}
	while (j != 0) {
		dst[--i] = src[--j];
	}
	return n;
}

#define FFS_INTCAP  32

static inline ffuint ffs_from_uint_10(ffuint64 i, char *dst, ffsize cap)
{
	char buf[32];
	ffuint k = sizeof(buf);

	if (i <= 0xffffffff) {
		// 32-bit division can be faster on some CPUs
		ffuint i4 = (ffuint)i;
		do {
			buf[--k] = (ffbyte)(i4 % 10 + '0');
			i4 /= 10;
		} while (i4 != 0);

	} else {
		do {
			buf[--k] = (ffbyte)(i % 10 + '0');
			i /= 10;
		} while (i != 0);
	}

	ffuint len = sizeof(buf) - k;
	if (len > cap)
		return 0; // not enough space
	ffmem_copy(dst, buf + k, len);
	return len;
}

/** Convert integer to string.
flags: enum FFS_FROMINT | FFS_INTWIDTH()
Return N of bytes written;  0 on error */
static inline ffuint ffs_fromint(ffuint64 i, char *dst, ffsize cap, ffuint flags)
{
	char buf[32];
	ffuint k = sizeof(buf);
	ffuint minus, sign;

	sign = 0;
	minus = 0;
	if ((flags & FFS_INTSIGN) && (ffint64)i < 0) {
		i = -(ffint64)i;
		minus = 1;
		sign = 1;
	} else if (flags & (FFS_INTSIGN | FFS_INTKEEPSIGN))
		sign = 1;

	switch (flags & (FFS_INTOCTAL | FFS_INTHEX)) {
	case FFS_INTOCTAL:
		do {
			buf[--k] = (ffbyte)((i & 0x07) + '0');
			i >>= 3;
		} while (i != 0);
		break;

	case 0:
		if (i <= 0xffffffff) {
			// 32-bit division can be faster on some CPUs
			ffuint i4 = (ffuint)i;
			do {
				buf[--k] = (ffbyte)(i4 % 10 + '0');
				i4 /= 10;
			} while (i4 != 0);

		} else {
			do {
				buf[--k] = (ffbyte)(i % 10 + '0');
				i /= 10;
			} while (i != 0);
		}

		if (flags & FFS_INTSEP1000) {
			if (sign)
				return 0; // not supported

			return _ffs_fromint_sep1000(dst, cap, buf + k, sizeof(buf) - k);
		}
		break;

	case FFS_INTHEX: {
		const char *phex = (flags & FFS_INTHEXUP) ? ffHEX : ffhex;
		do {
			buf[--k] = phex[i & 0x0f];
			i >>= 4;
		} while (i != 0);
		break;
	}

	default:
		return 0; // invalid flags
	}

	ffuint len = sizeof(buf) - k;
	ffuint n = 0;

	ffuint width = (flags >> 16) & 0xff;
	if (width > len + sign) {
		// "-000123"
		// "   -123"

		if (width > cap)
			return 0; // not enough space

		if (flags & FFS_INTZERO) {
			if (minus)
				dst[n++] = '-';
			else if (flags & FFS_INTKEEPSIGN)
				dst[n++] = '+';
			while (n + len != width) {
				dst[n++] = '0';
			}

		} else {
			while (n + len + sign != width) {
				dst[n++] = ' ';
			}
			if (minus)
				dst[n++] = '-';
			else if (flags & FFS_INTKEEPSIGN)
				dst[n++] = '+';
		}

		ffmem_copy(dst + n, buf + k, len);
		return n + len;
	}

	if (len + sign > cap)
		return 0; // not enough space
	if (minus)
		dst[n++] = '-';
	else if (flags & FFS_INTKEEPSIGN)
		dst[n++] = '+';
	ffmem_copy(dst + n, buf + k, len);
	return n + len;
}


/** Convert string to a floating point number
Return the number of bytes processed;
 0 on error */
static inline ffuint ffs_tofloat(const char *s, ffsize len, double *dst, ffuint flags)
{
	(void)flags;
	double d = 0;
	int minus = 0, negexp = 0, exp = 0, e = 0, digits = 0;
	ffsize i;
	enum {
		I_MINUS, I_INT, I_FRAC, I_EXPSIGN, I_EXP
	};
	int st = I_MINUS;

	for (i = 0;  i != len;  i++) {
		int ch = s[i];

		switch (st) {
		case I_MINUS:
			st = I_INT;
			if ((minus = (ch == '-')) || ch == '+')
				break;
			//fallthrough

		case I_INT:
			if (ch >= '0' && ch <= '9') {
				d = d * 10 + (ch - '0');
				digits++;
				break;
			}

			if (ch == '.') {
				st = I_FRAC;
				break;
			}
			//fallthrough

		case I_FRAC:
			if (ch >= '0' && ch <= '9') {
				d = d * 10 + (ch - '0');
				digits++;
				exp--;
				break;
			}

			if (!(ch == 'e' || ch == 'E'))
				goto proc;
			st = I_EXPSIGN;
			break;

		case I_EXPSIGN:
			st = I_EXP;
			if ((negexp = (ch == '-')) || ch == '+')
				break;
			//fallthrough

		case I_EXP:
			if (!(ch >= '0' && ch <= '9'))
				goto proc;
			e = e * 10 + ch - '0';
			break;
		}
	}

proc:
	exp += (negexp ? -e : e);

	if (digits == 0
		|| st == I_EXPSIGN
		|| exp > 308 || exp < -324)
		return 0;

	if (minus)
		d = -d;

	double p10 = 10.0;
	e = exp;
	if (e < 0)
		e = -e;

	while (e != 0) {

		if (e & 1) {
			if (exp < 0)
				d /= p10;
			else
				d *= p10;
		}

		e >>= 1;
		p10 *= p10;
	}

	*dst = d;
	return i;
}

enum FFS_FROMFLOAT {
	// 0000 00xx - precision

	// 0000 0x00 - flags
	FFS_FLTZERO = 0x0100,

	// 00xx 0000 - width
};

#define FFS_FLTWIDTH(w)  ((w) << 16)

#define FFS_FLTCAP  (32+1+32)

/** Convert float to string.
flags: precision | enum FFS_FROMFLOAT | FFS_FLTWIDTH()
Return N of bytes written;  0 on error */
static inline ffuint ffs_fromfloat(double d, char *dst, ffsize cap, ffuint flags)
{
	ffuint i = 0, minus;
	ffuint64 num, frac;

	if (isnan(d)) {
		if (3 > cap)
			return 0;
		ffmem_copy(dst, "nan", 3);
		return 3;
	}

	minus = 0;
	if (d < 0) {
		d = -d;
		minus = 1;
	}

	if (isinf(d)) {
		if (3 + minus > cap)
			return 0;
		if (minus)
			dst[i++] = '-';
		ffmem_copy(dst + i, "inf", 3);
		i += 3;
		return i;
	}

	num = (ffint64)d;

	ffuint prec = flags & 0xff;
	frac = 0;
	if (prec != 0) {
		ffuint64 scale = 1;
		for (ffuint n = 0;  n != prec;  n++) {
			scale *= 10;
		}
		frac = (ffuint64)((d - (double)num) * scale + 0.5);
		if (frac == scale) {
			num++;
			frac = 0;
		}
	}

	ffuint width = (flags >> 16) & 0xff;
	if (minus && num == 0) {
		// "-0000"
		// "   -0"

		if (width < 2)
			width = 2;

		if (1 + width > cap)
			return 0; // not enough space

		if (flags & FFS_FLTZERO) {
			dst[i++] = '-';
			while (i + 1 != width) {
				dst[i++] = '0';
			}

		} else {
			while (i + 1 + 1 != width) {
				dst[i++] = ' ';
			}
			dst[i++] = '-';
		}

		dst[i++] = '0';

	} else {
		ffuint iflags = (flags & FFS_FLTZERO) ? FFS_INTZERO : 0;
		if (minus && num != 0) {
			num = -(ffint64)num;
			iflags |= FFS_INTSIGN;
		}
		ffuint n = ffs_fromint(num, dst, cap, FFS_INTWIDTH(width) | iflags);
		if (n == 0)
			return 0;
		i += n;
	}

	if (prec != 0 && frac != 0) {
		if (i == cap)
			return 0;
		dst[i++] = '.';

		ffuint n = ffs_fromint(frac, dst + i, cap - i, FFS_INTWIDTH(prec) | FFS_INTZERO);
		if (n == 0)
			return 0;
		i += n;
	}

	return i;
}


/** Convert hex string to bytes
dst: if NULL, return the capacity needed
Return the number of bytes written;
 <0 on error */
static inline ffssize ffs_tohex(void *dst, ffsize cap, const char *s, ffsize len)
{
	if (dst == NULL)
		return len / 2;

	if (cap < len / 2
		|| (len % 2) != 0)
		return -1;

	ffsize n = 0;
	ffbyte *p = (ffbyte*)dst;
	for (ffsize i = 0;  i != len;  i += 2) {
		int h = ffchar_tohex(s[i]);
		int l = ffchar_tohex(s[i + 1]);
		if (h < 0 || l < 0)
			return -1;
		p[n++] = (h << 4) | l;
	}

	return n;
}

/** Convert bytes to hex string: [2]{0x30, 0x31} -> "3031"
flags: FFS_INTHEXUP
Return the N of bytes written */
static inline ffsize ffs_fromhex(char *dst, ffsize cap, const void *src, ffsize len, ffuint flags)
{
	if (dst == NULL)
		return len * 2;

	if (len * 2 > cap)
		return 0;

	const char *hex = !(flags & FFS_INTHEXUP) ? ffhex : ffHEX;

	for (ffsize i = 0;  i != len;  i++) {
		ffuint b = ((ffbyte*)src)[i];
		*dst++ = hex[b >> 4];
		*dst++ = hex[b & 0x0f];
	}
	return len * 2;
}
