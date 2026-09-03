#include "precode.h"

#define hf_at(W, i, j)                                                         \
  (uv_A((W)->F.type, (i)) ? uv_E((W)->UL, uv_A((W)->F.rowmap, (i)), (j))       \
                          : bm_at(&(W)->U, (i), (j)))

static void precode_matrix_make_identity(u32_vec *A, u32 dim, u32 x, u32 y) {
  for (u32 diag = 0; diag < dim; diag++)
    uv_push(A[x + diag], y + diag);
}

static void precode_matrix_make_LDPC1(u32_vec *A, u32 S, u32 B) {
  for (u32 col = 0; col < B; col++) {
    u32 stride = col / S;
    u32 r1 = (col % S);
    u32 r2 = (col + stride + 1) % S;
    u32 r3 = (col + 2 * (stride + 1)) % S;
    uv_push(A[r1], col);
    uv_push(A[r2], col);
    uv_push(A[r3], col);
  }
}

static void precode_matrix_make_LDPC2(u32_vec *A, u32 W, u32 S, u32 P) {
  for (u32 row = 0; row < S; row++) {
    u32 b1 = row % P;
    u32 b2 = (row + 1) % P;
    uv_push(A[row], W + b1);
    uv_push(A[row], W + b2);
  }
}

void precode_matrix_make_HDPC(params *P, pc *W) {
  u32 m = P->H, n = P->Kprime + P->S, col = n - 1;

  assert(m > 0 && n > 0);

  for (u32 row = 0; row < m; row++)
    uv_E(W->HDPC, row, col) = OCT_EXP[row];

  while (col > 0) {
    col--;
    for (u32 row = 0; row < m; row++)
      uv_E(W->HDPC, row, col) =
          (uv_E(W->HDPC, row, col + 1) == 0)
              ? 0
              : OCT_EXP[OCT_LOG[uv_E(W->HDPC, row, col + 1)] + 1];
    u32 b1 = rnd_get(col + 1, 6, m);
    u32 b2 = (b1 + rnd_get(col + 1, 7, m - 1) + 1) % m;
    uv_E(W->HDPC, b1, col) ^= 1;
    uv_E(W->HDPC, b2, col) ^= 1;
  }
}

static void precode_matrix_make_G_ENC(params *P, u32_vec *A) {
  for (u32 row = P->S + P->H; row < P->L; row++)
    params_set_idxs(P, row - P->S - P->H, &A[row]);
}

void precode_matrix_gen(params *P, pc *W) {
  precode_matrix_make_LDPC1(W->A, P->S, P->B);
  precode_matrix_make_identity(W->A, P->S, 0, P->B);
  precode_matrix_make_LDPC2(W->A, P->W, P->S, P->P);
  precode_matrix_make_G_ENC(P, W->A);
}

static bool precode_matrix_transpose(params *P, pc *W) {
  u32 u = W->cols - P->P;
  for (u32 row = 0; row < W->rows; row++) {
    u32 nz = 0;
    for (u32 it = 0; it < uv_size(W->A[row]); it++) {
      u32 col = uv_A(W->A[row], it);
      if (col < u)
        nz++;
      uv_A(W->cnz, col)++;
    }
    uv_A(W->nz, row) = nz;
  }
  arena *a = &W->prep_mem;
  if (!W->AT_mem_beg)
    W->AT_mem_beg = a->beg;
  a->beg = W->AT_mem_beg;
  for (u32 row = 0; row < W->cols; row++) {
    if (!u32_vec_init(&W->AT[row], a, 0, uv_A(W->cnz, row), 0))
      return false;
  }

  for (u32 row = 0; row < W->rows; row++)
    for (u32 it = 0; it < uv_size(W->A[row]); it++)
      uv_push(W->AT[uv_A(W->A[row], it)], row);
  return true;
}

static void precode_matrix_init_pv(pc *W) {
  for (u32 it = 0; it < W->cols; it++)
    uv_A(W->c, it) = uv_A(W->ci, it) = it;
  for (u32 it = 0; it < W->rows; it++)
    uv_A(W->d, it) = uv_A(W->di, it) = it;
}

