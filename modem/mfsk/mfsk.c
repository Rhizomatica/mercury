/* Mercury MFSK — non-coherent M-FSK over OFDM subcarriers (pure-C port)
 *
 * Pure-C port of Mercury v1's cl_mfsk (C++), originally by Fadi Jerji.
 *
 * Copyright (C) 2022-2024 Fadi Jerji (original C++ implementation)
 * Copyright (C) 2026 Rhizomatica (C port)
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mfsk.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

void mfsk_init(mfsk_t *m, int _M, int _Nc, int _nStreams)
{
    memset(m, 0, sizeof(*m));

    m->M = _M;
    m->Nc = _Nc;
    m->nStreams = _nStreams;
    if (m->nStreams < 1) m->nStreams = 1;
    if (m->nStreams > MFSK_MAX_STREAMS) m->nStreams = MFSK_MAX_STREAMS;

    /* nBits = log2(M) */
    m->nBits = 0;
    for (int temp = m->M; temp > 1; temp >>= 1)
        m->nBits++;

    /* Tone hop step: coprime with M for full-period cycling */
    if      (m->M == 32) m->tone_hop_step = 13;
    else if (m->M == 16) m->tone_hop_step = 7;
    else if (m->M == 8)  m->tone_hop_step = 3;
    else if (m->M == 4)  m->tone_hop_step = 1;
    else                 m->tone_hop_step = 1;

    /* Stream frequency allocation: center nStreams*M contiguous bins in Nc */
    int total_bins = m->nStreams * m->M;
    int global_offset = (m->Nc - total_bins) / 2;
    if (global_offset < 0) global_offset = 0;
    for (int k = 0; k < m->nStreams; k++)
        m->stream_offsets[k] = global_offset + k * m->M;

    /* Preamble tones (per M) */
    if (m->M == 32)
    {
        m->preamble_nSymb = 4;
        m->preamble_tones[0] = 4;  m->preamble_tones[1] = 20;
        m->preamble_tones[2] = 12; m->preamble_tones[3] = 28;
    }
    else if (m->M == 16)
    {
        m->preamble_nSymb = 4;
        m->preamble_tones[0] = 2;  m->preamble_tones[1] = 10;
        m->preamble_tones[2] = 6;  m->preamble_tones[3] = 14;
    }
    else if (m->M == 8)
    {
        m->preamble_nSymb = 8;
        int t[8] = {1, 5, 3, 7, 0, 6, 2, 4};
        for (int i = 0; i < 8; i++) m->preamble_tones[i] = t[i];
    }
    else if (m->M == 4)
    {
        m->preamble_nSymb = 8;
        int t[8] = {0, 2, 1, 3, 3, 1, 2, 0};
        for (int i = 0; i < 8; i++) m->preamble_tones[i] = t[i];
    }
    else
    {
        m->preamble_nSymb = 4;
        for (int i = 0; i < m->preamble_nSymb && i < MFSK_MAX_PREAMBLE_SYMB; i++)
            m->preamble_tones[i] =
                (i * m->M / m->preamble_nSymb + m->M / (2 * m->preamble_nSymb)) % m->M;
    }

    /* ACK pattern tones (Welch-Costas for WB, Sidelnikov for NB) */
    if (m->M == 32)
    {
        m->ack_pattern_len = 8; m->ack_pattern_nsymb = 16; m->ack_match_threshold = 8;
        int t[8] = {8, 14, 10, 24, 26, 2, 18, 30};
        for (int i = 0; i < 8; i++) m->ack_tones[i] = t[i];
    }
    else if (m->M == 16)
    {
        m->ack_pattern_len = 8; m->ack_pattern_nsymb = 16; m->ack_match_threshold = 8;
        int t[8] = {4, 7, 5, 12, 13, 1, 9, 15};
        for (int i = 0; i < 8; i++) m->ack_tones[i] = t[i];
    }
    else if (m->M == 8)
    {
        m->ack_pattern_len = 32; m->ack_pattern_nsymb = 32; m->ack_match_threshold = 24;
        int t[32] = {
            1, 3, 6, 5, 3, 7, 6, 5, 2, 5, 3, 6, 4, 1, 3, 7,
            7, 7, 6, 4, 1, 2, 4, 0, 1, 2, 5, 2, 4, 1, 3, 6};
        for (int i = 0; i < 32; i++) m->ack_tones[i] = t[i];
    }
    else if (m->M == 4)
    {
        m->ack_pattern_len = 48; m->ack_pattern_nsymb = 48; m->ack_match_threshold = 40;
        int t[48] = {
            0, 1, 2, 0, 1, 3, 2, 1, 2, 1, 2, 0, 1, 2, 0, 0,
            0, 1, 3, 3, 2, 0, 1, 3, 3, 3, 3, 2, 1, 3, 2, 0,
            1, 2, 1, 2, 1, 3, 2, 1, 3, 3, 3, 2, 0, 0, 1, 3};
        for (int i = 0; i < 48; i++) m->ack_tones[i] = t[i];
    }
    else
    {
        m->ack_pattern_len = 8; m->ack_pattern_nsymb = 16; m->ack_match_threshold = 8;
        for (int i = 0; i < m->ack_pattern_len; i++)
            m->ack_tones[i] = (i * m->M / m->ack_pattern_len + 1) % m->M;
    }

    /* BREAK pattern tones */
    if (m->M == 32)
    {
        int t[8] = {12, 28, 4, 6, 20, 16, 22, 30};
        for (int i = 0; i < 8; i++) m->break_tones[i] = t[i];
        m->break_match_threshold = 8;
    }
    else if (m->M == 16)
    {
        int t[8] = {6, 14, 2, 3, 10, 8, 11, 15};
        for (int i = 0; i < 8; i++) m->break_tones[i] = t[i];
        m->break_match_threshold = 12;
    }
    else if (m->M == 8)
    {
        int t[32] = {
            3, 7, 3, 2, 3, 3, 1, 6, 0, 2, 2, 6, 6, 7, 4, 7,
            6, 2, 4, 0, 4, 5, 4, 4, 6, 1, 7, 5, 5, 1, 1, 0};
        for (int i = 0; i < 32; i++) m->break_tones[i] = t[i];
        m->break_match_threshold = 24;
    }
    else if (m->M == 4)
    {
        int t[48] = {
            1, 3, 3, 3, 0, 1, 1, 0, 1, 3, 1, 0, 3, 0, 0, 0,
            2, 1, 2, 2, 2, 2, 1, 3, 3, 2, 2, 0, 0, 0, 3, 2,
            2, 3, 2, 0, 2, 3, 0, 3, 3, 3, 1, 2, 1, 1, 1, 1};
        for (int i = 0; i < 48; i++) m->break_tones[i] = t[i];
        m->break_match_threshold = 40;
    }
    else
    {
        m->break_match_threshold = 8;
        for (int i = 0; i < m->ack_pattern_len; i++)
            m->break_tones[i] = (m->ack_tones[i] + m->M / 2) % m->M;
    }

    /* HAIL pattern tones ("I am Mercury" beacon) */
    if (m->M == 32)
    {
        int t[8] = {0, 10, 2, 22, 6, 12, 14, 26};
        for (int i = 0; i < 8; i++) m->hail_tones[i] = t[i];
        m->hail_match_threshold = 8;
    }
    else if (m->M == 16)
    {
        int t[8] = {0, 5, 1, 11, 3, 6, 7, 13};
        for (int i = 0; i < 8; i++) m->hail_tones[i] = t[i];
        m->hail_match_threshold = 8;
    }
    else if (m->M == 8)
    {
        int t[32] = {
            4, 3, 0, 2, 5, 5, 6, 0, 4, 2, 7, 1, 5, 5, 4, 3,
            2, 7, 7, 0, 3, 1, 6, 6, 5, 3, 7, 1, 4, 2, 6, 6};
        for (int i = 0; i < 32; i++) m->hail_tones[i] = t[i];
        m->hail_match_threshold = 24;
    }
    else if (m->M == 4)
    {
        int t[48] = {
            0, 0, 1, 3, 2, 3, 1, 2, 3, 3, 1, 3, 0, 0, 0, 1,
            3, 2, 3, 1, 2, 3, 3, 1, 3, 0, 0, 0, 1, 3, 2, 3,
            1, 2, 3, 3, 1, 3, 0, 0, 0, 1, 3, 2, 3, 1, 2, 3};
        for (int i = 0; i < 48; i++) m->hail_tones[i] = t[i];
        m->hail_match_threshold = 40;
    }
    else
    {
        m->hail_match_threshold = 8;
        for (int i = 0; i < m->ack_pattern_len; i++)
            m->hail_tones[i] = (m->ack_tones[i] + m->M / 4) % m->M;
    }

    /* Postamble tones: the preamble shifted a quarter of the band (mod M), so
     * the two known sequences stay distinguishable for dual-ended acquisition.
     * The shift must clear the receiver's dial-offset search, +/-3 tones: at
     * the old +2 a burst's postamble WAS its preamble to that search (all 4
     * symbols at M=32), and a decoder that joined a burst midway anchored on
     * the postamble, rejected it, and lost the burst behind it (5 of 9 join
     * points; 5c41f6e guards the symptom).  M/4 = 8 at M=32, 4 at M=16: no
     * offset in range aligns a symbol.  The narrow sets (M=8, 4) span too few
     * tones for any shift to clear it, and keep +2. */
    m->postamble_nSymb = m->preamble_nSymb;
    int post_shift = m->M >= 16 ? m->M / 4 : 2;
    for (int i = 0; i < m->preamble_nSymb && i < MFSK_MAX_PREAMBLE_SYMB; i++)
        m->postamble_tones[i] = (m->preamble_tones[i] + post_shift) % m->M;

    mfsk_clear_hail_target(m);
}

