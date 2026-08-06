/* MFSK preamble acquisition — pure-C port of v1 cl_ofdm::time_sync_mfsk_corr.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original C++ implementation)
 * Copyright (C) 2026 Rhizomatica (C port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mfsk_sync.h"

#include <math.h>

/* Acceptance threshold for the normalised preamble correlation.
 *
 * The per-symbol statistic is |corr|^2/(E_tmpl * E_rx) = SNR_sym/(1+SNR_sym),
 * so a threshold T only admits SNR_sym >= T/(1-T).  The original 0.5 therefore
 * demanded SNR_sym >= 0 dB, and THAT was the fringe floor of the entire MFSK
 * mode -- not the code.  Sweeping all five LDPC rates (1/2 down to 1/16) gave
 * byte-identical FER curves, because below 0 dB nothing ever reached the
 * decoder.
 *
 * Measured on AWGN over the real two-burst (~104k sample) window with the
 * 4-symbol preamble:
 *
 *     noise-only max metric    0.037 - 0.049   (flat vs SNR -- it is normalised)
 *     signal @ SNR3k  -5.1 dB  0.219
 *     signal @ SNR3k  -9.1 dB  0.091
 *
 * 0.08 sits ~1.6x above the measured noise maximum (0 false syncs in 600
 * noise-only searches) and moves the 50% FER point from -0.6 dB to about
 * -11.4 dB SNR3k.  Below roughly -12 dB the signal metric reaches the noise
 * floor itself, which no threshold can recover: that needs a detector which
 * COMBINES the preamble symbols rather than averaging per-symbol normalised
 * ratios, since averaging gains nothing from preamble length.
 *
 * Overridable at build time (-DMFSK_SYNC_ACCEPT=...) for sweeps. */
#ifndef MFSK_SYNC_ACCEPT
#define MFSK_SYNC_ACCEPT 0.08
#endif

/* Per-symbol floor, applied as an AND across the preamble: one symbol below it
 * discards the whole candidate.  It must stay well under the accept threshold,
 * because at the fringe individual symbols scatter around the mean and a floor
 * close to the threshold throws away genuine preambles for a single weak
 * symbol.  (The old 0.05 was 62% of the old 0.5 gate.) */
#ifndef MFSK_SYNC_SYM_FLOOR
#define MFSK_SYNC_SYM_FLOOR (MFSK_SYNC_ACCEPT / 3.0)
#endif

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

    const double accept_thresh = MFSK_SYNC_ACCEPT;
    const double floor_thresh  = MFSK_SYNC_SYM_FLOOR;

    enum { P1_OVERSAMPLE = 4, P1_TOP_K = 8 };
    int p1_step = sym_period / P1_OVERSAMPLE;
    if (p1_step < 1) p1_step = 1;

    struct { int pos; double metric; } cand[P1_TOP_K];
    int n_cand = 0;
    double best_p1 = -1.0; int best_p1_pos = -1;
    const double per_sym_floor = floor_thresh;

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
        if (metric > accept_thresh) break;  /* earliest strong preamble */
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
        if (best_fine_metric > accept_thresh) break;
    }

    if (out_metric) *out_metric = best_fine_metric;
    return (best_fine_metric < accept_thresh) ? -1 : best_fine;
}

/* Pattern detection — port of v1 cl_ofdm::detect_ack_pattern (Phase 1).
 *
 * v1 slides a window and, per pattern symbol, counts it a match only when the
 * expected (hopped) tone is the PEAK bin for every stream — order-aware, so the
 * false-alarm rate is (1/M)^nStreams per symbol, not 1-(1-1/M)^nS. The decision
 * statistic is that matched-symbol count (caller compares to m->*_match_threshold);
 * E_target/E_total is a tie-break metric. Unlike v1 we depad first (same path as
 * mfsk_demod) instead of hand-mapping raw FFT bins + carrier-image mirrors, since
 * this pipeline's LPF+depad already rejects the image. */
int mfsk_detect_pattern(const mfsk_t *m, const ofdm_frame_t *o,
                        const double complex *rx, int rx_len,
                        const int *tones, int pattern_len, int nsymb,
                        int *out_pos)
{
    int Nofdm = ofdm_frame_nofdm(o);
    if (rx_len < nsymb * Nofdm) { if (out_pos) *out_pos = -1; return 0; }

    int step = Nofdm / 8;
    if (step < 1) step = 1;
    int last_base = rx_len - nsymb * Nofdm;

    int best_matched = -1, best_pos = -1;
    double best_metric = -1.0;

    double complex blk[2048], rmv[2048], fftd[2048], bins[1024];

    for (int base = 0; base <= last_base; base += step)
    {
        int matched = 0;
        double metric = 0.0;

        for (int p = 0; p < nsymb; p++)
        {
            int rbase = base + p * Nofdm;
            for (int n = 0; n < Nofdm; n++) blk[n] = rx[rbase + n];
            ofdm_gi_remover(o, blk, rmv);
            ofdm_fft(o, rmv, fftd);
            ofdm_zero_depadder(o, fftd, bins);

            int actual_tone = (tones[p % pattern_len] + p * m->tone_hop_step) % m->M;

            int streams_matched = 0;
            double e_target = 0.0;
            for (int st = 0; st < m->nStreams; st++)
            {
                int base_bin = m->stream_offsets[st];
                double peak_e = -1.0; int peak_t = -1;
                for (int t = 0; t < m->M; t++)
                {
                    double complex v = bins[base_bin + t];
                    double e = creal(v) * creal(v) + cimag(v) * cimag(v);
                    if (e > peak_e) { peak_e = e; peak_t = t; }
                    if (t == actual_tone) e_target += e;
                }
                if (peak_e > 0.0 && peak_t == actual_tone) streams_matched++;
            }
            if (streams_matched == m->nStreams) matched++;

            double e_total = 0.0;
            for (int k = 0; k < o->Nc; k++)
            {
                double complex v = bins[k];
                e_total += creal(v) * creal(v) + cimag(v) * cimag(v);
            }
            if (e_total > 0.0) metric += e_target / e_total;
        }

        if (matched > best_matched ||
            (matched == best_matched && metric > best_metric))
        {
            best_matched = matched; best_metric = metric; best_pos = base;
        }
    }

    if (out_pos) *out_pos = best_pos;
    return (best_matched < 0) ? 0 : best_matched;
}
