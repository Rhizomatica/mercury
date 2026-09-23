/* tests/sim/rs_erasure.c -- systematic Cauchy Reed-Solomon erasure code
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Generator (K+R) x K:  the identity on top (data pieces as sent), then R
 * Cauchy rows  c[j][i] = 1 / (x_j + y_i)  with x_j = j and y_i = R + i -- all
 * distinct, so every square submatrix of the Cauchy part is invertible and any
 * K rows of the whole generator are too (MDS).  "+" in GF(2^8) is XOR.
 * R here is not fixed per block: y_i is offset by RS_MAX_PIECES - K instead,
 * so repair piece j exists for every j < RS_MAX_PIECES - K and the encoder
 * need not know in advance how many repair pieces it will send.
 */
#include "rs_erasure.h"

#include <string.h>

static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static int     gf_ready;

static void gf_init(void)
{
    if (gf_ready) return;
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;              /* x^8 + x^4 + x^3 + x^2 + 1 */
    }
    for (int i = 255; i < 512; i++) gf_exp[i] = gf_exp[i - 255];
    gf_ready = 1;
}

static inline uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (!a || !b) return 0;
    return gf_exp[gf_log[a] + gf_log[b]];
}

static inline uint8_t gf_inv(uint8_t a)            /* a != 0 */
{
    return gf_exp[255 - gf_log[a]];
}

/* dst ^= c * src over n bytes */
static void gf_axpy(uint8_t *dst, const uint8_t *src, uint8_t c, size_t n)
{
    if (!c) return;
    if (c == 1) { for (size_t k = 0; k < n; k++) dst[k] ^= src[k]; return; }
    int lc = gf_log[c];
    for (size_t k = 0; k < n; k++)
        if (src[k]) dst[k] ^= gf_exp[lc + gf_log[src[k]]];
}

/* Generator coefficient for piece `idx`, data column i. */
static uint8_t gen_coef(int K, int idx, int i)
{
    if (idx < K) return (uint8_t)(idx == i);
    int j = idx - K;                            /* repair row */
    int y = (RS_MAX_PIECES - K) + i;            /* disjoint from x = j < 256-K */
    return gf_inv((uint8_t)(j ^ y));
}

int rs_encode_repair(int K, size_t P, const uint8_t *const *data, int j, uint8_t *out)
{
    gf_init();
    if (K < 1 || K >= RS_MAX_PIECES || j < 0 || j >= RS_MAX_PIECES - K) return -1;
    memset(out, 0, P);
    for (int i = 0; i < K; i++)
        gf_axpy(out, data[i], gen_coef(K, K + j, i), P);
    return 0;
}

int rs_decode(int K, size_t P, const int *idx, const uint8_t *const *pieces, uint8_t **out)
{
    gf_init();
    if (K < 1 || K >= RS_MAX_PIECES) return -1;

    /* Square system  M * data = pieces,  M[r][i] = gen_coef(idx[r], i).
     * Gauss-Jordan on M, applying the same row operations to the payloads. */
    static uint8_t M[RS_MAX_PIECES][RS_MAX_PIECES];
    int seen[RS_MAX_PIECES] = {0};
    for (int r = 0; r < K; r++) {
        if (idx[r] < 0 || idx[r] >= RS_MAX_PIECES || seen[idx[r]]) return -1;
        seen[idx[r]] = 1;
        for (int i = 0; i < K; i++) M[r][i] = gen_coef(K, idx[r], i);
        memcpy(out[r], pieces[r], P);
    }
    for (int col = 0; col < K; col++) {
        int piv = -1;
        for (int r = col; r < K; r++) if (M[r][col]) { piv = r; break; }
        if (piv < 0) return -1;
        if (piv != col) {
            for (int i = 0; i < K; i++) { uint8_t t = M[col][i]; M[col][i] = M[piv][i]; M[piv][i] = t; }
            uint8_t *t = out[col]; out[col] = out[piv]; out[piv] = t;
        }
        uint8_t inv = gf_inv(M[col][col]);
        if (inv != 1) {
            for (int i = 0; i < K; i++) M[col][i] = gf_mul(M[col][i], inv);
            for (size_t k = 0; k < P; k++) out[col][k] = gf_mul(out[col][k], inv);
        }
        for (int r = 0; r < K; r++) {
            if (r == col || !M[r][col]) continue;
            uint8_t f = M[r][col];
            for (int i = 0; i < K; i++) M[r][i] ^= gf_mul(f, M[col][i]);
            gf_axpy(out[r], out[col], f, P);
        }
    }
    return 0;
}
