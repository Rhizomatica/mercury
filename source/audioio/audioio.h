/** ffbase: testing helpers
2020, Simon Zolin
*/

#include <ffbase/string.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define AUDIO_SUBSYSTEM_ALSA 0
#define AUDIO_SUBSYSTEM_PULSE 1
#define AUDIO_SUBSYSTEM_WASAPI 2
#define AUDIO_SUBSYSTEM_DSOUND 3
#define AUDIO_SUBSYSTEM_COREAUDIO 4
#define AUDIO_SUBSYSTEM_OSS 5

#if 1
static inline void test_check(int ok, const char *expr, const char *file, ffuint line, const char *func)
{
	if (ok) {
		return;
	}

	fprintf(stderr, "FAIL: %s:%u: %s: %s\n"
		, file, line, func, expr);
	abort();
}

static inline void test_check_int_int(int ok, ffint64 i1, ffint64 i2, const char *file, ffuint line, const char *func)
{
	if (ok) {
		return;
	}

#if defined _WIN32 || defined _WIN64 || defined __CYGWIN__
	fprintf(stderr, "FAIL: %s:%u: %s: %d != %d\n"
		, file, line, func
		, (int)i1, (int)i2);
#else
	fprintf(stderr, "FAIL: %s:%u: %s: %lld != %lld\n"
		, file, line, func
		, i1, i2);
#endif
	abort();
}

static inline void test_check_str_sz(int ok, ffsize slen, const char *s, const char *sz, const char *file, ffuint line, const char *func)
{
	if (ok) {
		return;
	}

	fprintf(stderr, "FAIL: %s:%u: %s: %.*s != %s\n"
		, file, line, func
		, (int)slen, s, sz);
	abort();
}

#define x(expr) \
	test_check(expr, #expr, __FILE__, __LINE__, __func__)

#define xieq(i1, i2) \
({ \
	ffint64 __i1 = (i1); \
	ffint64 __i2 = (i2); \
	test_check_int_int(__i1 == __i2, __i1, __i2, __FILE__, __LINE__, __func__); \
})

#define xseq(s, sz) \
({ \
	ffstr __s = *(s); \
	test_check_str_sz(ffstr_eqz(&__s, sz), __s.len, __s.ptr, sz, __FILE__, __LINE__, __func__); \
})

#ifdef FF_WIN

static inline int file_readall(const char *fn, ffstr *dst)
{
	(void)fn; (void)dst;
	return -1;
}

#else

/** Read file data into a new buffer */
static inline int file_readall(const char *fn, ffstr *dst)
{
	int f = open(fn, O_RDONLY);
	if (f < 0)
		return -1;

	int r = -1;
	struct stat info;
	if (0 != fstat(f, &info))
		goto end;
	ffuint64 sz = info.st_size;

	if (NULL == ffstr_realloc(dst, sz))
		goto end;

	if (sz != (ffsize)read(f, dst->ptr, sz))
		goto end;

	dst->len = sz;
	r = 0;

end:
	close(f);
	return r;
}

/** Create (overwrite) file from buffer */
static inline int file_writeall(const char *fn, const void *data, ffsize len)
{
	int rc = -1;
	int f;
	if (-1 == (f = open(fn, O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, 0666)))
		return -1;

	if (len != (ffsize)write(f, data, len))
		goto end;

	rc = 0;

end:
	rc |= close(f);
	return rc;
}

#endif

#ifdef FF_WIN
static inline ffssize test_std_write(HANDLE h, const void *data, ffsize len)
{
	DWORD written;
	if (!WriteFile(h, data, len, &written, 0))
		return -1;
	return written;
}
static inline ffssize test_stdout_write(const void *data, ffsize len)
{
	return test_std_write(GetStdHandle(STD_OUTPUT_HANDLE), data, len);
}
#else
static inline ffssize test_stdout_write(const void *data, ffsize len)
{
	return write(1, data, len);
}
#endif

/** %-formatted output to stdout
NOT printf()-compatible (see ffs_formatv()) */
static inline ffssize test_stdout_fmtv(const char *fmt, va_list va)
{
	ffstr s = {};
	ffsize cap = 0;
	ffsize r = ffstr_growfmtv(&s, &cap, fmt, va);
	if (r != 0)
		r = test_stdout_write(s.ptr, r);
	ffstr_free(&s);
	return r;
}

static inline ffssize test_stdout_fmt(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ffsize r = test_stdout_fmtv(fmt, args);
	va_end(args);
	return r;
}

/** Write %-formatted text line to stdout */
#define xlog(fmt, ...)  (void) test_stdout_fmt(fmt "\n", ##__VA_ARGS__)

static inline void xlogv(const char *fmt, va_list va)
{
	test_stdout_fmtv(fmt, va);
	test_stdout_write("\n", 1);
}
#endif
