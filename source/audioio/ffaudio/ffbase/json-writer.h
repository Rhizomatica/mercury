/** ffbase: JSON writer
2020, Simon Zolin
*/

#pragma once

#include <ffbase/json.h>

/*
ffjsonw_init ffjsonw_close
ffjsonw_addstr ffjsonw_addstrz
ffjsonw_addkey ffjsonw_addkeyz
ffjsonw_addpair ffjsonw_addpairz
ffjsonw_addnull
ffjsonw_addint ffjsonw_addnum
ffjsonw_addbool
ffjsonw_addobj ffjsonw_addarr
ffjson_escape
ffjsonw_size
ffjsonw_add
*/

enum FFJSONW_F {
	/** Indent using tabs */
	FFJSONW_FPRETTY_TABS = 1,

	/** Indent using 4 spaces */
	FFJSONW_FPRETTY_4SPACES = 2,

	/** Don't escape strings */
	FFJSONW_FDONTESCAPE = 4,
};

/** JSON writer */
typedef struct ffjsonw {
	ffuint state;
	ffuint flags; // enum FFJSONW_F
	ffvec buf;
	ffvec ctxs; // char[]
} ffjsonw;

/** Initialize writer */
static inline void ffjsonw_init(ffjsonw *j)
{
	ffmem_zero_obj(j);
}

/** Close writer */
static inline void ffjsonw_close(ffjsonw *j)
{
	ffvec_free(&j->buf);
	ffvec_free(&j->ctxs);
}


static inline int ffjsonw_add(ffjsonw *j, ffuint t, const void *src);

/** Add string */
static inline int ffjsonw_addstr(ffjsonw *j, const ffstr *s)
{
	return ffjsonw_add(j, FFJSON_TSTR, s);
}

/** Add NULL-terminated string */
static inline int ffjsonw_addstrz(ffjsonw *j, const char *sz)
{
	ffstr s;
	ffstr_setz(&s, sz);
	return ffjsonw_add(j, FFJSON_TSTR, &s);
}

static inline int ffjsonw_addkey(ffjsonw *j, const ffstr *key)
{
	return ffjsonw_add(j, FFJSON_TOBJ_KEY, key);
}

static inline int ffjsonw_addkeyz(ffjsonw *j, const char *key)
{
	ffstr s;
	ffstr_setz(&s, key);
	return ffjsonw_add(j, FFJSON_TOBJ_KEY, &s);
}

/** Add key and value */
static inline int ffjsonw_addpair(ffjsonw *j, const ffstr *key, const ffstr *val)
{
	int r;
	if ((r = ffjsonw_add(j, FFJSON_TOBJ_KEY, key)) < 0)
		return r;
	int n = r;
	if ((r = ffjsonw_add(j, FFJSON_TSTR, val)) < 0)
		return r;
	return n + r;
}

/** Add key and value NULL-terminated string */
static inline int ffjsonw_addpairz(ffjsonw *j, const char *key, const char *val)
{
	ffstr k, v;
	ffstr_setz(&k, key);
	ffstr_setz(&v, val);
	return ffjsonw_addpair(j, &k, &v);
}

/** Add null */
static inline int ffjsonw_addnull(ffjsonw *j)
{
	return ffjsonw_add(j, FFJSON_TNULL, NULL);
}

/** Add integer */
static inline int ffjsonw_addint(ffjsonw *j, ffint64 val)
{
	return ffjsonw_add(j, FFJSON_TINT, &val);
}

/** Add number */
static inline int ffjsonw_addnum(ffjsonw *j, double val)
{
	return ffjsonw_add(j, FFJSON_TNUM, &val);
}

/** Add boolean */
static inline int ffjsonw_addbool(ffjsonw *j, ffint64 val)
{
	return ffjsonw_add(j, FFJSON_TBOOL, &val);
}

/** Add object */
static inline int ffjsonw_addobj(ffjsonw *j, ffuint open)
{
	return ffjsonw_add(j, open ? FFJSON_TOBJ_OPEN : FFJSON_TOBJ_CLOSE, NULL);
}

