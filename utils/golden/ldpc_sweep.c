/* ldpc_sweep — the C measurement tool behind docs/ldpc-figures/data.
 *
 * It reproduces, in C alone, every measurement the record
 * (docs/LDPC-DECODER.md) rests on: the frame error rate of each decoder
 * variant against Es/N0 on BPSK over AWGN, the cost of the modem's fixed LLR
 * scale at the real modulation, the normalised min-sum scale sweep, and the
 * cliff fixtures that ldpc_bench times.  The encoder, the modulator and the
 * LLR formulas are the ones the modem uses.  The random numbers come from
 * common/mercury_prng.c, so a seed replays the same frames on every platform.
 *
 * Modes.  Every measuring mode takes --seed S (default 1; the seed used is
 * printed) and --out FILE.
 *
 *   --fer --code NAME [--ebno LO:STEP:HI] [--frames 600] [--min-errors 30]
 *         [--batch 20] [--variants a,b,...] [--fixed-db X] [--nms-scale X]
 *       Per Eb/N0 point: Es/N0 = Eb/N0 + 10log10(K/N).  Uniform random info
 *       bits; codec2 encode() (codeword = info then parity); x = 1 - 2b;
 *       y = x + n with n ~ N(0, sigma^2), sigma^2 = 1/(2 Es/N0) per real
 *       dimension with Es = 1.  Exact LLR 2y/sigma^2 = 4 (Es/N0) y, positive
 *       = bit 0.  Fixed-EsNo LLR 4 * 10^(X/10) * y with X = 3 dB (10 dB for
 *       H_16200_9720): the modem's constant in place of 1/sigma^2.  LLRs are
 *       float32.  A point stops after --frames frames, or earlier once every
 *       variant has --min-errors frame errors.  A frame error is any of the K
 *       decoded info bits differing from the sent bits.  Every variant
 *       decodes the same noisy frames, so differences between columns are
 *       paired.  Writes "EsNo_dB,<variants>" with FER to 6 significant
 *       digits, then prints the Es/N0 crossings at FER 1e-1 and 1e-2 and the
 *       differences the record quotes.
 *   --crossings CSV [--code NAME]
 *       Re-reads a CSV written by --fer or --llr-scale and prints the same
 *       crossing lines, so a committed file can be read without re-running.
 *       The code name comes from --code or from the file name.
 *   --llr-scale --code NAME [--bps B] [--fixed-db X] [--ebno LO:STEP:HI]
 *         [--frames 300] [--min-errors 30] [--batch 10]
 *       Es/N0 per symbol = Eb/N0 + 10log10(R bps).  Symbols come from the
 *       modem's own modulator, psk_modulate_frame (consecutive codeword bits
 *       form one label, the first bit of the group is the MSB).  Complex AWGN
 *       of variance 1/(Es/N0), half per component.  LLRs from llr_from_qam
 *       (max-log, amplitudes 1) at the true Es/N0 and at the fixed constant;
 *       table sum-product at 50 iterations.  Defaults for the three pairs the
 *       record uses: H_256_768_22 bps 2, 3 dB, -1:0.5:3; H_4096_8192_3d
 *       bps 2, 3 dB, 0:0.5:4; H_16200_9720 bps 4, 10 dB, 1:0.5:8.  Writes
 *       "EsNo_dB,true,fixed" and prints the crossings and the cost fixed
 *       minus true at both targets.
 *   --nms-scale --code NAME [--scales 0.70,0.75,0.80,0.85,0.90] [--ebno ...]
 *         [--frames 400] [--min-errors 30] [--batch 20]
 *       Float normalised min-sum at each scale against sum-product, both at
 *       50 iterations on the same frames.  Prints one line per code,
 *       "<code>: sp=+0.00/+0.00 nms0.70=+a/+b ...", where a and b are the
 *       loss against sum-product in dB at FER 1e-1 and 1e-2.  This is the C
 *       counterpart of the dated nms_tuning.txt.  The C decoder has no offset
 *       min-sum, so there are no oms entries.
 *   --fixture --code NAME --esno X [--frames 100] --out BASE
 *       Writes BASE.f32 (N float32 exact LLRs per frame) and BASE.info.u8
 *       (K info bytes per frame): BPSK over AWGN at Es/N0 X dB.  These are
 *       the cliff fixtures ldpc_bench times (H_256_768_22 at -3.0 dB,
 *       H_1024_2048_4f -1.5, H_4096_8192_3d -1.4, H_16200_9720 -0.9).
 *
 * Crossings follow one rule everywhere: FER floored at 1e-6, the first grid
 * pair where the curve drops through the target, Es/N0 interpolated linearly
 * in log10(FER), NaN when the curve never crosses.  The record says a
 * crossing read off the 0.5 dB grid carries about +-0.1 dB.
 *
 * Legacy variants run run_ldpc_decoder_legacy() with ldpc->max_iter set.
 * That loop also halts when the decoded info bits are all zero, so payloads
 * are random.  The other variants run through one ldpc_ctx, switching the
 * algorithm per decode, with no deadline.  The min-sum scale is the code's
 * policy scale (ldpc_ctx_policy_scale) unless --nms-scale X.
 *
 * Copyright (C) 2026 Joseph Freivald
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpdecode_core.h"
#include "ldpc_ctx.h"
#include "ldpc_codes.h"
#include "interldpc.h"
#include "mercury_prng.h"

#define MAX_COLS 64     /* CSV columns after EsNo_dB */
#define MAX_PTS  256    /* grid points */
#define NAME_LEN 32

