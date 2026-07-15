/* Mercury MFSK LDPC — rate-1/16 code from Mercury v1 (ROBUST_0's FEC)
 *
 * Systematic IRA encoder + normalized min-sum belief-propagation decoder over
 * the v1 quasi-cyclic Tanner graph. N=1600 coded bits, K=100 info bits.
 * LLR convention (matches mfsk_demod): LLR > 0 favours bit 0.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original matrix/algorithm)
 * Copyright (C) 2026 Rhizomatica (C port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_MFSK_LDPC_H
#define MERCURY_MFSK_LDPC_H

#define MFSK_LDPC_N       1600   /* codeword length            */
#define MFSK_LDPC_K       100    /* info bits (rate 1/16)       */
#define MFSK_LDPC_P       1500   /* parity bits (N-K)           */
#define MFSK_LDPC_CWIDTH  4       /* max check-node degree      */

extern const int mfsk_ldpc_QCmatrixC[MFSK_LDPC_P][MFSK_LDPC_CWIDTH];
extern const int mfsk_ldpc_QCmatrixEnc[MFSK_LDPC_P][MFSK_LDPC_CWIDTH - 1];

/* Systematic encode: info[K] -> coded[N] (first K bits are the info bits). */
void mfsk_ldpc_encode(const int info[MFSK_LDPC_K], int coded[MFSK_LDPC_N]);

/* Min-sum decode: channel LLRs for the N coded bits -> K info bits.
 * Returns 1 if all parity checks were satisfied (converged), 0 otherwise. */
int mfsk_ldpc_decode(const float llr[MFSK_LDPC_N], int info_out[MFSK_LDPC_K],
                     int max_iter);

#endif /* MERCURY_MFSK_LDPC_H */
