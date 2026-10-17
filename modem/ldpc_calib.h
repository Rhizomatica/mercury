/* ldpc_calib.h — time the LDPC decoder on this host (mercury -B).
 *
 * Copyright (C) 2026 Joseph Freivald
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MERCURY_LDPC_CALIB_H
#define MERCURY_LDPC_CALIB_H
#include <stdio.h>
/* Prints ns per iteration per code and algorithm, plus the iterations each
 * code can run inside a 100 ms budget.  Returns 0. */
int ldpc_calib_run(FILE *out);
#endif
