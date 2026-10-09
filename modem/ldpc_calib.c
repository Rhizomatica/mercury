/* ldpc_calib.c — time the LDPC decoder on this host (mercury -B).
 *
 * Copyright (C) 2026 Joseph Freivald
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Decodes 20 non-converging iterations (random small LLRs) of every data-mode
 * code with each algorithm, three runs, best time kept, and reports ns per
 * iteration and how many iterations fit in 100 ms.  This is what the
 * per-mode budget in [ldpc] is spent against.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ldpc_calib.h"
#include "ldpc_ctx.h"
#include "mpdecode_core.h"
#include "ldpc_codes.h"

static const struct { const char *code; const char *modes; } codes[] = {
    {"H_256_768_22",   "DATAC15 DATAC16"},
    {"H_256_512_4",    "DATAC13"},
    {"H_128_256_5",    "DATAC0"},
    {"HRA_56_56",      "DATAC14"},
    {"H_1024_2048_4f", "DATAC3 DATAC4"},
    {"H_4096_8192_3d", "DATAC1"},
    {"H_16200_9720",   "DATAC17 QAM16C2"},
};
static const int algs[] = {LDPC_ALG_SPT, LDPC_ALG_NMS16, LDPC_ALG_SP};

int ldpc_calib_run(FILE *out)
{
    fprintf(out, "LDPC decoder calibration: ns per iteration (20 non-converging iterations, best of 3)\n");
    fprintf(out, "%-16s %-18s %-6s %9s %9s %9s   %s\n", "code", "modes", "policy", "spt", "nms16", "sp", "iters/100ms (policy)");
    uint32_t seed = 12345;
    for (size_t c = 0; c < sizeof codes / sizeof *codes; c++) {
        struct LDPC l;
        ldpc_codes_setup(&l, (char *) codes[c].code);
        ldpc_ctx_t *ctx = ldpc_ctx_create(&l);
        if (!ctx) continue;
        const int N = l.CodeLength;
        float *llr = malloc(sizeof(float) * N);
        uint8_t *bits = malloc(N);
        for (int n = 0; n < N; n++) {
            seed = seed * 1664525u + 1013904223u;
            llr[n] = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 2.0f;   /* +/-1: never converges */
        }
        double ns[3] = {0, 0, 0};
        for (size_t a = 0; a < 3; a++) {
            ldpc_ctx_set_alg(ctx, algs[a], 0.0f);
            double best = 1e30;
            for (int run = 0; run < 3; run++) {
                ldpc_stats_t st;
                uint64_t t0 = ldpc_now_ns();
                ldpc_ctx_decode(ctx, llr, bits, 20, 0, &st);
                uint64_t t1 = ldpc_now_ns();
                double per = (double)(t1 - t0) / (st.iters > 0 ? st.iters : 1);
                if (per < best) best = per;
            }
            ns[a] = best;
        }
        int pol = ldpc_ctx_policy_alg(codes[c].code);
        double pol_ns = pol == LDPC_ALG_NMS16 ? ns[1] : pol == LDPC_ALG_SP ? ns[2] : ns[0];
        fprintf(out, "%-16s %-18s %-6s %9.0f %9.0f %9.0f   %.0f\n", codes[c].code, codes[c].modes,
                ldpc_alg_name(pol), ns[0], ns[1], ns[2], 100e6 / pol_ns);
        free(llr); free(bits);
        ldpc_ctx_destroy(ctx);
    }
    fprintf(out, "Set [ldpc] max_iter / budget_ms in mercury.ini accordingly; the status report shows deadline hits.\n");
    return 0;
}
