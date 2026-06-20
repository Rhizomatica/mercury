/* noisebridge — an AWGN channel between two ALSA snd-aloop cables.
 *
 * Reads s32le stereo (48 kHz) from stdin, adds white Gaussian noise, writes
 * s32le stereo to stdout.  Sits in an arecord|noisebridge|aplay pipe to inject
 * a controlled, per-direction noise level into a loopback audio path so two
 * mercury instances can talk over a kernel-paced "channel" (see run_loopsim.sh).
 *
 * Usage: noisebridge <stddev_frac> [seed]
 *   stddev_frac : noise std-dev as a fraction of full-scale (2^31).
 *                 0.0 = clean; ~1.0 ≈ -4 dB; ~2.0 stalls a marginal ACK path.
 *   seed        : PRNG seed (default 12345); use different seeds per direction.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Box-Muller standard normal. */
static double gauss(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 1.0);
    double u2 = (rand() + 1.0) / (RAND_MAX + 1.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

int main(int argc, char **argv)
{
    double lvl = (argc > 1) ? atof(argv[1]) : 0.0;   /* noise stddev, fraction of FS */
    double sd  = lvl * 2147483647.0;
    srand((argc > 2) ? atoi(argv[2]) : 12345);

    int32_t buf[2048];
    size_t n;
    while ((n = fread(buf, sizeof(int32_t), 2048, stdin)) > 0)
    {
        if (sd > 0.0)
        {
            for (size_t i = 0; i < n; i++)
            {
                double v = (double)buf[i] + sd * gauss();
                if (v >  2147483647.0) v =  2147483647.0;
                if (v < -2147483648.0) v = -2147483648.0;
                buf[i] = (int32_t)v;
            }
        }
        fwrite(buf, sizeof(int32_t), n, stdout);
        fflush(stdout);
    }
    return 0;
}
