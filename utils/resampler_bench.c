/* resampler_bench — how much CPU does the 48 kHz <-> 8 kHz conversion cost?
 *
 * Mercury's own bench and both field stations run their sound cards at 8 kHz,
 * where resamp_*_process() takes a memcpy fast path and costs nothing.  A USB
 * codec locked to 48 kHz does not: every captured sample goes through a
 * 180-tap history, and that path had never been measured.  Issue #162 reported
 * 100 % CPU and "RX backlog exceeded ~2 s cap" on a Pi 4 with a 48 kHz codec,
 * with the backlog starting before any client connected or any session existed
 * -- i.e. in the idle capture path.
 *
 * Reports CPU time per second of audio, which is the number that matters: the
 * capture direction must stay well under 100 % of one core on the slowest
 * supported host or the RX ring falls behind in real time.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "resampler.h"

static double now_cpu_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    int rate    = (argc > 1) ? atoi(argv[1]) : 48000;
    int seconds = (argc > 2) ? atoi(argv[2]) : 20;

    int L = resampler_ratio_for_rate(rate);
    if (L <= 0) { fprintf(stderr, "unsupported rate %d\n", rate); return 1; }

    resampler_global_init();
    resampler_init_down(L);
    resampler_init_up(L);

    /* One ALSA period's worth at a time, as audioio does. */
    const int chunk_dev = rate / 100;           /* 10 ms of device-rate audio */
    const int chunk_mod = RESAMP_MODEM_FS / 100;

    int32_t *in_dev  = malloc(sizeof(int32_t) * (size_t)chunk_dev);
    int32_t *out_mod = malloc(sizeof(int32_t) * (size_t)(chunk_dev + 64));
    int32_t *in_mod  = malloc(sizeof(int32_t) * (size_t)chunk_mod);
    int32_t *out_dev = malloc(sizeof(int32_t) * (size_t)(chunk_mod * L + 64));
    if (!in_dev || !out_mod || !in_mod || !out_dev) return 1;

    for (int i = 0; i < chunk_dev; i++)
        in_dev[i] = (int32_t)(20000.0 * sin(2.0 * M_PI * 1000.0 * i / rate));
    for (int i = 0; i < chunk_mod; i++)
        in_mod[i] = (int32_t)(20000.0 * sin(2.0 * M_PI * 1000.0 * i / RESAMP_MODEM_FS));

    printf("device rate %d Hz (L=%d, %d taps)  |  %d s of audio\n\n",
           rate, L, L * RESAMP_TAPS_PER_PHASE, seconds);

    /* ---- capture direction: device rate -> 8 kHz ---- */
    {
        resamp_down_t d;
        resamp_down_reset(&d);
        int iters = seconds * 100;
        double t0 = now_cpu_s();
        for (int i = 0; i < iters; i++)
            resamp_down_process(&d, in_dev, chunk_dev, out_mod);
        double dt = now_cpu_s() - t0;
        printf("  capture  (down): %7.3f s CPU for %d s audio  = %6.2f %% of one core\n",
               dt, seconds, 100.0 * dt / seconds);
    }

    /* ---- playback direction: 8 kHz -> device rate ---- */
    {
        resamp_up_t u;
        resamp_up_reset(&u);
        int iters = seconds * 100;
        double t0 = now_cpu_s();
        for (int i = 0; i < iters; i++)
            resamp_up_process(&u, in_mod, chunk_mod, out_dev);
        double dt = now_cpu_s() - t0;
        printf("  playback (up)  : %7.3f s CPU for %d s audio  = %6.2f %% of one core\n",
               dt, seconds, 100.0 * dt / seconds);
    }

    printf("\n  (capture runs continuously; playback only while transmitting)\n");
    free(in_dev); free(out_mod); free(in_mod); free(out_dev);
    return 0;
}
