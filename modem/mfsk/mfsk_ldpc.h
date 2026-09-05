/* Mercury MFSK LDPC — v1 rate ladder (1/16 .. 8/16), all N=1600
 *
 * Systematic IRA encoder + normalized min-sum belief-propagation decoder over
 * the v1 quasi-cyclic Tanner graphs.  A small ladder of rates lets the mode
 * trade robustness (low rate, deep fringe) for payload (higher rate).  All
 * codes share N=1600 coded bits (same airtime); K (info bits) grows with rate.
 * LLR convention (matches mfsk_demod): LLR > 0 favours bit 0.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original matrices/algorithm)
 * Copyright (C) 2026 Rhizomatica (C port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_MFSK_LDPC_H
#define MERCURY_MFSK_LDPC_H

#define MFSK_LDPC_MAXN 1600   /* largest codeword (all codes are N=1600)   */
#define MFSK_LDPC_MAXK 800    /* largest info block (rate 8/16)            */

/* An LDPC code. C/Enc are flat row-major arrays (P rows).  A -1 entry is
 * padding.  ewidth = cwidth - 1. */
typedef struct
{
    int N, K, P, cwidth;
    const int *C;    /* [P*cwidth]      per parity-check -> variable indices  */
    const int *Enc;  /* [P*(cwidth-1)]  per parity-bit  -> codeword XOR set   */
    const char *name;
} mfsk_ldpc_code_t;

/* The v1 rate ladder (payload = K/8 bytes): 1/16≈12.5 B .. 8/16=100 B. */
extern const mfsk_ldpc_code_t mfsk_ldpc_1_16;   /* K=100  ~12.5 B  deepest   */
extern const mfsk_ldpc_code_t mfsk_ldpc_2_16;   /* K=200  25 B               */
extern const mfsk_ldpc_code_t mfsk_ldpc_3_16;   /* K=300  37.5 B             */
extern const mfsk_ldpc_code_t mfsk_ldpc_5_16;   /* K=500  62.5 B             */
extern const mfsk_ldpc_code_t mfsk_ldpc_8_16;   /* K=800  100 B  (rate 1/2)  */

/* Systematic encode: info[c->K] -> coded[c->N] (first K bits are info). */
void mfsk_ldpc_encode(const mfsk_ldpc_code_t *c, const int *info, int *coded);

/* Min-sum decode: LLRs for the c->N coded bits -> c->K info bits.
 * Returns 1 if all parity checks were satisfied (converged), else 0. */
int mfsk_ldpc_decode(const mfsk_ldpc_code_t *c, const float *llr,
                     int *info_out, int max_iter);

#endif /* MERCURY_MFSK_LDPC_H */
