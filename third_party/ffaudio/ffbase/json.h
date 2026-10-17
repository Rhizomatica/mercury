/** ffbase: JSON parser
2020, Simon Zolin
*/

#if 0
JSON syntax:

(null | boolean | number | "string" | object{} | array[])

boolean: true | false
number: 0 | 0.0 | 0.0e-5
string: UTF-8 text with \-escaped characters.
 Characters must be escaped: [0..0x19] (\b\f\r\n\t) 0x7f "" \
 Characters may be escaped: /
 Any UTF-16 character may be escaped with "\uXXXX".
object: { "key": value, ... }
array: [ value, ... ]

This implementation supports comments, not defined in the standard:
// one-line
/*
multi-line comment
*/
#endif

#pragma once

#ifndef _FFBASE_BASE_H
#include <ffbase/base.h>
#endif
#include <ffbase/string.h>
#include <ffbase/vector.h>

/*
ffjson_init
ffjson_parse
ffjson_fin
ffjson_errstr
ffjson_strval_acquire
ffjson_unescape
ffjson_validate
*/

typedef struct ffjson {
	ffuint state, nextstate;
	ffvec buf; // char[]: buffer for partial input and unescaped data
	ffvec ctxs; // char[]: context types

	char esc[4];
	ffuint esc_len;
	ffuint esc_hiword;

// public:
	ffuint flags; // enum FFJSON_F
	ffuint line, linechar; // line & character number

	ffstr val; // string value or object key
	ffint64 intval; // integer/boolean value
	double fltval; // number value
} ffjson;

enum FFJSON_T {
	FFJSON_TNULL = 1, // null
	FFJSON_TSTR, // "string"
	FFJSON_TINT, // 123
	FFJSON_TNUM, // 123.456
	FFJSON_TBOOL, // true | false
	FFJSON_TARR_OPEN, // [
	FFJSON_TARR_CLOSE, // ]
	FFJSON_TOBJ_OPEN, // {
	FFJSON_TOBJ_KEY, // "key":
	FFJSON_TOBJ_CLOSE, // }

	/** For use in ffjson_scheme.jtype */
	FFJSON_TANY,
};

enum FFJSON_F {
	/** Parse comments */
	FFJSON_FCOMMENTS = 1,

	/** Don't unescape data */
	FFJSON_FDONTUNESCAPE = 2,
};

enum FFJSON_E {
	FFJSON_ESYS = 1,
	FFJSON_EBADCHAR,
	FFJSON_EBADSTR,
	FFJSON_EBADESCAPE,
	FFJSON_EBADNUM,
	FFJSON_EBADINT,
	FFJSON_EBADBOOL,
	FFJSON_EBADNULL,
	FFJSON_EBADCTX,
	FFJSON_EBADCOMMENT,
	FFJSON_ELARGE,
	FFJSON_EFIN,
	FFJSON_EINCOMPLETE,
	FFJSON_ESCHEME,
};

/** Initialize before use */
static inline void ffjson_init(ffjson *j)
{
	ffmem_zero_obj(j);
	j->line = j->linechar = 1;
}

static inline int ffjson_parse(ffjson *j, ffstr *data);

/** Finalize reader
Return 0 on success
 <0: enum FFJSON_E */
static inline int ffjson_fin(ffjson *j)
{
	int r = ffjson_parse(j, NULL);
	ffvec_free(&j->buf);
	ffvec_free(&j->ctxs);
	return r;
}

/** Get error string from code (<0) */
static inline const char* ffjson_errstr(int err)
{
	if (err >= 0)
		return "";

	static const char* const jsonerr[] = {
		"FFJSON_ESYS",
		"FFJSON_EBADCHAR",
		"FFJSON_EBADSTR",
		"FFJSON_EBADESCAPE",
		"FFJSON_EBADNUM",
		"FFJSON_EBADINT",
		"FFJSON_EBADBOOL",
		"FFJSON_EBADNULL",
		"FFJSON_EBADCTX",
		"FFJSON_EBADCOMMENT",
		"FFJSON_ELARGE",
		"FFJSON_EFIN",
		"FFJSON_EINCOMPLETE",
		"FFJSON_ESCHEME",
	};
	return jsonerr[-err - 1];
}