typedef char colname_t[NAME_LEN];

/* ---- command line ------------------------------------------------------- */

static const char *opt(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}
static int has(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], name)) return 1;
    return 0;
}
static int opt_int(int argc, char **argv, const char *name, int dflt)
{
    const char *s = opt(argc, argv, name);
    return s ? atoi(s) : dflt;
}
static double opt_dbl(int argc, char **argv, const char *name, double dflt)
{
    const char *s = opt(argc, argv, name);
    return s ? atof(s) : dflt;
}
static int is_number(const char *s)
{
    char *end;
    if (!s || !*s) return 0;
    strtod(s, &end);
    return *end == '\0';
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s --fer --code NAME [--ebno LO:STEP:HI] [--frames 600] [--min-errors 30] [--batch 20]\n"
        "                 [--variants a,b,...] [--fixed-db X] [--nms-scale X] [--seed S] [--out FILE]\n"
        "       %s --crossings CSV [--code NAME]\n"
        "       %s --llr-scale --code NAME [--bps B] [--fixed-db X] [--ebno LO:STEP:HI] [--frames 300]\n"
        "                 [--min-errors 30] [--batch 10] [--seed S] [--out FILE]\n"
        "       %s --nms-scale --code NAME [--scales 0.70,0.75,0.80,0.85,0.90] [--ebno LO:STEP:HI]\n"
        "                 [--frames 400] [--min-errors 30] [--batch 20] [--seed S] [--out FILE]\n"
        "       %s --fixture --code NAME --esno X_dB [--frames 100] [--seed S] --out BASE\n"
        "\n"
        "--fer variants: C_leg10 C_leg50 C_sp50 C_nms50 C_leg10_fixedEsNo C_nms50_fixedEsNo\n"
        "                C_nms16_50 C_spt50 C_leg100_fixedEsNo C_nms16_100 C_spt100 (default: all)\n"
        "--nms-scale followed by a number is the min-sum scale for --fer; on its own it is the\n"
        "scale sweep (float nms at each scale against sp, cap 50).  The C decoder has no\n"
        "offset min-sum, so the sweep has no oms entries.\n"
        "--seed defaults to 1 and is printed.  The crossing rule: FER floored at 1e-6, first\n"
        "downward crossing, Es/N0 linear in log10(FER), nan if the curve never crosses.\n",
        prog, prog, prog, prog, prog);
}

/* "LO:STEP:HI" to a grid; returns the number of points */
static int parse_grid(const char *s, double *g, int max)
{
    double lo, step, hi;
    if (!s || sscanf(s, "%lf:%lf:%lf", &lo, &step, &hi) != 3 || step <= 0.0 || hi < lo) {
        fprintf(stderr, "bad grid '%s' (want LO:STEP:HI with STEP > 0)\n", s ? s : "");
        exit(2);
    }
    int n = (int) floor((hi - lo) / step + 1e-9) + 1;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) g[i] = lo + i * step;
    return n;
}

/* "a,b,c" to names; returns the count */
static int parse_list(const char *s, colname_t *names, int max)
{
    int n = 0;
    while (s && *s && n < max) {
        const char *comma = strchr(s, ',');
        size_t len = comma ? (size_t) (comma - s) : strlen(s);
        if (len >= NAME_LEN) len = NAME_LEN - 1;
        memcpy(names[n], s, len);
        names[n][len] = '\0';
        if (len) n++;
        s = comma ? comma + 1 : NULL;
    }
    return n;
}

/* ---- codes ------------------------------------------------------------- */

/* ldpc_codes_setup() asserts on an unknown name, so check the table first. */
static void setup_code(struct LDPC *l, const char *name)
{
    if (!name) { fprintf(stderr, "--code NAME is required\n"); exit(2); }
    for (int c = 0; c < ldpc_codes_num(); c++)
        if (!strcmp(ldpc_codes[c].name, name)) { ldpc_codes_setup(l, (char *) name); return; }
    fprintf(stderr, "unknown code '%s'; known codes:", name);
    for (int c = 0; c < ldpc_codes_num(); c++) fprintf(stderr, " %s", ldpc_codes[c].name);
    fputc('\n', stderr);
    exit(2);
}

/* ---- crossings --------------------------------------------------------- */

/* Es/N0 where the curve first drops through `target`.  FER is floored at
 * 1e-6 so a zero-error point has a finite log.  Linear in log10(FER) between
 * the two grid points.  NaN when there is no downward crossing. */
