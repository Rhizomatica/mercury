/** ffbase: JSON parser with scheme
2020, Simon Zolin
*/

#pragma once

#include <ffbase/json.h>

/*
ffjson_scheme_init ffjson_scheme_destroy
ffjson_scheme_addctx
ffjson_scheme_process
ffjson_parse_object
*/

/** Maps JSON key name to a C struct field offset or a handler function */
typedef struct ffjson_arg {
	/** Key name
	 or "*" for an array element
	 or NULL for the last element */
	const char *name;

	/**
	FFJSON_TSTR:
	 offset to ffstr or int handler(ffjson_scheme *js, void *obj, ffstr *s)
	 Inside handler() user may use ffjson_strval_acquire() to acquire buffer from parser.

	FFJSON_TINT:
	 offset to ffint64 or int handler(ffjson_scheme *js, void *obj, ffint64 i)

	FFJSON_TNUM:
	 offset to double or int handler(ffjson_scheme *js, void *obj, double d)

	FFJSON_TBOOL:
	 offset to ffbyte or int handler(ffjson_scheme *js, void *obj, ffint64 i)

	FFJSON_TARR_OPEN:
	 int handler(ffjson_scheme *js, void *obj)
	 User must call ffjson_scheme_addctx()
	 "null" is treated as "[]"

	FFJSON_TOBJ_OPEN:
	 int handler(ffjson_scheme *js, void *obj)
	 User must call ffjson_scheme_addctx()
	 "null" is treated as "{}"

	FFJSON_TANY:
	 int handler(ffjson_scheme *js, void *obj)
	 User gets JSON type from js->jtype
	 'name' MUST be "*"

	FFJSON_TARR_CLOSE or FFJSON_TOBJ_CLOSE:
	 int handler(ffjson_scheme *js, void *obj)
	 'name' MUST be NULL
	 Must be the last entry
	*/
	ffuint flags;

	/** Offset to ffstr, ffint64, double, ffbyte or int handler(ffjson_scheme *js, void *obj)
	Offset is converted to a real pointer like this:
	 ptr = current_ctx.obj + offset
	handler() returns 0 on success or enum FFJSON_E
	*/
	ffsize dst;
} ffjson_arg;

/** Find element by name */
static inline const ffjson_arg* _ffjson_arg_find(const ffjson_arg *args, const ffstr *name)
{
	for (ffuint i = 0;  args[i].name != NULL;  i++) {
		if (ffstr_eqz(name, args[i].name)) {
			return &args[i];
		}
	}
	return NULL;
}

/** Find element by name (Case-insensitive) */
static inline const ffjson_arg* _ffjson_arg_ifind(const ffjson_arg *args, const ffstr *name)
{
	for (ffuint i = 0;  args[i].name != NULL;  i++) {
		if (ffstr_ieqz(name, args[i].name)) {
			return &args[i];
		}
	}
	return NULL;
}


struct ffjson_schemectx {
	const ffjson_arg *args;
	void *obj;
};

typedef struct ffjson_scheme {
	ffjson *parser;
	ffuint jtype; // enum FFJSON_T
	ffuint flags; // enum FFJSON_SCHEMEF
	const ffjson_arg *arg;
	ffvec ctxs; // struct ffjson_schemectx[]
	const char *errmsg;
} ffjson_scheme;

/**
flags: enum FFJSON_SCHEMEF */
static inline void ffjson_scheme_init(ffjson_scheme *js, ffjson *j, ffuint flags)
{
	js->parser = j;
	js->flags = flags;
}

static inline void ffjson_scheme_destroy(ffjson_scheme *js)
{
	ffvec_free(&js->ctxs);
}

enum FFJSON_SCHEMEF {
	/** Case-insensitive key names */
	FFJSON_SCF_ICASE = 1,
};

static inline void ffjson_scheme_addctx(ffjson_scheme *js, const ffjson_arg *args, void *obj)
{
	struct ffjson_schemectx *c = ffvec_pushT(&js->ctxs, struct ffjson_schemectx);
	c->args = args;
	c->obj = obj;
}

