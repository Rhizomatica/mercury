#ifndef NANORQ_H
#define NANORQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NANORQ_SYM_DUP 2
#define NANORQ_SYM_IGN 1
#define NANORQ_SYM_ADDED 0
#define NANORQ_SYM_ERR -1
#define NANORQ_MAX_TRANSFER 946270874880ULL // ~881 GB

typedef struct nanorq nanorq;

// returns a new encoder configured with given parameters
nanorq *nanorq_encoder_new(size_t len, uint16_t T, uint8_t Al);
nanorq *nanorq_encoder_new_ex(size_t len, uint16_t T, uint16_t K, uint16_t Z,
                              uint8_t Al);

// returns success of generating symbols for a given sbn
bool nanorq_generate_symbols(nanorq *rq, uint8_t sbn, struct ioctx *io);

// frees up any resources used by a decoder/encoder
void nanorq_free(nanorq *rq);

// returns basic parameters to initialize a decoder
uint64_t nanorq_oti_common(nanorq *rq);

/*
 * HERMES size-optimized helpers.  LOCAL ADDITION -- not upstream nanorq.
 *
 * Mercury's broadcast frames are tiny (the robust modes carry 14 bytes), so the
 * standard 8-byte common OTI, 4-byte scheme-specific OTI and 4-byte payload tag
 * do not fit.  These write the same fields in the minimum bytes that actually
 * carry information:
 *
 *   tag_reduced                 3 B  sbn + 16-bit esi   (vs 4)
 *   oti_common_reduced          5 B  24-bit F + 16-bit T-1 (vs 8)
 *   oti_scheme_specific_align1  3 B  Z-1 + 16-bit N-1   (vs 4; Al is always 1)
 *
 * The ESI is capped at 16 bits as a consequence -- see nanorq_set_max_esi() use
 * in transmitter.c.  Round-tripped by rq_roundtrip_test.c; if you re-vendor
 * nanorq, re-apply these and re-run that test.
 */
uint8_t *nanorq_tag_reduced(uint8_t sbn, uint32_t esi, uint8_t *buffer);
uint8_t *nanorq_oti_scheme_specific_align1(nanorq *rq, uint8_t *buffer);
uint8_t *nanorq_oti_common_reduced(nanorq *rq, uint8_t *buffer);

// returns extended parameters to initialize a decoder
uint32_t nanorq_oti_scheme_specific(nanorq *rq);

// returns the total size of the payload to be transferred
size_t nanorq_transfer_length(nanorq *rq);

// returns the symbol size in bytes (T)
size_t nanorq_symbol_size(nanorq *rq);

// returns number of blocks (SBN's)
size_t nanorq_blocks(nanorq *rq);

// returns the number of symbol rows per sbn block
size_t nanorq_block_symbols(nanorq *rq, uint8_t sbn);

/*
 * Tag parsing
 */
uint32_t nanorq_tag(uint8_t sbn, uint32_t esi);

/*
 * TSNC Recoding API
 */
int nanorq_decoder_add_recoded_symbol(nanorq *rq, void *data, uint32_t tag,
                                      const uint8_t *coefs, struct ioctx *io);

bool nanorq_generate_recoded_symbol(nanorq *rq, struct ioctx *io, uint8_t sbn,
                                    uint32_t esi, uint8_t *out_coefs,
                                    void *out_payload);

// return the max number of blocks allowed
size_t nanorq_max_blocks(nanorq *rq);

// precalculate precode matrix inversion
bool nanorq_precalculate(nanorq *rq);

// return the number of bytes written for a given sbn and esi encode request
size_t nanorq_encode(nanorq *rq, void *data, uint32_t esi, uint8_t sbn,
                     struct ioctx *io);

// cleanup encoder resources of a given block
void nanorq_encoder_cleanup(nanorq *rq, uint8_t sbn);

// reset internal state
void nanorq_encoder_reset(nanorq *rq, uint8_t sbn);

// returns a new decoder initialized with given parameters
nanorq *nanorq_decoder_new(uint64_t common, uint32_t specific);

// set the largest encoding symbol id allowed
bool nanorq_set_max_esi(nanorq *rq, uint32_t max_esi);

// returns the result of adding a symbol to the decoder, see NANORQ_SYM_*
int nanorq_decoder_add_symbol(nanorq *rq, void *data, uint32_t tag,
                              struct ioctx *io);

// returns number of symbol gaps in decoder for given block
size_t nanorq_num_missing(nanorq *rq, uint8_t sbn);

// returns number of repair symbols in decoder for given block
size_t nanorq_num_repair(nanorq *rq, uint8_t sbn);

// return whether or not sbn was successfully repaired
bool nanorq_repair_block(nanorq *rq, struct ioctx *io, uint8_t sbn);

#ifdef __cplusplus
}
#endif

#endif