static void precode_matrix_sort(params *P, pc *W) {
  for (u32 row = 0; row < W->rows; row++) /* move HDPC to bottom */
    uv_A(W->d, row) = (row + P->S + P->H) % W->rows;
  for (u32 i = 0; i < W->rows; i++)
    uv_A(W->di, uv_A(W->d, i)) = i;
}

void precode_matrix_on_op(void *arg, u32 i, u32 j, u8 u) {
  (void)arg;
  (void)i;
  (void)j;
  (void)u;
}

static u32 precode_row_nz_at(pc *W, u32 row, u32 s, u32 e, u32 *at) {
  u32 r = 0, drow = uv_A(W->d, row);
  at[0] = at[1] = e;
  u32_vec rs = W->A[drow];
  for (u32 it = 0; it < uv_size(rs) && r < uv_A(W->nz, drow); it++) {
    u32 col = uv_A(W->ci, uv_A(rs, it));
    if (col >= s && col < e)
      at[r++] = col;
  }
  if (at[0] > at[1])
    TMPSWAP(u32, at[0], at[1]);
  return r;
}

static u8 precode_matrix_swap_cols(pc *W, u32 V0, u32 Vcols) {
  u32 ones[2], Ve = V0 + Vcols - 1, r = 0;
  r = precode_row_nz_at(W, V0, V0, V0 + Vcols, ones);
  if (ones[0] != V0) {
    TMPSWAP(u32, uv_A(W->c, V0), uv_A(W->c, ones[0]));
    TMPSWAP(u32, uv_A(W->ci, uv_A(W->c, V0)), uv_A(W->ci, uv_A(W->c, ones[0])));
  }
  if (r == 2 && ones[1] != Ve) {
    TMPSWAP(u32, uv_A(W->c, Ve), uv_A(W->c, ones[1]));
    TMPSWAP(u32, uv_A(W->ci, uv_A(W->c, Ve)), uv_A(W->ci, uv_A(W->c, ones[1])));
  }
  return r;
}

static void precode_matrix_update_nnz(pc *W, u32 V0, u32 Vcols, u32 r) {
  u32_vec cs = W->AT[uv_A(W->c, V0)];
  for (u32 it = 0; it < uv_size(cs); it++) {
    u32 row = uv_A(cs, it);
    u32 nz = --uv_A(W->nz, row);
    if (nz > 0 && nz < 3)
      uv_push(W->NZT[nz], row);
  }
  for (u32 col = 0; col < r - 1; col++) {
    cs = W->AT[uv_A(W->c, V0 + Vcols - col - 1)];
    for (u32 it = 0; it < uv_size(cs); it++) {
      u32 row = uv_A(cs, it);
      u32 nz = --uv_A(W->nz, row);
      if (nz > 0 && nz < 3)
        uv_push(W->NZT[nz], row);
    }
  }
}

static void precode_matrix_precond(params *P, pc *W) {
  u32 i = 0, u = P->P, Srows = W->rows - P->H;

  for (u32 row = 0; row < Srows; row++) {
    u32 drow = uv_A(W->d, row);
    u32 nz = uv_A(W->nz, drow);
    if (nz > 0 && nz < 3)
      uv_push(W->NZT[nz], drow);
  }
  while (i + u < P->L) {
    u32 Vrows = W->rows - i, Vcols = W->cols - i - u, V0 = i;
    u32 chosen =
        W->cb.on_choose(W->cb.on_choose_arg, W, V0, Vrows, Srows, Vcols);
    if (chosen >= Srows)
      break;
    if (V0 != chosen) {
      u32 rval = uv_A(W->d, V0);
      u32 nzval = uv_A(W->d, chosen);
      TMPSWAP(u32, uv_A(W->d, V0), uv_A(W->d, chosen));
      TMPSWAP(u32, uv_A(W->di, rval), uv_A(W->di, nzval));
    }
    u32 r = precode_matrix_swap_cols(W, V0, Vcols);
    precode_matrix_update_nnz(W, V0, Vcols, r);
    i++;
    u += r - 1;
  }
  W->i = i;
  W->u = P->L - i;
}

