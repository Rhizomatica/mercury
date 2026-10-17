/** ffbase: murmur hash 3
MurmurHash3 was written by Austin Appleby, and is placed in the public
domain. The author hereby disclaims copyright to this source code.
2020, Simon Zolin
*/

#pragma once
#define _FFBASE_MURMURHASH3_H

#ifndef _FFBASE_BASE_H
#include <ffbase/base.h>
#endif


static inline ffuint murmurhash3(const void *key, ffsize len, ffuint seed)
{
	ffsize nblocks = len / 4;
	ffuint h1 = seed;

	// body
	const ffuint *blocks = (ffuint*)key;

	for (ffsize i = 0;  i < nblocks;  i++) {
		ffuint k1 = blocks[i];

		k1 *= 0xcc9e2d51;
		k1 = (k1 << 15) | (k1 >> (32 - 15));
		k1 *= 0x1b873593;

		h1 ^= k1;
		h1 = (h1 << 13) | (h1 >> (32 - 13));
		h1 = h1 * 5 + 0xe6546b64;
	}

	// tail
	const ffbyte *tail = (ffbyte*)(blocks + nblocks);
	ffuint k1 = 0;

	switch(len & 3) {
	case 3:
		k1 ^= tail[2] << 16;
		// fallthrough
	case 2:
		k1 ^= tail[1] << 8;
		// fallthrough
	case 1:
		k1 ^= tail[0];
		k1 *= 0xcc9e2d51;
		k1 = (k1 << 15) | (k1 >> (32 - 15));
		k1 *= 0x1b873593;
		h1 ^= k1;
	}

	// finalization
	h1 ^= len;
	h1 ^= h1 >> 16;
	h1 *= 0x85ebca6b;
	h1 ^= h1 >> 13;
	h1 *= 0xc2b2ae35;
	h1 ^= h1 >> 16;
	return h1;
}