static double esno_at_fer(const double *x, const double *fer, int n, double target)
{
    const double lt = log10(target);
    for (int k = 0; k + 1 < n; k++) {
        double y0 = log10(fmax(fer[k], 1e-6)), y1 = log10(fmax(fer[k + 1], 1e-6));
        if (y0 >= lt && y1 < lt) return x[k] + (lt - y0) * (x[k + 1] - x[k]) / (y1 - y0);
    }
    return NAN;
}

static const char *fmt_db(char *buf, size_t len, double v)
{
    if (isnan(v)) snprintf(buf, len, "nan");
    else snprintf(buf, len, "%+.2f", v);
    return buf;
}

static int col_index(const colname_t *names, int ncols, const char *name)
{
    for (int v = 0; v < ncols; v++) if (!strcmp(names[v], name)) return v;
    return -1;
}

/* one derived line: column a minus column b at both targets, if both exist */
static void print_diff(const char *code, const char *label, const colname_t *names, int ncols,
                       const double *x1, const double *x2, const char *a, const char *b)
{
    int ia = col_index(names, ncols, a), ib = col_index(names, ncols, b);
    char s1[16], s2[16];
    if (ia < 0 || ib < 0) return;
    printf("%s: %s, %s - %s: %s dB at FER 1e-1, %s dB at FER 1e-2\n", code, label, a, b,
           fmt_db(s1, sizeof s1, x1[ia] - x1[ib]), fmt_db(s2, sizeof s2, x2[ia] - x2[ib]));
}

static void print_crossings(const char *code, const colname_t *names, int ncols,
                            const double *esno, double (*fer)[MAX_PTS], int npts)
{
    double x1[MAX_COLS], x2[MAX_COLS];
    char s[16];
    for (int v = 0; v < ncols; v++) {
        x1[v] = esno_at_fer(esno, fer[v], npts, 1e-1);
        x2[v] = esno_at_fer(esno, fer[v], npts, 1e-2);
    }
    printf("%s: Es/N0 dB at FER 1e-1:", code);
    for (int v = 0; v < ncols; v++) printf(" %s=%s", names[v], fmt_db(s, sizeof s, x1[v]));
    printf("\n%s: Es/N0 dB at FER 1e-2:", code);
    for (int v = 0; v < ncols; v++) printf(" %s=%s", names[v], fmt_db(s, sizeof s, x2[v]));
    printf("\n");
    /* the lines the record quotes; "new" is the policy decoder at cap 100 */
    const char *newcol = ldpc_ctx_policy_alg(code) == LDPC_ALG_NMS16 ? "C_nms16_100" : "C_spt100";
    print_diff(code, "deployed minus new", names, ncols, x1, x2, "C_leg100_fixedEsNo", newcol);
    print_diff(code, "cap cost", names, ncols, x1, x2, "C_leg10_fixedEsNo", "C_leg100_fixedEsNo");
    print_diff(code, "sp vs legacy", names, ncols, x1, x2, "C_sp50", "C_leg50");
    print_diff(code, "spt vs sp", names, ncols, x1, x2, "C_spt50", "C_sp50");
    print_diff(code, "nms16 vs nms", names, ncols, x1, x2, "C_nms16_50", "C_nms50");
    print_diff(code, "nms LLR-scale sensitivity", names, ncols, x1, x2, "C_nms50_fixedEsNo", "C_nms50");
    print_diff(code, "spt vs nms16 at cap 100", names, ncols, x1, x2, "C_spt100", "C_nms16_100");
    print_diff(code, "fixed-scale cost", names, ncols, x1, x2, "fixed", "true");
}

/* ---- CSV --------------------------------------------------------------- */

static void write_csv(const char *path, const colname_t *names, int ncols,
                      const double *esno, double (*fer)[MAX_PTS], int npts)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "EsNo_dB");
    for (int v = 0; v < ncols; v++) fprintf(f, ",%s", names[v]);
    fputc('\n', f);
    for (int ip = 0; ip < npts; ip++) {
        fprintf(f, "%.10g", esno[ip]);
        for (int v = 0; v < ncols; v++) fprintf(f, ",%.6g", fer[v][ip]);
        fputc('\n', f);
    }
    fclose(f);
    fprintf(stderr, "wrote %s\n", path);
}

/* reads "EsNo_dB,<names>" then one row per grid point; returns the column count */
static int read_csv(const char *path, colname_t *names, double *esno, double (*fer)[MAX_PTS], int *npts)
{
    FILE *f = fopen(path, "r");
    char line[8192];
    if (!f) { perror(path); exit(1); }
    if (!fgets(line, sizeof line, f)) { fprintf(stderr, "%s: empty\n", path); exit(1); }
    line[strcspn(line, "\r\n")] = '\0';
    colname_t all[MAX_COLS + 1];
    int ncols = parse_list(line, all, MAX_COLS + 1) - 1;
    if (ncols < 1 || strcmp(all[0], "EsNo_dB")) { fprintf(stderr, "%s: header must start with EsNo_dB\n", path); exit(1); }
    for (int v = 0; v < ncols; v++) memcpy(names[v], all[v + 1], NAME_LEN);
    int n = 0;
    while (n < MAX_PTS && fgets(line, sizeof line, f)) {
        char *p = line, *end;
        if (*p == '\n' || *p == '\r' || *p == '\0') continue;
        esno[n] = strtod(p, &end);
        for (int v = 0; v < ncols; v++) {
            if (*end != ',') { fprintf(stderr, "%s: row %d has fewer than %d columns\n", path, n + 1, ncols + 1); exit(1); }
            p = end + 1;
            fer[v][n] = strtod(p, &end);
        }
        n++;
    }
    fclose(f);
    *npts = n;
    return ncols;
}

