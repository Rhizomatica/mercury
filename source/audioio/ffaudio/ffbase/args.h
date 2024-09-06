/** Match command-line arguments against predefined schema.
2023, Simon Zolin */

/*
ffarg_merge
ffargs_process_argv
ffargs_process_line
*/

/* Usage example:

struct S {
	int abc, def;
};

static int S_abc(struct S *o, ffint64 v)
{
	if (v == 0) return 1;

	o->abc = v;
	return 0;
}

#define O(m)  (void*)(ffsize)FF_OFF(struct S, m)
static const struct ffarg schema[] = {
	{ "--abc",	'd',	S_abc },
	{ "--def",	'd',	O(def) },
	{ "-a",		'd',	S_abc },
	{ "-d",		'd',	O(def) },
	{}
};
#undef O

void main(int argc, const char **argv)
{
	struct S obj = {};
	struct ffargs args = {};
	int r = ffargs_process_argv(&args, schema, &obj, 0, argv, argc);
}
*/

#pragma once
#include <ffbase/string.h>
#include <ffbase/stringz.h>

#define _FFARG_MAXNAME 20

struct ffarg {
	/** Argument name.
	Case-sensitive, sorted in ascending alphabetic order.
	To match any argument use "\0\1".
	The last entry name must be "". */
	char name[_FFARG_MAXNAME];

	/**
	'1'		switch, byte
	'u'		unsigned int-32
	'd'		signed int-32
	'U'		unsigned int-64
	'D'		signed int-64
	'F'		double

	's'		char* NULL-terminated string
	'S'		ffstr string
	Modifiers:
	'='		Copy data rather than just assign pointers

	'0'		function

	'>'		new sub-context
	'{'		new sub-context function

	Modifiers:
	'+'		Allow multiple occurrences
	*/
	ffuint flags;

	/** Offset of a target field in a 'struct' or a function pointer */
	const void *value;
};

/** Merge two sorted arrays into one */
static inline ffssize ffarg_merge(struct ffarg *dst, ffsize cap
	, const struct ffarg *a, ffsize na
	, const struct ffarg *b, ffsize nb
	, ffuint flags)
{
	(void)flags;
	if (na + nb > cap) return -1;

	ffssize n = 0;
	ffsize ia = 0, ib = 0;
	int r;

	while (ia < na && ib < nb) {

		if (a[ia].name[0] == '\0'
			|| b[ib].name[0] == '\0') {

			ffushort sa = *(ffushort*)a[ia].name
				, sb = *(ffushort*)b[ib].name;

			if (sa == sb)
				return -1; // both arrays have the same key
			else if (sa > sb)
				r = -1;
			else
				r = 1;

		} else {
			r = ffsz_cmp(a[ia].name, b[ib].name);
		}

		if (r == 0)
			return -1; // both arrays have the same key
		else if (r < 0)
			dst[n] = a[ia++];
		else
			dst[n] = b[ib++];

		n++;
	}

	while (ia < na) {
		dst[n++] = a[ia++];
	}

	while (ib < nb) {
		dst[n++] = b[ib++];
	}

	return n;
}

enum FFARGS_E {
	FFARGS_E_OK,
	FFARGS_E_ARG,
	FFARGS_E_DUP,
	FFARGS_E_VAL,
	FFARGS_E_INT,
	FFARGS_E_FLOAT,
	FFARGS_E_REDIR,
	FFARGS_E_SCHEMA,
	FFARGS_E_NOMEM,
};

struct ffarg_ctx {
	const struct ffarg *scheme;
	void *obj;
};

enum FFARGS_OPT {
	/** Match the incompletely specified arguments by just their prefix */
	FFARGS_O_PARTIAL = 1,

	/** Fail on meeting duplicate argument unless it has '+' modifier */
	FFARGS_O_DUPLICATES = 2,

	/** Skip the first argument (for system command line it is the executable name) */
	FFARGS_O_SKIP_FIRST = 4,
};

struct ffargs {
	ffstr line;
	char **argv;
	ffuint argc, argi;
	struct ffarg_ctx ax;
	ffuint options; // enum FFARGS_OPT
	ffuint used_bits[2];
	char error[250];
};

#define _FFARG_TYPE(a)  (a->flags & 0xff)
#define _FFARG_ANY(a)  (a->name[0] == '\0' && a->name[1] == '\1')
#define _FFARG_MULTI(a)  ((a->flags & 0xff00) >> 8 == '+')
#define _FFARG_COPY(a)  ((a->flags & 0xff00) >> 8 == '=')

/** Set error message */
static int _ffargs_err(struct ffargs *as, int e, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	ffsz_formatv(as->error, sizeof(as->error), fmt, va);
	va_end(va);
	return -e;
}

union _ffarg_val {
	void *ptr;
	char *b;		// '1'
	int *i32;		// 'd', 'u'
	ffint64 *i64;	// 'D', 'U', 'Z'
	double *f64;	// 'F'
	ffstr *s;		// 'S'
	char **sz;		// 's'
};
union _ffarg_func {
	const void *ptr;
	int (*f_sw)(void*);
	int (*f_str)(void*, ffstr);
	int (*f_sz)(void*, const char*);
	int (*f_int64)(void*, ffuint64);
	int (*f_float)(void*, double);
	struct ffarg_ctx (*f_ctx)(void*);
};

