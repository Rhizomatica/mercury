/* Test seam: exposes the arq_conn accessors and a reset for unit tests.
 * Copyright (C) 2025 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ARQ_CONN_ACCESSORS_H
#define ARQ_CONN_ACCESSORS_H
#include <stddef.h>
void arq_set_trx(int trx);
int  arq_get_trx(void);
void arq_conn_get_calls(char *my_call, char *src_addr, char *dst_addr, size_t bufsz);
void arq_conn_test_reset(void);   /* test-only: zero arq_conn */
#endif