/* ---- the bench: one code, its buffers, one random stream ---------------- */

typedef struct {
    colname_t name;
    int alg;        /* LDPC_ALG_LEGACY or a context algorithm */
    int cap;        /* iteration cap */
    int llr_set;    /* 0: exact / true Es/N0 LLRs, 1: fixed-constant LLRs */
    float scale;    /* min-sum scale; 0 keeps the context's current one */
} variant_t;

typedef struct {
    struct LDPC l;
    int N, M, K;
    double R;
    ldpc_ctx_t *ctx;
    mercury_prng_t prng;
    unsigned char *info, *parity;
    uint8_t *bits;
    float *llr[2];
    /* the modulated path (--llr-scale) */
    int bps, nsym;
    int *cw;
    COMP *sym;
    float *amps;
} bench_t;

static void bench_open(bench_t *b, const char *code, unsigned seed)
{
    memset(b, 0, sizeof *b);
    setup_code(&b->l, code);
    b->N = b->l.CodeLength;
    b->M = b->l.NumberParityBits;
    b->K = b->N - b->M;
    b->R = (double) b->K / b->N;
    b->ctx = ldpc_ctx_create(&b->l);
    if (!b->ctx) { fprintf(stderr, "ldpc_ctx_create failed\n"); exit(1); }
    mercury_prng_seed(&b->prng, seed);
    b->info = malloc(b->K);
    b->parity = malloc(b->M);
    b->bits = malloc(b->N);
    b->llr[0] = malloc(sizeof(float) * b->N);
    b->llr[1] = malloc(sizeof(float) * b->N);
    if (!b->info || !b->parity || !b->bits || !b->llr[0] || !b->llr[1]) { fprintf(stderr, "out of memory\n"); exit(1); }
}

static void bench_open_qam(bench_t *b, int bps)
{
    int M = 0;
    if (!ldpc_qam_table(bps, &M)) { fprintf(stderr, "no constellation for %d bits per symbol (2, 4, 5, 6, 7, 8)\n", bps); exit(2); }
    if (b->N % bps) { fprintf(stderr, "N=%d is not a multiple of %d bits per symbol\n", b->N, bps); exit(2); }
    b->bps = bps;
    b->nsym = b->N / bps;
    b->cw = malloc(sizeof(int) * b->N);
    b->sym = malloc(sizeof(COMP) * b->nsym);
    b->amps = malloc(sizeof(float) * b->nsym);
    if (!b->cw || !b->sym || !b->amps) { fprintf(stderr, "out of memory\n"); exit(1); }
    for (int i = 0; i < b->nsym; i++) b->amps[i] = 1.0f;   /* equalised, unit amplitude */
}

static void bench_close(bench_t *b)
{
    ldpc_ctx_destroy(b->ctx);
    free(b->info); free(b->parity); free(b->bits);
    free(b->llr[0]); free(b->llr[1]);
    free(b->cw); free(b->sym); free(b->amps);
}

static void random_codeword(bench_t *b)
{
    for (int k = 0; k < b->K; k++) b->info[k] = (unsigned char) (mercury_prng_u31(&b->prng) & 1u);
    encode(&b->l, b->info, b->parity);
}

/* BPSK over AWGN at linear Es/N0: llr[0] exact, llr[1] with the fixed constant */
static void bpsk_frame(bench_t *b, double esno, double fixed_lin)
{
    const double sd = sqrt(1.0 / (2.0 * esno));   /* per real dimension, Es = 1 */
    random_codeword(b);
    for (int n = 0; n < b->N; n++) {
        int bit = n < b->K ? b->info[n] : b->parity[n - b->K];
        double y = (1 - 2 * bit) + sd * mercury_prng_gaussian(&b->prng);
        b->llr[0][n] = (float) (4.0 * esno * y);        /* 2y/sigma^2 */
        b->llr[1][n] = (float) (4.0 * fixed_lin * y);   /* the modem's constant for 1/sigma^2 */
    }
}

/* the real constellation over complex AWGN: max-log LLRs at the true Es/N0
 * (llr[0]) and at the fixed constant (llr[1]) */