/** Get or copy string value into a user's container */
static inline int ffjson_strval_acquire(ffjson *j, ffstr *dst)
{
	if (j->val.len == 0) {
		ffstr_null(dst);
		return 0;
	}
	if (j->buf.cap != 0) {
		FF_ASSERT(j->val.ptr == j->buf.ptr);
		*dst = j->val;
		ffvec_null(&j->buf);
		return 0;
	}
	if (NULL == ffstr_dup2(dst, &j->val))
		return -1;
	return 0;
}

/** Copy JSON string and unescape special characters
Return N of bytes copied
 0: not enough space;
 <0: incomplete or invalid input */
static inline ffssize ffjson_unescape(char *dst, ffsize cap, const char *s, ffsize len)
{
	static const char json_esc_char[] = "\"\\bfrnt/";
	static const char json_esc_byte[] = "\"\\\b\f\r\n\t/";
	ffsize n = 0;
	ffssize r;
	for (ffsize i = 0;  i < len;  ) {

		if (s[i] != '\\') {
			if (n == cap)
				return 0;
			dst[n++] = s[i];
			i++;
			continue;
		}

		i++;
		if (i == len)
			return -1;

		if (s[i] != 'u') {
			// 1-char escape
			ffssize pos = ffs_findchar(json_esc_char, FFS_LEN(json_esc_char), s[i]);
			if (pos < 0)
				return -1;

			if (n == cap)
				return 0;
			dst[n++] = json_esc_byte[pos];
			i++;
			continue;
		}

		// u-escape
		i++;
		if (i + 4 > len)
			return -1;

		ffuint u32;
		ffushort u16;
		if (4 != ffs_toint(&s[i], 4, &u16, FFS_INT16 | FFS_INTHEX))
			return -1;
		i += 4;

		if (ffutf16_basic(u16)) {
			if (0 == (r = ffutf8_encode(&dst[n], cap, u16)))
				return 0;
			n += r;
			continue;

		} else if (!ffutf16_highsurr(u16))
			return -1;

		// 2nd u-escape
		if (i + 6 > len)
			return -1;

		if (!(s[i++] == '\\'
			&& s[i++] == 'u'))
			return -1;

		u32 = u16;
		if (4 != ffs_toint(&s[i], 4, &u16, FFS_INT16 | FFS_INTHEX))
			return -1;
		i += 4;

		if (!ffutf16_lowsurr(u16))
			return -1;

		u32 = ffutf16_suppl(u32, u16);
		if (0 == (r = ffutf8_encode(&dst[n], cap, u32)))
			return 0;
		n += r;
	}
	return n;
}

/** Add (reference or copy) character
Don't copy data if internal buffer is empty
Grow buffer by twice the existing size */
static inline int _ffjson_valadd(ffjson *j, const char *d)
{
	if (j->buf.cap == 0) {
		if (j->buf.len == 0)
			j->buf.ptr = (char*)d;
		j->buf.len++;
		return 0;
	}

	if (ffvec_isfull(&j->buf)
		&& NULL == ffvec_growtwiceT(&j->buf, 128, char))
		return -1;

	*ffstr_push(&j->buf) = *d;
	return 0;
}

/** Add (copy) Unicode character */
static inline int _ffjson_valadd_u(ffjson *j, ffuint u)
{
	if (NULL == ffvec_growT(&j->buf, 4, char))
		return -1;

	j->buf.len += ffutf8_encode(ffstr_end(&j->buf), 4, u);
	return 0;
}

/** Parse next JSON element
Return enum FFJSON_T:
 FFJSON_TNULL
 FFJSON_TSTR: j->val
 FFJSON_TINT: j->intval
 FFJSON_TNUM: j->fltval
 FFJSON_TBOOL: j->intval (0 or 1)
 FFJSON_TARR_OPEN
 FFJSON_TARR_CLOSE
 FFJSON_TOBJ_OPEN
 FFJSON_TOBJ_KEY: j->val
 FFJSON_TOBJ_CLOSE
  0: need more data;
 <0: enum FFJSON_E */