/** Process value */
static int _ffargs_value(struct ffargs *as, const struct ffarg *a, ffstr key, ffstr val)
{
	struct ffarg_ctx *ax = &as->ax;

	const ffuint MAX_OFF = 64*1024;
	ffsize off = (ffsize)a->value;
	union _ffarg_val u = { .ptr = FF_PTR(ax->obj, off) };
	union _ffarg_func uf = { .ptr = a->value };

	ffuint t = _FFARG_TYPE(a);
	ffuint64 i = 0;
	double d;
	ffuint f = 0;
	int r;
	switch (t) {

	case 'S':
	case 's':
		if (_FFARG_COPY(a)) {
			if (NULL == (val.ptr = ffsz_dupstr(&val)))
				return _ffargs_err(as, FFARGS_E_NOMEM, "no memory");
		}

		if (t == 'S') {
			if (off < MAX_OFF)
				*u.s = val;
			else
				return uf.f_str(ax->obj, val);

		} else {
			if (val.ptr[val.len] != '\0')
				val.ptr[val.len] = '\0';

			if (off < MAX_OFF)
				*u.sz = val.ptr;
			else
				return uf.f_sz(ax->obj, val.ptr);
		}
		break;

	case 'D':
	case 'd':
		f |= FFS_INTSIGN;
		// fallthrough
	case 'U':
	case 'Z':
	case 'u':
		if (t == 'D' || t == 'U' || t == 'Z')
			f |= FFS_INT64;
		else
			f |= FFS_INT32;

		r = ffs_toint(val.ptr, val.len, &i, f);
		if (r == 0 || (ffuint)r != val.len) {

			if (t == 'Z' && (ffuint)r + 1 == val.len) {
				switch (val.ptr[val.len - 1] & ~0x20) {
				case 'K': i *= 1024; break;
				case 'M': i *= 1024*1024; break;
				case 'G': i *= 1024*1024*1024; break;
				default: r = 0; // error
				}

			} else {
				r = 0; // error
			}

			if (r == 0)
				return _ffargs_err(as, FFARGS_E_INT, "'%S': expected integer number, got '%S'", &key, &val);
		}

		if (off < MAX_OFF) {
			if (t == 'D' || t == 'U' || t == 'Z')
				*u.i64 = i;
			else
				*u.i32 = i;
		} else {
			return uf.f_int64(ax->obj, i);
		}
		break;

	case 'F':
		if (!ffstr_to_float(&val, &d))
			return _ffargs_err(as, FFARGS_E_FLOAT, "'%S': expected number, got '%S'", &key, &val);

		if (off < MAX_OFF)
			*u.f64 = d;
		else
			return uf.f_float(ax->obj, d);
		break;

	default:
		FF_ASSERT(0);
		return -FFARGS_E_SCHEMA;
	}

	return 0;
}

/** Process argument */
static int _ffargs_arg(struct ffargs *as, const struct ffarg *a, ffstr arg)
{
	struct ffarg_ctx *ax = &as->ax;

	const ffuint MAX_OFF = 64*1024;
	ffsize off = (ffsize)a->value;
	union _ffarg_val u = { .ptr = FF_PTR(ax->obj, off) };
	union _ffarg_func uf = { .ptr = a->value };

	ffuint pos = a - ax->scheme;
	if ((as->options & FFARGS_O_DUPLICATES)
		&& pos < sizeof(as->used_bits)*8
		&& ffbit_array_set(&as->used_bits, pos)
		&& !_FFARG_ANY(a)
		&& !_FFARG_MULTI(a))
		return _ffargs_err(as, FFARGS_E_DUP, "'%S': argument should be specified only once", &arg);

	switch (_FFARG_TYPE(a)) {
	case '{':
		as->ax = uf.f_ctx(ax->obj);  break;

	case '>':
		as->ax.scheme = (struct ffarg*)a->value;  break;

	case '1':
		if (off < MAX_OFF)
			*u.b = 1;
		else
			return uf.f_sw(ax->obj);
		return 0;

	case 0: case '0':
		if (uf.f_sw)
			return uf.f_sw(ax->obj);
		return 0;

	default:
		if (_FFARG_ANY(a))
			return _ffargs_value(as, a, arg, arg);

		return -FFARGS_E_VAL;
	}

	ffmem_zero_obj(as->used_bits);
	if (_FFARG_ANY(a))
		return -FFARGS_E_REDIR;

	return 0;
}

