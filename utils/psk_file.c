/* psk_file -- modem/psk frames to and from raw audio, for on-air tests:
 *
 *   psk_file tx <level> <nframes> <seed> <peak> <gap s> <out.s16>
 *       nframes frames of known payloads (from seed and the frame index),
 *       each scaled to |peak|, with gap s of silence between and around
 *       them; raw s16le at 8 kHz.
 *   psk_file rx <level> <nframes> <seed> <in.s16>
 *       how many of those frames come back from a recording: each window
 *       one frame long plus margin, slid by half a frame, is searched and
 *       decoded; a decode is counted once per frame index.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../modem/psk/mpsk.h"

static void payload(uint8_t *p, int nb, unsigned seed, int idx)
{
    uint32_t s = seed * 2654435761u + (uint32_t)idx * 40503u + 1u;
    for (int i = 0; i < nb - 2; i++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        p[i] = (uint8_t)s;
    }
    p[0] = (uint8_t)(idx >> 8);                  /* the index, for the count */
    p[1] = (uint8_t)idx;
}

static int level_of(const char *name)
{
    for (int i = 0; i < mpsk_nlevels; i++)
        if (!strcmp(name, mpsk_levels[i].name)) return i;
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: %s tx <level> <nframes> <seed> <peak> <gap s> <out.s16>\n"
                        "       %s rx <level> <nframes> <seed> <in.s16>\n", argv[0], argv[0]);
        return 1;
    }
    int lv = level_of(argv[2]), nf = atoi(argv[3]);
    unsigned seed = (unsigned)strtoul(argv[4], NULL, 10);
    mpsk_t *m = lv >= 0 ? mpsk_open(lv) : NULL;
    if (!m || nf <= 0 || nf > 65536) { fprintf(stderr, "bad level or count\n"); return 1; }
    int nb = mpsk_frame_bytes(m), ns = mpsk_tx_samples(m);
    uint8_t *pl = malloc((size_t)nb), *got = malloc((size_t)nb), *want = malloc((size_t)nb);

    if (!strcmp(argv[1], "tx") && argc >= 8) {
        double peak = atof(argv[5]);
        int gap = (int)(atof(argv[6]) * MPSK_FS);
        int total = gap + nf * (ns + gap);
        int16_t *out = calloc((size_t)total, sizeof(int16_t));
        for (int f = 0; f < nf; f++) {
            payload(pl, nb, seed, f);
            mpsk_modulate(m, pl, out + gap + f * (ns + gap), peak);
        }
        FILE *fo = fopen(argv[7], "wb");
        if (!fo) { perror(argv[7]); return 1; }
        fwrite(out, sizeof(int16_t), (size_t)total, fo);
        fclose(fo);
        printf("%s: %d frames of %d B, %.2f s each, %.1f s in all\n", argv[2], nf, nb - 2,
               (double)ns / MPSK_FS, (double)total / MPSK_FS);
        return 0;
    }
    if (!strcmp(argv[1], "rx")) {
        FILE *fi = fopen(argv[5], "rb");
        if (!fi) { perror(argv[5]); return 1; }
        fseek(fi, 0, SEEK_END);
        long n = ftell(fi) / 2;
        fseek(fi, 0, SEEK_SET);
        int16_t *in = malloc(sizeof(int16_t) * (size_t)n);
        if (fread(in, sizeof(int16_t), (size_t)n, fi) != (size_t)n) { fclose(fi); return 1; }
        fclose(fi);
        char *seen = calloc((size_t)nf, 1);
        /* a window holds a whole frame when it starts at most 1 s before it:
         * step 0.5 s */
        int win = ns + MPSK_FS, step = MPSK_FS / 2, ok = 0, found = 0;
        double snrsum = 0.0;
        for (long a = 0; a + win <= n; a += step) {
            int f0 = 0;
            float snr = 0.0f;
            if (!mpsk_demod(m, in + a, win, got, &f0, &snr)) continue;
            found++;
            int idx = got[0] << 8 | got[1];
            if (idx >= nf || seen[idx]) continue;
            payload(want, nb, seed, idx);
            if (memcmp(got, want, (size_t)(nb - 2))) continue;
            seen[idx] = 1;
            ok++;
            snrsum += snr;
            printf("  frame %d decoded at %.1f s, raw est %.1f dB\n", idx, (double)a / MPSK_FS, snr);
        }
        printf("%s: %d of %d frames decoded", argv[2], ok, nf);
        if (ok) printf(", raw est %.1f dB", snrsum / ok);
        printf("\n");
        return 0;
    }
    fprintf(stderr, "tx or rx\n");
    return 1;
}
