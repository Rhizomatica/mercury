/* MFSK modem backend — see modem_mfsk.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "modem_mfsk.h"
#include "mfsk.h"
#include "mfsk_ofdm.h"
#include "mfsk_sync.h"
#include "mfsk_ldpc.h"
#include "freedv_api.h"   /* freedv_gen_crc16 — codec-independent CRC16 */

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Passband geometry — matches the validated offline harness (docs/MFSK-PORT.md). */
#define MFSK_FS       8000.0
#define MFSK_FC       2000.0
#define MFSK_NFFT     256
#define MFSK_NCAR     50
#define MFSK_GI       0.25
#define MFSK_M        32
#define MFSK_TXAMP    6000.0     /* int16 passband peak (normalised later by tx_gain) */
#define MFSK_LPF_TAPS 63
#define MFSK_LPF_FC   1000.0

typedef struct {
    int mode;
    mfsk_t m;
    ofdm_frame_t o;
    const mfsk_ldpc_code_t *code;   /* rate 8/16 */
    int Nofdm, P, bps, NPAY;
    size_t frame_bytes;             /* K/8 (payload + CRC16) */

    double lpf[MFSK_LPF_TAPS];
    double w;                       /* carrier radians/sample */
    long tx_n;                      /* TX carrier phase counter (per burst) */

    /* RX sliding window (int16 passband) */
    int16_t *rxbuf;
    int      rxlen, rxcap;
    long     since_search;          /* samples appended since last detect attempt */

    /* RX scratch (preallocated) */
    double complex *bf;             /* downmixed+filtered baseband, rxcap long */
    mfsk_cplx      *rb;             /* NPAY*NCAR depadded bins */
    float          *llr;            /* N LLRs */
    int            *info;           /* K info bits */
    double complex *preT, *pstT;
    double          preE[MFSK_MAX_PREAMBLE_SYMB], pstE[MFSK_MAX_PREAMBLE_SYMB];
    int             preN, pstN;

    int   last_sync;
    float last_snr;
} mfsk_modem_t;

/* Windowed-sinc low-pass (matches the harness). */
static void mklpf(double *lpf, double fc)
{
    double s = 0.0;
    for (int i = 0; i < MFSK_LPF_TAPS; i++)
    {
        int k = i - MFSK_LPF_TAPS / 2;
        double h = (k == 0) ? 2.0 * M_PI * fc / MFSK_FS
                            : sin(2.0 * M_PI * fc / MFSK_FS * k) / (M_PI * k);
        h *= 0.54 - 0.46 * cos(2.0 * M_PI * i / (MFSK_LPF_TAPS - 1));
        lpf[i] = h; s += h;
    }
    for (int i = 0; i < MFSK_LPF_TAPS; i++) lpf[i] /= s;
}

/* ---- lifecycle -------------------------------------------------------- */

