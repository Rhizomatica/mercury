/* rxcost_bench — what does the idle RX chain actually cost per second of audio?
 *
 * Issue #162: 100 % CPU and "RX backlog exceeded ~2 s cap" on a Pi 4, with the
 * backlog appearing BEFORE any client connected and before any ARQ session
 * existed.  That rules out the ARQ layer and points at whatever the capture
 * path does on every chunk regardless of state.
 *
 * Mercury runs a decoder per plane when split control/data mode switching is
 * enabled, so an idle station demodulates its control mode AND its payload
 * mode over the same samples, continuously, searching for a preamble that is
 * not there.  This measures that, per mode and combined, as a percentage of
 * one core -- the figure that decides whether the RX ring keeps up in real
 * time.
 *
 * Feed it noise, which is the honest idle case: a decoder that never syncs
 * does the most search work.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "freedv_api.h"

#define FS 8000

static double now_cpu_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static unsigned long long rg = 7777;
static double ur(void)
{
    rg = rg * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((rg >> 11) & 0x1FFFFFFFFFFFFFULL) / (double)0x20000000000000ULL;
}

static int mode_of(const char *s)
{
    if (!strcmp(s, "DATAC16")) return FREEDV_MODE_DATAC16;
    if (!strcmp(s, "DATAC15")) return FREEDV_MODE_DATAC15;
    if (!strcmp(s, "DATAC13")) return FREEDV_MODE_DATAC13;
    if (!strcmp(s, "DATAC4"))  return FREEDV_MODE_DATAC4;
    if (!strcmp(s, "DATAC3"))  return FREEDV_MODE_DATAC3;
    if (!strcmp(s, "DATAC1"))  return FREEDV_MODE_DATAC1;
    if (!strcmp(s, "DATAC0"))  return FREEDV_MODE_DATAC0;
    return -1;
}

/* Seconds of CPU burned demodulating `seconds` of noise in one mode. */
static double cost_of(const char *name, int seconds)
{
    int m = mode_of(name);
    if (m < 0) { fprintf(stderr, "unknown mode %s\n", name); exit(1); }

    struct freedv *f = freedv_open(m);
    if (!f) { fprintf(stderr, "freedv_open(%s) failed\n", name); exit(1); }
    freedv_set_frames_per_burst(f, 1);

    int nbytes = freedv_get_bits_per_modem_frame(f) / 8;
    unsigned char *out = malloc((size_t)nbytes + 16);
    short chunk[16384];

    long fed = 0;
    long need = (long)seconds * FS;
    double t0 = now_cpu_s();
    while (fed < need)
    {
        int nin = freedv_nin(f);
        if (nin <= 0 || nin > (int)(sizeof(chunk) / sizeof(chunk[0]))) break;
        for (int i = 0; i < nin; i++)
            chunk[i] = (short)((ur() - 0.5) * 6000.0);
        freedv_rawdatarx(f, out, chunk);
        fed += nin;
    }
    double dt = now_cpu_s() - t0;

    free(out);
    freedv_close(f);
    return dt;
}

int main(int argc, char **argv)
{
    int seconds = (argc > 1) ? atoi(argv[1]) : 20;

    static const char *modes[] = { "DATAC16", "DATAC15", "DATAC13",
                                   "DATAC4", "DATAC3", "DATAC1" };

    printf("Idle RX demod cost, %d s of noise per mode\n", seconds);
    printf("(a decoder that never syncs does the most search work)\n\n");
    printf("  mode       CPU s   %% of one core\n");

    double c16 = 0.0, c3 = 0.0;
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
    {
        double dt = cost_of(modes[i], seconds);
        printf("  %-9s %7.3f   %8.2f %%\n", modes[i], dt, 100.0 * dt / seconds);
        if (!strcmp(modes[i], "DATAC16")) c16 = dt;
        if (!strcmp(modes[i], "DATAC3"))  c3  = dt;
    }

    printf("\n  dual decoder as issue #162 runs it (DATAC16 control + DATAC3 payload):\n");
    printf("    %.2f %% of one core on THIS machine\n", 100.0 * (c16 + c3) / seconds);
    printf("    a Pi 4 core is roughly 5-10x slower than a modern x86 core,\n"
           "    so scale accordingly before drawing a conclusion.\n");
    return 0;
}