/** Add array */
static inline int ffjsonw_addarr(ffjsonw *j, ffuint open)
{
	return ffjsonw_add(j, open ? FFJSON_TARR_OPEN : FFJSON_TARR_CLOSE, NULL);
}

/** Write JSON string and escape special characters
dst: NULL: return capacity
Return N of bytes written;
 0 on error */
static inline ffsize ffjson_escape(char *dst, ffsize cap, const char *s, ffsize len)
{
	static const char json_esc_btoch[256] = {
		1,1,1,1,1,1,1,1,'b','t','n',1,'f','r',1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
		0,0,'"',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,'\\',0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
		0,//...
	};
	ffsize n = 0;

	if (dst == NULL) {
		for (ffsize i = 0;  i < len;  i++) {
			ffuint ch = json_esc_btoch[(ffbyte)s[i]];
			if (ch == 0)
				n++;
			else if (ch == 1)
				n += FFS_LEN("\\uXXXX");
			else
				n += FFS_LEN("\\X");
		}
		return n;
	}

	for (ffsize i = 0;  i < len;  i++) {
		ffuint ch = json_esc_btoch[(ffbyte)s[i]];

		if (ch == 0) {
			dst[n++] = s[i];

		} else if (ch == 1) {
			if (n + FFS_LEN("\\uXXXX") > cap)
				return 0;

			dst[n++] = '\\';
			dst[n++] = 'u';
			dst[n++] = '0';
			dst[n++] = '0';
			dst[n++] = ffHEX[(ffbyte)s[i] >> 4];
			dst[n++] = ffHEX[(ffbyte)s[i] & 0x0f];

		} else {
			if (n + FFS_LEN("\\X") > cap)
				return 0;

			dst[n++] = '\\';
			dst[n++] = ch;
		}
	}

	return n;
}

/** Determine the maximum required capacity to convert the input data to JSON */
static inline ffsize ffjsonw_size(ffjsonw *j, ffuint t, const void *src)
{
	ffsize cap = 1; // ,

	if (j->flags & (FFJSONW_FPRETTY_TABS | FFJSONW_FPRETTY_4SPACES))
		cap += 1 + 4 * j->ctxs.len; // \n + "    "*ctxs

	switch (t) {
	case FFJSON_TSTR:
	case FFJSON_TOBJ_KEY: {
		const ffstr *s = (ffstr*)src;
		cap += 4; // '"": '
		if (j->flags & FFJSONW_FDONTESCAPE)
			cap += s->len;
		else
			cap += ffjson_escape(NULL, 0, s->ptr, s->len);
		break;
	}

	case FFJSON_TINT:
		cap += FFS_INTCAP;
		break;

	case FFJSON_TNUM:
		cap += FFS_FLTCAP;
		break;

	case FFJSON_TNULL:
	case FFJSON_TBOOL:
		cap += FFS_LEN("false"); // true | false | null
		break;

	case FFJSON_TOBJ_OPEN:
	case FFJSON_TARR_OPEN:
	case FFJSON_TOBJ_CLOSE:
	case FFJSON_TARR_CLOSE:
		cap++;
	}
	return cap;
}

/** Add 1 JSON element
Reallocate buffer by twice the size
Allows values without a global context
Allows a value without a key in object context
Allows a key after a key in object context
Allows a key in array context

Format:
[,] [WHITESPACE] ["] INPUT ["] [:] [ ]

t: enum FFJSON_T
src: int64 | double | ffstr
Return N of bytes written;
 <0 on error */