static void qam_frame(bench_t *b, double esno, double fixed_lin)
{
    const double sd = sqrt(1.0 / (2.0 * esno));   /* complex variance 1/EsNo, half per component */
    random_codeword(b);
    for (int n = 0; n < b->N; n++) b->cw[n] = n < b->K ? b->info[n] : b->parity[n - b->K];
    psk_modulate_frame(b->bps, b->sym, b->cw, b->nsym);
    for (int i = 0; i < b->nsym; i++) {
        b->sym[i].real = (float) (b->sym[i].real + sd * mercury_prng_gaussian(&b->prng));
        b->sym[i].imag = (float) (b->sym[i].imag + sd * mercury_prng_gaussian(&b->prng));
    }
    llr_from_qam(b->llr[0], b->sym, b->amps, 1.0f, (float) esno, b->bps, b->nsym);
    llr_from_qam(b->llr[1], b->sym, b->amps, 1.0f, (float) fixed_lin, b->bps, b->nsym);
}

static void decode_variant(bench_t *b, const variant_t *v)
{
    float *llr = b->llr[v->llr_set];
    if (v->alg == LDPC_ALG_LEGACY) {
        int pcc = 0;   /* the legacy loop writes through this; it must not be NULL */
        b->l.max_iter = v->cap;
        run_ldpc_decoder_legacy(&b->l, b->bits, llr, &pcc);
    } else {
        ldpc_ctx_set_alg(b->ctx, v->alg, v->scale);
        ldpc_ctx_decode(b->ctx, llr, b->bits, v->cap, 0, NULL);
    }
}

typedef void (*frame_fn)(bench_t *b, double esno, double fixed_lin);

/* The point loop of the record's sweeps.  Frames come in batches; a point
 * ends at `frames` frames or once the variant with the fewest errors has
 * `min_errors`.  Every variant decodes every frame, so the columns are a
 * paired comparison and share one frame count per point. */
static void run_sweep(bench_t *b, const variant_t *vars, int nv, const double *esno_db, int npts,
                      double fixed_lin, int frames, int min_errors, int batch, frame_fn make_frame,
                      double (*fer)[MAX_PTS])
{
    for (int ip = 0; ip < npts; ip++) {
        const double esno = pow(10.0, esno_db[ip] / 10.0);
        long nerr[MAX_COLS] = {0};
        int ntot = 0;
        uint64_t t0 = ldpc_now_ns();
        for (;;) {
            long fewest = nerr[0];
            for (int v = 1; v < nv; v++) if (nerr[v] < fewest) fewest = nerr[v];
            if (ntot >= frames || fewest >= min_errors) break;
            int F = batch < frames - ntot ? batch : frames - ntot;
            for (int f = 0; f < F; f++) {
                make_frame(b, esno, fixed_lin);
                for (int v = 0; v < nv; v++) {
                    decode_variant(b, &vars[v]);
                    if (memcmp(b->bits, b->info, b->K)) nerr[v]++;
                }
            }
            ntot += F;
        }
        fprintf(stderr, "  Es/N0 %+.2f dB: %d frames, %.1f s, errors", esno_db[ip], ntot, (ldpc_now_ns() - t0) / 1e9);
        for (int v = 0; v < nv; v++) {
            fer[v][ip] = (double) nerr[v] / ntot;
            fprintf(stderr, " %s=%ld", vars[v].name, nerr[v]);
        }
        fputc('\n', stderr);
    }
}

/* ---- modes -------------------------------------------------------------- */

static const variant_t fer_variants[] = {
    {"C_leg10",            LDPC_ALG_LEGACY,  10, 0, 0.0f},
    {"C_leg50",            LDPC_ALG_LEGACY,  50, 0, 0.0f},
    {"C_sp50",             LDPC_ALG_SP,      50, 0, 0.0f},
    {"C_nms50",            LDPC_ALG_NMS,     50, 0, 0.0f},
    {"C_leg10_fixedEsNo",  LDPC_ALG_LEGACY,  10, 1, 0.0f},
    {"C_nms50_fixedEsNo",  LDPC_ALG_NMS,     50, 1, 0.0f},
    {"C_nms16_50",         LDPC_ALG_NMS16,   50, 0, 0.0f},
    {"C_spt50",            LDPC_ALG_SPT,     50, 0, 0.0f},
    {"C_leg100_fixedEsNo", LDPC_ALG_LEGACY, 100, 1, 0.0f},
    {"C_nms16_100",        LDPC_ALG_NMS16,  100, 0, 0.0f},
    {"C_spt100",           LDPC_ALG_SPT,    100, 0, 0.0f},
};
#define N_FER_VARIANTS ((int) (sizeof fer_variants / sizeof *fer_variants))

static double default_fixed_db(const char *code)
{
    /* ofdm_mode.c: EsNodB 3.0 for every mode, 10.0 for the 16200-bit code */
    return !strcmp(code, "H_16200_9720") ? 10.0 : 3.0;
}

