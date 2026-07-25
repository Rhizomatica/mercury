/* pattern_fade_probe — deterministic PHY-robustness probe for the fast-windowed
 * ACK epoch-tagged pattern (MERCURY_FAST_ACK).  Emits N independent pattern
 * segments (tagged or bare) as int16 passband, or reads a (Watterson-faded)
 * int16 stream and detects each segment — reporting the two rates that decide
 * whether the flag is safe to enable on-air:
 *
 *   epoch-decode-rate  (speed)   — tagged pattern recovered with the RIGHT epoch
 *   false-tag-rate     (SAFETY)  — a BARE pattern mis-read as tagged.  This one
 *                                  must stay ~0: a false tag would make the ISS
 *                                  retire_all when only one block was clean
 *                                  (over-retirement -> data loss).
 *
 * The channel is applied BETWEEN emit and detect by utils/watterson_test (the
 * calibrated Watterson model, matches freedv ch.c), driven by fastack_fade_phy.sh.
 *
 *   pattern_fade_probe emit   <kind_hex> <N> <out.raw>
 *   pattern_fade_probe detect <kind_hex> <N> <in.raw>
 * kind_hex: 0/1 = bare ACK/break;  0x80|epoch<<1|brk = tagged (e.g. 0x84 = epoch2).
 *
 * Copyright (C) 2026 Rhizomatica  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "modem_mfsk.h"

static int seg_len(void) { return 3 * mfsk_pattern_max_tx_samples(); }   /* gap+pat+gap */
static int gap_len(void) { return mfsk_pattern_max_tx_samples(); }

/* one segment: [gap zeros][pattern(kind)][gap zeros]; the faded stream gets
 * AWGN over the zeros too, so each segment is an independent fade realisation. */
static int emit_segment(int kind, int16_t *seg)
{
    int L = seg_len(), g = gap_len();
    memset(seg, 0, (size_t)L * sizeof(int16_t));
    int n = mfsk_pattern_tx(seg + g, kind);
    return (n > 0) ? L : -1;
}

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: %s emit|detect <kind_hex> <N> <file>\n", argv[0]); return 2; }
    int  kind = (int)strtol(argv[2], NULL, 0);
    int  N    = atoi(argv[3]);
    const char *file = argv[4];
    int  L = seg_len();

    if (!strcmp(argv[1], "emit"))
    {
        FILE *f = fopen(file, "wb");
        if (!f) { perror("fopen"); return 1; }
        int16_t *seg = malloc((size_t)L * sizeof(int16_t));
        for (int i = 0; i < N; i++)
        {
            if (emit_segment(kind, seg) < 0) { fprintf(stderr, "emit failed\n"); return 1; }
            fwrite(seg, sizeof(int16_t), (size_t)L, f);
        }
        free(seg); fclose(f);
        return 0;
    }

    if (!strcmp(argv[1], "detect"))
    {
        FILE *f = fopen(file, "rb");
        if (!f) { perror("fopen"); return 1; }
        int16_t *seg = malloc((size_t)L * sizeof(int16_t));
        int exp_tagged = (kind & MFSK_PATTERN_TAGGED) != 0;
        int exp_epoch  = (kind >> 1) & 3;
        int exp_brk    = kind & 1;
        int correct = 0, misdecode = 0, got_bare = 0, miss = 0, false_tag = 0, got = 0;
        for (int i = 0; i < N; i++)
        {
            if (fread(seg, sizeof(int16_t), (size_t)L, f) != (size_t)L) break;
            got++;
            int out = 0;
            int rc = mfsk_pattern_detect(seg, L, /*expect_epoch=*/1, &out);
            if (rc != 1) { miss++; continue; }        /* base pattern lost to fade */
            int is_tag = (out & MFSK_PATTERN_TAGGED) != 0;
            if (exp_tagged) {
                if (is_tag && ((out >> 1) & 3) == exp_epoch && (out & 1) == exp_brk) correct++;
                else if (is_tag) misdecode++;         /* tagged but wrong epoch/brk */
                else got_bare++;                       /* fell back to bare (safe) */
            } else {
                if (is_tag) false_tag++;               /* SAFETY violation */
                else correct++;                        /* stayed bare (safe) */
            }
        }
        fclose(f); free(seg);
        /* machine-readable one-liner for the driver to aggregate */
        printf("PROBE kind=0x%02x tagged=%d n=%d correct=%d misdecode=%d bare=%d miss=%d false_tag=%d\n",
               kind, exp_tagged, got, correct, misdecode, got_bare, miss, false_tag);
        return 0;
    }

    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}
