#include "params.h"
#include "rand.h"
#include "tuple.h"

static int is_prime(u32 n) {
  if (n <= 1)
    return 0;
  if (n <= 3)
    return 1;
  if (n % 2 == 0 || n % 3 == 0)
    return 0;
  for (u32 i = 5; i * i <= n; i = i + 6)
    if (n % i == 0 || n % (i + 2) == 0)
      return 0;
  return 1;
}

params params_init(u32 K) {
  params P;

  for (u32 i = 0; i < K_padded_size; i++) {
    if (K <= K_padded[i]) {
      P.Kprime = K_padded[i];
      P.J = J_K_padded[i];
      P.S = S_H_W[i][0];
      P.H = S_H_W[i][1];
      P.W = S_H_W[i][2];
      break;
    }
  }

  P.K = K;
  P.L = P.Kprime + P.S + P.H;
  P.P = P.L - P.W;
  P.U = P.P - P.H;
  P.B = P.W - P.S;
  P.P1 = P.P;

  while (!is_prime(P.P1))
    P.P1++;

  return P;
}

u32 params_set_idxs(params *P, u32 X, u32_vec *dst) {
  u32 num = 0;
  tuple t = gen_tuple(X, P);

  uv_push(*dst, t.b);
  num++;
  for (u32 j = 1; j < t.d; j++) {
    t.b = (t.b + t.a) % P->W;
    uv_push(*dst, t.b);
    num++;
  }
  while (t.b1 >= P->P)
    t.b1 = (t.b1 + t.a1) % P->P1;

  uv_push(*dst, P->W + t.b1);
  num++;
  for (u32 j = 1; j < t.d1; j++) {
    t.b1 = (t.b1 + t.a1) % P->P1;
    while (t.b1 >= P->P)
      t.b1 = (t.b1 + t.a1) % P->P1;
    uv_push(*dst, P->W + t.b1);
    num++;
  }
  return num;
}