static void *mfsk_be_open(int mode)
{
    mfsk_modem_t *h = (mfsk_modem_t *)calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->mode = mode;
    mfsk_init(&h->m, MFSK_M, MFSK_NCAR, 1);
    ofdm_frame_init(&h->o, MFSK_NFFT, MFSK_NCAR, MFSK_GI, 0);
    h->code  = &mfsk_ldpc_8_16;          /* rate 1/2: 100-byte frame */
    h->Nofdm = ofdm_frame_nofdm(&h->o);
    h->P     = h->m.preamble_nSymb;
    h->bps   = mfsk_bits_per_symbol(&h->m);
    h->NPAY  = (h->code->N + h->bps - 1) / h->bps;
    h->frame_bytes = (size_t)h->code->K / 8;
    h->w     = 2.0 * M_PI * MFSK_FC / MFSK_FS;
    mklpf(h->lpf, MFSK_LPF_FC);

    /* RX window: one full burst (pre + data + post) plus a few symbols slack. */
    h->rxcap = (h->P + h->NPAY + h->P + 8) * h->Nofdm;
    h->rxbuf = (int16_t *)malloc((size_t)h->rxcap * sizeof(int16_t));
    h->bf    = (double complex *)malloc((size_t)h->rxcap * sizeof(double complex));
    h->rb    = (mfsk_cplx *)calloc((size_t)h->NPAY * MFSK_NCAR, sizeof(mfsk_cplx));
    h->llr   = (float *)malloc((size_t)h->code->N * sizeof(float));
    h->info  = (int *)malloc((size_t)h->code->K * sizeof(int));
    h->preT  = (double complex *)malloc((size_t)h->P * h->Nofdm * sizeof(double complex));
    h->pstT  = (double complex *)malloc((size_t)h->P * h->Nofdm * sizeof(double complex));
    if (!h->rxbuf || !h->bf || !h->rb || !h->llr || !h->info || !h->preT || !h->pstT)
    {
        free(h->rxbuf); free(h->bf); free(h->rb); free(h->llr);
        free(h->info); free(h->preT); free(h->pstT); free(h);
        return NULL;
    }
    h->preN = mfsk_sync_build_template(&h->m, &h->o, h->preT, h->preE);
    h->pstN = mfsk_sync_build_postamble_template(&h->m, &h->o, h->pstT, h->pstE);
    return h;
}

static void mfsk_be_close(void *ctx)
{
    mfsk_modem_t *h = (mfsk_modem_t *)ctx;
    if (!h) return;
    free(h->rxbuf); free(h->bf); free(h->rb); free(h->llr);
    free(h->info); free(h->preT); free(h->pstT); free(h);
}

static void mfsk_be_configure(void *ctx, int frames_per_burst, int verbosity)
{
    (void)ctx; (void)frames_per_burst; (void)verbosity; /* MFSK is always 1 frame/burst */
}

/* ---- geometry --------------------------------------------------------- */

static int mfsk_be_bits_per_frame(void *ctx) { return ((mfsk_modem_t *)ctx)->code->K; }
static int mfsk_be_n_tx_samples(void *ctx)
{
    mfsk_modem_t *h = ctx; return h->NPAY * h->Nofdm;
}
static int mfsk_be_n_max_rx_samples(void *ctx)
{
    /* framework demod buffer; MFSK owns the real accumulation window */
    return 2 * ((mfsk_modem_t *)ctx)->Nofdm;
}
static int mfsk_be_n_nom_samples(void *ctx) { return mfsk_be_n_tx_samples(ctx); }
static int mfsk_be_sample_rate(void *ctx)   { (void)ctx; return (int)MFSK_FS; }
static int mfsk_be_get_mode(void *ctx)       { return ((mfsk_modem_t *)ctx)->mode; }
static int mfsk_be_frames_per_burst(void *ctx) { (void)ctx; return 1; }

/* ---- TX --------------------------------------------------------------- */

/* Emit nsym freq-domain MFSK symbols as int16 passband, advancing the burst
 * carrier phase (h->tx_n) so preamble→data→postamble is phase-continuous. */
static int mfsk_emit(mfsk_modem_t *h, const mfsk_cplx *syms, int nsym, int16_t *out)
{
    int written = 0;
    for (int s = 0; s < nsym; s++)
    {
        double complex bins[MFSK_NCAR], pad[MFSK_NFFT], t[MFSK_NFFT], cp[MFSK_NFFT + 128];
        for (int k = 0; k < MFSK_NCAR; k++)
            bins[k] = syms[s * MFSK_NCAR + k].re + syms[s * MFSK_NCAR + k].im * I;
        ofdm_zero_padder(&h->o, bins, pad);
        ofdm_ifft(&h->o, pad, t);
        ofdm_gi_adder(&h->o, t, cp);
        for (int n = 0; n < h->Nofdm; n++)
        {
            double ph = h->w * (double)h->tx_n++;
            double v = MFSK_TXAMP * (creal(cp[n]) * cos(ph) + cimag(cp[n]) * sin(ph));
            if (v > 32767.0) v = 32767.0; else if (v < -32768.0) v = -32768.0;
            out[written++] = (int16_t)lrint(v);
        }
    }
    return written;
}