static int mode_fer(int argc, char **argv, const char *code, unsigned seed, const char *out, const char *nms_override)
{
    bench_t b;
    bench_open(&b, code, seed);
    double ebno[MAX_PTS], esno_db[MAX_PTS];
    int npts = parse_grid(opt(argc, argv, "--ebno") ? opt(argc, argv, "--ebno") : "0:0.5:4", ebno, MAX_PTS);
    int frames = opt_int(argc, argv, "--frames", 600);
    int min_errors = opt_int(argc, argv, "--min-errors", 30);
    int batch = opt_int(argc, argv, "--batch", 20);
    double fixed_db = opt_dbl(argc, argv, "--fixed-db", default_fixed_db(code));
    float scale = nms_override ? (float) atof(nms_override) : ldpc_ctx_policy_scale(code);
    if (nms_override && (scale <= 0.0f || scale > 1.0f)) { fprintf(stderr, "--nms-scale must be in (0, 1]\n"); return 2; }
    if (frames < 1 || batch < 1 || min_errors < 1) { fprintf(stderr, "--frames, --batch and --min-errors must be positive\n"); return 2; }

    variant_t vars[MAX_COLS];
    colname_t names[MAX_COLS];
    int nv = 0;
    const char *sel = opt(argc, argv, "--variants");
    if (sel) {
        colname_t want[MAX_COLS];
        int nw = parse_list(sel, want, MAX_COLS);
        for (int i = 0; i < nw; i++) {
            int found = -1;
            for (int v = 0; v < N_FER_VARIANTS; v++) if (!strcmp(fer_variants[v].name, want[i])) found = v;
            if (found < 0) { fprintf(stderr, "unknown variant '%s'\n", want[i]); usage(argv[0]); return 2; }
            vars[nv++] = fer_variants[found];
        }
    } else {
        for (int v = 0; v < N_FER_VARIANTS; v++) vars[nv++] = fer_variants[v];
    }
    for (int v = 0; v < nv; v++) { vars[v].scale = scale; memcpy(names[v], vars[v].name, NAME_LEN); }
    for (int ip = 0; ip < npts; ip++) esno_db[ip] = ebno[ip] + 10.0 * log10(b.R);

    printf("# ldpc_sweep --fer %s: N=%d K=%d R=%.4f BPSK/AWGN, seed=%u, frames=%d, min_errors=%d, batch=%d, "
           "fixed_db=%g, nms_scale=%.2f, Eb/N0 %g:%g:%g dB (%d points)\n",
           code, b.N, b.K, b.R, seed, frames, min_errors, batch, fixed_db, (double) scale,
           ebno[0], npts > 1 ? ebno[1] - ebno[0] : 0.0, ebno[npts - 1], npts);
    fflush(stdout);
    fprintf(stderr, "%s --fer: seed %u, %d variants, %d points\n", code, seed, nv, npts);

    static double fer[MAX_COLS][MAX_PTS];
    uint64_t t0 = ldpc_now_ns();
    run_sweep(&b, vars, nv, esno_db, npts, pow(10.0, fixed_db / 10.0), frames, min_errors, batch, bpsk_frame, fer);
    fprintf(stderr, "%s --fer: %.1f s\n", code, (ldpc_now_ns() - t0) / 1e9);

    char path[1024];
    if (!out) { snprintf(path, sizeof path, "ldpc_fer_%s.csv", code); out = path; }
    write_csv(out, names, nv, esno_db, fer, npts);
    print_crossings(code, names, nv, esno_db, fer, npts);
    bench_close(&b);
    return 0;
}

static int mode_crossings(int argc, char **argv, const char *csv)
{
    colname_t names[MAX_COLS];
    static double esno[MAX_PTS], fer[MAX_COLS][MAX_PTS];
    int npts = 0;
    int ncols = read_csv(csv, names, esno, fer, &npts);
    /* the code name: --code, else the file name ldpc_fer_<code>.csv or llr_scale_<code>.csv */
    char code[NAME_LEN * 2];
    const char *given = opt(argc, argv, "--code");
    if (given) {
        snprintf(code, sizeof code, "%s", given);
    } else {
        const char *base = strrchr(csv, '/');
        base = base ? base + 1 : csv;
        if (!strncmp(base, "ldpc_fer_", 9)) base += 9;
        else if (!strncmp(base, "llr_scale_", 10)) base += 10;
        snprintf(code, sizeof code, "%s", base);
        char *dot = strstr(code, ".csv");
        if (dot) *dot = '\0';
    }
    printf("# %s: %d columns, %d points\n", csv, ncols, npts);
    print_crossings(code, names, ncols, esno, fer, npts);
    return 0;
}

/* the three code/modulation pairs the record uses */
static const struct { const char *code; int bps; double fixed_db; const char *ebno; } llr_pairs[] = {
    {"H_256_768_22",   2,  3.0, "-1:0.5:3"},
    {"H_4096_8192_3d", 2,  3.0, "0:0.5:4"},
    {"H_16200_9720",   4, 10.0, "1:0.5:8"},
};

