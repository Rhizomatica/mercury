/* Mercury MFSK LDPC — generic rate-ladder encoder + min-sum decoder.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original matrices/algorithm)
 * Copyright (C) 2026 Rhizomatica (C port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mfsk_ldpc.h"

#include <math.h>
#include <string.h>

/* --- encoder: systematic IRA (v1 cl_ldpc::encode) ----------------------- */
void mfsk_ldpc_encode(const mfsk_ldpc_code_t *c, const int *info, int *coded)
{
    int ew = c->cwidth - 1;
    for (int i = 0; i < c->K; i++)
        coded[i] = info[i] & 1;

    for (int i = 0; i < c->P; i++)
    {
        int p = 0;
        for (int j = 0; j < ew; j++)
        {
            int idx = c->Enc[i * ew + j];
            if (idx != -1) p ^= coded[idx];   /* refs earlier bits -> accumulator */
        }
        coded[c->K + i] = p & 1;
    }
}

/* --- min-sum belief-propagation decoder --------------------------------- */
#define MAXEDGE 17000                 /* max P*cwidth over the ladder (5/16) */
#define MAXVDEG 24

static const mfsk_ldpc_code_t *g_code = NULL;   /* cached graph's code       */
static int g_ne;
static int g_ec[MAXEDGE];             /* edge -> check                       */
static int g_ev[MAXEDGE];             /* edge -> variable                    */
static int g_vdeg[MFSK_LDPC_MAXN];
static int g_vedge[MFSK_LDPC_MAXN][MAXVDEG];

static void build_graph(const mfsk_ldpc_code_t *c)
{
    g_ne = 0;
    memset(g_vdeg, 0, sizeof(g_vdeg));
    for (int ch = 0; ch < c->P; ch++)
        for (int j = 0; j < c->cwidth; j++)
        {
            int v = c->C[ch * c->cwidth + j];
            if (v < 0) continue;
            int e = g_ne++;
            g_ec[e] = ch; g_ev[e] = v;
            if (g_vdeg[v] < MAXVDEG) g_vedge[v][g_vdeg[v]++] = e;
        }
    g_code = c;
}

int mfsk_ldpc_decode(const mfsk_ldpc_code_t *c, const float *llr,
                     int *info_out, int max_iter)
{
    if (g_code != c) build_graph(c);
    const double alpha = 0.75;
    static double m_vc[MAXEDGE], m_cv[MAXEDGE];
    double total[MFSK_LDPC_MAXN];

    for (int e = 0; e < g_ne; e++) { m_vc[e] = llr[g_ev[e]]; m_cv[e] = 0.0; }

    int converged = 0;
    for (int it = 0; it < max_iter && !converged; it++)
    {
        /* check-node update (normalized min-sum, exclude self) */
        int e = 0;
        while (e < g_ne)
        {
            int ch = g_ec[e], e0 = e;
            double sign = 1.0, min1 = 1e300, min2 = 1e300;
            while (e < g_ne && g_ec[e] == ch)
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
                double s = (m_vc[k] < 0) ? -sign : sign;
                double mag = (a <= min1) ? min2 : min1;
                double v = alpha * s * mag;
                if (v > 1e3) v = 1e3; else if (v < -1e3) v = -1e3;
                m_cv[k] = v;
            }
        }
        /* variable-node update */
        for (int v = 0; v < c->N; v++)
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
        int bad = 0, ee = 0;
        while (ee < g_ne)
        {
            int ch = g_ec[ee], par = 0;
            while (ee < g_ne && g_ec[ee] == ch)
            {
                if (total[g_ev[ee]] < 0) par ^= 1;
                ee++;
            }
            if (par) { bad = 1; break; }
        }
        if (!bad) converged = 1;
    }

    for (int i = 0; i < c->K; i++)
        info_out[i] = (total[i] < 0) ? 1 : 0;
    return converged;
}
