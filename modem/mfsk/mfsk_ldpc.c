/* Mercury MFSK LDPC — generic rate-ladder encoder + min-sum decoder.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original matrices/algorithm)
 * Copyright (C) 2026 Rhizomatica (C port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mfsk_ldpc.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

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

/* Everything a decode writes: the code's Tanner graph (built on first use of
 * that code) and the edge messages.  Each decoder owns one -- they used to be
 * file statics shared by every caller, and two MFSK decoders running at once
 * (the call listener beside the payload decoder, or a monitor beside either)
 * overwrote each other's messages: 792 of 800 concurrent decodes of valid
 * codewords failed, against 0 of 400 one at a time. */
struct mfsk_ldpc_ws
{
    const mfsk_ldpc_code_t *code;      /* the code the graph below is for */
    int ne;
    int ec[MAXEDGE];                   /* edge -> check                   */
    int ev[MAXEDGE];                   /* edge -> variable                */
    int vdeg[MFSK_LDPC_MAXN];
    int vedge[MFSK_LDPC_MAXN][MAXVDEG];
    double m_vc[MAXEDGE], m_cv[MAXEDGE];
};

mfsk_ldpc_ws_t *mfsk_ldpc_ws_new(void) { return (mfsk_ldpc_ws_t *)calloc(1, sizeof(mfsk_ldpc_ws_t)); }
void mfsk_ldpc_ws_free(mfsk_ldpc_ws_t *ws) { free(ws); }

static void build_graph(mfsk_ldpc_ws_t *ws, const mfsk_ldpc_code_t *c)
{
    ws->ne = 0;
    memset(ws->vdeg, 0, sizeof(ws->vdeg));
    for (int ch = 0; ch < c->P; ch++)
        for (int j = 0; j < c->cwidth; j++)
        {
            int v = c->C[ch * c->cwidth + j];
            if (v < 0) continue;
            int e = ws->ne++;
            ws->ec[e] = ch; ws->ev[e] = v;
            if (ws->vdeg[v] < MAXVDEG) ws->vedge[v][ws->vdeg[v]++] = e;
        }
    ws->code = c;
}

int mfsk_ldpc_decode_ws(mfsk_ldpc_ws_t *ws, const mfsk_ldpc_code_t *c, const float *llr,
                        int *info_out, int max_iter)
{
    if (ws->code != c) build_graph(ws, c);
    const double alpha = 0.75;
    const int ne = ws->ne;
    const int *ec = ws->ec, *ev = ws->ev;
    double *m_vc = ws->m_vc, *m_cv = ws->m_cv;
    double total[MFSK_LDPC_MAXN];

    for (int e = 0; e < ne; e++) { m_vc[e] = llr[ev[e]]; m_cv[e] = 0.0; }

    int converged = 0;
    for (int it = 0; it < max_iter && !converged; it++)
    {
        /* check-node update (normalized min-sum, exclude self) */
        int e = 0;
        while (e < ne)
        {
            int ch = ec[e], e0 = e;
            double sign = 1.0, min1 = 1e300, min2 = 1e300;
            while (e < ne && ec[e] == ch)
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
            for (int i = 0; i < ws->vdeg[v]; i++) sum += m_cv[ws->vedge[v][i]];
            total[v] = sum;
            for (int i = 0; i < ws->vdeg[v]; i++)
            {
                double t = sum - m_cv[ws->vedge[v][i]];
                if (t > 1e3) t = 1e3; else if (t < -1e3) t = -1e3;
                m_vc[ws->vedge[v][i]] = t;
            }
        }
        /* syndrome */
        int bad = 0, ee = 0;
        while (ee < ne)
        {
            int ch = ec[ee], par = 0;
            while (ee < ne && ec[ee] == ch)
            {
                if (total[ev[ee]] < 0) par ^= 1;
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

/* For tools and tests: a workspace per calling thread, made on first use.
 * A long-lived decoder (the modem's) owns its own instead. */
int mfsk_ldpc_decode(const mfsk_ldpc_code_t *c, const float *llr,
                     int *info_out, int max_iter)
{
    static _Thread_local mfsk_ldpc_ws_t *ws;
    if (!ws && !(ws = mfsk_ldpc_ws_new())) return 0;
    return mfsk_ldpc_decode_ws(ws, c, llr, info_out, max_iter);
}
