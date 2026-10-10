/* ldpc_bench — drive codec2's LDPC encoder and SumProduct decoder on files, with
 * a controllable iteration budget, for the MATLAB golden tests.
 *
 *   ldpc_bench --code NAME --enc in.u8 out.u8
 *       in: K bytes per frame (0/1), out: N bytes per frame (info then parity)
 *   ldpc_bench --code NAME --dec in.f32 out.u8 [--mi N] [--stats file]
 *       in: N float32 LLRs per frame, codec2 sign (positive = bit 0),
 *       out: N bytes per frame (decoded codeword), stats: "iters parity_ok hit_deadline" per frame
 *
 * Note: SumProduct() also halts when the decoded information bits are all zero
 * (run_ldpc_decoder passes a zero reference vector), so tests must use random
 * payloads.
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

int main(int argc, char **argv)
{
    const char *code = opt(argc, argv, "--code");
    const char *enc = opt(argc, argv, "--enc");
    const char *dec = opt(argc, argv, "--dec");
    const char *mi = opt(argc, argv, "--mi");
    const char *stats = opt(argc, argv, "--stats");
    const char *alg = opt(argc, argv, "--alg");          /* legacy | sp | nms */
    const char *dl = opt(argc, argv, "--deadline-ms");   /* per-frame decode deadline */
    if (!code || (!enc && !dec) || has(argc, argv, "--help")) {
        fprintf(stderr, "usage: %s --code NAME --enc in.u8 out.u8 | --dec in.f32 out.u8 [--mi N] [--alg legacy|sp|spt|nms|nms16|auto] [--deadline-ms T] [--stats f]\n", argv[0]);
        return 2;
    }
    /* the output file is the positional argument after the input */
    const char *out = NULL;
    for (int i = 1; i < argc - 1; i++)
        if ((enc && !strcmp(argv[i], enc)) || (dec && !strcmp(argv[i], dec))) { out = argv[i + 1]; break; }
    if (!out) { fprintf(stderr, "missing output file\n"); return 2; }

    struct LDPC l;
    ldpc_codes_setup(&l, (char *) code);
    const int N = l.CodeLength, M = l.NumberParityBits, K = N - M;
    if (mi) l.max_iter = atoi(mi);
    if (alg) ldpc_set_default_alg(!strcmp(alg, "legacy") ? LDPC_ALG_LEGACY : !strcmp(alg, "nms") ? LDPC_ALG_NMS :
                                  !strcmp(alg, "nms16") ? LDPC_ALG_NMS16 : !strcmp(alg, "spt") ? LDPC_ALG_SPT :
                                  !strcmp(alg, "auto") ? LDPC_ALG_AUTO : LDPC_ALG_SP);
    double deadline_ms = dl ? atof(dl) : 0.0;

    FILE *fi = fopen(enc ? enc : dec, "rb"), *fo = fopen(out, "wb");
    if (!fi || !fo) { perror("open"); return 1; }
    FILE *fs = stats ? fopen(stats, "w") : NULL;

    if (enc) {
        unsigned char *ib = malloc(K), *pb = malloc(M);
        long frames = 0;
        while (fread(ib, 1, K, fi) == (size_t) K) {
            encode(&l, ib, pb);
            fwrite(ib, 1, K, fo);
            fwrite(pb, 1, M, fo);
            frames++;
        }
        fprintf(stderr, "%s: encoded %ld frames (K=%d N=%d)\n", l.name, frames, K, N);
        free(ib); free(pb);
    } else {
        float *llr = malloc(sizeof(float) * N);
        uint8_t *ob = malloc(N);
        long frames = 0, iters_tot = 0;
        while (fread(llr, sizeof(float), N, fi) == (size_t) N) {
            int pcc = 0;
            ldpc_stats_t st;
            uint64_t deadline = deadline_ms > 0.0 ? ldpc_now_ns() + (uint64_t)(deadline_ms * 1e6) : 0;
            int it = run_ldpc_decoder_ex(&l, ob, llr, &pcc, deadline, &st);
            fwrite(ob, 1, N, fo);
            if (fs) fprintf(fs, "%d %d %d\n", it, pcc == M, st.hit_deadline);
            iters_tot += it;
            frames++;
        }
        fprintf(stderr, "%s: decoded %ld frames, alg=%d max_iter=%d, mean iters=%.2f\n",
                l.name, frames, ldpc_get_default_alg(), l.max_iter, frames ? (double) iters_tot / frames : 0.0);
        free(llr); free(ob);
    }
    fclose(fi); fclose(fo);
    if (fs) fclose(fs);
    return 0;
}