static int mfsk_be_preamble_tx(void *ctx, int16_t *out)
{
    mfsk_modem_t *h = ctx;
    h->tx_n = 0;   /* preamble is always first in a burst */
    mfsk_cplx *pre = (mfsk_cplx *)calloc((size_t)h->P * MFSK_NCAR, sizeof(mfsk_cplx));
    if (!pre) return 0;
    mfsk_generate_preamble(&h->m, pre, h->P);
    int n = mfsk_emit(h, pre, h->P, out);
    free(pre);
    return n;
}

static int mfsk_be_rawdata_tx(void *ctx, int16_t *out, const uint8_t *frame)
{
    mfsk_modem_t *h = ctx;
    int *info = h->info;
    for (int i = 0; i < h->code->K; i++)
        info[i] = (frame[i >> 3] >> (7 - (i & 7))) & 1;   /* MSB-first */

    static int coded[MFSK_LDPC_MAXN];
    mfsk_ldpc_encode(h->code, info, coded);

    int nb = h->NPAY * h->bps;
    int *cbits = (int *)malloc((size_t)nb * sizeof(int));
    if (!cbits) return 0;
    for (int i = 0; i < nb; i++) cbits[i] = (i < h->code->N) ? coded[i] : 0;

    mfsk_cplx *dat = (mfsk_cplx *)calloc((size_t)h->NPAY * MFSK_NCAR, sizeof(mfsk_cplx));
    if (!dat) { free(cbits); return 0; }
    mfsk_mod(&h->m, cbits, nb, dat);
    int n = mfsk_emit(h, dat, h->NPAY, out);
    free(dat); free(cbits);
    return n;
}

static int mfsk_be_postamble_tx(void *ctx, int16_t *out)
{
    mfsk_modem_t *h = ctx;
    mfsk_cplx *pst = (mfsk_cplx *)calloc((size_t)h->P * MFSK_NCAR, sizeof(mfsk_cplx));
    if (!pst) return 0;
    mfsk_generate_postamble(&h->m, pst, h->P);
    int n = mfsk_emit(h, pst, h->P, out);
    free(pst);
    return n;
}

/* ---- RX --------------------------------------------------------------- */

static int mfsk_be_nin(void *ctx) { return ((mfsk_modem_t *)ctx)->Nofdm; }

/* Downmix rxbuf[0..rxlen) to complex baseband + LPF into h->bf. Non-coherent
 * demod/sync are invariant to the (arbitrary) carrier phase origin. */
static void mfsk_downmix(mfsk_modem_t *h)
{
    int L = h->rxlen;
    double complex *bb = (double complex *)malloc((size_t)L * sizeof(double complex));
    if (!bb) { h->rxlen = 0; return; }
    for (int i = 0; i < L; i++)
    {
        double x = (double)h->rxbuf[i];
        double ph = h->w * (double)i;
        bb[i] = 2.0 * x * cos(ph) + I * 2.0 * x * sin(ph);
    }
    for (int i = 0; i < L; i++)
    {
        double complex a = 0;
        for (int k = 0; k < MFSK_LPF_TAPS; k++)
        {
            int j = i - k + MFSK_LPF_TAPS / 2;
            if (j >= 0 && j < L) a += h->lpf[k] * bb[j];
        }
        h->bf[i] = a;
    }
    free(bb);
}

/* Demod one MFSK burst whose payload symbols start at bf[payoff], decode LDPC,
 * verify CRC16. Returns frame_bytes on success (bytes_out filled), else 0. */
