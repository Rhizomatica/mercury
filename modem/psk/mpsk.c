/* mpsk -- few-carrier differential QPSK rungs; see mpsk.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mpsk.h"

#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ldpc_codes.h"
#include "mpdecode_core.h"

/* Three layouts (carriers, symbols, markers, pulse) at 100 baud: 3 wide
 * carriers with the one-symbol pulse, 6 and 8 narrower ones with the RRC.  Codes: rate ~1/2, sized to the data
 * cells: N = 2 x nc x (nsym - 1 - nmark). */
const mpsk_level_t mpsk_levels[] = {
    { "psk3", 3, { 900, 1500, 2100 }, 375, 12, 0, "H_IRA_2172_1080" },
    { "psk6", 6, { 700, 1020, 1340, 1660, 1980, 2300 }, 354, 6, 2, "H_IRA_4164_2072" },
    { "psk8", 8, { 702, 930, 1158, 1386, 1614, 1842, 2070, 2298 }, 351, 5, 2, "H_IRA_5520_2752" },
};
const int mpsk_nlevels = (int)(sizeof(mpsk_levels) / sizeof(mpsk_levels[0]));

#define LPF_N   161                  /* 8 kHz decimating filter, delay 80 = 16 x DEC */
#define CHI_MIN 20.0

struct mpsk {
    int dbg_t; double dbg_chi;                /* last lock (debug)            */
    float *dbg_llr;                           /* last soft bits (debug)       */
    const mpsk_level_t *lv;
    int lvi;
    struct LDPC ldpc;
    int N, K, nbytes;
    double complex ph[MPSK_MAXC][400];        /* scrambling phasor per symbol */
    unsigned char pil[MPSK_MAXC][400];        /* marker ("no change") symbols */
    int cell[MPSK_MAXC][400];                 /* data cell index, or -1       */
    int ncell;
    int *perm;                                /* coded-bit interleaver        */
    double *txp; int ntxp;                    /* TX pulse at 8 kHz            */
    double mf[160]; int nmf;                  /* matched filter at 1600 Hz    */
    double lpf[LPF_N];
    int st[MPSK_MAXC];                        /* TX stagger, 8 kHz samples    */
    int stmax;                                /* the largest, receiver samples */
    int flush;
    /* streaming receiver (mpsk_rx) */
    double complex nco[MPSK_MAXC], nrot[MPSK_MAXC];
    double complex u[MPSK_MAXC][2 * LPF_N];   /* mixed input, doubled ring    */
    double complex y[MPSK_MAXC][2 * 160];     /* decimated, doubled ring      */
    double complex *zr[MPSK_MAXC], *dr[MPSK_MAXC], *d4[MPSK_MAXC];
    double complex qa[MPSK_MAXC][400], q4[MPSK_MAXC][400];
    int np, nd, upos, zmask;
    long nin, ny, zn, nextT, bT;
    double best;
    float *llr; uint8_t *cw;
    uint16_t seed; int accept_plain, seeded;
};

/* Gray QPSK: v = 2 b0 + b1. */
static const double complex CST[4] = { 1.0, I, -I, -1.0 };

static uint32_t xs32(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }

