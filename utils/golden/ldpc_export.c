/* ldpc_export — dump every codec2 LDPC code as an explicit parity-check matrix.
 *
 * codec2 stores only the information-part columns of H (H_rows, 1-based,
 * column-major: row i, slot j at H_rows[i + j*M]).  For "H1" codes
 * (NumberRowsHcols != CodeLength) the parity part is an implied staircase that
 * init_c_v_nodes() (mpdecode_core.c) materialises as check-node indices.  This
 * tool mirrors that index arithmetic exactly, so the exported H is the matrix
 * the C decoder actually uses.  MATLAB verifies it against codewords from the
 * C encoder (H*c = 0).
 *
 * Output per code: <dir>/<name>.rows  — line 1 "N M", then M lines of 1-based
 * column indices; and <dir>/codes.txt — one summary line per code.
 *
 * Copyright (C) 2026 Joseph Freivald
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "mpdecode_core.h"
#include "ldpc_codes.h"

static const char *names[] = {
    "HRA_112_112", "HRA_56_56", "H_2064_516_sparse", "HRAb_396_504",
    "H_212_158", "H_256_768_22", "H_256_512_4", "HRAa_1536_512",
    "H_128_256_5", "H_4096_8192_3d", "H_16200_9720", "H_1024_2048_4f"};

/* variable indices (0-based) touched by check row i; returns degree */
static int row_indices(const struct LDPC *l, int i, int *idx)
{
    const int N = l->CodeLength, M = l->NumberParityBits, K = N - M;
    const int H1 = (l->NumberRowsHcols == N) ? 0 : 1;
    int shift = (M + l->NumberRowsHcols) - N;
    if (!H1) shift = 0;
    int count = 0;
    for (int j = 0; j < l->max_row_weight; j++) {
        int v = l->H_rows[i + j * M];
        if (v > 0) idx[count++] = v - 1;
    }
    if (!H1) return count;
    if (shift == 0) {
        if (i == 0) { idx[count++] = K; }
        else { idx[count++] = K + i - 1; idx[count++] = K + i; }
        return count;
    }
    /* shift > 0: block staircase, mirroring init_c_v_nodes */
    int nblk = M / shift, blk = i / shift, k = i % shift;
    if (blk == 0 || blk == nblk - 1) {
        idx[count++] = (blk == nblk - 1) ? K + k + shift * blk : K + k + shift * (blk + 1);
    } else {
        idx[count++] = K + k + shift * blk;
        idx[count++] = K + k + shift * (blk + 1);
    }
    return count;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : ".";
    char path[1024];
    snprintf(path, sizeof path, "%s/codes.txt", dir);
    FILE *sum = fopen(path, "w");
    if (!sum) { perror(path); return 1; }
    fprintf(sum, "# name N M K NumberRowsHcols shift H1 max_row_w max_col_w dec_type max_iter\n");
    for (size_t c = 0; c < sizeof names / sizeof *names; c++) {
        struct LDPC l;
        ldpc_codes_setup(&l, (char *) names[c]);
        const int N = l.CodeLength, M = l.NumberParityBits;
        const int H1 = (l.NumberRowsHcols == N) ? 0 : 1;
        int shift = H1 ? (M + l.NumberRowsHcols) - N : 0;
        fprintf(sum, "%s %d %d %d %d %d %d %d %d %d %d\n", l.name, N, M, N - M,
                l.NumberRowsHcols, shift, H1, l.max_row_weight, l.max_col_weight,
                l.dec_type, l.max_iter);
        snprintf(path, sizeof path, "%s/%s.rows", dir, l.name);
        FILE *f = fopen(path, "w");
        if (!f) { perror(path); return 1; }
        fprintf(f, "%d %d\n", N, M);
        int *idx = malloc(sizeof(int) * (l.max_row_weight + 2));
        for (int i = 0; i < M; i++) {
            int d = row_indices(&l, i, idx);
            for (int j = 0; j < d; j++) fprintf(f, "%s%d", j ? " " : "", idx[j] + 1);
            fputc('\n', f);
        }
        free(idx);
        fclose(f);
        fprintf(stderr, "exported %s (N=%d M=%d H1=%d shift=%d)\n", l.name, N, M, H1, shift);
    }
    fclose(sum);
    return 0;
}