static int mfsk_try_payload(mfsk_modem_t *h, int payoff, uint8_t *bytes_out)
{
    if (payoff < 0 || payoff + h->NPAY * h->Nofdm > h->rxlen)
        return 0;

    double sig = 0.0, noise = 0.0;   /* coarse SNR accumulators */
    for (int s = 0; s < h->NPAY; s++)
    {
        int b = payoff + s * h->Nofdm;
        double complex rmv[MFSK_NFFT], ff[MFSK_NFFT], dep[MFSK_NCAR];
        ofdm_gi_remover(&h->o, &h->bf[b], rmv);
        ofdm_fft(&h->o, rmv, ff);
        ofdm_zero_depadder(&h->o, ff, dep);
        for (int k = 0; k < MFSK_NCAR; k++)
        {
            h->rb[s * MFSK_NCAR + k].re = creal(dep[k]);
            h->rb[s * MFSK_NCAR + k].im = cimag(dep[k]);
        }
    }
    mfsk_demod(&h->m, h->rb, h->NPAY * h->bps, h->llr);
    for (int i = 0; i < h->code->N; i++) sig += fabs(h->llr[i]);   /* proxy */
    (void)noise;

    static int out_bits[MFSK_LDPC_MAXK];
    mfsk_ldpc_decode(h->code, h->llr, out_bits, 50);

    /* pack info bits -> bytes (MSB-first, matching TX) */
    size_t nbytes = h->frame_bytes;
    for (size_t by = 0; by < nbytes; by++)
    {
        uint8_t v = 0;
        for (int bit = 0; bit < 8; bit++)
            v = (uint8_t)((v << 1) | (out_bits[by * 8 + bit] & 1));
        bytes_out[by] = v;
    }
    /* CRC16 over payload (all but last 2), compare trailer — FreeDV's contract */
    uint16_t crc = freedv_gen_crc16(bytes_out, (int)(nbytes - 2));
    uint16_t got = (uint16_t)((bytes_out[nbytes - 2] << 8) | bytes_out[nbytes - 1]);
    if (crc != got)
        return 0;

    /* coarse SNR proxy from mean |LLR| (clamped at +/-5 in the demod): higher
     * mean magnitude => cleaner tones. Mapped loosely to dB; OTA-calibrated. */
    h->last_snr = (float)(sig / h->code->N);
    return (int)nbytes;
}

static int mfsk_be_rawdata_rx(void *ctx, uint8_t *bytes_out, const int16_t *demod_in)
{
    mfsk_modem_t *h = ctx;
    int chunk = h->Nofdm;   /* == nin */

    /* Append this chunk to the sliding window (drop oldest if over capacity). */
    if (h->rxlen + chunk > h->rxcap)
    {
        int drop = h->rxlen + chunk - h->rxcap;
        memmove(h->rxbuf, h->rxbuf + drop, (size_t)(h->rxlen - drop) * sizeof(int16_t));
        h->rxlen -= drop;
    }
    memcpy(h->rxbuf + h->rxlen, demod_in, (size_t)chunk * sizeof(int16_t));
    h->rxlen += chunk;
    h->since_search += chunk;

    /* Only attempt detection once a full burst could be present, and rate-limit
     * the (heavy) downmix+search to a few times per burst. */
    int burst = (h->P + h->NPAY) * h->Nofdm;   /* preamble + data */
    if (h->rxlen < burst || h->since_search < 4 * h->Nofdm)
        return 0;
    h->since_search = 0;

    mfsk_downmix(h);

    /* Primary sync anchor: the preamble at the burst head.  The payload's NPAY
     * symbols start P symbols after it. */
    double metric = 0.0;
    int off = mfsk_sync_search(h->bf, h->rxlen, 1, h->preT, h->preE,
                               h->preN, h->Nofdm, 0, &metric);
    int payoff = (off >= 0) ? off + h->P * h->Nofdm : -1;
    int nbytes = (payoff >= 0) ? mfsk_try_payload(h, payoff, bytes_out) : 0;

    /* Fallback anchor: the postamble at the burst tail.  On a half-duplex HF
     * link the HEAD of a burst is the fragile part — the far end may still be
     * keyed, releasing PTT, or the local AGC/turnaround is still settling — so a
     * clipped preamble is common (seen directly over the -x sock virtual clock:
     * the ISS starts data while the IRS is finishing its connect turnaround, and
     * the ~13.5 s MFSK burst is then lost to a full ACK-timeout retransmit).
     * The postamble is the same P known symbols emitted after the data, so it
     * anchors the payload just as well: the NPAY data symbols immediately
     * PRECEDE it.  Only used when the preamble path did not already decode. */
    if (nbytes <= 0)
    {
        double pmetric = 0.0;
        int poff = mfsk_sync_search(h->bf, h->rxlen, 1, h->pstT, h->pstE,
                                    h->pstN, h->Nofdm, 0, &pmetric);
        if (poff >= 0)
        {
            int ppayoff = poff - h->NPAY * h->Nofdm;
            int pn = (ppayoff >= 0) ? mfsk_try_payload(h, ppayoff, bytes_out) : 0;
            if (pn > 0)
            {
                off = poff;
                nbytes = pn;
            }
        }
    }

    h->last_sync = (nbytes > 0) || (off >= 0);
    if (nbytes <= 0)
        return 0;
    h->rxlen = 0;   /* burst consumed */
    return nbytes;
}