static uint16_t crc16(const uint8_t *d, int n)
{
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

/* The pulse at sps samples a symbol: a raised cosine one symbol long, or
 * the root raised cosine (roll-off 1) over +/- span symbols. */
static int pulse(int sps, int span, double *h)
{
    int n = 0;
    if (!span) {
        double t = sps / 2.0;
        for (int i = -sps / 2; i <= sps / 2; i++) {
            double x = i / t;
            h[n++] = i == 0 ? 1.0 : fabs(fabs((double)i) - t / 2.0) < 1e-9 ? 0.5
                   : sin(M_PI * x) * cos(M_PI * x) / (x * M_PI * (1.0 - 4.0 * x * x));
        }
    } else {
        double t = sps;
        int half = sps * span;
        for (int i = -half; i <= half; i++) {
            double x = i / t;
            h[n++] = i == 0 ? 4.0 / M_PI
                   : fabs(fabs((double)i) - t / 4.0) < 1e-9
                       ? (1.0 / sqrt(2.0)) * ((1.0 + 2.0 / M_PI) * sin(M_PI / 4.0) + (1.0 - 2.0 / M_PI) * cos(M_PI / 4.0))
                       : 4.0 * x * cos(2.0 * M_PI * x) / (M_PI * x * (1.0 - 16.0 * x * x));
        }
    }
    return n;
}


static void rx_free(mpsk_t *m);

mpsk_t *mpsk_open(int level)
{
    if (level < 0 || level >= mpsk_nlevels) return NULL;
    mpsk_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->lvi = level;
    m->lv = &mpsk_levels[level];
    const mpsk_level_t *l = m->lv;
    if (l->nsym > 400 || l->nc > MPSK_MAXC) { free(m); return NULL; }
    int ci = -1;
    for (int i = 0; i < ldpc_codes_num(); i++)
        if (!strcmp(ldpc_codes[i].name, l->code)) ci = i;
    if (ci < 0) { free(m); return NULL; }
    ldpc_codes_setup(&m->ldpc, (char *)l->code);
    if (getenv("MPSK_ITER")) m->ldpc.max_iter = atoi(getenv("MPSK_ITER"));
    m->N = m->ldpc.CodeLength;
    m->K = m->N - m->ldpc.NumberParityBits;
    m->nbytes = m->K / 8;

    /* Scrambling, markers and data cells.  Markers evenly spread over
     * symbols 2..nsym-1, each carrier's offset by a share of the spacing. */
    for (int c = 0; c < l->nc; c++) {
        uint32_t s = 0x9E3779B9u ^ (uint32_t)(level * 131 + c * 7919 + 1);
        for (int k = 0; k < l->nsym; k++) {
            /* Sixteen phases, not four: the search's data term correlates the
             * fourth power of each product with that of the scrambling, and a
             * QPSK scrambling phase to the fourth is 1 -- the data term then
             * ignored the scrambling, scored any window mostly over the frame
             * about as high as the right one, and under CCIR Moderate psk8
             * locked 20-29 symbols early, 3 frames in 30 at +14 dB. */
            m->ph[c][k] = cexp(I * 2.0 * M_PI * (double)(xs32(&s) & 15) / 16.0);
            m->pil[c][k] = 0;
            m->cell[c][k] = -1;
        }
        for (int j = 0; j < l->nmark; j++) {
            int k = 2 + (int)(((j + 0.5 + (double)c / l->nc) * (l->nsym - 2)) / l->nmark);
            if (k >= l->nsym) k -= l->nsym - 2;
            m->pil[c][k] = 1;
        }
    }
    /* Data cells frequency first, so a carrier's fade spreads over the code. */
    m->ncell = 0;
    for (int k = 1; k < l->nsym; k++)
        for (int c = 0; c < l->nc; c++)
            if (!m->pil[c][k]) m->cell[c][k] = m->ncell++;
    if (2 * m->ncell != m->N) { free(m); return NULL; }
    m->perm = malloc(sizeof(int) * (size_t)m->N);
    /* A fixed pseudo-random permutation.  An arithmetic one (i p mod N) put a
     * fade's run of symbols on a regular pattern of codeword positions, and
     * the repeat-accumulate code failed about one frame in twenty with 3 %
     * raw errors under CCIR Moderate, where frames with 7 % decoded. */
    for (int i = 0; i < m->N; i++) m->perm[i] = i;
    uint32_t ps = 0x2545F491u ^ (uint32_t)m->N;
    for (int i = m->N - 1; i > 0; i--) {
        int j = (int)(xs32(&ps) % (uint32_t)(i + 1));
        int t = m->perm[i]; m->perm[i] = m->perm[j]; m->perm[j] = t;
    }

    m->txp = malloc(sizeof(double) * (size_t)(2 * MPSK_SPS * (l->rrc_span ? l->rrc_span : 1) + 2));
    m->ntxp = pulse(MPSK_SPS, l->rrc_span, m->txp);
    m->nmf = pulse(MPSK_RSPS, l->rrc_span, m->mf);
    double sum = 0.0;
    for (int i = 0; i < m->nmf; i++) sum += m->mf[i];
    for (int i = 0; i < m->nmf; i++) m->mf[i] /= sum;
    m->flush = l->rrc_span ? 2 * l->rrc_span : 1;

    /* Decimating filter: half the carrier spacing (the closest pair). */
    double sp = 1e9;
    for (int c = 1; c < l->nc; c++) if (l->fc[c] - l->fc[c - 1] < sp) sp = l->fc[c] - l->fc[c - 1];
    double fcut = (l->nc > 1 ? sp / 2.0 : 300.0) / MPSK_FS;
    if (fcut * MPSK_FS > 350.0) fcut = 350.0 / MPSK_FS;
    double hs = 0.0;
    for (int i = 0; i < LPF_N; i++) {
        double n = i - (LPF_N - 1) / 2.0;
        double h = n == 0 ? 2.0 * fcut : sin(2.0 * M_PI * fcut * n) / (M_PI * n);
        h *= 0.54 - 0.46 * cos(2.0 * M_PI * i / (LPF_N - 1));
        m->lpf[i] = h;
        hs += h;
    }
    for (int i = 0; i < LPF_N; i++) m->lpf[i] /= hs;

    /* Staggered symbol clocks, which lower the peak of the sum: about a
     * third of a symbol apart for three carriers, else half a symbol on
     * every other one, in whole receiver samples. */
    for (int c = 0; c < l->nc; c++)
        m->st[c] = l->nc == 3 ? (int[]){ 0, 25, 55 }[c] : (c & 1) ? MPSK_SPS / 2 : 0;   /* multiples of DEC */
    m->stmax = 0;
    for (int c = 0; c < l->nc; c++) if (m->st[c] / MPSK_DEC > m->stmax) m->stmax = m->st[c] / MPSK_DEC;
    /* The search's references: marker products against the scrambling, the
     * data's fourth powers against its fourth power. */
    m->np = m->nd = 0;
    for (int c = 0; c < l->nc; c++)
        for (int k = 1; k < l->nsym; k++) {
            double complex q = m->ph[c][k] * conj(m->ph[c][k - 1]), q2 = q * q;
            m->qa[c][k] = q;
            m->q4[c][k] = q2 * q2;
            if (m->pil[c][k]) m->np++; else m->nd++;
        }
    m->llr = malloc(sizeof(float) * (size_t)m->N);
    m->cw = malloc((size_t)m->N);
    mpsk_rx_reset(m);
    return m;
}

void mpsk_close(mpsk_t *m)
{
    if (!m) return;
    free(m->perm);
    free(m->txp);
    free(m->dbg_llr);
    free(m->llr); free(m->cw);
    rx_free(m);
    free(m);
}

const mpsk_level_t *mpsk_level(const mpsk_t *m) { return m->lv; }
int mpsk_frame_bytes(const mpsk_t *m) { return m->nbytes; }

int mpsk_tx_samples(const mpsk_t *m)
{
    return (MPSK_LEAD + m->lv->nsym + m->flush) * MPSK_SPS + m->ntxp + MPSK_SPS;
}

int mpsk_modulate(mpsk_t *m, uint8_t *frame, int16_t *out, double peak)
{
    const mpsk_level_t *l = m->lv;
    uint16_t crc = crc16(frame, m->nbytes - 2);
    frame[m->nbytes - 2] = (uint8_t)(crc >> 8);
    frame[m->nbytes - 1] = (uint8_t)crc;
    unsigned char *ib = malloc((size_t)m->K), *pb = malloc((size_t)(m->N - m->K)), *cw = malloc((size_t)m->N);
    for (int i = 0; i < m->K; i++) ib[i] = (frame[i >> 3] >> (7 - (i & 7))) & 1;
    encode(&m->ldpc, ib, pb);
    memcpy(cw, ib, (size_t)m->K);
    memcpy(cw + m->K, pb, (size_t)(m->N - m->K));

    int n = mpsk_tx_samples(m);
    double *acc = calloc((size_t)n, sizeof(double));
    double complex *bb = malloc(sizeof(double complex) * (size_t)n);
    uint32_t s = 0xC0FFEEu ^ (uint32_t)m->lvi;
    for (int c = 0; c < l->nc; c++) {
        memset(bb, 0, sizeof(double complex) * (size_t)n);
        int nsy = MPSK_LEAD + l->nsym;
        double complex d = CST[xs32(&s) & 3];
        for (int i = 0; i < nsy; i++) {
            double complex x;
            if (i < MPSK_LEAD) {
                x = CST[xs32(&s) & 3];
            } else {
                int k = i - MPSK_LEAD;
                if (k == 0) d = CST[xs32(&s) & 3];
                else if (!m->pil[c][k]) {
                    int j = m->cell[c][k];
                    d *= CST[2 * cw[m->perm[2 * j]] + cw[m->perm[2 * j + 1]]];
                }
                x = d * conj(m->ph[c][k]);
            }
            int base = m->st[c] + i * MPSK_SPS;
            for (int t = 0; t < m->ntxp; t++) bb[base + t] += x * m->txp[t];
        }
        double w = 2.0 * M_PI * l->fc[c] / MPSK_FS;
        for (int i = 0; i < n; i++) acc[i] += creal(bb[i] * cexp(I * w * i));
    }
    double pk = 0.0;
    for (int i = 0; i < n; i++) if (fabs(acc[i]) > pk) pk = fabs(acc[i]);
    double g = pk > 0 ? peak / pk : 0.0;
    for (int i = 0; i < n; i++) out[i] = (int16_t)lrint(acc[i] * g);
    free(acc); free(bb); free(ib); free(pb); free(cw);
    return n;
}

/* The receiver streams.  Each carrier is mixed down once per input sample,
 * low-pass filtered and decimated to 1600 Hz (16 samples a symbol), matched
 * filtered, and kept in a ring (z) with its differential products (df) and
 * their fourth powers (d4).  The frame search scores one new start position
 * per receiver sample: chi(T) = |A|^2/np + |B|^2/nd, A the markers' products
 * against the scrambling, B the data's fourth powers against its fourth
 * power.  A start whose score passes CHI_MIN and is not beaten for two symbols
 * is decoded (and one sample either side, for timing). */
#define RX_LAG   (2 * MPSK_RSPS)

static double complex unitc(double complex v) { double a = cabs(v); return a > 1e-30 ? v / a : 0; }

static void rx_free(mpsk_t *m)
{
    for (int c = 0; c < MPSK_MAXC; c++) {
        free(m->zr[c]); free(m->dr[c]); free(m->d4[c]);
        m->zr[c] = m->dr[c] = m->d4[c] = NULL;
    }
}

void mpsk_rx_reset(mpsk_t *m)
{
    const mpsk_level_t *l = m->lv;
    if (!m->zr[0]) {
        int need = (l->nsym + 4) * MPSK_RSPS + m->stmax + 2 * RX_LAG + 8, sz = 1;
        while (sz < need) sz <<= 1;
        m->zmask = sz - 1;
        for (int c = 0; c < l->nc; c++) {
            m->zr[c] = calloc((size_t)sz, sizeof(double complex));
            m->dr[c] = calloc((size_t)sz, sizeof(double complex));
            m->d4[c] = calloc((size_t)sz, sizeof(double complex));
        }
    }
    for (int c = 0; c < l->nc; c++) {
        m->nco[c] = 1.0;
        m->nrot[c] = cexp(-I * 2.0 * M_PI * l->fc[c] / MPSK_FS);
        memset(m->u[c], 0, sizeof(m->u[c]));
        memset(m->y[c], 0, sizeof(m->y[c]));
    }
    m->nin = 0; m->ny = 0; m->zn = 0;
    m->upos = 0;
    m->nextT = MPSK_RSPS; m->best = -1.0; m->bT = -1;
}

#define ZI(a, c, i) (m->a[c][(i) & m->zmask])

/* Decode the frame starting at z index T.  1 with frame[] filled and its CRC
 * good (seeded or, if allowed, plain). */
static int decode_at(mpsk_t *m, long T, uint8_t *frame, float *snr_db)
{
    const mpsk_level_t *l = m->lv;
    const int R = MPSK_RSPS;
    double complex acc = 0;
    static __thread double complex ds[MPSK_MAXC][400];
    static __thread double pw[MPSK_MAXC][400];            /* |z|^2 of each symbol      */
    for (int c = 0; c < l->nc; c++) {
        long b0 = T + m->st[c] / MPSK_DEC;
        pw[c][0] = creal(ZI(zr, c, b0) * conj(ZI(zr, c, b0)));
        for (int k = 1; k < l->nsym; k++) {
            double complex a = ZI(zr, c, b0 + (long)k * R) * m->ph[c][k];
            double complex b = ZI(zr, c, b0 + (long)(k - 1) * R) * m->ph[c][k - 1];
            ds[c][k] = a * conj(b);
            pw[c][k] = creal(a * conj(a));
            if (m->pil[c][k]) acc += unitc(ds[c][k]);
        }
    }
    double complex r = unitc(conj(acc));
    double g = 0.0; int ng = 0;
    for (int c = 0; c < l->nc; c++) for (int k = 1; k < l->nsym; k++) { ds[c][k] *= r; g += cabs(ds[c][k]); ng++; }
    g = g / ng + 1e-30;
    /* Soft bits for differential detection through a fade.  A product
     * z_k z*_(k-1) carries the signal |h|^2 and noise whose variance is
     * s2 (|z_k|^2 + |z_(k-1)|^2): each symbol's noise times the other
     * symbol.  So each product gets its own noise, and is compared with
     * the constellation at its own amplitude sqrt(|z_k|^2 |z_(k-1)|^2).
     * s2 comes from the markers: the part of their products across the
     * expected phase is noise alone, half of its variance.  One noise for
     * the whole frame (with the constellation at unit size) left strong
     * symbols overconfident and failed frames with 3 % raw errors under
     * CCIR Moderate; the phase-only deviation from |d| gave half the
     * noise on white noise, and the soft bits were twice too confident. */
    for (int c = 0; c < l->nc; c++)
        for (int k = 0; k < l->nsym; k++) { if (k) ds[c][k] /= g; pw[c][k] /= g; }
    int nm = 0;
    double s2 = 0.0;
    for (int c = 0; c < l->nc; c++)
        for (int k = 1; k < l->nsym; k++)
            if (m->pil[c][k]) {
                double q = cimag(ds[c][k]), e = pw[c][k] + pw[c][k - 1];
                if (e > 1e-9) { s2 += 2.0 * q * q / e; nm++; }
            }
    s2 = nm ? s2 / nm : 1.0;
    if (s2 < 1e-4) s2 = 1e-4;
    if (snr_db) *snr_db = (float)(10.0 * log10(1.0 / s2));
    float *llr = m->llr;
    double lsc = getenv("MPSK_LSC") ? atof(getenv("MPSK_LSC")) : 1.0;
    /* Soft bits clipped at 5: a few confidently wrong bits (3-5 of 4164)
     * sank frames with 0.5 % raw errors that decode clipped (psk6, AWGN:
     * 35/40 at +11 dB unclipped, 40/40 from +9 clipped; 3-5 alike). */
    double lclip = getenv("MPSK_CLIP") ? atof(getenv("MPSK_CLIP")) : 5.0;
    for (int c = 0; c < l->nc; c++)
        for (int k = 1; k < l->nsym; k++) {
            int j = m->cell[c][k];
            if (j < 0) continue;
            double nvk = s2 * (pw[c][k] + pw[c][k - 1]) + 1e-9;
            double amp = sqrt(pw[c][k] * pw[c][k - 1]);
            double dv[4];
            for (int v = 0; v < 4; v++) { double complex e = ds[c][k] - amp * CST[v]; dv[v] = creal(e * conj(e)); }
            /* bit b0: v in {0,1} -> 0, {2,3} -> 1; b1: v even -> 0, odd -> 1 */
            double l0 = (fmin(dv[2], dv[3]) - fmin(dv[0], dv[1])) / nvk;
            double l1 = (fmin(dv[1], dv[3]) - fmin(dv[0], dv[2])) / nvk;
            l0 *= lsc; l1 *= lsc;
            if (l0 > lclip) l0 = lclip; else if (l0 < -lclip) l0 = -lclip;
            if (l1 > lclip) l1 = lclip; else if (l1 < -lclip) l1 = -lclip;
            llr[m->perm[2 * j]] = (float)l0;
            llr[m->perm[2 * j + 1]] = (float)l1;
        }
    uint8_t *cw = m->cw;
    int pcc = 0;
    run_ldpc_decoder(&m->ldpc, cw, llr, &pcc);
    memset(frame, 0, (size_t)m->nbytes);
    for (int i = 0; i < m->K; i++) if (cw[i]) frame[i >> 3] |= (uint8_t)(1 << (7 - (i & 7)));
    if (pcc != m->ldpc.NumberParityBits) return 0;
    uint16_t crc = crc16(frame, m->nbytes - 2);
    uint16_t got = (uint16_t)(frame[m->nbytes - 2] << 8 | frame[m->nbytes - 1]);
    if (m->seed && (uint16_t)(crc ^ m->seed) == got) { m->seeded = 1; return 1; }
    if ((!m->seed || m->accept_plain) && crc == got) { m->seeded = 0; return 1; }
    return 0;
}

/* One receiver sample (z index j) arrived: its products, and the search at
 * the newest start that fits. */
static int rx_step(mpsk_t *m, uint8_t *frame, int *found, float *snr_db)
{
    const mpsk_level_t *l = m->lv;
    const int R = MPSK_RSPS;
    long j = m->zn - 1;
    for (int c = 0; c < l->nc; c++) {
        double complex v = j >= R ? unitc(ZI(zr, c, j) * conj(ZI(zr, c, j - R))) : 0;
        double complex v2 = v * v;
        ZI(dr, c, j) = v;
        ZI(d4, c, j) = v2 * v2;
    }
    long T = j - m->stmax - (long)(l->nsym - 1) * R;
    if (T < m->nextT) return 0;
    double complex A = 0, B = 0;
    for (int c = 0; c < l->nc; c++) {
        long b0 = T + m->st[c] / MPSK_DEC;
        for (int k = 1; k < l->nsym; k++) {
            long i = b0 + (long)k * R;
            if (m->pil[c][k]) A += ZI(dr, c, i) * m->qa[c][k];
            else              B += ZI(d4, c, i) * m->q4[c][k];
        }
    }
    double chi = creal(A * conj(A)) / m->np + creal(B * conj(B)) / m->nd;
    if (chi > m->best) { m->best = chi; m->bT = T; }
    if (m->best < CHI_MIN || T < m->bT + RX_LAG) {
        if (m->best < CHI_MIN && T > m->bT + RX_LAG) { m->best = -1.0; m->bT = -1; }
        return 0;
    }
    /* a detection, not beaten for two symbols: decode it */
    if (found) *found = 1;
    m->dbg_t = (int)m->bT; m->dbg_chi = m->best;
    int ok = 0;
    for (int tryi = 0; !ok && tryi < (getenv("MPSK_NOTREF") ? 1 : 3); tryi++)
        ok = decode_at(m, m->bT + (tryi == 1 ? -1 : tryi == 2 ? 1 : 0), frame, snr_db);
    m->nextT = ok ? m->bT + (long)l->nsym * R : m->bT + RX_LAG;
    m->best = -1.0; m->bT = -1;
    return ok;
}

int mpsk_rx(mpsk_t *m, const int16_t *in, int n, uint8_t *frame, int *found, float *snr_db)
{
    const mpsk_level_t *l = m->lv;
    int got = 0;
    for (int i = 0; i < n; i++) {
        double x = (double)in[i];
        int p = m->upos;
        for (int c = 0; c < l->nc; c++) {
            double complex u = x * m->nco[c];
            m->u[c][p] = m->u[c][p + LPF_N] = u;       /* doubled ring */
            m->nco[c] *= m->nrot[c];
        }
        m->upos = (p + 1) % LPF_N;
        m->nin++;
        if ((m->nin & 1023) == 0) for (int c = 0; c < l->nc; c++) m->nco[c] /= cabs(m->nco[c]);
        /* an output every DEC samples, once the filter is full */
        if (m->nin < LPF_N || (m->nin - LPF_N) % MPSK_DEC) continue;
        int yp = m->ny % m->nmf;
        for (int c = 0; c < l->nc; c++) {
            const double complex *w = m->u[c] + m->upos;   /* oldest first */
            double complex a = 0;
            for (int t = 0; t < LPF_N; t++) a += w[t] * m->lpf[t];
            m->y[c][yp] = m->y[c][yp + m->nmf] = a;
        }
        m->ny++;
        if (m->ny < m->nmf) continue;
        int y0 = m->ny % m->nmf;                          /* oldest of the last nmf */
        for (int c = 0; c < l->nc; c++) {
            const double complex *w = m->y[c] + y0;
            double complex a = 0;
            for (int t = 0; t < m->nmf; t++) a += w[t] * m->mf[t];
            ZI(zr, c, m->zn) = a;
        }
        m->zn++;
        if (!got && rx_step(m, frame, found, snr_db)) got = 1;
    }
    return got;
}

int mpsk_demod(mpsk_t *m, const int16_t *in, int n, uint8_t *frame, int *found, float *snr_db)
{
    if (found) *found = 0;
    mpsk_rx_reset(m);
    return mpsk_rx(m, in, n, frame, found, snr_db);
}

void mpsk_set_crc_seed(mpsk_t *m, uint16_t seed, int accept_plain) { m->seed = seed; m->accept_plain = accept_plain; }
int  mpsk_rx_seeded(const mpsk_t *m) { return m->seeded; }

/* Debug: the codeword of frame[] (its CRC as mpsk_modulate writes it) and the
 * receiver's last soft bits. */
int mpsk_debug_codeword(mpsk_t *m, uint8_t *frame, uint8_t *cwout)
{
    uint16_t crc = crc16(frame, m->nbytes - 2);
    frame[m->nbytes - 2] = (uint8_t)(crc >> 8);
    frame[m->nbytes - 1] = (uint8_t)crc;
    unsigned char *ib = malloc((size_t)m->K), *pb = malloc((size_t)(m->N - m->K));
    for (int i = 0; i < m->K; i++) ib[i] = (frame[i >> 3] >> (7 - (i & 7))) & 1;
    encode(&m->ldpc, ib, pb);
    memcpy(cwout, ib, (size_t)m->K);
    memcpy(cwout + m->K, pb, (size_t)(m->N - m->K));
    free(ib); free(pb);
    return m->N;
}
const float *mpsk_debug_llr(const mpsk_t *m) { return m->llr; }

/* Debug: raw errors of the last frame by carrier and by eighth of the frame. */
void mpsk_debug_errmap(const mpsk_t *m, const uint8_t *cw, char *out, int cap)
{
    const mpsk_level_t *l = m->lv;
    int ec[MPSK_MAXC] = {0}, et[8] = {0}, big = 0;
    for (int c = 0; c < l->nc; c++)
        for (int k = 1; k < l->nsym; k++) {
            int j = m->cell[c][k];
            if (j < 0) continue;
            for (int b = 0; b < 2; b++) {
                int i = m->perm[2 * j + b];
                float L = m->llr[i];
                if ((L < 0) != (cw[i] != 0)) { ec[c]++; et[k * 8 / l->nsym]++; if (fabsf(L) > 5) big++; }
            }
        }
    int o = snprintf(out, (size_t)cap, "by carrier:");
    for (int c = 0; c < l->nc; c++) o += snprintf(out + o, (size_t)(cap - o), " %d", ec[c]);
    o += snprintf(out + o, (size_t)(cap - o), " | by eighth:");
    for (int t = 0; t < 8; t++) o += snprintf(out + o, (size_t)(cap - o), " %d", et[t]);
    snprintf(out + o, (size_t)(cap - o), " | confident wrong (|llr|>5): %d", big);
}

/* Debug: where the last search locked, and the z index frame symbol 0 of
 * carrier 0 has when the frame starts at input sample off. */
int mpsk_debug_lock(const mpsk_t *m, int off, double *chi)
{
    if (chi) *chi = m->dbg_chi;
    int s0 = off + MPSK_LEAD * MPSK_SPS + (m->ntxp - 1) / 2;
    int expect = (s0 - (LPF_N - 1) / 2) / MPSK_DEC - (m->nmf - 1) / 2;
    return m->dbg_t - expect;
}