static int mode_llr_scale(int argc, char **argv, const char *code, unsigned seed, const char *out)
{
    int bps = 0;
    double fixed_db = default_fixed_db(code);
    const char *grid = "0:0.5:4";
    for (size_t i = 0; i < sizeof llr_pairs / sizeof *llr_pairs; i++)
        if (!strcmp(llr_pairs[i].code, code)) { bps = llr_pairs[i].bps; fixed_db = llr_pairs[i].fixed_db; grid = llr_pairs[i].ebno; }
    bps = opt_int(argc, argv, "--bps", bps);
    fixed_db = opt_dbl(argc, argv, "--fixed-db", fixed_db);
    if (opt(argc, argv, "--ebno")) grid = opt(argc, argv, "--ebno");
    if (bps < 1) { fprintf(stderr, "--bps B is required for %s\n", code); return 2; }
    int frames = opt_int(argc, argv, "--frames", 300);
    int min_errors = opt_int(argc, argv, "--min-errors", 30);
    int batch = opt_int(argc, argv, "--batch", 10);
    if (frames < 1 || batch < 1 || min_errors < 1) { fprintf(stderr, "--frames, --batch and --min-errors must be positive\n"); return 2; }

    bench_t b;
    bench_open(&b, code, seed);
    bench_open_qam(&b, bps);
    double ebno[MAX_PTS], esno_db[MAX_PTS];
    int npts = parse_grid(grid, ebno, MAX_PTS);
    for (int ip = 0; ip < npts; ip++) esno_db[ip] = ebno[ip] + 10.0 * log10(b.R * bps);   /* per symbol */

    variant_t vars[2] = {
        {"true",  LDPC_ALG_SPT, 50, 0, 0.0f},
        {"fixed", LDPC_ALG_SPT, 50, 1, 0.0f},
    };
    colname_t names[2];
    for (int v = 0; v < 2; v++) memcpy(names[v], vars[v].name, NAME_LEN);

    printf("# ldpc_sweep --llr-scale %s: N=%d K=%d R=%.4f, %d bits/symbol over complex AWGN, max-log LLRs, "
           "spt at cap 50, seed=%u, frames=%d, min_errors=%d, batch=%d, fixed_db=%g, Eb/N0 %s dB (%d points)\n",
           code, b.N, b.K, b.R, bps, seed, frames, min_errors, batch, fixed_db, grid, npts);
    fflush(stdout);
    fprintf(stderr, "%s --llr-scale: seed %u, bps %d, fixed %g dB, %d points\n", code, seed, bps, fixed_db, npts);

    static double fer[MAX_COLS][MAX_PTS];
    uint64_t t0 = ldpc_now_ns();
    run_sweep(&b, vars, 2, esno_db, npts, pow(10.0, fixed_db / 10.0), frames, min_errors, batch, qam_frame, fer);
    fprintf(stderr, "%s --llr-scale: %.1f s\n", code, (ldpc_now_ns() - t0) / 1e9);

    char path[1024];
    if (!out) { snprintf(path, sizeof path, "llr_scale_%s.csv", code); out = path; }
    write_csv(out, names, 2, esno_db, fer, npts);
    print_crossings(code, names, 2, esno_db, fer, npts);
    bench_close(&b);
    return 0;
}

