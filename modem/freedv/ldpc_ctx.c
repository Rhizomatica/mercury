/* ldpc_ctx.c — persistent, allocation-free LDPC decoder context.
 *
 * Copyright (C) 2026 Joseph Freivald
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ldpc_ctx.h.
 *
 * Graph layout.  Check rows are sorted by degree and packed into blocks of
 * V = 8 rows of equal degree; a block of degree d owns d*V message slots at
 * `blk_off[b]`, slot (j, v) = blk_off[b] + j*V + v for edge slot j of the row
 * in lane v.  Rows short of a full block are padded with dummy lanes whose
 * slots no variable references.  The same slot numbering is used by every
 * algorithm; `col_ptr`/`col_edge` give each variable its slots, `slot_var`
 * gives each slot its variable.  Messages: R (check->variable) and Q
 * (variable->check) per slot, float for SP/NMS and int16 for NMS16; `hb` holds
 * the hard decision of the slot's variable so the parity check is a streaming
 * XOR over the block layout with no gather.
 *
 * NMS16 check pass uses 8-lane int16 vectors via compiler vector extensions
 * (SSE2 on x86-64, NEON on arm64/armv7-a, scalar fallback elsewhere).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ldpc_ctx.h"
#include "mpdecode_core.h"
#include "phi0.h"

#define V 8
#define Q16_MAX 32767
#define Q16_MIN (-32767)

typedef int16_t v8i16 __attribute__((vector_size(16)));

struct ldpc_ctx {
    int N, M, E;          /* variables, checks, real edges                       */
    int nblk, S;          /* blocks, total slots (incl. padding)                 */
    int alg;
    float scale;          /* NMS normalisation                                   */
    int32_t *blk_off;     /* nblk+1: first slot of block                         */
    int16_t *blk_deg;     /* nblk: degree                                        */
    int8_t  *blk_real;    /* nblk: real rows in block (1..V)                     */
    int32_t *slot_var;    /* S: variable of slot, -1 for padding                 */
    int32_t *col_ptr;     /* N+1                                                 */
    int32_t *col_edge;    /* E: slots of each variable                           */
    float   *R, *Q, *Qtot;
    int16_t *R16, *Q16, *llr16;
    uint8_t *Qs, *hb, *hard;
    size_t   bytes;
};

uint64_t ldpc_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

int ldpc_ctx_row_indices(const struct LDPC *l, int i, int *idx)
{
    const int N = l->CodeLength, M = l->NumberParityBits, K = N - M;
    const int H1 = (l->NumberRowsHcols == N) ? 0 : 1;
    int shift = H1 ? (M + l->NumberRowsHcols) - N : 0;
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
    int nblk = M / shift, blk = i / shift, k = i % shift;
    if (blk == 0 || blk == nblk - 1) {
        idx[count++] = (blk == nblk - 1) ? K + k + shift * blk : K + k + shift * (blk + 1);
    } else {
        idx[count++] = K + k + shift * blk;
        idx[count++] = K + k + shift * (blk + 1);
    }
    return count;
}

/* Per-code normalised-min-sum scaling, from the MATLAB sweep against belief
 * propagation at 50 iterations (docs/MODEM-ANALYSIS.md, F2).  Codes not listed
 * use the default. */
static const struct { const char *name; int alg; float scale; } policy_table[] = {
    {"H_256_768_22",   LDPC_ALG_NMS16, 0.85f}, /* DATAC15/16: 0.85 = BP; 0.75 cost 0.3-0.4 dB      */
    {"H_4096_8192_3d", LDPC_ALG_NMS16, 0.85f}, /* DATAC1: 0.85 beats BP@50 by 0.1-0.3 dB            */
    {"H_256_512_4",    LDPC_ALG_NMS16, 0.80f}, /* DATAC13: MATLAB nms within 0.1 dB of BP           */
    {"H_128_256_5",    LDPC_ALG_NMS16, 0.80f}, /* DATAC0                                            */
    {"HRA_56_56",      LDPC_ALG_NMS16, 0.80f}, /* DATAC14                                           */
    {"H_1024_2048_4f", LDPC_ALG_SPT,   0.70f}, /* DATAC3/4: col weight 54, min-sum +0.1-0.2 dB; SPT gate passed */
    {"H_16200_9720",   LDPC_ALG_SPT,   0.80f}, /* DATAC17/QAM16C2: min-sum +0.3-0.45 dB; SPT gate passed */
};
#define NMS_SCALE_DEFAULT 0.80f

