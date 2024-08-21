/** ffbase: CRC-32C
2024, Simon Zolin */

#ifdef __SSE4_2__
#include <smmintrin.h>

static inline ffuint crc32c(ffuint crc, const void *data, ffsize len)
{
	const ffbyte *d = (ffbyte*)data;
	for (;  len >= 8;  len -= 8,  d += 8) {
		crc = _mm_crc32_u64(crc, *(ffuint64*)d);
	}

	if (len >= 4) {
		crc = _mm_crc32_u32(crc, *(ffuint*)d);
		len -= 4,  d += 4;
	}

	if (len >= 2) {
		crc = _mm_crc32_u16(crc, *(ffushort*)d);
		len -= 2,  d += 2;
	}

	if (len >= 1)
		crc = _mm_crc32_u8(crc, *d);

	return crc;
}
#endif