static void mfsk_be_get_stats(void *ctx, int *sync, float *snr)
{
    mfsk_modem_t *h = ctx;
    if (sync) *sync = h->last_sync;
    if (snr)  *snr  = h->last_snr;
}

static int mfsk_be_get_rx_status(void *ctx) { (void)ctx; return 0; }

/* ======================================================================
 * Pattern ACK (Welch-Costas tone burst) — TX and detect
 *
 * A pattern ACK is a short tone burst (ack_tones = plain ACK, break_tones =
 * ACK+TURN) at the MFSK geometry.  No preamble, no LDPC: the detector slides a
 * non-coherent matched filter over the baseband and counts matched symbols.
 * These helpers own a lazily-initialised (mfsk_t, ofdm_frame_t) at the same
 * geometry as the MFSK backend so the datalink layer can emit/detect patterns
 * without holding a full mfsk_modem_t.
 * ====================================================================== */

static mfsk_t       g_pat_m;
static ofdm_frame_t g_pat_o;
static double       g_pat_lpf[MFSK_LPF_TAPS];
static double       g_pat_w;
static int          g_pat_nofdm;
static bool         g_pat_ready = false;

static void mfsk_pattern_lazy_init(void)
{
    if (g_pat_ready) return;
    mfsk_init(&g_pat_m, MFSK_M, MFSK_NCAR, 1);
    ofdm_frame_init(&g_pat_o, MFSK_NFFT, MFSK_NCAR, MFSK_GI, 0);
    g_pat_nofdm = ofdm_frame_nofdm(&g_pat_o);
    g_pat_w     = 2.0 * M_PI * MFSK_FC / MFSK_FS;
    mklpf(g_pat_lpf, MFSK_LPF_FC);
    g_pat_ready = true;
}

/* Fast windowed ACK (Phase 2c): a clean multi-block burst is acked by the
 * 0.64 s pattern instead of the 3.74 s coded frame, disambiguated from a stale
 * pattern by a 2-bit epoch.  The epoch rides ONE extra 4-ary tone symbol
 * appended after the (unchanged) 16-symbol base ack/break pattern — tones
 * 0 / M/4 / M/2 / 3M/4 (for M=32: 0,8,16,24).  pattern_kind:
 *   0 / 1                       = bare ACK / break (byte-identical to before;
 *                                 the MFSK-floor fringe path is UNTOUCHED)
 *   MFSK_PAT_TAGGED|epoch<<1|brk = epoch-tagged fast ACK (16 base + 1 epoch sym)
 * A misread epoch fails the ISS's epoch==keydown check -> retry/coded fallback
 * (fail-safe: never an over-retirement, only a missed speedup). */
