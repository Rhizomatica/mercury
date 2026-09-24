/* datalink_arq/rs_erasure.h -- systematic Reed-Solomon erasure code over GF(256)
 *
 * A block is K data pieces of P bytes.  Pieces 0..K-1 ARE the data (systematic);
 * pieces K..K+R-1 are repair, each a Cauchy combination of all data pieces.
 * The code is MDS: ANY K distinct pieces of the K+R recover the block, never
 * more -- so a receiver can say exactly how many it still needs.
 *
 * Limits: K >= 1 and K + R <= 256 (the Cauchy points must be distinct field
 * elements).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RS_ERASURE_H
#define RS_ERASURE_H

#include <stddef.h>
#include <stdint.h>

#define RS_MAX_PIECES 256

/* Repair piece j (0-based) of a block of K data pieces, P bytes each.
 * data[i] points to data piece i.  Returns 0, or -1 if the parameters are out
 * of range. */
int rs_encode_repair(int K, size_t P, const uint8_t *const *data, int j, uint8_t *out);

/* Recover the K data pieces from K distinct pieces.
 * idx[r] is the piece index (0..K+R-1) of pieces[r], r = 0..K-1.
 * On success writes data piece i to out[i] and returns 0; returns -1 on bad
 * input (duplicate indices, out of range, singular -- which cannot happen for
 * distinct indices of an MDS code). */
int rs_decode(int K, size_t P, const int *idx, const uint8_t *const *pieces, uint8_t **out);

#endif