int ldpc_ctx_policy_alg(const char *name)
{
    for (size_t i = 0; i < sizeof policy_table / sizeof *policy_table; i++)
        if (name && !strcmp(name, policy_table[i].name)) return policy_table[i].alg;
    return LDPC_ALG_SP;
}

int ldpc_alg_from_name(const char *s)
{
    if (!s) return -2;
    if (!strcmp(s, "auto")) return LDPC_ALG_AUTO;
    if (!strcmp(s, "sp")) return LDPC_ALG_SP;
    if (!strcmp(s, "spt")) return LDPC_ALG_SPT;
    if (!strcmp(s, "nms")) return LDPC_ALG_NMS;
    if (!strcmp(s, "nms16")) return LDPC_ALG_NMS16;
    if (!strcmp(s, "legacy")) return LDPC_ALG_LEGACY;
    return -2;
}

const char *ldpc_alg_name(int alg)
{
    switch (alg) {
    case LDPC_ALG_AUTO: return "auto";
    case LDPC_ALG_SP: return "sp";
    case LDPC_ALG_SPT: return "spt";
    case LDPC_ALG_NMS: return "nms";
    case LDPC_ALG_NMS16: return "nms16";
    case LDPC_ALG_LEGACY: return "legacy";
    default: return "?";
    }
}

float ldpc_ctx_policy_scale(const char *name)
{
    for (size_t i = 0; i < sizeof policy_table / sizeof *policy_table; i++)
        if (name && !strcmp(name, policy_table[i].name)) return policy_table[i].scale;
    return NMS_SCALE_DEFAULT;
}

/* phi(x) = -log(tanh(x/2)), two-segment table with linear interpolation:
 * [0, 1) in steps of 1/4096 (phi is steep there: 10 at x~0, 5.5 at 1/128,
 * 0.96 at 1) and [1, 16) in steps of 1/128.  Returns 0 beyond 16 (legacy
 * phi0 returns 0 beyond 10 and 10 below 9e-5). */
#define PHI_A_N 4096
#define PHI_A_STEP 4096.0f
#define PHI_B_N 1920
#define PHI_B_STEP 128.0f
static float phi_a[PHI_A_N + 1], phi_b[PHI_B_N + 1];
static int phi_tab_ready = 0;

static void phi_tab_init(void)
{
    if (phi_tab_ready) return;
    phi_a[0] = 10.0f;
    for (int i = 1; i <= PHI_A_N; i++) phi_a[i] = (float) (-log(tanh(((double) i / PHI_A_STEP) / 2.0)));
    for (int i = 0; i <= PHI_B_N; i++) phi_b[i] = (float) (-log(tanh((1.0 + (double) i / PHI_B_STEP) / 2.0)));
    phi_tab_ready = 1;
}

static inline float phi_t(float x)
{
    if (x < 1.0f) {
        if (x <= 0.0f) return 10.0f;
        float f = x * PHI_A_STEP; int i = (int) f; float t = f - (float) i;
        return phi_a[i] + t * (phi_a[i + 1] - phi_a[i]);
    }
    float f = (x - 1.0f) * PHI_B_STEP;
    if (f >= (float) PHI_B_N) return 0.0f;
    int i = (int) f; float t = f - (float) i;
    return phi_b[i] + t * (phi_b[i + 1] - phi_b[i]);
}

static void *xmalloc(size_t n, size_t *acc) { *acc += n; return malloc(n); }
static void *xcalloc(size_t n, size_t sz, size_t *acc) { *acc += n * sz; return calloc(n, sz); }