static void precode_matrix_fwd_GE(pc *W, u32 s, u32 e) {
  for (u32 row = 0; row < W->i; row++) {
    u32 mv = s < row ? row : s;
    u32_vec cs = W->AT[uv_A(W->c, row)];
    for (u32 it = 0; it < uv_size(cs); it++) {
      u32 tmp = uv_A(cs, it), h = uv_A(W->di, tmp);
      if (h > mv && h < e) {
        u32 drow = uv_A(W->d, row);
        bm_add(&W->U, tmp, drow);
        W->cb.on_op(W->cb.on_op_arg, tmp, drow, 1);
      }
    }
  }
}

static void precode_matrix_fill_U(pc *W) {
  for (u32 row = 0; row < W->rows; row++) {
    for (u32 it = 0; it < uv_size(W->A[row]); it++) {
      u32 col = uv_A(W->ci, uv_A(W->A[row], it));
      if (col >= W->i)
        bm_set(&W->U, row, col - W->i);
    }
  }
}

static void hf_scal(pc *W, u32 i, u8 beta) {
  u8 *a = (u8 *)&uv_E(W->UL, uv_A(W->F.rowmap, i), 0);
  nanorq_oblas.scal(a, beta, W->UL.s);
}

static void hf_axpy(pc *W, u32 i, u32 j, u8 beta) {
  if (uv_A(W->F.type, i) == uv_A(W->F.type, j)) {
    if (uv_A(W->F.type, i)) {
      u8 *a = (u8 *)&uv_E(W->UL, uv_A(W->F.rowmap, i), 0);
      u8 *b = (u8 *)&uv_E(W->UL, uv_A(W->F.rowmap, j), 0);
      nanorq_oblas.axpy(a, b, beta, W->UL.s);
    } else {
      bm_add(&W->U, i, j);
    }
  } else {
    /* if target row is in gf256, axpy in place from gf2 row */
    if (uv_A(W->F.type, i)) {
      u8 *a = (u8 *)&uv_E(W->UL, uv_A(W->F.rowmap, i), 0);
      u32 *b = &uv_E(W->U, j, 0);
      nanorq_oblas.axpyb32(a, b, beta, W->UL.s);
    } else {
      assert(W->F.used < W->F.max);
      u8 *tmp = &uv_E(W->UL, W->F.used, 0);
      bm_fill(&W->U, i, tmp);
      uv_A(W->F.type, i) = 1; /* row i is now a gf256 row */
      uv_A(W->F.rowmap, i) = W->F.used;
      W->F.used++;
      u8 *a = (u8 *)&uv_E(W->UL, uv_A(W->F.rowmap, i), 0);
      u8 *b = (u8 *)&uv_E(W->UL, uv_A(W->F.rowmap, j), 0);
      nanorq_oblas.axpy(a, b, beta, W->UL.s);
    }
  }
}

static void precode_matrix_fill_HDPC(params *P, pc *W) {
  precode_matrix_make_HDPC(P, W);
  for (u32 row = 0; row < P->H; row++) {
    for (u32 col = 0; col < W->u - P->H; col++)
      uv_E(W->UL, row, col) = uv_E(W->HDPC, row, uv_A(W->c, P->L - W->u + col));
    uv_E(W->UL, row, row + W->u - P->H) = 1; /* init I_H */
  }
  for (u32 row = P->S; row < P->S + P->H; row++) {
    uv_A(W->F.type, row) = 1;
    uv_A(W->F.rowmap, row) = row - P->S;
  }
  W->F.used = P->H;
  W->F.max = 2 * P->H;
  for (u32 row = 0; row < W->i; row++) {
    u32 crow = uv_A(W->c, row), drow = uv_A(W->d, row);
    for (u32 h = 0, del_row = W->rows - P->H; h < P->H; h++, del_row++) {
      u8 beta = uv_E(W->HDPC, h, crow);
      if (beta) {
        hf_axpy(W, uv_A(W->d, del_row), drow, beta);
        W->cb.on_op(W->cb.on_op_arg, uv_A(W->d, del_row), drow, beta);
      }
    }
  }
}

static void precode_matrix_make_U(params *P, pc *W) {
  precode_matrix_fill_U(W);
  precode_matrix_fwd_GE(W, 0, W->i);
  W->cb.on_op(W->cb.on_op_arg, 0, 0, 0);
  precode_matrix_fwd_GE(W, W->i == 0 ? 0 : W->i - 1, W->rows - P->H);
}