void mfsk_set_hail_target(mfsk_t *m, const char *callsign, int len)
{
    if (!callsign || len <= 0 || m->M == 0)
    {
        mfsk_clear_hail_target(m);
        return;
    }

    /* FNV-1a 32-bit hash of uppercased callsign */
    uint32_t hash = 2166136261u;
    for (int i = 0; i < len; i++)
    {
        char c = callsign[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        hash ^= (uint8_t)c;
        hash *= 16777619u;
    }

    for (int i = 0; i < MFSK_HAIL_SUFFIX_LEN; i++)
        m->hail_suffix[i] = (int)((hash >> (i * 5)) & 0x1F) % m->M;

    m->hail_directed = true;

    for (int s = 0; s < m->ack_pattern_nsymb; s++)
        m->hail_detect_tones[s] = m->hail_tones[s % m->ack_pattern_len];
    for (int s = 0; s < MFSK_HAIL_SUFFIX_LEN; s++)
        m->hail_detect_tones[m->ack_pattern_nsymb + s] = m->hail_suffix[s];

    m->hail_detect_nsymb = m->ack_pattern_nsymb + MFSK_HAIL_SUFFIX_LEN;
    m->hail_detect_threshold = m->hail_match_threshold + MFSK_HAIL_SUFFIX_LEN;
}

void mfsk_clear_hail_target(mfsk_t *m)
{
    m->hail_directed = false;
    for (int i = 0; i < MFSK_HAIL_SUFFIX_LEN; i++)
        m->hail_suffix[i] = 0;

    for (int s = 0; s < m->ack_pattern_nsymb; s++)
        m->hail_detect_tones[s] = m->hail_tones[s % m->ack_pattern_len];

    m->hail_detect_nsymb = m->ack_pattern_nsymb;
    m->hail_detect_threshold = m->hail_match_threshold;
}

void mfsk_deinit(mfsk_t *m)
{
    m->M = 0;
    m->nBits = 0;
    m->Nc = 0;
    m->nStreams = 0;
    m->preamble_nSymb = 0;
}

/* --- known-tone sequence generators ------------------------------------- */

static void gen_zero_symbol(const mfsk_t *m, mfsk_cplx *out, int s)
{
    for (int k = 0; k < m->Nc; k++)
    {
        out[s * m->Nc + k].re = 0.0;
        out[s * m->Nc + k].im = 0.0;
    }
}

void mfsk_generate_preamble(const mfsk_t *m, mfsk_cplx *out, int nSymb)
{
    if (m->M == 0 || m->Nc == 0 || m->nStreams == 0) return;

    double amp = sqrt((double)m->Nc / m->nStreams);

    for (int s = 0; s < nSymb; s++)
    {
        gen_zero_symbol(m, out, s);
        int tone = m->preamble_tones[s % m->preamble_nSymb];
        for (int st = 0; st < m->nStreams; st++)
            out[s * m->Nc + m->stream_offsets[st] + tone].re = amp;
    }
}

void mfsk_generate_postamble(const mfsk_t *m, mfsk_cplx *out, int nSymb)
{
    if (m->M == 0 || m->Nc == 0 || m->nStreams == 0) return;

    double amp = sqrt((double)m->Nc / m->nStreams);

    for (int s = 0; s < nSymb; s++)
    {
        gen_zero_symbol(m, out, s);
        int tone = m->postamble_tones[s % m->postamble_nSymb];
        for (int st = 0; st < m->nStreams; st++)
            out[s * m->Nc + m->stream_offsets[st] + tone].re = amp;
    }
}

/* Shared body for ACK/BREAK/HAIL: place hopped tones from a tone table. */
static void gen_pattern(const mfsk_t *m, mfsk_cplx *out, const int *tones,
                        int table_len, int nsymb)
{
    if (m->M == 0 || m->Nc == 0 || m->nStreams == 0) return;

    double amp = sqrt((double)m->Nc / m->nStreams);

    for (int s = 0; s < nsymb; s++)
    {
        gen_zero_symbol(m, out, s);
        int tone_base = tones[s % table_len];
        int actual_tone = (tone_base + s * m->tone_hop_step) % m->M;
        for (int st = 0; st < m->nStreams; st++)
            out[s * m->Nc + m->stream_offsets[st] + actual_tone].re = amp;
    }
}

void mfsk_generate_ack_pattern(const mfsk_t *m, mfsk_cplx *out)
{
    gen_pattern(m, out, m->ack_tones, m->ack_pattern_len, m->ack_pattern_nsymb);
}

void mfsk_generate_break_pattern(const mfsk_t *m, mfsk_cplx *out)
{
    gen_pattern(m, out, m->break_tones, m->ack_pattern_len, m->ack_pattern_nsymb);
}

void mfsk_generate_hail_pattern(const mfsk_t *m, mfsk_cplx *out)
{
    /* detect array is already flat (prefix + optional suffix), hop by index */
    gen_pattern(m, out, m->hail_detect_tones, m->hail_detect_nsymb,
                m->hail_detect_nsymb);
}

/* --- modulator ---------------------------------------------------------- */

void mfsk_mod(const mfsk_t *m, const int *bits_in, int total_bits,
              mfsk_cplx *symbols_out)
{
    if (m->M == 0 || m->nBits == 0 || m->Nc == 0 || m->nStreams == 0) return;

    int bps = m->nBits * m->nStreams;
    int nSymbols = total_bits / bps;
    double amp = sqrt((double)m->Nc / m->nStreams);

    for (int s = 0; s < nSymbols; s++)
    {
        gen_zero_symbol(m, symbols_out, s);

        for (int st = 0; st < m->nStreams; st++)
        {
            int bit_offset = s * bps + st * m->nBits;

            int tone_index = 0;
            for (int b = 0; b < m->nBits; b++)
                if (bits_in[bit_offset + b])
                    tone_index |= (1 << (m->nBits - 1 - b));

            /* Gray -> binary */
            int binary_index = tone_index;
            for (int shift = 1; shift < m->nBits; shift++)
                binary_index ^= (tone_index >> shift);
            tone_index = binary_index;

            if (tone_index >= m->M) tone_index = m->M - 1;

            int actual_tone = (tone_index + s * m->tone_hop_step) % m->M;
            symbols_out[s * m->Nc + m->stream_offsets[st] + actual_tone].re = amp;
        }
    }
}

/* --- demodulator (non-coherent energy detection -> soft LLRs) ----------- */

void mfsk_interleave_init(int *perm, int n)
{
    if (n <= 0) return;
    for (int i = 0; i < n; i++) perm[i] = i;

    /* xorshift64*, seeded by a constant: the table must come out bit-identical
     * on every build and platform, so no rand(), no floating point. */
    unsigned long long st = 0x9E3779B97F4A7C15ULL;
    for (int i = n - 1; i > 0; i--)
    {
        st ^= st << 13; st ^= st >> 7; st ^= st << 17;
        int j = (int)(st % (unsigned long long)(i + 1));
        int t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
}

void mfsk_demod(const mfsk_t *m, const mfsk_cplx *fft_in, int total_bits,
                float *llr_out)
{
    if (m->M == 0 || m->nBits == 0 || m->Nc == 0 || m->nStreams == 0) return;

    int bps = m->nBits * m->nStreams;
    int nSymbols = total_bits / bps;

    for (int s = 0; s < nSymbols; s++)
    {
        /* Estimate noise variance from bins outside all stream bands */
        int band_start = m->stream_offsets[0];
        int band_end = m->stream_offsets[m->nStreams - 1] + m->M;
        double noise_sum = 0.0;
        int noise_bins = 0;
        for (int k = 0; k < m->Nc; k++)
        {
            if (k < band_start || k >= band_end)
            {
                mfsk_cplx val = fft_in[s * m->Nc + k];
                double e = val.re * val.re + val.im * val.im;
                if (isfinite(e))
                {
                    noise_sum += e;
                    noise_bins++;
                }
            }
        }
        double noise_var = (noise_bins > 0) ? noise_sum / noise_bins : 1e-30;
        if (noise_var < 1e-30) noise_var = 1e-30;

        double llr_scale = 1.0 / (2.0 * noise_var);

        for (int st = 0; st < m->nStreams; st++)
        {
            double E_raw[64]; /* M <= 64 */
            for (int mm = 0; mm < m->M; mm++)
            {
                mfsk_cplx val = fft_in[s * m->Nc + m->stream_offsets[st] + mm];
                E_raw[mm] = val.re * val.re + val.im * val.im;
                if (!isfinite(E_raw[mm])) E_raw[mm] = 0.0;
            }

            /* Reverse tone hopping */
            double E[64];
            int hop = (s * m->tone_hop_step) % m->M;
            for (int mm = 0; mm < m->M; mm++)
            {
                int actual = (mm + hop) % m->M;
                E[mm] = E_raw[actual];
            }

            int llr_offset = s * bps + st * m->nBits;
            for (int k = 0; k < m->nBits; k++)
            {
                int mask = 1 << (m->nBits - 1 - k);
                double max_E1 = -1e30;
                double max_E0 = -1e30;

                for (int mm = 0; mm < m->M; mm++)
                {
                    int gray_m = mm ^ (mm >> 1);
                    if (gray_m & mask)
                    {
                        if (E[mm] > max_E1) max_E1 = E[mm];
                    }
                    else
                    {
                        if (E[mm] > max_E0) max_E0 = E[mm];
                    }
                }

                double llr = (max_E0 - max_E1) * llr_scale;
                if (!isfinite(llr)) llr = 0.0;
                else if (llr > 5.0) llr = 5.0;
                else if (llr < -5.0) llr = -5.0;
                llr_out[llr_offset + k] = (float)llr;
            }
        }
    }
}

/* --- NAV header ----------------------------------------------------------- */

/* Found by search (scratch nav.py, seed 20261005): 8 distinct tones each, self
 * agreement <= 3 off zero shift, <= 3 against ACK/BREAK/HAIL/preamble/
 * postamble, <= 4 between classes (12 classes do not fit at 3). */
const int mfsk_nav_tones[MFSK_NAV_CLASSES][8] = {
    {  1,  0, 24,  8, 17, 11, 30, 28 }, { 12,  3, 18, 15, 31, 29,  0, 20 },
    { 30, 31, 28, 27, 29, 26, 18, 12 }, { 10, 19, 14, 18,  0, 21,  5, 12 },
    { 16,  5, 18, 22, 21,  7, 20, 14 }, { 13, 28, 18, 16, 15, 22,  9, 29 },
    {  5,  8, 10, 16, 31,  3, 21,  6 }, {  8, 27, 22, 13,  6, 23, 24, 28 },
    {  1, 19,  9,  0, 24, 17, 12,  4 }, { 21, 20, 30,  6, 18, 23, 16, 19 },
    { 31, 28, 22, 29, 30, 18,  5,  4 }, {  4,  1, 23, 19, 24, 21,  0, 28 },
};

/* 2 s to 32 s, a factor of 1.287 a class: a hold overshoots by under 29 %. */
static const uint32_t nav_class_ms[MFSK_NAV_CLASSES] = {
    2000, 2574, 3313, 4264, 5487, 7062, 9089, 11698, 15055, 19376, 24937, 32094,
};

uint32_t mfsk_nav_class_ms(int k)
{
    if (k < 0) k = 0;
    if (k >= MFSK_NAV_CLASSES) k = MFSK_NAV_CLASSES - 1;
    return nav_class_ms[k];
}

int mfsk_nav_class_for_ms(uint32_t ms)
{
    for (int k = 0; k < MFSK_NAV_CLASSES; k++)
        if (ms <= nav_class_ms[k]) return k;
    return MFSK_NAV_CLASSES - 1;
}

/* --- session-bound patterns --------------------------------------------- */

void mfsk_pattern_expand(const mfsk_t *m, const int *tones, int *out)
{
    for (int p = 0; p < m->ack_pattern_nsymb; p++)
        out[p] = (tones[p % m->ack_pattern_len] + p * m->tone_hop_step) % m->M;
}

int mfsk_seq_xmatch(int M, const int *a, int na, const int *b, int nb, int dmax, bool skip0)
{
    int best = 0;
    for (int tau = -(nb - 1); tau < na; tau++)
        for (int d = -dmax; d <= dmax; d++) {
            if (skip0 && tau == 0 && d == 0) continue;
            int c = 0;
            for (int i = 0; i < na; i++) {
                int j = i - tau;
                if (j < 0 || j >= nb) continue;
                int t = b[j] + d;          /* a shifted tone leaves the band */
                if (t >= 0 && t < M && a[i] == t) c++;
            }
            if (c > best) best = c;
        }
    return best;
}

static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Agreement allowed between a session list and anything else, in symbols of
 * ack_pattern_nsymb; frequency shifts up to SESS_DMAX bins: the detector's
 * +/-2 search plus up to 2 bins of offset between the radios. */
#define SESS_XMATCH 3
#define SESS_DMAX   4
#define SESS_TRIES  20000

bool mfsk_session_patterns(const mfsk_t *m, uint32_t key,
                           const int *const *extra, int nextra,
                           int *ack_out, int *brk_out)
{
    int L = m->ack_pattern_len, NS = m->ack_pattern_nsymb;
    for (int i = 0; i < L; i++) { ack_out[i] = m->ack_tones[i]; brk_out[i] = m->break_tones[i]; }
    if (!key || m->M != 32 || L != 8 || NS != 16) return false;

    /* everything a session list must stay clear of, hop-expanded */
    enum { MAXREF = 8 + 24 };   /* the fixed five, then extras */
    int ref[MAXREF][MFSK_MAX_ACK_TONES], nref = 0, rlen[MAXREF];
    mfsk_pattern_expand(m, m->ack_tones, ref[nref]);   rlen[nref++] = NS;
    mfsk_pattern_expand(m, m->break_tones, ref[nref]); rlen[nref++] = NS;
    mfsk_pattern_expand(m, m->hail_tones, ref[nref]);  rlen[nref++] = NS;
    for (int i = 0; i < NS; i++) {
        ref[nref][i]     = m->preamble_tones[i % m->preamble_nSymb];
        ref[nref + 1][i] = m->postamble_tones[i % m->postamble_nSymb];
    }
    rlen[nref++] = NS; rlen[nref++] = NS;
    for (int e = 0; e < nextra && nref < MAXREF; e++) {
        mfsk_pattern_expand(m, extra[e], ref[nref]); rlen[nref++] = NS;
    }
    /* the NAV classes, at the limit they keep among themselves */
    int nav[MFSK_NAV_CLASSES][MFSK_MAX_ACK_TONES];
    for (int k = 0; k < MFSK_NAV_CLASSES; k++) mfsk_pattern_expand(m, mfsk_nav_tones[k], nav[k]);

    uint64_t st = 0x6D6572637572793AULL ^ (uint64_t)key;   /* "mercury:" */
    int found = 0, cand[2][8], exp_c[2][MFSK_MAX_ACK_TONES];
    for (int t = 0; t < SESS_TRIES && found < 2; t++) {
        /* 8 distinct tones: a partial Fisher-Yates over 0..M-1 */
        int pool[32];
        for (int i = 0; i < 32; i++) pool[i] = i;
        for (int i = 0; i < L; i++) {
            int j = i + (int)(splitmix64(&st) % (uint64_t)(32 - i));
            int x = pool[i]; pool[i] = pool[j]; pool[j] = x;
            cand[found][i] = pool[i];
        }
        int *e = exp_c[found];
        mfsk_pattern_expand(m, cand[found], e);
        if (mfsk_seq_xmatch(m->M, e, NS, e, NS, SESS_DMAX, true) > SESS_XMATCH) continue;
        bool ok = true;
        for (int r = 0; r < nref && ok; r++)
            ok = mfsk_seq_xmatch(m->M, e, NS, ref[r], rlen[r], SESS_DMAX, false) <= SESS_XMATCH;
        for (int k = 0; k < MFSK_NAV_CLASSES && ok; k++)
            ok = mfsk_seq_xmatch(m->M, e, NS, nav[k], NS, SESS_DMAX, false) <= SESS_XMATCH + 1;
        if (ok && found == 1)
            ok = mfsk_seq_xmatch(m->M, e, NS, exp_c[0], NS, SESS_DMAX, false) <= SESS_XMATCH;
        if (ok) found++;
    }
    if (found < 2) return false;
    for (int i = 0; i < L; i++) { ack_out[i] = cand[0][i]; brk_out[i] = cand[1][i]; }
    return true;
}
