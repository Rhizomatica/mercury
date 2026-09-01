/* HERMES Modem — reproducible pseudo-random source
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A private, reentrant copy of glibc's TYPE_3 additive-feedback generator --
 * the one random(3) uses by default -- carried here so that channel models and
 * test harnesses do NOT depend on libc's rand().
 *
 * Three reasons this exists rather than a call to rand():
 *
 *   1. REPRODUCIBILITY ACROSS PLATFORMS.  rand() is a different generator on
 *      glibc, musl, macOS and MinGW, so "the same seed" reproduces a different
 *      channel on each.  A fading measurement that cannot be replayed on
 *      another machine cannot be checked by anyone else.
 *
 *   2. NO SHARED GLOBAL STATE.  rand() has one hidden global sequence, so any
 *      other caller -- a test fixture, a library, a second channel instance --
 *      silently reorders the draws a channel sees.  Each user of this module
 *      owns its state, so two channels in one process cannot perturb one
 *      another and neither perturbs libc.
 *
 *   3. IT IS EASY TO FORGET TO SEED.  rand() without srand() behaves exactly
 *      as if seeded with 1: it returns a valid-looking stream, so nothing
 *      fails, and every "independent" run replays ONE realisation.  That is
 *      precisely the defect this module was written to retire -- see
 *      watterson_seed_auto().
 *
 * Bit-identical to glibc random() for a given seed (verified in
 * tests/common/test_mercury_prng.c against a table of known outputs), so
 * existing pinned-seed results remain reproducible on glibc hosts.
 *
 * Ported from mercuryv1's source/common/os_interop.cc, reduced to the single
 * TYPE_3 degree Mercury actually uses and made reentrant: state lives in the
 * caller's struct, and the front/rear cursors are indices rather than pointers
 * so the struct is trivially copyable.
 *
 * NOT cryptographically secure, and not meant to be.
 */
#ifndef MERCURY_PRNG_H
#define MERCURY_PRNG_H

#include <stdint.h>

/* Degree of the x**31 + x**3 + 1 polynomial (glibc TYPE_3). */
#define MERCURY_PRNG_DEG 31

typedef struct
{
    int32_t state[MERCURY_PRNG_DEG];
    int     f;   /* front cursor, index into state[] */
    int     r;   /* rear cursor                      */
} mercury_prng_t;

/** Seed the generator.  A seed of 0 is mapped to 1, as glibc does. */
void mercury_prng_seed(mercury_prng_t *p, unsigned int seed);

/** Next value in [0, 2^31-1], matching random(). */
uint32_t mercury_prng_u31(mercury_prng_t *p);

/** Uniform in [0, 1], both ends inclusive (see the note in the .c). */
float mercury_prng_uniform(mercury_prng_t *p);

/** One sample from N(0, 1), via the Box-Muller transform. */
float mercury_prng_gaussian(mercury_prng_t *p);

/**
 * Seed from the environment variable @p env if it is set and non-empty,
 * otherwise from the wall clock XOR the pid so that concurrent runs differ.
 *
 * @return the seed actually used, so a caller can PRINT it -- an unreproducible
 *         measurement is only worth having if the run can be replayed later.
 */
unsigned int mercury_prng_seed_auto(mercury_prng_t *p, const char *env);

#endif /* MERCURY_PRNG_H */