static int precode_matrix_solve_gf2(params *P, pc *W) {
  u32 row, nzrow, rows = W->rows - P->H;
  for (row = W->i; row < P->L; row++) {
    u32 col = row - W->i;
    u32 drow = uv_A(W->d, row);
    for (nzrow = row; nzrow < rows; nzrow++)
      if (bm_at(&W->U, uv_A(W->d, nzrow), col))
        break;
    if (nzrow == rows)
      break;
    if (row != nzrow) {
      u32 rval = uv_A(W->d, row);
      u32 nzval = uv_A(W->d, nzrow);
      TMPSWAP(u32, uv_A(W->d, row), uv_A(W->d, nzrow));
      TMPSWAP(u32, uv_A(W->di, rval), uv_A(W->di, nzval));
      drow = nzval;
    }
    for (u32 del_row = row + 1; del_row < rows; del_row++) {
      u32 ddrow = uv_A(W->d, del_row);
      if (bm_at(&W->U, ddrow, col) == 0)
        continue;
      bm_add(&W->U, ddrow, drow);
      W->cb.on_op(W->cb.on_op_arg, ddrow, drow, 1);
    }
  }
  return row;
}

static int precode_matrix_solve_gf256(params *P, pc *W) {
  u32 row, nzrow;
  for (row = W->i; row < P->L; row++) {
    u32 col = row - W->i, beta = 0;
    for (nzrow = row; nzrow < W->rows; nzrow++) {
      beta = hf_at(W, uv_A(W->d, nzrow), col);
      if (beta != 0)
        break;
    }
    if (nzrow == W->rows)
      break;
    if (row != nzrow) {
      u32 rval = uv_A(W->d, row);
      u32 nzval = uv_A(W->d, nzrow);
      TMPSWAP(u32, uv_A(W->d, row), uv_A(W->d, nzrow));
      TMPSWAP(u32, uv_A(W->di, rval), uv_A(W->di, nzval));
    }
    if (beta > 1) {
      hf_scal(W, uv_A(W->d, row), OCT_INV[beta]);
      W->cb.on_op(W->cb.on_op_arg, uv_A(W->d, row), OCT_INV[beta], 0);
    }
    for (u32 del_row = row + 1; del_row < W->rows; del_row++) {
      beta = hf_at(W, uv_A(W->d, del_row), col);
      if (beta == 0)
        continue;
      hf_axpy(W, uv_A(W->d, del_row), uv_A(W->d, row), beta);
      W->cb.on_op(W->cb.on_op_arg, uv_A(W->d, del_row), uv_A(W->d, row), beta);
    }
  }
  return row;
}

static void precode_matrix_backsolve(params *P, pc *W) {
  for (u32 row = P->L - 1; row >= W->i; row--) {
    u32_vec cs = W->AT[uv_A(W->c, row)];
    for (u32 it = 0; it < uv_size(cs); it++) {
      u32 del_row = uv_A(W->di, uv_A(cs, it));
      if (del_row < W->i)
        W->cb.on_op(W->cb.on_op_arg, uv_A(W->d, del_row), uv_A(W->d, row), 1);
    }
    for (u32 del_row = W->i; del_row < row; del_row++) {
      u8 beta = hf_at(W, uv_A(W->d, del_row), row - W->i);
      if (beta == 0)
        continue;
      W->cb.on_op(W->cb.on_op_arg, uv_A(W->d, del_row), uv_A(W->d, row), beta);
    }
  }
}

bool precode_matrix_prepare(params *P, pc *W) {
  precode_matrix_init_pv(W);
  precode_matrix_sort(P, W);
  if (!precode_matrix_transpose(P, W))
    return false;
  precode_matrix_precond(P, W);
  return true;
}

int precode_matrix_invert(params *P, pc *W) {
  assert((W->i + W->u) == P->L);

  precode_matrix_make_U(P, W);

  u32 rank = 0;
  if ((W->rows - P->H) >= P->L)
    rank = precode_matrix_solve_gf2(P, W);

  if (rank < P->L) {
    precode_matrix_fill_HDPC(P, W);
    rank = precode_matrix_solve_gf256(P, W);
    if (rank < P->L)
      return 0;
  }
  W->cb.on_op(W->cb.on_op_arg, 0, 0, 0);
  precode_matrix_backsolve(P, W);
  return 1;
}