ldpc_ctx_t *ldpc_ctx_create(const struct LDPC *code)
{
    ldpc_ctx_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->N = code->CodeLength;
    c->M = code->NumberParityBits;
    c->alg = LDPC_ALG_SP;
    c->scale = ldpc_ctx_policy_scale(code->name);
    phi_tab_init();
    size_t bytes = sizeof *c;

    const int maxdeg = code->max_row_weight + 2;
    int *deg = malloc(sizeof(int) * c->M);
    int *order = malloc(sizeof(int) * c->M);
    int *idx = malloc(sizeof(int) * maxdeg);
    if (!deg || !order || !idx) goto fail;

    int E = 0, dmax = 0;
    for (int m = 0; m < c->M; m++) { deg[m] = ldpc_ctx_row_indices(code, m, idx); E += deg[m]; if (deg[m] > dmax) dmax = deg[m]; }
    c->E = E;

    /* stable sort rows by degree (counting sort) */
    int *cnt = calloc(dmax + 2, sizeof(int));
    if (!cnt) goto fail;
    for (int m = 0; m < c->M; m++) cnt[deg[m] + 1]++;
    for (int d = 1; d <= dmax + 1; d++) cnt[d] += cnt[d - 1];
    for (int m = 0; m < c->M; m++) order[cnt[deg[m]]++] = m;
    free(cnt);

    /* blocks: V rows of equal degree, last block of a degree padded */
    int nblk = 0, S = 0;
    for (int i = 0; i < c->M;) {
        int d = deg[order[i]], n = 0;
        while (i + n < c->M && n < V && deg[order[i + n]] == d) n++;
        nblk++; S += d * V; i += n;
    }
    c->nblk = nblk; c->S = S;
    c->blk_off  = xmalloc(sizeof(int32_t) * (nblk + 1), &bytes);
    c->blk_deg  = xmalloc(sizeof(int16_t) * nblk, &bytes);
    c->blk_real = xmalloc(nblk, &bytes);
    c->slot_var = xmalloc(sizeof(int32_t) * S, &bytes);
    c->col_ptr  = xcalloc(c->N + 1, sizeof(int32_t), &bytes);
    c->col_edge = xmalloc(sizeof(int32_t) * E, &bytes);
    c->hb       = xmalloc(S, &bytes);
    c->hard     = xmalloc(c->N, &bytes);
    c->Qtot     = xmalloc(sizeof(float) * c->N, &bytes);
    if (!c->blk_off || !c->blk_deg || !c->blk_real || !c->slot_var || !c->col_ptr ||
        !c->col_edge || !c->hb || !c->hard || !c->Qtot) goto fail;

    int b = 0, off = 0;
    for (int i = 0; i < c->M;) {
        int d = deg[order[i]], n = 0;
        while (i + n < c->M && n < V && deg[order[i + n]] == d) n++;
        c->blk_off[b] = off; c->blk_deg[b] = (int16_t) d; c->blk_real[b] = (int8_t) n;
        for (int v = 0; v < V; v++) {
            if (v < n) {
                int dd = ldpc_ctx_row_indices(code, order[i + v], idx);
                for (int j = 0; j < dd; j++) { c->slot_var[off + j * V + v] = idx[j]; c->col_ptr[idx[j] + 1]++; }
            } else {
                for (int j = 0; j < d; j++) c->slot_var[off + j * V + v] = -1;
            }
        }
        off += d * V; b++; i += n;
    }
    c->blk_off[nblk] = off;
    for (int n = 0; n < c->N; n++) c->col_ptr[n + 1] += c->col_ptr[n];
    int32_t *fill = calloc(c->N, sizeof(int32_t));
    if (!fill) goto fail;
    for (int s = 0; s < S; s++) {
        int n = c->slot_var[s];
        if (n >= 0) c->col_edge[c->col_ptr[n] + fill[n]++] = s;
    }
    free(fill); free(deg); free(order); free(idx);
    deg = order = idx = NULL;

    /* message storage for every algorithm (float and int16), allocated once */
    c->R = xmalloc(sizeof(float) * S, &bytes);
    c->Q = xmalloc(sizeof(float) * S, &bytes);
    c->Qs = xmalloc(S, &bytes);
    c->R16 = xmalloc(sizeof(int16_t) * S, &bytes);
    c->Q16 = xmalloc(sizeof(int16_t) * S, &bytes);
    c->llr16 = xmalloc(sizeof(int16_t) * c->N, &bytes);
    if (!c->R || !c->Q || !c->Qs || !c->R16 || !c->Q16 || !c->llr16) goto fail;
    memset(c->hb, 0, S);
    c->bytes = bytes;
    return c;
fail:
    free(deg); free(order); free(idx);
    ldpc_ctx_destroy(c);
    return NULL;
}

