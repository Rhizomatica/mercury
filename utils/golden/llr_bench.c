/* llr_bench — run codec2's soft demappers on a file of received symbols.
 *
 *   llr_bench --bps B --esno dB [--path somap|maxlog] in.f32 out.f32
 *     in : interleaved (re, im) float32 symbols, unit-power constellation,
 *          already equalised (amplitude 1)
 *     out: B float32 LLRs per symbol, positive = bit 0, k = 0 is the label MSB
 *   somap  = symbols_to_llrs (Demod2D + Somap, bps 2 and 4 only)
 *   maxlog = llr_from_qam (bps 2, 4, 5, 6, 7, 8)
 *
 * Copyright (C) 2026 Joseph Freivald
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mpdecode_core.h"

static const char *opt(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv)
{
    const char *bps_s = opt(argc, argv, "--bps"), *esno_s = opt(argc, argv, "--esno"), *path = opt(argc, argv, "--path");
    const char *dump = opt(argc, argv, "--dump-table");
    if (bps_s && dump) {
        /* write the constellation the C side uses: M interleaved (re, im) float32 */
        int M = 0;
        const COMP *t = ldpc_qam_table(atoi(bps_s), &M);
        if (!t) { fprintf(stderr, "no table for bps %s\n", bps_s); return 1; }
        FILE *fd = fopen(dump, "wb");
        if (!fd) { perror(dump); return 1; }
        fwrite(t, sizeof(COMP), (size_t) M, fd);
        fclose(fd);
        return 0;
    }
    if (!bps_s || !esno_s || argc < 3) {
        fprintf(stderr, "usage: %s --bps B --esno dB [--path somap|maxlog] in.f32 out.f32\n       %s --bps B --dump-table table.f32\n", argv[0], argv[0]);
        return 2;
    }
    const char *fin = argv[argc - 2], *fout = argv[argc - 1];
    int bps = atoi(bps_s);
    float EsNo = powf(10.0f, (float) atof(esno_s) / 10.0f);
    int maxlog = !(path && !strcmp(path, "somap"));
    FILE *fi = fopen(fin, "rb"), *fo = fopen(fout, "wb");
    if (!fi || !fo) { perror("open"); return 1; }
    enum { CH = 4096 };
    COMP sym[CH]; float amps[CH], llr[CH * 8];
    size_t n;
    long total = 0;
    while ((n = fread(sym, sizeof(COMP), CH, fi)) > 0) {
        for (size_t i = 0; i < n; i++) amps[i] = 1.0f;
        if (maxlog) llr_from_qam(llr, sym, amps, 1.0f, EsNo, bps, (int) n);
        else        symbols_to_llrs(llr, sym, amps, EsNo, 1.0f, bps, (int) n);
        fwrite(llr, sizeof(float), n * bps, fo);
        total += (long) n;
    }
    fprintf(stderr, "%s: %ld symbols, bps %d, EsNo %.2f dB\n", maxlog ? "maxlog" : "somap", total, bps, 10.0 * log10(EsNo));
    fclose(fi); fclose(fo);
    return 0;
}