static int mode_nms_scale(int argc, char **argv, const char *code, unsigned seed, const char *out)
{
    colname_t scale_s[MAX_COLS];
    int ns = parse_list(opt(argc, argv, "--scales") ? opt(argc, argv, "--scales") : "0.70,0.75,0.80,0.85,0.90",
                        scale_s, MAX_COLS - 1);
    if (ns < 1) { fprintf(stderr, "--scales needs at least one value\n"); return 2; }
    int frames = opt_int(argc, argv, "--frames", 400);
    int min_errors = opt_int(argc, argv, "--min-errors", 30);
    int batch = opt_int(argc, argv, "--batch", 20);
    if (frames < 1 || batch < 1 || min_errors < 1) { fprintf(stderr, "--frames, --batch and --min-errors must be positive\n"); return 2; }

    bench_t b;
    bench_open(&b, code, seed);
    double ebno[MAX_PTS], esno_db[MAX_PTS];
    const char *grid = opt(argc, argv, "--ebno") ? opt(argc, argv, "--ebno") : "0:0.5:4";
    int npts = parse_grid(grid, ebno, MAX_PTS);
    for (int ip = 0; ip < npts; ip++) esno_db[ip] = ebno[ip] + 10.0 * log10(b.R);

    /* column 0 is the reference, sum-product; then float min-sum per scale */
    variant_t vars[MAX_COLS];
    colname_t names[MAX_COLS];
    int nv = 0;
    vars[nv] = (variant_t) {"sp", LDPC_ALG_SP, 50, 0, 0.0f};
    nv++;
    for (int i = 0; i < ns; i++) {
        double sc = atof(scale_s[i]);
        if (sc <= 0.0 || sc > 1.0) { fprintf(stderr, "scale '%s' is outside (0, 1]\n", scale_s[i]); return 2; }
        vars[nv] = (variant_t) {"", LDPC_ALG_NMS, 50, 0, (float) sc};
        snprintf(vars[nv].name, NAME_LEN, "nms%.2f", sc);
        nv++;
    }
    for (int v = 0; v < nv; v++) memcpy(names[v], vars[v].name, NAME_LEN);

    fprintf(stderr, "%s --nms-scale: seed %u, %d scales, %d points, cap 50, %d frames per point\n",
            code, seed, ns, npts, frames);
    static double fer[MAX_COLS][MAX_PTS];
    uint64_t t0 = ldpc_now_ns();
    /* every variant decodes the exact LLRs, so the fixed constant is unused */
    run_sweep(&b, vars, nv, esno_db, npts, 1.0, frames, min_errors, batch, bpsk_frame, fer);
    fprintf(stderr, "%s --nms-scale: %.1f s\n", code, (ldpc_now_ns() - t0) / 1e9);

    char path[1024];
    if (!out) { snprintf(path, sizeof path, "nms_scale_%s.csv", code); out = path; }
    write_csv(out, names, nv, esno_db, fer, npts);

    /* one line in the nms_tuning.txt format: loss against sp at FER 1e-1 / 1e-2 */
    double x1[MAX_COLS], x2[MAX_COLS];
    char s1[16], s2[16];
    for (int v = 0; v < nv; v++) {
        x1[v] = esno_at_fer(esno_db, fer[v], npts, 1e-1);
        x2[v] = esno_at_fer(esno_db, fer[v], npts, 1e-2);
    }
    printf("%s: sp=+0.00/+0.00", code);
    for (int v = 1; v < nv; v++)
        printf(" %s=%s/%s", names[v], fmt_db(s1, sizeof s1, x1[v] - x1[0]), fmt_db(s2, sizeof s2, x2[v] - x2[0]));
    printf("  # sp at %s/%s dB; float nms vs sp, cap 50, %d frames per point, BPSK/AWGN, Eb/N0 %s dB, seed %u\n",
           fmt_db(s1, sizeof s1, x1[0]), fmt_db(s2, sizeof s2, x2[0]), frames, grid, seed);
    bench_close(&b);
    return 0;
}

static int mode_fixture(int argc, char **argv, const char *code, unsigned seed, const char *out)
{
    const char *esno_s = opt(argc, argv, "--esno");
    if (!esno_s || !out) { fprintf(stderr, "--fixture needs --esno X_dB and --out BASE\n"); return 2; }
    const double esno_db = atof(esno_s), esno = pow(10.0, esno_db / 10.0);
    int frames = opt_int(argc, argv, "--frames", 100);
    if (frames < 1) { fprintf(stderr, "--frames must be positive\n"); return 2; }

    bench_t b;
    bench_open(&b, code, seed);
    char fl[1024], fi[1024];
    snprintf(fl, sizeof fl, "%s.f32", out);
    snprintf(fi, sizeof fi, "%s.info.u8", out);
    FILE *fL = fopen(fl, "wb"), *fI = fopen(fi, "wb");
    if (!fL || !fI) { perror(out); return 1; }
    for (int f = 0; f < frames; f++) {
        bpsk_frame(&b, esno, 1.0);   /* only the exact LLRs are written */
        fwrite(b.llr[0], sizeof(float), b.N, fL);
        fwrite(b.info, 1, b.K, fI);
    }
    fclose(fL);
    fclose(fI);
    printf("%s: wrote %s (%d frames x %d float32 exact LLRs) and %s (%d frames x %d info bytes), "
           "BPSK/AWGN at Es/N0 %.2f dB, seed %u\n", code, fl, frames, b.N, fi, frames, b.K, esno_db, seed);
    bench_close(&b);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || has(argc, argv, "--help")) { usage(argv[0]); return 2; }

    /* --nms-scale is the sweep mode on its own; followed by a number it is
     * the --fer override of the min-sum scale */
    int mode_nms = 0;
    const char *nms_override = NULL;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--nms-scale")) {
            if (i + 1 < argc && is_number(argv[i + 1])) nms_override = argv[i + 1];
            else mode_nms = 1;
        }
    const char *csv = opt(argc, argv, "--crossings");
    int modes = has(argc, argv, "--fer") + (csv != NULL) + has(argc, argv, "--llr-scale") + mode_nms + has(argc, argv, "--fixture");
    if (modes != 1) { usage(argv[0]); return 2; }

    if (csv) return mode_crossings(argc, argv, csv);

    const char *code = opt(argc, argv, "--code");
    const char *out = opt(argc, argv, "--out");
    const char *seed_s = opt(argc, argv, "--seed");
    unsigned seed = seed_s ? (unsigned) strtoul(seed_s, NULL, 0) : 1u;
    if (!code) { usage(argv[0]); return 2; }

    if (has(argc, argv, "--fer")) return mode_fer(argc, argv, code, seed, out, nms_override);
    if (has(argc, argv, "--llr-scale")) return mode_llr_scale(argc, argv, code, seed, out);
    if (mode_nms) return mode_nms_scale(argc, argv, code, seed, out);
    return mode_fixture(argc, argv, code, seed, out);
}
