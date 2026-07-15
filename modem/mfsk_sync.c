/* MFSK preamble acquisition — pure-C port of v1 cl_ofdm::time_sync_mfsk_corr.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original C++ implementation)
 * Copyright (C) 2026 Rhizomatica (C port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mfsk_sync.h"

#include <math.h>

static int build_tmpl(const mfsk_t *m, const ofdm_frame_t *o, int postamble,
                      double complex *tmpl_out, double *sym_energy_out)
{
    int Nofdm = ofdm_frame_nofdm(o);
    int P = postamble ? m->postamble_nSymb : m->preamble_nSymb;

    /* known tone bins (frequency domain), P symbols x Nc */
    mfsk_cplx pbins[8 * 512];         /* P<=8, Nc<=512 */
    if (postamble) mfsk_generate_postamble(m, pbins, P);
    else           mfsk_generate_preamble(m, pbins, P);

    for (int s = 0; s < P; s++)
    {
        double complex bins[512], pad[1024], t[1024], cp[1088];
        for (int k = 0; k < o->Nc; k++)
            bins[k] = pbins[s * m->Nc + k].re + pbins[s * m->Nc + k].im * I;
        ofdm_zero_padder(o, bins, pad);
        ofdm_ifft(o, pad, t);
        ofdm_gi_adder(o, t, cp);       /* Nofdm time samples */
        double e = 0.0;
        for (int n = 0; n < Nofdm; n++)
        {
            tmpl_out[s * Nofdm + n] = cp[n];
            e += creal(cp[n] * conj(cp[n]));
        }
        sym_energy_out[s] = e;
    }
    return P;
}

int mfsk_sync_build_template(const mfsk_t *m, const ofdm_frame_t *o,
                             double complex *tmpl_out, double *sym_energy_out)
{
    return build_tmpl(m, o, 0, tmpl_out, sym_energy_out);
}

int mfsk_sync_build_postamble_template(const mfsk_t *m, const ofdm_frame_t *o,
                                       double complex *tmpl_out,
                                       double *sym_energy_out)
{
    return build_tmpl(m, o, 1, tmpl_out, sym_energy_out);
}

int mfsk_sync_search(const double complex *rx, int rx_len, int interp,
                     const double complex *tmpl, const double *sym_energy,
                     int template_nsymb, int Nofdm, int search_start_symb,
                     double *out_metric)
{
    int sym_period = Nofdm * interp;
    int buffer_nsymb = rx_len / sym_period;

    int p1_start = (search_start_symb > 0) ? search_start_symb * sym_period : 0;
    int p1_end = (buffer_nsymb - template_nsymb) * sym_period;

    enum { P1_OVERSAMPLE = 4, P1_TOP_K = 8 };
    int p1_step = sym_period / P1_OVERSAMPLE;
    if (p1_step < 1) p1_step = 1;

    struct { int pos; double metric; } cand[P1_TOP_K];
    int n_cand = 0;
    double best_p1 = -1.0; int best_p1_pos = -1;
    const double per_sym_floor = 0.05;

    for (int base = p1_start; base <= p1_end; base += p1_step)
    {
        if (base + (template_nsymb * Nofdm - 1) * interp >= rx_len) break;

        double total = 0.0; int valid = 0; int rejected = 0;
        for (int k = 0; k < template_nsymb && !rejected; k++)
        {
            int toff = k * Nofdm;
            int roff = base + k * sym_period;
            double cr = 0.0, cim = 0.0, erx = 0.0;
            for (int n = 0; n < Nofdm; n++)
            {
                double complex r = rx[roff + n * interp];
                double t_re = creal(tmpl[toff + n]), t_im = cimag(tmpl[toff + n]);
                double r_re = creal(r), r_im = cimag(r);
                cr  += t_re * r_re + t_im * r_im;
                cim += t_im * r_re - t_re * r_im;
                erx += r_re * r_re + r_im * r_im;
            }
            double denom = sym_energy[k] * erx;
            if (denom > 1e-30)
            {
                double sm = (cr * cr + cim * cim) / denom;
                if (sm < per_sym_floor) { rejected = 1; break; }
                total += sm; valid++;
            }
        }
        if (rejected) continue;
        double metric = (valid > 0) ? total / valid : 0.0;

        /* insert into descending top-K */
        if (n_cand < P1_TOP_K || metric > cand[n_cand - 1].metric)
        {
            int at = (n_cand < P1_TOP_K) ? n_cand : P1_TOP_K - 1;
            for (int t = 0; t < n_cand && t < P1_TOP_K; t++)
                if (metric > cand[t].metric) { at = t; break; }
            int end = (n_cand < P1_TOP_K) ? n_cand : P1_TOP_K - 1;
            for (int u = end; u > at; u--) cand[u] = cand[u - 1];
            cand[at].pos = base; cand[at].metric = metric;
            if (n_cand < P1_TOP_K) n_cand++;
        }
        if (metric > best_p1) { best_p1 = metric; best_p1_pos = base; }
        if (metric > 0.5) break;      /* earliest strong preamble */
    }

    if (best_p1_pos < 0) { if (out_metric) *out_metric = best_p1; return -1; }

    /* Phase 2: fine refinement on all top-K candidates */
    int search_half = sym_period;
    int best_fine = best_p1_pos; double best_fine_metric = -1.0;

    for (int c = 0; c < n_cand; c++)
    {
        int coarse = cand[c].pos;
        for (int d = coarse - search_half; d <= coarse + search_half; d += interp)
        {
            if (d < 0) continue;
            double total = 0.0; int valid = 0, oob = 0;
            for (int k = 0; k < template_nsymb && !oob; k++)
            {
                int toff = k * Nofdm;
                int rbase = d + k * sym_period;
                if (rbase + (Nofdm - 1) * interp >= rx_len) { oob = 1; break; }
                double cr = 0.0, cim = 0.0, erx = 0.0;
                for (int n = 0; n < Nofdm; n++)
                {
                    double complex r = rx[rbase + n * interp];
                    double t_re = creal(tmpl[toff + n]), t_im = cimag(tmpl[toff + n]);
                    cr  += t_re * creal(r) + t_im * cimag(r);
                    cim += t_im * creal(r) - t_re * cimag(r);
                    erx += creal(r) * creal(r) + cimag(r) * cimag(r);
                }
                double denom = sym_energy[k] * erx;
                if (denom > 1e-30) { total += (cr*cr + cim*cim) / denom; valid++; }
            }
            if (oob) continue;
            double metric = (valid > 0) ? total / valid : 0.0;
            if (metric > best_fine_metric) { best_fine_metric = metric; best_fine = d; }
        }
        if (best_fine_metric > 0.5) break;
    }

    if (out_metric) *out_metric = best_fine_metric;
    return (best_fine_metric < 0.5) ? -1 : best_fine;
}
