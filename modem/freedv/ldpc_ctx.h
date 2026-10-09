/* ldpc_ctx.h — persistent, allocation-free LDPC decoder context.
 *
 * Copyright (C) 2026 Joseph Freivald
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Builds the Tanner graph of a codec2 `struct LDPC` once, in a
 * structure-of-arrays layout (edges in check-row order plus one column-order
 * permutation), and decodes against it with no heap traffic.  Two algorithms
 * share the layout: normalised min-sum (scale-invariant, vectorisable) and the
 * phi-domain sum-product that reproduces the legacy SumProduct() results.
 * A decode can be bounded by an iteration cap and by a monotonic deadline, and
 * reports iterations, parity count and whether the deadline cut it short.
 */
#ifndef LDPC_CTX_H
#define LDPC_CTX_H

#include <stddef.h>
#include <stdint.h>

struct LDPC;
typedef struct ldpc_ctx ldpc_ctx_t;

typedef struct {
    int iters;        /* iterations run                                    */
    int parity_ok;    /* 1 if every parity check is satisfied at exit      */
    int parity_count; /* satisfied parity checks at exit                   */
    int hit_deadline; /* 1 if the deadline ended the decode                */
} ldpc_stats_t;

enum {
    LDPC_ALG_LEGACY = -1, /* original array-of-structs SumProduct (A/B only) */
    LDPC_ALG_SP     = 0,  /* phi-domain sum-product on the SoA graph         */
    LDPC_ALG_NMS    = 1,  /* normalised min-sum, float, SoA graph            */
    LDPC_ALG_NMS16  = 2,  /* normalised min-sum, int16 messages, row-blocked  */
                          /* layout (8 rows per block, lanes = rows), check   */
                          /* pass in 128-bit vector ops                       */
    LDPC_ALG_SPT    = 3,  /* sum-product with a table-driven phi (no calls)   */
    LDPC_ALG_AUTO   = 4   /* per-code policy (ldpc_ctx_policy_alg)            */
};

/* int16 message fixed point: LLR units of 1/LDPC_Q16_SCALE, saturating */
#define LDPC_Q16_SCALE 16.0f

/* Iteration cap used by the data modes (was 10 in freedv_700.c; early exit on
 * parity means clean frames still cost 2-3 iterations, and a deadline bounds
 * the worst case on slow hardware). */
#define LDPC_MAX_ITER_DEFAULT 100

ldpc_ctx_t *ldpc_ctx_create(const struct LDPC *code);
void        ldpc_ctx_destroy(ldpc_ctx_t *ctx);

size_t ldpc_ctx_bytes(const ldpc_ctx_t *ctx);   /* working memory held       */
int    ldpc_ctx_edges(const ldpc_ctx_t *ctx);
void   ldpc_ctx_set_alg(ldpc_ctx_t *ctx, int alg, float nms_scale);  /* nms_scale <= 0 keeps current */
int    ldpc_ctx_alg(const ldpc_ctx_t *ctx);

/* Decode N LLRs (positive = bit 0, codec2 convention) into N hard bits.
 * max_iter caps iterations; deadline_ns (ldpc_now_ns() scale, 0 = none) ends
 * the decode after the iteration during which it passes.  At least one
 * iteration always runs.  Returns iterations used; stats may be NULL. */
int ldpc_ctx_decode(ldpc_ctx_t *ctx, const float *llr, uint8_t *bits,
                    int max_iter, uint64_t deadline_ns, ldpc_stats_t *stats);

uint64_t ldpc_now_ns(void);

/* Per-code default algorithm (NMS16 where the MATLAB sweep shows it matches
 * belief propagation, table sum-product elsewhere) and its min-sum scale. */
int   ldpc_ctx_policy_alg(const char *code_name);
int   ldpc_alg_from_name(const char *s);   /* "auto"|"sp"|"spt"|"nms"|"nms16"|"legacy", -2 if unknown */
const char *ldpc_alg_name(int alg);
float ldpc_ctx_policy_scale(const char *code_name);

/* Variable indices (0-based) touched by check row `row`, mirroring the
 * implied-staircase arithmetic of init_c_v_nodes().  idx must hold
 * max_row_weight + 2 entries.  Returns the row degree. */
int ldpc_ctx_row_indices(const struct LDPC *code, int row, int *idx);

#endif