void ldpc_ctx_destroy(ldpc_ctx_t *c)
{
    if (!c) return;
    free(c->blk_off); free(c->blk_deg); free(c->blk_real); free(c->slot_var);
    free(c->col_ptr); free(c->col_edge); free(c->hb); free(c->hard); free(c->Qtot);
    free(c->R); free(c->Q); free(c->Qs); free(c->R16); free(c->Q16); free(c->llr16);
    free(c);
}

size_t ldpc_ctx_bytes(const ldpc_ctx_t *c) { return c ? c->bytes : 0; }
int    ldpc_ctx_edges(const ldpc_ctx_t *c) { return c ? c->E : 0; }
int    ldpc_ctx_alg(const ldpc_ctx_t *c)   { return c ? c->alg : LDPC_ALG_SP; }

void ldpc_ctx_set_alg(ldpc_ctx_t *c, int alg, float nms_scale)
{
    if (!c) return;
    c->alg = (alg == LDPC_ALG_NMS || alg == LDPC_ALG_NMS16 || alg == LDPC_ALG_SPT) ? alg : LDPC_ALG_SP;
    if (nms_scale > 0.0f) c->scale = nms_scale;
}

/* ---- parity over the block layout: streaming XOR of hb, no gather ------- */

static int parity_count(const ldpc_ctx_t *c)
{
    int ok = 0;
    for (int b = 0; b < c->nblk; b++) {
        const uint8_t *p = c->hb + c->blk_off[b];
        const int d = c->blk_deg[b];
        uint8_t x[V];
        memcpy(x, p, V);
        for (int j = 1; j < d; j++)
            for (int v = 0; v < V; v++) x[v] ^= p[j * V + v];
        for (int v = 0; v < c->blk_real[b]; v++) ok += !x[v];
    }
    return ok;
}

/* ---- float normalised min-sum ------------------------------------------- */

static void nms_check_pass(ldpc_ctx_t *c)
{
    const float scale = c->scale;
    for (int b = 0; b < c->nblk; b++) {
        const int off = c->blk_off[b], d = c->blk_deg[b];
        for (int v = 0; v < V; v++) {
            float min1 = INFINITY, min2 = INFINITY;
            int jmin = 0, sgn = 0;
            for (int j = 0; j < d; j++) {
                float q = c->Q[off + j * V + v], a = fabsf(q);
                sgn ^= (q < 0.0f);
                if (a < min1) { min2 = min1; min1 = a; jmin = j; }
                else if (a < min2) { min2 = a; }
            }
            for (int j = 0; j < d; j++) {
                float q = c->Q[off + j * V + v];
                float r = scale * ((j == jmin) ? min2 : min1);
                c->R[off + j * V + v] = (sgn ^ (q < 0.0f)) ? -r : r;
            }
        }
    }
}

static void nms_var_pass(ldpc_ctx_t *c, const float *llr)
{
    for (int n = 0; n < c->N; n++) {
        const int s = c->col_ptr[n], t = c->col_ptr[n + 1];
        float tot = llr[n];
        for (int k = s; k < t; k++) tot += c->R[c->col_edge[k]];
        c->Qtot[n] = tot;
        const uint8_t h = (tot < 0.0f);
        c->hard[n] = h;
        for (int k = s; k < t; k++) {
            int e = c->col_edge[k];
            c->Q[e] = tot - c->R[e];
            c->hb[e] = h;
        }
    }
}

/* ---- phi-domain sum-product (mirrors legacy SumProduct) ---------------- */

