/* car_zstd — the optional payload-compression backend (#506).
 *
 * One block at a time: the caller compresses a block's payload through a
 * session-long stream, flushed at every block boundary (so a block still
 * decodes on its own -- the floor's included -- and a stored block costs
 * the stream nothing), and the receiver feeds the block's bytes back
 * through a stream in the same order.  The wrapper answers one question
 * per side and hides the library: is there a backend, compress this,
 * decompress this.
 *
 * Without libzstd (config.mk, ARQ_ZSTD=0) every entry point compiles to a
 * stub that says so, the way arq_crypto does without libsodium: a build
 * without the backend negotiates no compression and never sets the
 * block's compressed bit.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CAR_ZSTD_H
#define CAR_ZSTD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The backend was built in (config.mk, ARQ_ZSTD).  A session offers
 * compression only when this is true. */
bool car_zstd_available(void);

/* A session-direction stream, or NULL without the backend or on
 * allocation failure.  Free with car_zstd_stream_free. */
void *car_zstd_stream_open(bool compress);

void  car_zstd_stream_free(void *stream, bool compress);

/* Compress the next block through the session stream, flushed at the
 * block's end.  @return the compressed length, or 0 without the backend
 * or on error (the caller stores the block uncompressed).  @out must have
 * car_zstd_bound(@len) bytes. */
size_t car_zstd_compress_block(void *stream, const uint8_t *in, size_t len,
                               uint8_t *out, size_t out_cap);

/* Decompress the next block through the session stream, in order.  @out
 * must be at least @app_max bytes (the largest block the session's io
 * ever hands over -- the bound the sender's tx_read already enforces).
 * @return the decompressed length, or (size_t)-1 on error. */
size_t car_zstd_decompress_block(void *stream, const uint8_t *in, size_t len,
                                 uint8_t *out, size_t app_max);

/* An upper bound on the compressed size of @len bytes.  Without the
 * backend: @len (nothing compresses). */
size_t car_zstd_bound(size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CAR_ZSTD_H */
