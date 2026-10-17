/** ffaudio
2015, Simon Zolin */

#pragma once
#include <ffbase/base.h>
#include <math.h>

struct pcm_af {
	ushort format; // enum FFAUDIO_F
	u_char channels;
	u_char interleaved :1;
	uint rate;
};

/** Bits per sample */
#define pcm_f_bits(f)  ((f) & 0xff)

/** Return 1 if objects are equal */
static inline int pcm_af_eq(const struct pcm_af *a, const struct pcm_af *b) {
	return (*(unsigned long long*)a == *(unsigned long long*)b);
}

union pcm_data {
	void *p;

	char *i8;
	short *i16;
	int *i32;
	float *f32;
	double *f64;

	u_char **pu8;
	char **pi8;
	short **pi16;
	int **pi32;
	float **pf32;
	double **pf64;
};

#define PCM_CHAN_MAX 8
#define PCM_CHAN_MASK 0x0f

#ifdef FF_SSE2
	#include <emmintrin.h>

#elif defined FF_ARM64
	#include <arm_neon.h>
#endif

#ifdef FF_ARM64
typedef float64x2_t __m128d;
/** m128[0:63] = f64; m128[64:127] = 0 */
static inline __m128d neon_load_sd(const double *d)
{
	return vsetq_lane_f64(*d, vdupq_n_f64(0), 0);
}
#endif

/** Convert FP number to integer. */
static inline int int_ftoi(double d)
{
#if defined FF_SSE2
	return _mm_cvtsd_si32(_mm_load_sd(&d));

#elif defined FF_ARM64
	return vgetq_lane_f64(vrndiq_f64(neon_load_sd(&d)), 0);

#else
	return (int)((d < 0) ? d - 0.5 : d + 0.5);
#endif
}

static inline int pcm_i32_i24(const void *p)
{
	const ffbyte *b = (ffbyte*)p;
	uint n = ((uint)b[2] << 16) | ((uint)b[1] << 8) | b[0];
	if (n & 0x00800000)
		n |= 0xff000000;
	return n;
}

static inline void pcm_i24_i32(void *p, int n)
{
	ffbyte *o = (ffbyte*)p;
	o[0] = (ffbyte)n;
	o[1] = (ffbyte)(n >> 8);
	o[2] = (ffbyte)(n >> 16);
}

#define pcm_max8  (128.0)
#define pcm_max16  (32768.0)
#define pcm_max24  (8388608.0)
#define pcm_max32  (2147483648.0)

static inline char pcm_i8_flt(float f)
{
	double d = f * pcm_max8;
	return (d < -pcm_max8) ? -0x80
		: (d > pcm_max8 - 1) ? 0x7f
		: int_ftoi(d);
}

/** Convert S8 to FLOAT */
#define pcm_flt_i8(i8)  ((double)(i8) * (1 / pcm_max8))

static inline short pcm_i16_flt(double f)
{
	double d = f * pcm_max16;
	return (d < -pcm_max16) ? -0x8000
		: (d > pcm_max16 - 1) ? 0x7fff
		: int_ftoi(d);
}

/** Convert S16 to FLOAT */
#define pcm_flt_i16(i16)  ((double)(i16) * (1 / pcm_max16))

static inline int pcm_i24_flt(double f)
{
	double d = f * pcm_max24;
	return (d < -pcm_max24) ? -0x800000
		: (d > pcm_max24 - 1) ? 0x7fffff
		: int_ftoi(d);
}

#define pcm_flt_i24(i24)  ((double)(i24) * (1 / pcm_max24))

static inline int pcm_i32_flt(double d)
{
	d *= pcm_max32;
	return (d < -pcm_max32) ? (int)-0x80000000
		: (d > pcm_max32 - 1) ? (int)0x7fffffff
		: int_ftoi(d);
}

#define pcm_flt_i32(i32)  ((double)(i32) * (1 / pcm_max32))

static inline double pcm_limf(double d)
{
	return (d > 1.0) ? 1.0
		: (d < -1.0) ? -1.0
		: d;
}

/** Set non-interleaved array from interleaved data. */
static inline char** pcm_setni(void **ni, void *b, uint fmt, uint nch)
{
	for (uint i = 0;  i != nch;  i++) {
		ni[i] = (char*)b + i * pcm_f_bits(fmt) / 8;
	}
	return (char**)ni;
}
