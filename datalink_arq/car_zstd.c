#include "car_zstd.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef ARQ_HAVE_ZSTD
#include <zstd.h>
#endif

/* The level: the sketch's "moderate".  zstd's default (3) measured 2.63x
 * on the #506 corpus at a fraction of gzip -6's CPU; the session's CPU
 * budget is a rounding error next to the airtime it buys
 * (docs/EFFICIENCY-FINDINGS.md). */
#define CAR_ZSTD_LEVEL 3

bool car_zstd_available(void)
{
#ifdef ARQ_HAVE_ZSTD
    return true;
#else
    return false;
#endif
}

void *car_zstd_stream_open(bool compress)
{
#ifdef ARQ_HAVE_ZSTD
    return compress ? (void *)ZSTD_createCStream() : (void *)ZSTD_createDStream();
#else
    (void)compress;
    return NULL;
#endif
}

void car_zstd_stream_free(void *stream, bool compress)
{
#ifdef ARQ_HAVE_ZSTD
    if (!stream) return;
    if (compress) ZSTD_freeCStream((ZSTD_CStream *)stream);
    else          ZSTD_freeDStream((ZSTD_DStream *)stream);
#else
    (void)stream; (void)compress;
#endif
}

size_t car_zstd_bound(size_t len)
{
#ifdef ARQ_HAVE_ZSTD
    return ZSTD_compressBound(len);
#else
    return len;
#endif
}

size_t car_zstd_compress_block(void *stream, const uint8_t *in, size_t len,
                               uint8_t *out, size_t out_cap)
{
#ifdef ARQ_HAVE_ZSTD
    if (!stream || !len) return 0;
    ZSTD_inBuffer zi = { .src = in,  .size = len, .pos = 0 };
    ZSTD_outBuffer zo = { .dst = out, .size = out_cap, .pos = 0 };
    /* The flush ends the block: what comes out decodes on its own, given
     * the stream's carried context. */
    size_t r = ZSTD_compressStream2((ZSTD_CStream *)stream, &zo, &zi, ZSTD_e_end);
    if (r != 0 || zi.pos != len) return 0;    /* not finished: store instead */
    return zo.pos;
#else
    (void)stream; (void)in; (void)len; (void)out; (void)out_cap;
    return 0;
#endif
}

size_t car_zstd_decompress_block(void *stream, const uint8_t *in, size_t len,
                                 uint8_t *out, size_t app_max)
{
#ifdef ARQ_HAVE_ZSTD
    if (!stream) return (size_t)-1;
    ZSTD_inBuffer zi = { .src = in,  .size = len, .pos = 0 };
    ZSTD_outBuffer zo = { .dst = out, .size = app_max, .pos = 0 };
    while (zi.pos < zi.size && zo.pos < zo.size) {
        size_t r = ZSTD_decompressStream((ZSTD_DStream *)stream, &zo, &zi);
        if (ZSTD_isError(r)) return (size_t)-1;
        if (r == 0) break;                    /* frame complete */
    }
    if (zi.pos != zi.size) return (size_t)-1; /* trailing bytes: not ours */
    return zo.pos;
#else
    (void)stream; (void)in; (void)len; (void)out; (void)app_max;
    return (size_t)-1;
#endif
}