#define _FFJSON_ERR(j, msg) \
	(j)->errmsg = msg,  -FFJSON_ESCHEME

/** Process 1 JSON element
Return 'r';
 <0 on error: enum FFJSON_E */
static inline int ffjson_scheme_process(ffjson_scheme *js, int r)
{
	if (r <= 0)
		return r;

	const ffuint MAX_OFF = 64*1024;
	int r2;
	struct ffjson_schemectx *ctx = ffslice_lastT(&js->ctxs, struct ffjson_schemectx);
	union {
		ffstr *s;
		ffint64 *i64;
		double *d;
		ffbyte *b;
		int (*func)(ffjson_scheme *js, void *obj);
		int (*func_str)(ffjson_scheme *js, void *obj, ffstr *s);
		int (*func_int)(ffjson_scheme *js, void *obj, ffint64 i);
		int (*func_float)(ffjson_scheme *js, void *obj, double d);
	} u;
	u.b = NULL;
	ffuint t = 0;
	if (js->arg != NULL) {
		t = js->arg->flags & 0x0f;

		if (t == FFJSON_TANY) {
			if (js->arg->dst < MAX_OFF)
				return _FFJSON_ERR(js, "FFJSON_TANY handler must be a function");
			t = r;
		}

		u.b = (ffbyte*)js->arg->dst;
		if (js->arg->dst < MAX_OFF)
			u.b = (ffbyte*)FF_PTR(ctx->obj, js->arg->dst);

		js->jtype = r;
	}

	// map JSON "null" to a type from scheme
	if (r == FFJSON_TNULL) {
		switch (t) {
		case FFJSON_TSTR:
		case FFJSON_TINT:
		case FFJSON_TNUM:
		case FFJSON_TBOOL:
		case FFJSON_TOBJ_OPEN:
		case FFJSON_TARR_OPEN:
			break;
		default:
			return _FFJSON_ERR(js, "invalid type in scheme");
		}

		r = t;
	}

	switch (r) {
	case FFJSON_TOBJ_OPEN:
	case FFJSON_TARR_OPEN:
		if (js->parser->ctxs.len != 1) {
			if (t != (ffuint)r)
				return _FFJSON_ERR(js, "got JSON object/array, expected something else");

			ffsize nctx = js->ctxs.len;
			if (0 != (r2 = u.func(js, ctx->obj)))
				return -r2; // user error
			if (nctx + 1 != js->ctxs.len)
				return _FFJSON_ERR(js, "object/array handler must add a new context");
			js->arg = NULL;
		}

		if (r == FFJSON_TARR_OPEN) {
			ctx = ffslice_lastT(&js->ctxs, struct ffjson_schemectx);

			ffstr any_name;
			ffstr_setz(&any_name, "*");
			if (NULL == (js->arg = _ffjson_arg_find(ctx->args, &any_name)))
				return _FFJSON_ERR(js, "array context must have \"*\" element");
			/* 'js->arg' is always the same for array context.
			We reset its value every time this context is activated (FFJSON_T..._CLOSE) */
		}

		if (js->jtype != FFJSON_TNULL) {
			break;
		}

		ctx = ffslice_lastT(&js->ctxs, struct ffjson_schemectx);
		// treat "null" as "[]" or "{}"
		// fallthrough

	case FFJSON_TOBJ_CLOSE:
	case FFJSON_TARR_CLOSE:
		for (ffuint i = 0;  ;  i++) {
			if (ctx->args[i].name == NULL) {
				js->arg = &ctx->args[i];
				t = js->arg->flags & 0x0f;
				if (t == FFJSON_TARR_CLOSE || t == FFJSON_TOBJ_CLOSE) {
					u.b = (ffbyte*)js->arg->dst;
					if (0 != (r2 = u.func(js, ctx->obj)))
						return -r2; // user error
				}
				break;
			}
		}

		js->ctxs.len--;

		if (js->parser->ctxs.len != 0
			&& *ffslice_lastT(&js->parser->ctxs, char) == '[') {

			ctx = ffslice_lastT(&js->ctxs, struct ffjson_schemectx);

			ffstr any_name;
			ffstr_setz(&any_name, "*");
			if (NULL == (js->arg = _ffjson_arg_find(ctx->args, &any_name)))
				return _FFJSON_ERR(js, "array context must have \"*\" element");
		}
		break;

	case FFJSON_TOBJ_KEY:
		if (js->flags & FFJSON_SCF_ICASE)
			js->arg = _ffjson_arg_ifind(ctx->args, &js->parser->val);
		else
			js->arg = _ffjson_arg_find(ctx->args, &js->parser->val);
		if (js->arg == NULL)
			return _FFJSON_ERR(js, "no such key in the current context");
		break;

	case FFJSON_TSTR:
		if (t != FFJSON_TSTR)
			return _FFJSON_ERR(js, "got JSON string, expected something else");

		if (js->arg->dst < MAX_OFF) {
			ffstr_free(u.s);
			if (0 != ffjson_strval_acquire(js->parser, u.s))
				return -FFJSON_ESYS;
		} else if (0 != (r2 = u.func_str(js, ctx->obj, &js->parser->val)))
			return -r2; // user error
		break;

	case FFJSON_TINT:
		if (t == FFJSON_TINT) {
			if (js->arg->dst < MAX_OFF)
				*u.i64 = js->parser->intval;
			else if (0 != (r2 = u.func_int(js, ctx->obj, js->parser->intval)))
				return -r2; // user error
			break;
		} else if (t != FFJSON_TNUM) {
			return _FFJSON_ERR(js, "got JSON integer, expected something else");
		}

		// got JSON integer, expected JSON number
		js->parser->fltval = js->parser->intval;
		// fallthrough

	case FFJSON_TNUM:
		if (t != FFJSON_TNUM)
			return _FFJSON_ERR(js, "got JSON number, expected something else");

		if (js->arg->dst < MAX_OFF)
			*u.d = js->parser->fltval;
		else if (0 != (r2 = u.func_float(js, ctx->obj, js->parser->fltval)))
			return -r2; // user error
		break;

	case FFJSON_TBOOL:
		if (t != FFJSON_TBOOL)
			return _FFJSON_ERR(js, "got JSON boolean, expected something else");

		if (js->arg->dst < MAX_OFF)
			*u.b = !!js->parser->intval;
		else if (0 != (r2 = u.func_int(js, ctx->obj, js->parser->intval)))
			return -r2; // user error
		break;
	}

	return r;
}