#define SP_PASSES(NAME, PHI)                                                        \
static void NAME##_check_pass(ldpc_ctx_t *c)                                       \
{                                                                                  \
    for (int b = 0; b < c->nblk; b++) {                                            \
        const int off = c->blk_off[b], d = c->blk_deg[b];                          \
        for (int v = 0; v < c->blk_real[b]; v++) {                                 \
            float sum = 0.0f;                                                      \
            int sgn = 0;                                                           \
            for (int j = 0; j < d; j++) { int e = off + j * V + v; sum += c->Q[e]; sgn ^= c->Qs[e]; } \
            for (int j = 0; j < d; j++) {                                          \
                int e = off + j * V + v;                                           \
                float r = PHI(sum - c->Q[e]);                                      \
                c->R[e] = (sgn ^ c->Qs[e]) ? -r : r;                               \
            }                                                                      \
        }                                                                          \
    }                                                                              \
}                                                                                  \
static void NAME##_var_pass(ldpc_ctx_t *c, const float *llr)                       \
{                                                                                  \
    for (int n = 0; n < c->N; n++) {                                               \
        const int s = c->col_ptr[n], t = c->col_ptr[n + 1];                        \
        float tot = llr[n];                                                        \
        for (int k = s; k < t; k++) tot += c->R[c->col_edge[k]];                   \
        c->Qtot[n] = tot;                                                          \
        const uint8_t h = (tot < 0.0f);                                            \
        c->hard[n] = h;                                                            \
        for (int k = s; k < t; k++) {                                              \
            int e = c->col_edge[k];                                                \
            float q = tot - c->R[e];                                               \
            c->Q[e]  = PHI(fabsf(q));                                              \
            c->Qs[e] = (q <= 0.0f);   /* legacy: sign = 1 unless temp_sum > 0 */   \
            c->hb[e] = h;                                                          \
        }                                                                          \
    }                                                                              \
}
SP_PASSES(sp, phi0)      /* exact legacy phi0(), result-equivalent to SumProduct */
SP_PASSES(spt, phi_t)    /* table phi, no call per edge                          */

/* ---- int16 normalised min-sum, row-blocked, 8-lane vectors -------------- */

static inline v8i16 ld8(const int16_t *p) { v8i16 x; memcpy(&x, p, sizeof x); return x; }
static inline void st8(int16_t *p, v8i16 x) { memcpy(p, &x, sizeof x); }

static void nms16_check_pass(ldpc_ctx_t *c)
{
    const int16_t alpha_q = (int16_t) lrintf(c->scale * 256.0f);
    const v8i16 zero = {0, 0, 0, 0, 0, 0, 0, 0};
    const v8i16 big  = {Q16_MAX, Q16_MAX, Q16_MAX, Q16_MAX, Q16_MAX, Q16_MAX, Q16_MAX, Q16_MAX};
    for (int b = 0; b < c->nblk; b++) {
        const int off = c->blk_off[b], d = c->blk_deg[b];
        const int16_t *Q = c->Q16 + off;
        int16_t *R = c->R16 + off;
        v8i16 min1 = big, min2 = big, jmin = zero, sgn = zero;
        for (int j = 0; j < d; j++) {
            v8i16 q = ld8(Q + j * V);
            v8i16 neg = q < zero;                 /* all-ones where negative */
            v8i16 a = (neg & (zero - q)) | (~neg & q);
            sgn ^= neg;
            v8i16 lt1 = a < min1;
            v8i16 lt2 = a < min2;
            v8i16 jv = {(int16_t) j, (int16_t) j, (int16_t) j, (int16_t) j, (int16_t) j, (int16_t) j, (int16_t) j, (int16_t) j};
            min2 = (lt1 & min1) | (~lt1 & ((lt2 & a) | (~lt2 & min2)));
            min1 = (lt1 & a) | (~lt1 & min1);
            jmin = (lt1 & jv) | (~lt1 & jmin);
        }
        /* scaled magnitudes once per block: (mag * alpha) >> 8 in 32-bit */
        v8i16 m1s, m2s;
        {
            int16_t t1[V], t2[V];
            memcpy(t1, &min1, sizeof t1); memcpy(t2, &min2, sizeof t2);
            for (int v = 0; v < V; v++) {
                t1[v] = (int16_t) (((int32_t) t1[v] * alpha_q) >> 8);
                t2[v] = (int16_t) (((int32_t) t2[v] * alpha_q) >> 8);
            }
            m1s = ld8(t1); m2s = ld8(t2);
        }
        for (int j = 0; j < d; j++) {
            v8i16 q = ld8(Q + j * V);
            v8i16 neg = q < zero;
            v8i16 jv = {(int16_t) j, (int16_t) j, (int16_t) j, (int16_t) j, (int16_t) j, (int16_t) j, (int16_t) j, (int16_t) j};
            v8i16 isj = jv == jmin;
            v8i16 mag = (isj & m2s) | (~isj & m1s);
            v8i16 s = sgn ^ neg;                  /* all-ones where product of the OTHER signs is negative */
            v8i16 r = (s & (zero - mag)) | (~s & mag);
            st8(R + j * V, r);
        }
    }
}

