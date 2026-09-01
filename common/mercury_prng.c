/* HERMES Modem — reproducible pseudo-random source
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See mercury_prng.h for why this is not just rand().
 */

#include "mercury_prng.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#define MERCURY_GETPID() _getpid()
#else
#include <unistd.h>
#define MERCURY_GETPID() getpid()
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Separation between the front and rear cursors for TYPE_3 (x**31 + x**3 + 1). */
#define SEP 3

void mercury_prng_seed(mercury_prng_t *p, unsigned int seed)
{
    int32_t word;
    int     i;

    if (!p)
        return;

    /* glibc maps 0 to 1: seeding the additive generator with an all-zero
     * state would make it emit zeros forever. */
    if (seed == 0)
        seed = 1;

    p->state[0] = (int32_t)seed;
    word = (int32_t)seed;

    /* state[i] = (16807 * state[i-1]) % 2147483647, computed by Schrage's
     * method so the intermediate product never overflows 31 bits. */
    for (i = 1; i < MERCURY_PRNG_DEG; ++i)
    {
        long hi = word / 127773;
        long lo = word % 127773;
        word = (int32_t)(16807 * lo - 2836 * hi);
        if (word < 0)
            word += 2147483647;
        p->state[i] = word;
    }

    p->f = SEP;
    p->r = 0;

    /* Discard the first 10*DEG outputs so the low-quality LCG warm-up does not
     * reach the caller.  glibc does exactly this. */
    for (i = 0; i < MERCURY_PRNG_DEG * 10; ++i)
        (void)mercury_prng_u31(p);
}

uint32_t mercury_prng_u31(mercury_prng_t *p)
{
    uint32_t val;

    /* Additive feedback: state[f] += state[r], both cursors then advance and
     * wrap.  Unsigned arithmetic because the addition is meant to wrap. */
    val = (uint32_t)p->state[p->f] + (uint32_t)p->state[p->r];
    p->state[p->f] = (int32_t)val;

    if (++p->f >= MERCURY_PRNG_DEG)
        p->f = 0;
    if (++p->r >= MERCURY_PRNG_DEG)
        p->r = 0;

    /* Chuck the least random bit, as random() does. */
    return val >> 1;
}

float mercury_prng_uniform(mercury_prng_t *p)
{
    /* Deliberately spelled to match the expression this replaced, which was
     *
     *     (float)rand() / RAND_MAX
     *
     * -- a FLOAT divide by (float)2147483647, which rounds to 2^31.  Using a
     * cleaner double divide here would be a fractional-ULP change, but the
     * Doppler-shaping IIR has poles within ~1e-4 of the unit circle, so it is
     * chaotically sensitive: a last-bit difference in the driving noise gives
     * a COMPLETELY different fade a few thousand samples later.  Matching the
     * old arithmetic exactly is what lets this port be verified as a
     * no-behaviour-change against every result measured before it.
     *
     * Range is [0, 1] inclusive, as it was; callers must tolerate both ends. */
    return (float)mercury_prng_u31(p) / (float)2147483647;
}

float mercury_prng_gaussian(mercury_prng_t *p)
{
    float x = mercury_prng_uniform(p);
    float y = mercury_prng_uniform(p);

    /* avoid log(0) — extremely unlikely but safe */
    if (x <= 0.0f)
        x = 1e-9f;

    /* M_PI stays a DOUBLE here, exactly as in the expression this replaced:
     * 2.0f * M_PI promotes to double, and only cosf() rounds back to float.
     * Doing the multiply in float instead changes the last bit, which the
     * near-unit-circle Doppler IIR turns into a different fade. */
    return sqrtf(-2.0f * logf(x)) * cosf(2.0f * M_PI * y);
}

unsigned int mercury_prng_seed_auto(mercury_prng_t *p, const char *env)
{
    const char  *e = env ? getenv(env) : NULL;
    unsigned int seed;

    if (e && *e)
        seed = (unsigned int)strtoul(e, NULL, 0);
    else
        /* Clock XOR pid, so that runs started in the same second -- which a
         * parallel sweep does constantly -- still get different channels. */
        seed = (unsigned int)time(NULL) ^ ((unsigned int)MERCURY_GETPID() << 16);

    mercury_prng_seed(p, seed);
    return seed;
}