#define MFSK_PAT_TAGGED    0x80
#define MFSK_PAT_EPOCHS    4
#define MFSK_EPOCH_LEN     6    /* the 2-bit epoch rides a short Welch-Costas
                                 * mini-pattern of this many symbols, appended
                                 * after the base pattern and detected by the SAME
                                 * robust matched filter (mfsk_detect_pattern) —
                                 * NOT a raw FFT read, which the coarse base
                                 * locate + OFDM ICI make hopeless to align */
#define MFSK_EPOCH_MIN_MATCH 4  /* accept an epoch only if its mini-pattern matches
                                 * >= this many of MFSK_EPOCH_LEN symbols (tolerates
                                 * two symbol errors); a bare pattern's trailing
                                 * noise matches ~0-2, so this cleanly rejects it */
#define MFSK_EPOCH_MARGIN    2  /* ...and only if it beats the runner-up epoch by
                                 * this many matched symbols (guards a lucky near
                                 * miss on an alias/adjacent sequence) */

/* The 4 epoch mini-patterns (M=32): a spread Welch-Costas base sequence shifted
 * by 2*e tones, so every epoch differs from every other in EVERY symbol (by 2,4
 * or 6 tones — never M/2, which would alias) while each sequence itself hops
 * across the band.  mfsk_detect_pattern applies its own +j*tone_hop_step, so
 * these are the pre-hop table entries. */
static const int MFSK_EPOCH_BASE_SEQ[MFSK_EPOCH_LEN] = { 4, 20, 12, 28, 8, 24 };
static void mfsk_epoch_tones(int e, int *tones)
{
    for (int j = 0; j < MFSK_EPOCH_LEN; j++)
        tones[j] = (MFSK_EPOCH_BASE_SEQ[j] + 2 * (e % MFSK_PAT_EPOCHS)) % g_pat_m.M;
}

int mfsk_pattern_nsymb(void)
{
    mfsk_pattern_lazy_init();
    return g_pat_m.ack_pattern_nsymb;          /* base symbols (detect anchor) */
}

int mfsk_pattern_max_tx_samples(void)
{
    mfsk_pattern_lazy_init();
    /* + the appended epoch mini-pattern so RX windows/buffers fit the tagged form. */
    return (g_pat_m.ack_pattern_nsymb + MFSK_EPOCH_LEN) * g_pat_nofdm;
}

/* Synthesise one OFDM symbol from frequency bins fb[] into out[] (int16
 * passband), advancing the upconvert phase counter *tx_n.  Returns samples. */
static int mfsk_pat_emit_symbol(const double complex *fb, int16_t *out, long *tx_n)
{
    double complex pad[MFSK_NFFT], t[MFSK_NFFT], cp[MFSK_NFFT + 128];
    ofdm_zero_padder(&g_pat_o, (double complex *)fb, pad);
    ofdm_ifft(&g_pat_o, pad, t);
    ofdm_gi_adder(&g_pat_o, t, cp);
    for (int n = 0; n < g_pat_nofdm; n++)
    {
        double ph = g_pat_w * (double)(*tx_n)++;
        double v = MFSK_TXAMP * (creal(cp[n]) * cos(ph) + cimag(cp[n]) * sin(ph));
        if (v > 32767.0) v = 32767.0; else if (v < -32768.0) v = -32768.0;
        out[n] = (int16_t)lrint(v);
    }
    return g_pat_nofdm;
}