static inline int16_t sat16(int32_t x) { return x > Q16_MAX ? Q16_MAX : x < Q16_MIN ? Q16_MIN : (int16_t) x; }

static void nms16_var_pass(ldpc_ctx_t *c)
{
    for (int n = 0; n < c->N; n++) {
        const int s = c->col_ptr[n], t = c->col_ptr[n + 1];
        int32_t tot = c->llr16[n];
        for (int k = s; k < t; k++) tot += c->R16[c->col_edge[k]];
        const uint8_t h = (tot < 0);
        c->hard[n] = h;
        for (int k = s; k < t; k++) {
            int e = c->col_edge[k];
            c->Q16[e] = sat16(tot - c->R16[e]);
            c->hb[e] = h;
        }
    }
}

/* ---- decode ------------------------------------------------------------- */

int ldpc_ctx_decode(ldpc_ctx_t *c, const float *llr, uint8_t *bits,
                    int max_iter, uint64_t deadline_ns, ldpc_stats_t *st)
{
    const int alg = c->alg;
    if (alg == LDPC_ALG_NMS16) {
        for (int n = 0; n < c->N; n++) c->llr16[n] = sat16((int32_t) lrintf(llr[n] * LDPC_Q16_SCALE));
        for (int s = 0; s < c->S; s++) {
            int n = c->slot_var[s];
            c->Q16[s] = (n >= 0) ? c->llr16[n] : Q16_MAX;   /* padding lanes: large positive, sign 0 */
            c->R16[s] = 0;
        }
    } else {
        for (int s = 0; s < c->S; s++) {
            int n = c->slot_var[s];
            float v = (n >= 0) ? llr[n] : 1e30f;
            if (alg == LDPC_ALG_NMS) { c->Q[s] = v; }
            else if (alg == LDPC_ALG_SPT) { c->Q[s] = (n >= 0) ? phi_t(fabsf(v)) : 0.0f; c->Qs[s] = (v < 0.0f); }
            else { c->Q[s] = (n >= 0) ? phi0(fabsf(v)) : 0.0f; c->Qs[s] = (v < 0.0f); }
            c->R[s] = 0.0f;
        }
    }
    memset(c->hb, 0, c->S);
    if (max_iter < 1) max_iter = 1;
    int iter = 0, pc = 0, hit = 0;
    for (;;) {
        iter++;
        if (alg == LDPC_ALG_NMS16)    { nms16_check_pass(c); nms16_var_pass(c); }
        else if (alg == LDPC_ALG_NMS) { nms_check_pass(c);   nms_var_pass(c, llr); }
        else if (alg == LDPC_ALG_SPT) { spt_check_pass(c);   spt_var_pass(c, llr); }
        else                          { sp_check_pass(c);    sp_var_pass(c, llr); }
        pc = parity_count(c);
        if (pc == c->M) break;
        if (iter >= max_iter) break;
        if (deadline_ns && ldpc_now_ns() >= deadline_ns) { hit = 1; break; }
    }
    memcpy(bits, c->hard, c->N);
    if (st) { st->iters = iter; st->parity_ok = (pc == c->M); st->parity_count = pc; st->hit_deadline = hit; }
    return iter;
}