static inline int ffjson_parse(ffjson *j, ffstr *data)
{
	enum {
		I_SPC_BEFORE_VAL, // -> I_CMT, I_STR, I_NUM, I_TRUE, I_FALSE, I_NULL, I_SPC_ARR, I_SPC_OBJ
		I_SPC_ARR, // -> I_CMT, I_STR, I_TRUE, I_FALSE, I_NULL
		I_CMT, I_CMT_SL, I_CMT_LN, I_CMT_MLN, I_CMT_MLN_SL,
		I_STR, // -> I_STR_ESC, I_SPC_AFTER_VAL
		I_KEYSTR, // -> I_SPC_AFTER_KEY
		I_STR_ESC, I_STR_ESC_U, I_STR_ESC_U2_BKSL, I_STR_ESC_U2,
		I_NUM, I_TRUE, I_FALSE, I_NULL, // -> I_SPC_AFTER_VAL
		I_SPC_AFTER_VAL, I_AFTER_VAL, // -> I_CMT, I_SPC_BEFORE_VAL, I_SPC_OBJ
		I_SPC_OBJ, // -> I_CMT, I_STR, I_SPC_AFTER_VAL, I_KEYSTR
		I_SPC_AFTER_KEY, // -> I_SPC_BEFORE_VAL
	};

	if (data == NULL) {
		if (j->ctxs.len == 0
			&& (j->state == I_CMT_LN
				|| j->state == I_SPC_AFTER_VAL))
			return 0;
		return -FFJSON_EINCOMPLETE;
	}

	int r = 0;
	ffuint st = j->state;
	const char *d = data->ptr;
	const char *end = ffstr_end(data);

	while (d != end) {

		ffuint ch = (ffbyte)*d;

		switch (st) {

		case I_SPC_ARR:
		case I_SPC_BEFORE_VAL:
			switch (ch) {

			case ' ': case '\t':
			case '\r': case '\n':
				break;

			case '/':
				j->nextstate = st;
				st = I_CMT;
				continue;

			case '"':
				st = I_STR;
				break;

			case 't':
				st = I_TRUE;
				j->esc_len = 1;
				break;

			case 'f':
				st = I_FALSE;
				j->esc_len = 1;
				break;

			case 'n':
				st = I_NULL;
				j->esc_len = 1;
				break;

			case '[':
				*ffvec_pushT(&j->ctxs, char) = '[';
				st = I_SPC_ARR;
				r = FFJSON_TARR_OPEN;
				break;

			case ']': // empty array []
				if (st != I_SPC_ARR)
					return -FFJSON_EBADCHAR;

				j->ctxs.len--;
				st = I_SPC_AFTER_VAL;
				r = FFJSON_TARR_CLOSE;
				break;

			case '{':
				*ffvec_pushT(&j->ctxs, char) = '{';
				st = I_SPC_OBJ;
				r = FFJSON_TOBJ_OPEN;
				break;

			default:
				st = I_NUM;
				continue;
			}
			break;

		case I_CMT:
			if (!(j->flags & FFJSON_FCOMMENTS))
				return -FFJSON_EBADCOMMENT;
			st = I_CMT_SL;
			break;

		case I_CMT_SL:
			switch (ch) {
			case '/':
				st = I_CMT_LN; break;

			case '*':
				st = I_CMT_MLN; break;

			default:
				return -FFJSON_EBADCOMMENT;
			}
			break;

		case I_CMT_LN:
			if (ch == '\n')
				st = j->nextstate;
			break;

		case I_CMT_MLN:
			if (ch == '*')
				st = I_CMT_MLN_SL;
			break;

		case I_CMT_MLN_SL:
			if (ch == '/')
				st = j->nextstate;
			else
				st = I_CMT_MLN;
			break;

		case I_STR:
		case I_KEYSTR:
			switch (ch) {
			case '"':
				if (st == I_KEYSTR) {
					r = FFJSON_TOBJ_KEY;
					st = I_SPC_AFTER_KEY;
				} else {
					r = FFJSON_TSTR;
					st = I_SPC_AFTER_VAL;
				}
				ffstr_set2(&j->val, &j->buf);
				break;

			case '\\':
				if (!(j->flags & FFJSON_FDONTUNESCAPE)) {
					j->esc_len = 0;
					j->esc_hiword = 0;
					j->nextstate = st;
					st = I_STR_ESC;
					break;
				}
				// fallthrough

			default:
				if (ch < 0x20 || ch == 0x7f)
					return -FFJSON_EBADSTR;

				if (0 != _ffjson_valadd(j, d))
					return -FFJSON_ESYS;
			}
			break;

		case I_STR_ESC: {
			static const char json_esc_char[] = "\"\\bfrnt/";
			static const char json_esc_byte[] = "\"\\\b\f\r\n\t/";
			ffssize pos;
			if (0 <= (pos = ffs_findchar(json_esc_char, FFS_LEN(json_esc_char), ch))) {
				if (0 != _ffjson_valadd_u(j, json_esc_byte[pos]))
					return -FFJSON_ESYS;
				st = j->nextstate;

			} else if (ch == 'u') {
				st = I_STR_ESC_U;

			} else {
				return -FFJSON_EBADESCAPE; // bad escape character after backslash
			}
			break;
		}

		case I_STR_ESC_U2_BKSL:
			if (ch != '\\')
				return -FFJSON_EBADESCAPE; // expected 'u' escape low surrogate pair
			st = I_STR_ESC_U2;
			break;

		case I_STR_ESC_U2:
			if (ch != 'u')
				return -FFJSON_EBADESCAPE; // expected 'u' escape low surrogate pair
			st = I_STR_ESC_U;
			break;

		case I_STR_ESC_U: {
			j->esc[j->esc_len++] = ch;
			if (j->esc_len != 4)
				break;

			ffuint u32;
			ffushort u16;
			if (4 != ffs_toint(j->esc, 4, &u16, FFS_INT16 | FFS_INTHEX))
				return -FFJSON_EBADESCAPE; // bad 'u' escape sequence

			if (ffutf16_basic(u16)) {
				if (j->esc_hiword != 0)
					return -FFJSON_EBADESCAPE; // bad 'u' escape surrogate pair
				u32 = u16;

			} else if (ffutf16_highsurr(u16)) {
				if (j->esc_hiword != 0)
					return -FFJSON_EBADESCAPE; // bad 'u' escape surrogate pair
				j->esc_hiword = u16;
				j->esc_len = 0;
				st = I_STR_ESC_U2_BKSL;
				break;

			} else {
				if (j->esc_hiword == 0)
					return -FFJSON_EBADESCAPE; // bad 'u' escape surrogate pair
				u32 = ffutf16_suppl(j->esc_hiword, u16);
			}

			if (0 != _ffjson_valadd_u(j, u32))
				return -FFJSON_ESYS;
			st = j->nextstate;
			break;
		}

		case I_NUM:
			switch (ch) {
			case '-': case '+':
			case '.':
			case 'e': case 'E':
				break;

			default:
				if (ch >= '0' && ch <= '9')
					break;

				if (j->buf.len == 0)
					return -FFJSON_EBADCHAR;

				if (j->buf.len == ffs_toint((char*)j->buf.ptr, j->buf.len, &j->intval, FFS_INT64 | FFS_INTSIGN)) {
					r = FFJSON_TINT;
					st = I_SPC_AFTER_VAL;
					goto end;

				} else if (j->buf.len == ffs_tofloat((char*)j->buf.ptr, j->buf.len, &j->fltval, 0)) {
					r = FFJSON_TNUM;
					st = I_SPC_AFTER_VAL;
					goto end;

				} else
					return -FFJSON_EBADCHAR;
			}

			if (j->buf.len == FFS_FLTCAP)
				return -FFJSON_ELARGE;

			if (0 != _ffjson_valadd(j, d))
				return -FFJSON_ESYS;
			break;

		case I_TRUE:
			if (ch != (ffuint)"true"[j->esc_len])
				return -FFJSON_EBADBOOL;  // bad 'true' boolean value

			if (++j->esc_len == FFS_LEN("true")) {
				j->intval = 1;
				r = FFJSON_TBOOL;
				st = I_SPC_AFTER_VAL;
			}
			break;

		case I_FALSE:
			if (ch != (ffuint)"false"[j->esc_len])
				return -FFJSON_EBADBOOL;  // bad 'false' boolean value

			if (++j->esc_len == FFS_LEN("false")) {
				r = FFJSON_TBOOL;
				st = I_SPC_AFTER_VAL;
			}
			break;

		case I_NULL:
			if (ch != (ffuint)"null"[j->esc_len])
				return -FFJSON_EBADNULL;

			if (++j->esc_len == FFS_LEN("null")) {
				r = FFJSON_TNULL;
				st = I_SPC_AFTER_VAL;
			}
			break;

		case I_SPC_AFTER_VAL:
			switch (ch) {

			case ' ': case '\t':
			case '\r': case '\n':
				break;

			case '/':
				j->nextstate = I_SPC_AFTER_VAL;
				st = I_CMT;
				continue;

			default:
				st = I_AFTER_VAL;
				continue;
			}
			break;

		case I_AFTER_VAL:
			j->intval = 0;
			j->fltval = 0;
			ffstr_null(&j->val);
			ffvec_free(&j->buf);

			if (j->ctxs.len == 0) {
				return -FFJSON_EFIN; //document finished. no more entities expected
			}

			switch (ch) {

			case ',':
				st = I_SPC_BEFORE_VAL;
				if (*ffslice_lastT(&j->ctxs, char) == '{')
					st = I_SPC_OBJ;
				break;

			case ']':
				if (*ffslice_lastT(&j->ctxs, char) != '[')
					return -FFJSON_EBADCTX;

				j->ctxs.len--;
				r = FFJSON_TARR_CLOSE;
				st = I_SPC_AFTER_VAL;
				break;

			case '}':
				if (*ffslice_lastT(&j->ctxs, char) != '{')
					return -FFJSON_EBADCTX;

				j->ctxs.len--;
				r = FFJSON_TOBJ_CLOSE;
				st = I_SPC_AFTER_VAL;
				break;

			default:
				return -FFJSON_EBADCHAR; // bad character after value in object/array
			}
			break;

		case I_SPC_OBJ:
			switch (ch) {

			case ' ': case '\t':
			case '\r': case '\n':
				break;

			case '/':
				j->nextstate = st;
				st = I_CMT;
				continue;

			case '"':
				st = I_KEYSTR;
				break;

			case '}':
				j->ctxs.len--;
				r = FFJSON_TOBJ_CLOSE;
				st = I_SPC_AFTER_VAL;
				break;

			default:
				return -FFJSON_EBADCHAR; // bad character in object
			}
			break;

		case I_SPC_AFTER_KEY:
			switch (ch) {

			case ' ': case '\t':
			case '\r': case '\n':
				break;

			case '/':
				j->nextstate = st;
				st = I_CMT;
				continue;

			case ':': // "key":
				ffstr_null(&j->val);
				ffvec_free(&j->buf);
				st = I_SPC_BEFORE_VAL;
				break;

			default:
				return -FFJSON_EBADCHAR; // bad character after key in object
			}
			break;

		default:
			return -FFJSON_EBADCHAR;
		}

		d++;
		j->linechar++;
		if (ch == '\n') {
			j->line++;
			j->linechar = 1;
		}

		if (r != 0)
			goto end;
	}

	// copy referenced data to internal buffer
	if (j->buf.len != 0 && j->buf.cap == 0)
		if (NULL == ffvec_growT(&j->buf, 0, char))
			return -FFJSON_ESYS;

end:
	j->state = st;
	ffstr_set(data, d, end - d);
	return r;
}

/** Parse the whole input data
Return 0 on success;
 <0: enum FFJSON_E */
static inline int ffjson_validate(ffjson *j, const char *data, ffsize len)
{
	ffstr s;
	ffstr_set(&s, data, len);

	while (s.len != 0) {
		int r = ffjson_parse(j, &s);
		if (r < 0)
			return r;
		else if (r == 0)
			return -FFJSON_EINCOMPLETE;
	}

	return ffjson_parse(j, NULL);
}