/* Generate the ack/break (+ optional epoch) pattern as int16 passband. */
int mfsk_pattern_tx(int16_t *out, int pattern_kind)
{
    mfsk_pattern_lazy_init();
    int ns      = g_pat_m.ack_pattern_nsymb;
    bool tagged = (pattern_kind & MFSK_PAT_TAGGED) != 0;
    int  brk    = pattern_kind & 1;
    int  epoch  = (pattern_kind >> 1) & (MFSK_PAT_EPOCHS - 1);

    mfsk_cplx *bins = (mfsk_cplx *)calloc((size_t)ns * MFSK_NCAR, sizeof(mfsk_cplx));
    if (!bins) return 0;
    if (brk) mfsk_generate_break_pattern(&g_pat_m, bins);
    else     mfsk_generate_ack_pattern(&g_pat_m, bins);

    int  written = 0;
    long tx_n    = 0;
    for (int s = 0; s < ns; s++)
    {
        double complex fb[MFSK_NCAR];
        for (int k = 0; k < MFSK_NCAR; k++)
            fb[k] = bins[s * MFSK_NCAR + k].re + bins[s * MFSK_NCAR + k].im * I;
        written += mfsk_pat_emit_symbol(fb, out + written, &tx_n);
    }
    if (tagged)
    {
        /* Append the epoch's Welch-Costas mini-pattern: symbol j carries the
         * (internally hopped) epoch tone at base-symbol amplitude, on every
         * stream — a standalone sub-pattern (hop index restarts at 0) so the RX
         * detects it with mfsk_detect_pattern exactly like the base pattern. */
        double amp = sqrt((double)g_pat_m.Nc / g_pat_m.nStreams);
        int etones[MFSK_EPOCH_LEN];
        mfsk_epoch_tones(epoch, etones);
        for (int j = 0; j < MFSK_EPOCH_LEN; j++)
        {
            double complex fb[MFSK_NCAR];
            for (int k = 0; k < MFSK_NCAR; k++) fb[k] = 0;
            int actual = (etones[j] + j * g_pat_m.tone_hop_step) % g_pat_m.M;
            for (int st = 0; st < g_pat_m.nStreams; st++)
                fb[g_pat_m.stream_offsets[st] + actual] = amp;
            written += mfsk_pat_emit_symbol(fb, out + written, &tx_n);
        }
    }
    free(bins);
    return written;
}

/* Detect a pattern ACK in an int16 passband chunk.  Returns 1 on a match and
 * sets *out_kind: 0/1 for a bare ACK/break, or MFSK_PAT_TAGGED|epoch<<1|brk
 * when a valid epoch symbol follows the base pattern; 0 (no match) otherwise. */
