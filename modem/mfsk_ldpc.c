/* Mercury MFSK LDPC — rate-1/16 encoder + min-sum decoder.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original matrix/algorithm)
 * Copyright (C) 2026 Rhizomatica (C port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mfsk_ldpc.h"

#include <math.h>
#include <string.h>

/* --- encoder: systematic IRA (v1 cl_ldpc::encode) ----------------------- */
void mfsk_ldpc_encode(const int info[MFSK_LDPC_K], int coded[MFSK_LDPC_N])
{
    for (int i = 0; i < MFSK_LDPC_K; i++)
        coded[i] = info[i] & 1;

    for (int i = 0; i < MFSK_LDPC_P; i++)
    {
        int p = 0;
        for (int j = 0; j < MFSK_LDPC_CWIDTH - 1; j++)
        {
            int idx = mfsk_ldpc_QCmatrixEnc[i][j];
            if (idx != -1) p ^= coded[idx];   /* refs earlier bits -> accumulator */
        }
        coded[MFSK_LDPC_K + i] = p & 1;
    }
}

/* --- min-sum belief-propagation decoder --------------------------------- */
/* Edge structure built once from QCmatrixC (check -> variable adjacency). */
#define MFSK_LDPC_MAXEDGE (MFSK_LDPC_P * MFSK_LDPC_CWIDTH)

static int   g_ready = 0;
static int   g_ne;                              /* number of edges          */
static int   g_ec[MFSK_LDPC_MAXEDGE];           /* edge -> check            */
static int   g_ev[MFSK_LDPC_MAXEDGE];           /* edge -> variable         */
static int   g_vdeg[MFSK_LDPC_N];               /* variable degree          */
static int   g_vedge[MFSK_LDPC_N][16];          /* variable -> edge indices */

static void build_graph(void)
{
    g_ne = 0;
    memset(g_vdeg, 0, sizeof(g_vdeg));
    for (int c = 0; c < MFSK_LDPC_P; c++)
        for (int j = 0; j < MFSK_LDPC_CWIDTH; j++)
        {
            int v = mfsk_ldpc_QCmatrixC[c][j];
            if (v < 0) continue;
            int e = g_ne++;
            g_ec[e] = c; g_ev[e] = v;
            if (g_vdeg[v] < 16) g_vedge[v][g_vdeg[v]++] = e;
        }
    g_ready = 1;
}

int mfsk_ldpc_decode(const float llr[MFSK_LDPC_N], int info_out[MFSK_LDPC_K],
                     int max_iter)
{
    if (!g_ready) build_graph();
    const double alpha = 0.75;
    static double m_vc[MFSK_LDPC_MAXEDGE];   /* variable->check messages   */
    static double m_cv[MFSK_LDPC_MAXEDGE];   /* check->variable messages   */
    double total[MFSK_LDPC_N];

    for (int e = 0; e < g_ne; e++) m_vc[e] = llr[g_ev[e]];
    for (int e = 0; e < g_ne; e++) m_cv[e] = 0.0;

    int converged = 0;
    for (int it = 0; it < max_iter && !converged; it++)
    {
        /* check-node update (normalized min-sum, exclude self) */
        for (int c = 0; c < MFSK_LDPC_P; c++) { (void)c; }
        /* per-check pass over its edges */
        int e = 0;
        while (e < g_ne)
        {
            int c = g_ec[e], e0 = e;
            double sign = 1.0, min1 = 1e300, min2 = 1e300;
            while (e < g_ne && g_ec[e] == c)
            {
                double a = fabs(m_vc[e]);
                if (m_vc[e] < 0) sign = -sign;
                if (a < min1) { min2 = min1; min1 = a; }
                else if (a < min2) min2 = a;
                e++;
            }
            for (int k = e0; k < e; k++)
            {
                double a = fabs(m_vc[k]);
                double s = (m_vc[k] < 0) ? -sign : sign;   /* product of others */
                double mag = (a <= min1) ? min2 : min1;
                double v = alpha * s * mag;
                if (v > 1e3) v = 1e3; else if (v < -1e3) v = -1e3;
                m_cv[k] = v;
            }
        }
        /* variable-node update + hard decision */
        for (int v = 0; v < MFSK_LDPC_N; v++)
        {
            double sum = llr[v];
            for (int i = 0; i < g_vdeg[v]; i++) sum += m_cv[g_vedge[v][i]];
            total[v] = sum;
            for (int i = 0; i < g_vdeg[v]; i++)
            {
                double t = sum - m_cv[g_vedge[v][i]];
                if (t > 1e3) t = 1e3; else if (t < -1e3) t = -1e3;
                m_vc[g_vedge[v][i]] = t;
            }
        }
        /* syndrome */
        int bad = 0;
        int ee = 0;
        while (ee < g_ne)
        {
            int c = g_ec[ee], par = 0;
            while (ee < g_ne && g_ec[ee] == c)
            {
                if (total[g_ev[ee]] < 0) par ^= 1;
                ee++;
            }
            if (par) { bad = 1; break; }
        }
        if (!bad) converged = 1;
    }

    for (int i = 0; i < MFSK_LDPC_K; i++)
        info_out[i] = (total[i] < 0) ? 1 : 0;
    return converged;
}