static inline int ffjsonw_add(ffjsonw *j, ffuint t, const void *src)
{
	enum {
		COMMA = 0x10,
		WHSPACE = 0x20,
	};

	ffsize r = ffjsonw_size(j, t, src);
	if (NULL == ffvec_growtwiceT(&j->buf, r, char))
		return -FFJSON_ESYS;

	ffsize oldlen = j->buf.len;

	switch (t) {
	case FFJSON_TSTR:
	case FFJSON_TOBJ_KEY:
	case FFJSON_TINT:
	case FFJSON_TNUM:
	case FFJSON_TNULL:
	case FFJSON_TBOOL:
	case FFJSON_TOBJ_OPEN:
	case FFJSON_TARR_OPEN:
		if (j->flags & COMMA) {
			*ffstr_push(&j->buf) = ',';
		}
		// fallthrough

	case FFJSON_TOBJ_CLOSE:
	case FFJSON_TARR_CLOSE:
		if ((j->flags & (FFJSONW_FPRETTY_TABS | FFJSONW_FPRETTY_4SPACES))
			&& (j->flags & WHSPACE)
			&& j->ctxs.len != 0) {

			*ffstr_push(&j->buf) = '\n';

			ffuint fillchar = '\t';
			ffsize nfill = j->ctxs.len;

			if (t == FFJSON_TOBJ_CLOSE || t == FFJSON_TARR_CLOSE)
				nfill--;

			if (j->flags & FFJSONW_FPRETTY_4SPACES) {
				fillchar = ' ';
				nfill *= 4;
			}

			ffstr_addfill((ffstr*)&j->buf, j->buf.cap, fillchar, nfill);
		}
	}

	switch (t) {
	case FFJSON_TSTR:
	case FFJSON_TOBJ_KEY:
		if (t == FFJSON_TSTR)
			j->flags |= COMMA | WHSPACE;
		else
			j->flags &= ~(COMMA | WHSPACE);

		{
		const ffstr *s = (ffstr*)src;
		*ffstr_push(&j->buf) = '\"';
		if (j->flags & FFJSONW_FDONTESCAPE)
			ffstr_add((ffstr*)&j->buf, j->buf.cap, s->ptr, s->len);
		else
			j->buf.len += ffjson_escape(ffstr_end(&j->buf), ffvec_unused(&j->buf), s->ptr, s->len);
		*ffstr_push(&j->buf) = '\"';
		}

		if (t == FFJSON_TOBJ_KEY) {
			*ffstr_push(&j->buf) = ':';

			if (j->flags & (FFJSONW_FPRETTY_TABS | FFJSONW_FPRETTY_4SPACES))
				*ffstr_push(&j->buf) = ' ';
		}
		break;

	case FFJSON_TINT: {
		j->flags |= COMMA | WHSPACE;

		const ffint64 *i = (ffint64*)src;
		j->buf.len += ffs_fromint(*i, ffstr_end(&j->buf), ffvec_unused(&j->buf), FFS_INTSIGN);
		break;
	}

	case FFJSON_TNUM: {
		j->flags |= COMMA | WHSPACE;

		const double *d = (double*)src;
		j->buf.len += ffs_fromfloat(*d, ffstr_end(&j->buf), ffvec_unused(&j->buf), 6);
		break;
	}

	case FFJSON_TNULL:
		j->flags |= COMMA | WHSPACE;

		ffstr_add((ffstr*)&j->buf, j->buf.cap, "null", 4);
		break;

	case FFJSON_TBOOL: {
		j->flags |= COMMA | WHSPACE;

		const ffint64 *i = (ffint64*)src;
		ffstr b = FFSTR_INIT("false");
		if (*i != 0)
			ffstr_setz(&b, "true");
		ffstr_add2((ffstr*)&j->buf, j->buf.cap, &b);
		break;
	}

	case FFJSON_TOBJ_OPEN:
		j->flags &= ~COMMA;
		j->flags |= WHSPACE;

		*ffstr_push(&j->buf) = '{';
		*ffvec_pushT(&j->ctxs, char) = '}';
		break;

	case FFJSON_TARR_OPEN:
		j->flags &= ~COMMA;
		j->flags |= WHSPACE;

		*ffstr_push(&j->buf) = '[';
		*ffvec_pushT(&j->ctxs, char) = ']';
		break;

	case FFJSON_TOBJ_CLOSE:
	case FFJSON_TARR_CLOSE: {
		j->flags |= COMMA | WHSPACE;

		if (j->ctxs.len == 0)
			return -FFJSON_EFIN;

		char ch = (t == FFJSON_TOBJ_CLOSE) ? '}' : ']';
		if (ch != *ffstr_last(&j->ctxs))
			return -FFJSON_EBADCTX;

		j->ctxs.len--;
		*ffstr_push(&j->buf) = ch;
		break;
	}

	default:
		return -FFJSON_EBADCHAR;
	}

	return j->buf.len - oldlen;
}