static const struct ffarg* _ffargs_find(struct ffargs *as, ffstr arg, ffuint flags)
{
	ffuint i;
	const struct ffarg *a;
	const struct ffarg_ctx *ax = &as->ax;

	for (i = 0;  ax->scheme[i].name[0] != '\0';  i++) {
		a = &ax->scheme[i];

		int r = ffs_cmpz_n(arg.ptr, arg.len, a->name);

		if (!r) {
			return a;

		} else if (r < 0) {
			if ((flags & FFARGS_O_PARTIAL)
				&& (ffuint)-r - 1 == arg.len // e.g. arg="-a", cursor="-abc"
				&& (ax->scheme[i+1].name[0] == '\0'
					|| !ffsz_match(ax->scheme[i+1].name, arg.ptr, arg.len)))
				return a;

			for (;  ax->scheme[i].name[0] != '\0';  i++) {}
			break;
		}
	}

	if (!(a = (ax->scheme[i].name[1] == '\1') ? &ax->scheme[i] : NULL))
		_ffargs_err(as, FFARGS_E_ARG, "unknown argument '%S'", &arg);
	return a;
}

/** Find last entry */
static const struct ffarg* _ffarg_ctx_done(const struct ffarg_ctx *ax, ffuint i)
{
	for (;  ax->scheme[i].name[0] != '\0';  i++) {}

	if (ax->scheme[i].name[1] == '\1')
		i++;
	return &ax->scheme[i];
}

/** Process argv[] array.
Return 0 on success;
  <0: enum FFARGS_E;
  >0: error code from a user function */
static inline int ffargs_process_argv(struct ffargs *as, const struct ffarg *scheme, void *obj, ffuint options,
	char **argv, ffuint argc)
{
	as->ax.scheme = scheme;
	as->ax.obj = obj;
	as->options = options;
	as->argv = argv;
	as->argc = argc;

	const struct ffarg *a;
	int expecting_value = 0;
	ffstr arg, key = {};

	if (options & FFARGS_O_SKIP_FIRST)
		as->argi++;

	while (as->argi != as->argc) {

		arg = FFSTR_Z(as->argv[as->argi]);
		as->argi++;

		if (expecting_value) {
			expecting_value = 0;
			int r = _ffargs_value(as, a, key, arg);
			if (r) return r;
			continue;
		}

		for (ffuint ir = 0; ; ir++) {
			FF_ASSERT(ir < 100); (void)ir;
			if (!(a = _ffargs_find(as, arg, options)))
				return -FFARGS_E_ARG;

			int r = _ffargs_arg(as, a, arg);
			if (r == -FFARGS_E_VAL) {
				expecting_value = 1;
				key = arg;
			} else if (r == -FFARGS_E_REDIR) {
				continue;
			} else if (r) {
				return r;
			}
			break;
		}
	}

	if (expecting_value)
		return _ffargs_err(as, FFARGS_E_VAL, "expecting value after '%S'", &key);

	int (*on_done)(void*) = (int(*)(void*))_ffarg_ctx_done(&as->ax, 0)->value;
	return (on_done) ? on_done(as->ax.obj) : 0;
}

/** Get next argument from command line.
e.g. arg0 "arg 1" arg2 */
static inline int _ffargs_next(ffstr *line, ffstr *arg)
{
	ffstr_skipchar(line, ' ');
	if (line->len == 0)
		return 1;

	int ch = ' ';
	if (line->ptr[0] == '"') {
		ffstr_shift(line, 1);
		ch = '"';
	}
	arg->ptr = line->ptr;
	ffssize i = ffstr_findchar(line, ch);
	if (i < 0) {
		i = line->len;
		ffstr_shift(line, i);
	} else {
		ffstr_shift(line, i + 1);
	}

	arg->len = i;
	return 0;
}

/** Process command-line string.
Return 0 on success;
  <0: enum FFARGS_E;
  >0: error code from a user function */
static inline int ffargs_process_line(struct ffargs *as, const struct ffarg *scheme, void *obj, ffuint options,
	const char *line)
{
	as->ax.scheme = scheme;
	as->ax.obj = obj;
	as->options = options;
	as->line = FFSTR_Z(line);

	const struct ffarg *a = NULL;
	int expecting_value = 0;
	ffstr arg, key = {};

	if (options & FFARGS_O_SKIP_FIRST)
		_ffargs_next(&as->line, &arg);

	for (;;) {

		if (_ffargs_next(&as->line, &arg))
			break;

		if (expecting_value) {
			expecting_value = 0;
			int r = _ffargs_value(as, a, key, arg);
			if (r) return r;
			continue;
		}

		for (ffuint ir = 0; ; ir++) {
			FF_ASSERT(ir < 100); (void)ir;
			if (!(a = _ffargs_find(as, arg, options)))
				return -FFARGS_E_ARG;

			int r = _ffargs_arg(as, a, arg);
			if (r == -FFARGS_E_VAL) {
				expecting_value = 1;
				key = arg;
			} else if (r == -FFARGS_E_REDIR) {
				continue;
			} else if (r) {
				return r;
			}
			break;
		}
	}

	if (expecting_value)
		return _ffargs_err(as, FFARGS_E_VAL, "expecting value after '%S'", &key);

	int (*on_done)(void*) = (int(*)(void*))_ffarg_ctx_done(&as->ax, 0)->value;
	return (on_done) ? on_done(as->ax.obj) : 0;
}
