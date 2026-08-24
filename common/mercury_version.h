/* Mercury version — single source of truth
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_VERSION_H
#define MERCURY_VERSION_H

#include <stdio.h>

/* The release version string.  The Makefile also reads this line
 * (grep 'define MERCURY_VERSION') to name release artifacts, so keep the
 * "..." form on one line. */
#define MERCURY_VERSION "1.9.13"

/* GIT_HASH is injected by the build via -DGIT_HASH (see config.mk).  It is
 * resolved in the including translation unit, so any unit that is rebuilt when
 * the hash changes — main.o (via .git_hash_stamp) for the daemon, and
 * mercury_bridge.o (recompiled on every libmercury_core.a build) for the UI —
 * shows the current hash. */
#ifndef GIT_HASH
#define GIT_HASH "unknown000"
#endif

/* Print the startup banner exactly as the daemon does: ANSI red on Linux,
 * plain text elsewhere. */
static inline void mercury_print_version_banner(void)
{
#if defined(__linux__)
    printf("\e[0;31mRhizomatica Mercury Version %s (git %.8s)\e[0m\n",
           MERCURY_VERSION, GIT_HASH); /* we go red */
#else
    printf("Rhizomatica Mercury Version %s (git %.8s)\n",
           MERCURY_VERSION, GIT_HASH);
#endif
}

#endif /* MERCURY_VERSION_H */