/** Convert JSON data into a C object
scheme_flags: enum FFJSON_SCHEMEF
parser_flags: enum FFJSON_F
errmsg: (optional) error message; must free with ffstr_free()
Return 0 on success
 <0 on error: enum FFJSON_E */
static inline int ffjson_parse_object(const ffjson_arg *args, void *obj, ffstr *data, ffuint scheme_flags, ffuint parser_flags, ffstr *errmsg)
{
	int r, r2;
	ffjson j = {};
	ffjson_init(&j);
	j.flags = parser_flags;

	ffjson_scheme js = {};
	ffjson_scheme_init(&js, &j, scheme_flags);
	ffjson_scheme_addctx(&js, args, obj);

	for (;;) {
		r = ffjson_parse(&j, data);
		if (r < 0)
			goto end;
		else if (r == 0)
			break;

		r = ffjson_scheme_process(&js, r);
		if (r < 0)
			goto end;
	}

end:
	ffjson_scheme_destroy(&js);
	r2 = ffjson_fin(&j);
	if (r == 0)
		r = r2;

	if (r != 0 && errmsg != NULL) {
		ffsize cap = 0;
		const char *err = ffjson_errstr(r);
		if (r == -FFJSON_ESCHEME)
			err = js.errmsg;
		ffstr_growfmt(errmsg, &cap, "%u:%u: %s"
			, (int)j.line, (int)j.linechar
			, err);
	}

	return r;
}

#undef _FFJSON_ERR