int mfsk_pattern_detect(const int16_t *pb, int n, int *out_kind)
{
    mfsk_pattern_lazy_init();
    if (out_kind) *out_kind = 0;
    if (!pb || n < g_pat_m.ack_pattern_nsymb * g_pat_nofdm)
        return 0;

    /* Downmix passband -> complex baseband + LPF (same as mfsk_downmix). */
    double complex *bb = (double complex *)malloc((size_t)n * sizeof(double complex));
    double complex *bf = (double complex *)malloc((size_t)n * sizeof(double complex));
    if (!bb || !bf) { free(bb); free(bf); return 0; }
    for (int i = 0; i < n; i++)
    {
        double x = (double)pb[i];
        double ph = g_pat_w * (double)i;
        bb[i] = 2.0 * x * cos(ph) + I * 2.0 * x * sin(ph);
    }
    for (int i = 0; i < n; i++)
    {
        double complex a = 0;
        for (int k = 0; k < MFSK_LPF_TAPS; k++)
        {
            int j = i - k + MFSK_LPF_TAPS / 2;
            if (j >= 0 && j < n) a += g_pat_lpf[k] * bb[j];
        }
        bf[i] = a;
    }

    int ns   = g_pat_m.ack_pattern_nsymb;
    int apos = -1, bpos = -1;
    int ack_score = mfsk_detect_pattern(&g_pat_m, &g_pat_o, bf, n,
                                        g_pat_m.ack_tones, g_pat_m.ack_pattern_len,
                                        ns, &apos);
    int brk_score = mfsk_detect_pattern(&g_pat_m, &g_pat_o, bf, n,
                                        g_pat_m.break_tones, g_pat_m.ack_pattern_len,
                                        ns, &bpos);

    bool ack_hit = ack_score >= g_pat_m.ack_match_threshold;
    bool brk_hit = brk_score >= g_pat_m.break_match_threshold;
    if (!ack_hit && !brk_hit) { free(bb); free(bf); return 0; }

    int brk = 0, pos = apos;
    if (brk_hit && (!ack_hit || brk_score >= ack_score)) { brk = 1; pos = bpos; }

    int kind = brk;

    /* Optional epoch mini-pattern after the base pattern (Phase-2c fast ACK).
     *
     * The 2-bit epoch rides a short Welch-Costas sub-pattern appended right
     * after the base pattern.  We recover it with the SAME robust matched filter
     * used for the base (mfsk_detect_pattern): score each of the 4 candidate
     * epoch sequences over the region just past the base pattern and take the
     * best — but only if it clears MFSK_EPOCH_MIN_MATCH symbols AND beats the
     * runner-up by MFSK_EPOCH_MARGIN.  mfsk_detect_pattern slides its own fine
     * timing search and uses per-symbol peak-tone matching, so it tolerates the
     * coarse base locate and OFDM ICI that defeat a fixed-offset FFT read.  A
     * bare pattern (no mini-pattern -> trailing noise) scores ~0-1 for every
     * candidate, so it stays bare — fail-safe (a miss only costs the fast ACK). */
    int epos = pos + ns * g_pat_nofdm;
    int step = g_pat_nofdm / 8; if (step < 1) step = 1;
    int rstart = epos - 2 * step;                 /* small slack for a slightly-late base */
    if (rstart < 0) rstart = 0;
    int rlen = (MFSK_EPOCH_LEN + 4) * g_pat_nofdm; /* + slack for base-locate error */
    if (rstart + rlen > n) rlen = n - rstart;

    if (pos >= 0 && rlen >= MFSK_EPOCH_LEN * g_pat_nofdm)
    {
        int best_e = -1, best_sc = -1, second_sc = -1;
        for (int e = 0; e < MFSK_PAT_EPOCHS; e++)
        {
            int etones[MFSK_EPOCH_LEN], ep = -1;
            mfsk_epoch_tones(e, etones);
            int sc = mfsk_detect_pattern(&g_pat_m, &g_pat_o, bf + rstart, rlen,
                                         etones, MFSK_EPOCH_LEN, MFSK_EPOCH_LEN, &ep);
            if (sc > best_sc) { second_sc = best_sc; best_sc = sc; best_e = e; }
            else if (sc > second_sc) { second_sc = sc; }
        }
        if (best_e >= 0 && best_sc >= MFSK_EPOCH_MIN_MATCH &&
            best_sc - second_sc >= MFSK_EPOCH_MARGIN)
            kind = MFSK_PAT_TAGGED | (best_e << 1) | brk;
    }

    free(bb); free(bf);
    if (out_kind) *out_kind = kind;
    return 1;
}

const modem_backend_t modem_backend_mfsk = {
    .name             = "mfsk",
    .open             = mfsk_be_open,
    .close            = mfsk_be_close,
    .configure        = mfsk_be_configure,
    .bits_per_frame   = mfsk_be_bits_per_frame,
    .n_tx_samples     = mfsk_be_n_tx_samples,
    .n_max_rx_samples = mfsk_be_n_max_rx_samples,
    .n_nom_samples    = mfsk_be_n_nom_samples,
    .sample_rate      = mfsk_be_sample_rate,
    .get_mode         = mfsk_be_get_mode,
    .frames_per_burst = mfsk_be_frames_per_burst,
    .preamble_tx      = mfsk_be_preamble_tx,
    .rawdata_tx       = mfsk_be_rawdata_tx,
    .postamble_tx     = mfsk_be_postamble_tx,
    .nin              = mfsk_be_nin,
    .rawdata_rx       = mfsk_be_rawdata_rx,
    .get_stats        = mfsk_be_get_stats,
    .get_rx_status    = mfsk_be_get_rx_status,
    .harq_reset       = NULL,   /* MFSK: single-shot decode (no Chase combining yet) */
    .set_harq         = NULL,
};
