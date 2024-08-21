# Short SSE guide

## SSE2

All AMD64 CPUs support SSE2.

	dst = _mm_setzero_si128()
	Set zero integer
------

	dst = _mm_loadu_si128(addr)
	Load 128bit unaligned integer
------

	dst = _mm_load_sd(addr)
	Load 64bit unaligned double
------

	i = _mm_cvtsd_si32(d)
	Convert 64bit double -> 32bit integer
------

## SSE4.2

Check if CPU supports SSE4.2 by `cpuid.ecx.bits[20] == 1`.

	idx = _mm_cmpestri(mask, mask_len, r, r_len, _SIDD_CMP_EQUAL_ANY)

	Find any byte from `mask` in range [0..mask_len)
	  "ANY"
	in `r` in range [0..r_len)
	  "DATA" ([0]='D', [1]='A', ...)
	Return the least significant matched byte index [0..15]
	  1
	Return 16 if nothing found
------

	idx = _mm_cmpestri(mask, mask_len, r, r_len, _SIDD_CMP_RANGES)

	Find any byte from ranges [mask[0]..mask[1]], ..., [mask[mask_len-2]..mask[mask_len-1]]
	  "\x00\x20"
	in `r` in range [0..r_len)
	  "K V"
	Return the least significant matched byte index [0..15]
	  1
	Return 16 if nothing found
------
