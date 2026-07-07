/* Standalone build of the arq_conn accessors for unit testing.
 * Copyright (C) 2025 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include "arq.h"
#include "arq_conn_accessors.h"

arq_info arq_conn;
static pthread_mutex_t g_conn_lock = PTHREAD_MUTEX_INITIALIZER;

void arq_conn_test_reset(void) { memset(&arq_conn, 0, sizeof(arq_conn)); }

void arq_set_trx(int trx)
{ pthread_mutex_lock(&g_conn_lock); arq_conn.TRX = trx; pthread_mutex_unlock(&g_conn_lock); }

int arq_get_trx(void)
{ pthread_mutex_lock(&g_conn_lock); int t = arq_conn.TRX; pthread_mutex_unlock(&g_conn_lock); return t; }

void arq_conn_get_calls(char *my_call, char *src_addr, char *dst_addr, size_t bufsz)
{
    if (bufsz == 0) return;
    pthread_mutex_lock(&g_conn_lock);
    if (my_call)  snprintf(my_call,  bufsz, "%s", arq_conn.my_call_sign);
    if (src_addr) snprintf(src_addr, bufsz, "%s", arq_conn.src_addr);
    if (dst_addr) snprintf(dst_addr, bufsz, "%s", arq_conn.dst_addr);
    pthread_mutex_unlock(&g_conn_lock);
}
