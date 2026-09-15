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

/* How much may be Chase-combined before the accumulator is abandoned.  Sized
 * from the MFSK CALL retry cadence (~18 s apart, at most a handful of tries):
 * long enough to hold one genuine retry sequence together, short enough that an
 * abandoned sequence cannot reach the next caller. */
#define MFSK_HARQ_MAX_COPIES 4
#define MFSK_HARQ_WINDOW_MS  90000ULL
#include "freedv_api.h"   /* freedv_gen_crc16 — codec-independent CRC16 */

#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

/* Passband geometry — matches the validated offline harness (docs/MFSK-PORT.md). */
#define MFSK_FS       8000.0
#define MFSK_FC       2000.0
#define MFSK_NFFT     256
#define MFSK_NCAR     50
#define MFSK_GI       0.25
#define MFSK_M        32
/* Passband amplitude.
 *
 * 32-carrier OFDM has a high peak-to-average ratio, and this scale is applied
 * to the IFFT output directly: at 6000 the true peak came to 42426 against a
 * 32767 rail, so mfsk_emit() hard-clipped 45% of its own payload samples before
 * the signal ever reached the radio.  PAPR measured 1.9 dB -- the burst was
 * closer to a square wave than to OFDM.  Sync survived that (correlation is
 * robust to clipping, the preamble still peaked at 0.889) but the subcarriers
 * lost orthogonality and the LDPC payload never passed CRC on the air, while
 * the offline round-trip still "passed" because both ends saw the same
 * deterministic distortion.
 *
 * 2200 puts the true peak at ~15.5k, about -6.5 dBFS, which leaves room for the
 * operator's TX gain (up to +6 dB) before the rail.  Do not raise this without
 * re-measuring the peak: the clamp below hides the damage instead of reporting
 * it. */
#define MFSK_TXAMP    2200.0
/* Frequency hypotheses in HALF subcarriers (15.625 Hz), spanning +/-3 bins,
 * i.e. about +/-94 Hz of dial error -- well past what a pair of HF radios
 * drift apart by.
 *
 * Half-bin steps, not whole: a whole-bin grid leaves the worst case sitting
 * exactly between two hypotheses, where the tone energy splits across two FFT
 * bins and neither template captures it. Measured with a whole-bin grid --
 * 31/62/94 Hz recovered to 10/10, but 16/47/110 Hz stayed at 0/10. Half-bin
 * steps bound the residual at a quarter bin (7.8 Hz), which decodes cleanly.
 *
 * Ordered nearest-first at search time and latched once found, so a correctly
 * tuned link pays for exactly one correlation, as before. */
#define MFSK_FREQ_HYPS   13
#define MFSK_FREQ_HYP_LO (-6)          /* in half-bins */

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
    double complex *bb;             /* downmixed, pre-LPF, rxcap long (persistent:
                                     * the downmix+FIR is incremental, see
                                     * mfsk_downmix) */
    int      bb_len;                /* samples of bb that are downmixed        */
    int      bf_len;                /* samples of bf that are FINAL (filtered) */
    long     n_abs;                 /* absolute index of rxbuf[0] — keeps the
                                     * carrier phase tied to the sample, not to
                                     * its position in a sliding buffer        */
    mfsk_cplx      *rb;             /* NPAY*NCAR depadded bins */
    int             nb;             /* NPAY*bps transmitted bit slots (>= N) */
    int            *ilv;            /* interleaver: coded index per tx slot   */
    float          *llr;            /* nb LLRs, in transmitted order          */
    float          *llr_di;         /* N LLRs, deinterleaved into code order  */
    float          *llr_acc;        /* HARQ: summed LLRs across received copies */
    int             harq_on;        /* accumulate across bursts when non-zero   */
    int             harq_copies;    /* copies currently summed into llr_acc     */
    uint64_t        harq_first_ms;  /* when the current accumulation started    */
    int            *info;           /* K info bits */
    double complex *preT, *pstT;
    /* Preamble templates pre-rotated by each frequency hypothesis. A dial
     * error shifts every tone; beyond half a subcarrier (15.6 Hz here) the
     * nominal FFT bins see nothing and the mode goes stone deaf. Measured on
     * the unmodified decoder: 12 Hz decoded 10/10, 16 Hz decoded 0/10. */
    double complex *preT_hyp[MFSK_FREQ_HYPS];
    int             freq_hb;        /* latched hypothesis, in HALF subcarriers */
    int             freq_locked;    /* a decode confirmed freq_hb            */
    double          preE[MFSK_MAX_PREAMBLE_SYMB], pstE[MFSK_MAX_PREAMBLE_SYMB];
    int             preN, pstN;

    long  tried_abs;       /* anchor whose payload we have already decoded once */
    long  reject_abs;      /* furthest anchor whose resident payload failed CRC;
                            * the next search restarts past it so one false peak
                            * cannot mask the real burst behind it */
    long  anchor_abs;      /* absolute sample index of a located preamble, or
                            * -1: lets a burst be tracked without re-correlating
                            * the whole window on every attempt */
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

static void mfsk_be_close(void *ctx);

static void *mfsk_be_open(int mode)
{
    mfsk_modem_t *h = (mfsk_modem_t *)calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->mode = mode;
    mfsk_init(&h->m, MFSK_M, MFSK_NCAR, 1);
    ofdm_frame_init(&h->o, MFSK_NFFT, MFSK_NCAR, MFSK_GI, 0);
    /* Code rate.  All five ported codes share N=1600, so the burst is 13.5 s
     * whatever the rate -- a lower rate costs payload, not airtime.  Measured
     * anyway: with acquisition fixed, rate 1/8 buys only ~0.5 dB over rate 1/2
     * (4/20 vs 0/20 at SNR3k -12.1 dB) because the limit there is the preamble
     * detector reaching its own noise floor, not the code.  Not worth 4x the
     * airtime per byte, so the fringe rung stays at rate 1/2. */
    h->code  = &mfsk_ldpc_8_16;          /* rate 1/2: 100-byte frame */
    h->Nofdm = ofdm_frame_nofdm(&h->o);
    h->P     = h->m.preamble_nSymb;
    h->bps   = mfsk_bits_per_symbol(&h->m);
    h->NPAY  = (h->code->N + h->bps - 1) / h->bps;
    h->frame_bytes = (size_t)h->code->K / 8;
    h->w     = 2.0 * M_PI * MFSK_FC / MFSK_FS;
    mklpf(h->lpf, MFSK_LPF_FC);

    /* RX window: one full burst (pre + data + post) plus a few symbols slack. */
    /* Two bursts of window, not one.
     *
     * A payload can only be demodulated while its whole burst is resident, i.e.
     * off + (P+NPAY)*Nofdm <= bf_len.  With a one-burst window that is true only
     * for the ~0.48 s between the data ending and the preamble sliding out the
     * front -- a razor-thin catch that the search, running every 4 symbols, kept
     * missing: measured, `off` never even reached 8000.  Sizing for two bursts
     * makes the burst decodable for a whole burst-length of slide instead, which
     * is the difference between "usually" and "reliably". */
    h->rxcap = (2 * (h->P + h->NPAY) + h->P + 8) * h->Nofdm;
    h->rxbuf = (int16_t *)malloc((size_t)h->rxcap * sizeof(int16_t));
    h->bf    = (double complex *)malloc((size_t)h->rxcap * sizeof(double complex));
    h->bb    = (double complex *)malloc((size_t)h->rxcap * sizeof(double complex));
    h->bb_len = h->bf_len = 0;
    h->n_abs  = 0;
    h->anchor_abs = -1;
    h->tried_abs  = -1;
    h->reject_abs = -1;
    h->rb    = (mfsk_cplx *)calloc((size_t)h->NPAY * MFSK_NCAR, sizeof(mfsk_cplx));
    /* NPAY*bps, not N: mfsk_demod emits one LLR per transmitted bit slot, and
     * bps need not divide N (M=8 gives 1602 slots for a 1600-bit codeword). */
    h->nb    = h->NPAY * h->bps;
    h->ilv   = (int *)malloc((size_t)h->nb * sizeof(int));
    h->llr   = (float *)malloc((size_t)h->nb * sizeof(float));
    h->llr_di= (float *)malloc((size_t)h->code->N * sizeof(float));
    h->llr_acc = (float *)calloc((size_t)h->code->N, sizeof(float));
    h->harq_on = 0;
    h->harq_copies = 0;
    h->harq_first_ms = 0;
    h->info  = (int *)malloc((size_t)h->code->K * sizeof(int));
    h->preT  = (double complex *)malloc((size_t)h->P * h->Nofdm * sizeof(double complex));
    h->pstT  = (double complex *)malloc((size_t)h->P * h->Nofdm * sizeof(double complex));
    if (!h->rxbuf || !h->bf || !h->rb || !h->llr || !h->llr_di || !h->llr_acc || !h->ilv ||
        !h->info || !h->preT || !h->pstT)
    {
        free(h->rxbuf); free(h->bf); free(h->rb); free(h->llr); free(h->llr_di); free(h->llr_acc);
        free(h->ilv); free(h->info); free(h->preT); free(h->pstT); free(h);
        return NULL;
    }
    mfsk_interleave_init(h->ilv, h->nb);
    h->preN = mfsk_sync_build_template(&h->m, &h->o, h->preT, h->preE);
    /* One template per hypothesis: rotating the TEMPLATE by +k bins matches a
     * received signal shifted by +k bins, and costs nothing at run time beyond
     * the extra correlations. Rotation is continuous across the whole template
     * because the received burst is. */
    for (int hy = 0; hy < MFSK_FREQ_HYPS; hy++)
    {
        int hb = MFSK_FREQ_HYP_LO + hy;   /* half-bins */
        h->preT_hyp[hy] = (double complex *)malloc((size_t)h->preN * h->Nofdm *
                                                   sizeof(double complex));
        if (!h->preT_hyp[hy]) { mfsk_be_close(h); return NULL; }
        for (int n = 0; n < h->preN * h->Nofdm; n++)
        {
            double ph = M_PI * (double)hb * (double)n / (double)MFSK_NFFT;
            h->preT_hyp[hy][n] = h->preT[n] * (cos(ph) + I * sin(ph));
        }
    }
    h->freq_hb = 0;
    h->freq_locked = 0;
    h->pstN = mfsk_sync_build_postamble_template(&h->m, &h->o, h->pstT, h->pstE);
    return h;
}

static void mfsk_be_close(void *ctx)
{
    mfsk_modem_t *h = (mfsk_modem_t *)ctx;
    if (!h) return;
    /* bb was never freed here: it is rxcap complex doubles (~3.4 MB), leaked on
     * every open/close cycle. */
    for (int hy = 0; hy < MFSK_FREQ_HYPS; hy++) free(h->preT_hyp[hy]);
    free(h->rxbuf); free(h->bf); free(h->bb); free(h->rb);
    free(h->llr); free(h->llr_di); free(h->llr_acc); free(h->ilv);
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
/* Occupied bandwidth: the modulator lights nStreams*M contiguous OFDM bins
 * (mfsk_init), spaced MFSK_FS/MFSK_NFFT = 31.25 Hz apart.  Same convention the
 * freedv backend reports (lit carriers x subcarrier spacing), so the two
 * numbers the UI shows for the two backends mean the same thing. */
static int mfsk_be_bandwidth_hz(void *ctx)
{
    const mfsk_t *m = &((mfsk_modem_t *)ctx)->m;
    double spacing = MFSK_FS / (double)MFSK_NFFT;
    return (int)((double)(m->nStreams * m->M) * spacing + 0.5);
}

static int mfsk_be_get_mode(void *ctx)       { return ((mfsk_modem_t *)ctx)->mode; }
static int mfsk_be_frames_per_burst(void *ctx) { (void)ctx; return 1; }

/* ---- TX --------------------------------------------------------------- */

/* Emit nsym freq-domain MFSK symbols as int16 passband, advancing the burst
 * carrier phase (h->tx_n) so preamble→data→postamble is phase-continuous. */
static uint64_t mfsk_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

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

    /* Scatter the codeword across the burst.  Transmitted slot t carries coded
     * bit ilv[t], so a fade that wipes a run of consecutive symbols damages
     * codeword positions spread over the whole block instead of one contiguous
     * stretch -- which is the difference between "the LDPC fixes it" and "the
     * frame is lost". Slots past N are padding and carry nothing. */
    int nb = h->nb;
    int *cbits = (int *)malloc((size_t)nb * sizeof(int));
    if (!cbits) return 0;
    for (int t = 0; t < nb; t++)
    {
        int j = h->ilv[t];
        cbits[t] = (j < h->code->N) ? coded[j] : 0;
    }

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
    /* Incremental: downmix and filter only what arrived since the last call.
     *
     * This used to malloc the whole window and recompute it end to end on every
     * search: 107520 samples, a 1.7 MB allocation, 215k trig calls and a
     * 63-tap FIR over the lot -- 6.8M complex MACs, ~6 times a second.  The RX
     * thread could not keep up, ran at ~42% of real time, and the capture ring
     * dropped audio, so a 13.5 s burst was never contiguous in the window and
     * could not be demodulated however the buffer was sized.  Cost is now
     * proportional to the new samples, not the window (~84x less work here),
     * which is also what makes this viable on 32-bit ARM.
     *
     * The carrier phase is taken from the ABSOLUTE sample index (n_abs), so
     * sliding the buffer does not rotate previously computed samples. */
    for (int i = h->bb_len; i < h->rxlen; i++)
    {
        double x  = (double)h->rxbuf[i];
        double ph = h->w * (double)(h->n_abs + (long)i);
        h->bb[i] = 2.0 * x * cos(ph) + I * 2.0 * x * sin(ph);
    }
    h->bb_len = h->rxlen;

    /* bf[i] needs bb up to i + TAPS/2, so it is final only once that has
     * arrived.  Everything below bf_len is already final and is never
     * recomputed. */
    int last = h->bb_len - MFSK_LPF_TAPS / 2;
    if (last > h->rxlen) last = h->rxlen;
    for (int i = h->bf_len; i < last; i++)
    {
        double complex a = 0;
        for (int k = 0; k < MFSK_LPF_TAPS; k++)
        {
            int j = i - k + MFSK_LPF_TAPS / 2;
            if (j >= 0 && j < h->bb_len) a += h->lpf[k] * h->bb[j];
        }
        h->bf[i] = a;
    }
    if (last > h->bf_len) h->bf_len = last;
}

/* Demod one MFSK burst whose payload symbols start at bf[payoff], decode LDPC,
 * verify CRC16. Returns frame_bytes on success (bytes_out filled), else 0. */
static int mfsk_try_payload(mfsk_modem_t *h, int payoff, uint8_t *bytes_out)
{
    if (payoff < 0 || payoff + h->NPAY * h->Nofdm > h->bf_len)
        return 0;

    double sig = 0.0, noise = 0.0;   /* coarse SNR accumulators */
    for (int s = 0; s < h->NPAY; s++)
    {
        int b = payoff + s * h->Nofdm;
        double complex rmv[MFSK_NFFT], ff[MFSK_NFFT], dep[MFSK_NCAR];
        double complex sym[MFSK_NFFT * 2];
        const double complex *src = &h->bf[b];
        if (h->freq_hb != 0)
        {
            /* Undo the dial offset the acquisition found, so the tones land
             * back on their nominal bins. Phase restarts each symbol, which is
             * harmless: the demod is non-coherent, only the frequency matters. */
            for (int n = 0; n < h->Nofdm; n++)
            {
                double ph = -M_PI * (double)h->freq_hb * (double)n /
                            (double)MFSK_NFFT;
                sym[n] = h->bf[b + n] * (cos(ph) + I * sin(ph));
            }
            src = sym;
        }
        ofdm_gi_remover(&h->o, src, rmv);
        ofdm_fft(&h->o, rmv, ff);
        ofdm_zero_depadder(&h->o, ff, dep);
        for (int k = 0; k < MFSK_NCAR; k++)
        {
            h->rb[s * MFSK_NCAR + k].re = creal(dep[k]);
            h->rb[s * MFSK_NCAR + k].im = cimag(dep[k]);
        }
    }
    mfsk_demod(&h->m, h->rb, h->nb, h->llr);
    for (int t = 0; t < h->nb; t++)
    {
        int j = h->ilv[t];
        if (j < h->code->N) h->llr_di[j] = h->llr[t];   /* undo the scatter */
    }
    for (int i = 0; i < h->code->N; i++) sig += fabs(h->llr_di[i]);   /* proxy */
    (void)noise;

    /* HARQ Chase combining across separated bursts.
     *
     * A chunked CALL keys the same codeword several times with unkeyed gaps
     * between copies, so the caller can listen for an early ACCEPT.  Each copy
     * alone is far too weak at the fringe; summing their LLRs is what makes
     * the sequence equivalent to one long burst.  Measured on flat Rayleigh at
     * equal airtime, gapped copies match a contiguous burst and beat it at
     * 0.1 Hz (FER 0.050 vs 0.133 at SNR3k -15 dB) because the copies
     * decorrelate across fades instead of sitting inside one.
     *
     * Decode the single shot FIRST and only fall back to the combined buffer
     * when it fails: a good copy must never be corrupted by summing it with
     * stale ones -- that ordering mistake once turned HARQ into a data-plane
     * outage here (issue #223 lineage), so it is deliberate.
     */
    static int out_bits[MFSK_LDPC_MAXK];
    int ok = mfsk_ldpc_decode(h->code, h->llr_di, out_bits, 50);

    if (h->harq_on)
    {
        /* Bound what may be summed together.
         *
         * The accumulator has no way to know which codeword a copy belongs to:
         * it sums LLRs BEFORE the decode, and a listening station has no
         * session yet, so consecutive CALLs from different callers would
         * otherwise pile into one sum.  That cannot produce a wrong frame --
         * single-shot is decoded first and the CRC16 gate below rejects
         * garbage -- but a stale sum quietly wastes the combining opportunity
         * for every later copy, which is the whole point of having it.
         *
         * So discard an accumulation that has grown too old or too deep and
         * start fresh from the current copy.  MFSK CALL retries arrive about
         * one retry_interval apart (18 s), so a window of a few intervals
         * keeps a genuine retry sequence together while ensuring an abandoned
         * one cannot poison the next caller. */
        uint64_t now = mfsk_now_ms();
        if (h->harq_copies > 0 &&
            (h->harq_copies >= MFSK_HARQ_MAX_COPIES ||
             now - h->harq_first_ms > MFSK_HARQ_WINDOW_MS))
        {
            memset(h->llr_acc, 0, (size_t)h->code->N * sizeof(float));
            h->harq_copies = 0;
        }
        if (h->harq_copies == 0)
            h->harq_first_ms = now;

        for (int i = 0; i < h->code->N; i++) h->llr_acc[i] += h->llr_di[i];
        h->harq_copies++;
        if (!ok && h->harq_copies > 1)
        {
            /* Average, not raw sum: the min-sum decoder is scale-sensitive,
             * so a growing magnitude changes its behaviour as copies pile up. */
            float inv = 1.0f / (float)h->harq_copies;
            for (int i = 0; i < h->code->N; i++) h->llr_di[i] = h->llr_acc[i] * inv;
            ok = mfsk_ldpc_decode(h->code, h->llr_di, out_bits, 50);
        }
        if (ok)
        {
            memset(h->llr_acc, 0, (size_t)h->code->N * sizeof(float));
            h->harq_copies = 0;
            h->harq_first_ms = 0;
        }
    }
    (void)ok;

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
        /* Keep the derived buffers aligned with the raw one, or the incremental
         * downmix would recompute the wrong region. */
        if (h->bb_len > drop)
            memmove(h->bb, h->bb + drop, (size_t)(h->bb_len - drop) * sizeof(double complex));
        if (h->bf_len > drop)
            memmove(h->bf, h->bf + drop, (size_t)(h->bf_len - drop) * sizeof(double complex));
        h->bb_len = (h->bb_len > drop) ? h->bb_len - drop : 0;
        h->bf_len = (h->bf_len > drop) ? h->bf_len - drop : 0;
        h->n_abs += drop;
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
    /* Only search offsets whose payload could still be resident.  A preamble
     * found beyond bf_len - (P+NPAY)*Nofdm cannot be demodulated yet, so
     * correlating out there is pure cost -- and with a two-burst window that is
     * half the buffer.  Trimming the range keeps the search proportional to
     * what is actually decodable. */
    /* Re-use a preamble we have already located rather than correlating for it
     * again.  The burst does not move: only the window slides under it, and
     * n_abs tracks exactly how far.  Searching afresh every 4 symbols meant
     * re-finding the same peak six times a second over a multi-second window,
     * which is most of this decoder's CPU -- and CPU is what decides whether it
     * keeps up on a lossy real-time capture (ALSA drops on overrun; the FIFO
     * path, which blocks instead, decodes the same burst fine) and whether it
     * runs at all on a 32-bit Pi. */
    double metric = 0.0;
    int off = -1;
    int search_len = h->bf_len - h->NPAY * h->Nofdm;

    if (h->anchor_abs >= 0)
    {
        long rel = h->anchor_abs - h->n_abs;
        if (rel >= 0 && rel <= (long)search_len)
        {
            off = (int)rel;          /* still in view: no correlation needed */
            metric = 1.0;
        }
        else
        {
            h->anchor_abs = -1;      /* slid out of the window */
        }
    }

    if (off < 0 && search_len > h->P * h->Nofdm)
    {
        /* Resume past a peak we already proved wrong.  The search returns the
         * EARLIEST offset above threshold, so a rejected peak at X means there
         * is nothing above threshold before X -- restarting after it cannot
         * skip the real burst, and without this a single false peak is re-found
         * and re-rejected forever while the true preamble sits behind it. */
        int start_symb = 0;
        if (h->reject_abs >= 0)
        {
            long rel = h->reject_abs - h->n_abs;
            if (rel < 0) h->reject_abs = -1;                 /* slid out */
            else start_symb = (int)(rel / h->Nofdm) + 1;
        }
        /* Sweep frequency hypotheses, not just time. Without this a dial
         * error of half a subcarrier makes the preamble invisible, and no
         * amount of time searching recovers it.
         *
         * Take the BEST hypothesis, not the first one over the threshold. At
         * half-bin spacing a neighbouring hypothesis is only 0.5 bin off and
         * still correlates well enough to pass the gate, so first-past-the-post
         * latches the wrong offset and the payload de-rotation is then half a
         * bin out -- acquisition "succeeds" and every frame fails CRC. (Whole-
         * bin spacing hid this: neighbours were a full bin away and fell below
         * the gate.)
         *
         * A confirmed lock short-circuits the sweep to a single correlation,
         * because the offset belongs to the radio pair and does not wander
         * mid-session. The lock is dropped again whenever a resident payload
         * fails CRC, so a wrong latch cannot wedge the decoder. */
        if (h->freq_locked)
        {
            int hy = h->freq_hb - MFSK_FREQ_HYP_LO;
            if (hy >= 0 && hy < MFSK_FREQ_HYPS)
                off = mfsk_sync_search(h->bf, search_len, 1, h->preT_hyp[hy],
                                       h->preE, h->preN, h->Nofdm, start_symb,
                                       &metric);
            if (off < 0) h->freq_locked = 0;   /* stale lock: re-sweep below */
        }

        if (off < 0)
        {
            double best_metric = -1.0;
            int    best_off = -1, best_hb = h->freq_hb;
            for (int hy = 0; hy < MFSK_FREQ_HYPS; hy++)
            {
                double m = 0.0;
                int o = mfsk_sync_search(h->bf, search_len, 1, h->preT_hyp[hy],
                                         h->preE, h->preN, h->Nofdm,
                                         start_symb, &m);
                if (o >= 0 && m > best_metric)
                {
                    best_metric = m;
                    best_off    = o;
                    best_hb     = MFSK_FREQ_HYP_LO + hy;
                }
            }
            if (best_off >= 0)
            {
                off        = best_off;
                metric     = best_metric;
                h->freq_hb = best_hb;
            }
        }

        if (off >= 0)
            h->anchor_abs = h->n_abs + (long)off;
    }
    int payoff = (off >= 0) ? off + h->P * h->Nofdm : -1;
    /* Decode a given burst ONCE.  Once the anchor is fixed and the payload is
     * fully resident, the samples feeding the demod no longer change, so
     * re-running a 50-iteration LDPC decode every 4 symbols cannot produce a
     * different answer -- it just burns CPU in bursts, and a spike is exactly
     * what makes a real-time capture overrun and lose the NEXT burst.  (The
     * FIFO path blocks instead of dropping, which is why it completes transfers
     * while ALSA and PulseAudio both stall after one frame.) */
    int payload_resident = (payoff >= 0) &&
                           (payoff + h->NPAY * h->Nofdm <= h->bf_len);
    int already_tried = (h->anchor_abs >= 0) && (h->tried_abs == h->anchor_abs);
    int nbytes = 0;
    if (payoff >= 0 && !already_tried)
    {
        nbytes = mfsk_try_payload(h, payoff, bytes_out);
        if (payload_resident && h->anchor_abs >= 0)
        {
            if (nbytes > 0)
            {
                h->tried_abs = h->anchor_abs;  /* settled: do not redo this burst */
                h->freq_locked = 1;            /* a CRC pass confirms the offset */
            }
            else
            {
                /* Could be a false peak, or a right peak at the wrong offset.
                 * Drop the frequency lock either way so the next search
                 * re-argmaxes instead of trusting a hypothesis that has just
                 * produced an undecodable frame. */
                h->freq_locked = 0;
                /* A fully-resident payload that fails CRC is either a false
                 * peak or an undecodable burst.  Either way this anchor has
                 * nothing left to give: release it and let the search move on,
                 * rather than caching it and going blind until it slides out. */
                if (h->anchor_abs > h->reject_abs) h->reject_abs = h->anchor_abs;
                h->tried_abs  = h->anchor_abs;
                h->anchor_abs = -1;
            }
        }
    }

    /* Fallback anchor: the postamble at the burst tail.  On a half-duplex HF
     * link the HEAD of a burst is the fragile part — the far end may still be
     * keyed, releasing PTT, or the local AGC/turnaround is still settling — so a
     * clipped preamble is common (seen directly over the -x sock virtual clock:
     * the ISS starts data while the IRS is finishing its connect turnaround, and
     * the ~13.5 s MFSK burst is then lost to a full ACK-timeout retransmit).
     * The postamble is the same P known symbols emitted after the data, so it
     * anchors the payload just as well: the NPAY data symbols immediately
     * PRECEDE it.  Only used when the preamble path did not already decode. */
    /* Only worth the second correlation when the preamble anchor genuinely had
     * nothing to offer: either it found no preamble at all, or it found one
     * whose payload was present and still failed.  While the burst is merely
     * incomplete there is nothing for this to recover, and running it on every
     * such attempt doubled the search cost for no chance of a frame. */
    int payload_was_resident = (payoff >= 0) &&
                               (payoff + h->NPAY * h->Nofdm <= h->bf_len);
    if (nbytes <= 0 && (off < 0 || payload_was_resident))
    {
        double pmetric = 0.0;
        int poff = mfsk_sync_search(h->bf, h->bf_len, 1, h->pstT, h->pstE,
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
    h->n_abs += h->rxlen;
    h->rxlen = h->bb_len = h->bf_len = 0;   /* burst consumed */
    h->anchor_abs = -1;
    h->tried_abs  = -1;
    h->reject_abs = -1;                     /* next burst searches from the top */
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

int mfsk_pattern_nsymb(void)
{
    mfsk_pattern_lazy_init();
    return g_pat_m.ack_pattern_nsymb;
}

int mfsk_pattern_max_tx_samples(void)
{
    mfsk_pattern_lazy_init();
    return g_pat_m.ack_pattern_nsymb * g_pat_nofdm;
}

/* Generate the ack/break pattern as int16 passband.  Returns sample count. */
int mfsk_pattern_tx(int16_t *out, int pattern_kind)
{
    mfsk_pattern_lazy_init();
    int ns = g_pat_m.ack_pattern_nsymb;

    mfsk_cplx *bins = (mfsk_cplx *)calloc((size_t)ns * MFSK_NCAR, sizeof(mfsk_cplx));
    if (!bins) return 0;
    if (pattern_kind == 1)   /* ARQ_PATTERN_BREAK */
        mfsk_generate_break_pattern(&g_pat_m, bins);
    else
        mfsk_generate_ack_pattern(&g_pat_m, bins);

    int written = 0;
    long tx_n = 0;
    for (int s = 0; s < ns; s++)
    {
        double complex fb[MFSK_NCAR], pad[MFSK_NFFT], t[MFSK_NFFT], cp[MFSK_NFFT + 128];
        for (int k = 0; k < MFSK_NCAR; k++)
            fb[k] = bins[s * MFSK_NCAR + k].re + bins[s * MFSK_NCAR + k].im * I;
        ofdm_zero_padder(&g_pat_o, fb, pad);
        ofdm_ifft(&g_pat_o, pad, t);
        ofdm_gi_adder(&g_pat_o, t, cp);
        for (int n = 0; n < g_pat_nofdm; n++)
        {
            double ph = g_pat_w * (double)tx_n++;
            double v = MFSK_TXAMP * (creal(cp[n]) * cos(ph) + cimag(cp[n]) * sin(ph));
            if (v > 32767.0) v = 32767.0; else if (v < -32768.0) v = -32768.0;
            out[written++] = (int16_t)lrint(v);
        }
    }
    free(bins);
    return written;
}

/* Detect a pattern ACK in an int16 passband chunk.  Returns 1 on a match and
 * sets *is_break (1 = ACK+TURN break, 0 = plain ACK); 0 if none. */
int mfsk_pattern_detect(const int16_t *pb, int n, int *is_break)
{
    mfsk_pattern_lazy_init();
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

    /* Score ack and break in ONE pass.  These two calls used to redo every
     * FFT over the same samples for the sake of a different tone list, which
     * doubled the cost of the most expensive thing in the RX loop (measured
     * 3.5k samp/s against 8k arriving — the reason arq.c only runs this inside
     * bounded windows). */
    int ns  = g_pat_m.ack_pattern_nsymb;
    const int *lists[2] = { g_pat_m.ack_tones, g_pat_m.break_tones };
    int scores[2] = {0, 0};
    mfsk_detect_patterns(&g_pat_m, &g_pat_o, bf, n, lists, 2,
                         g_pat_m.ack_pattern_len, ns, scores, NULL);
    int ack_score = scores[0];
    int brk_score = scores[1];
    free(bb); free(bf);

    bool ack_hit = ack_score >= g_pat_m.ack_match_threshold;
    bool brk_hit = brk_score >= g_pat_m.break_match_threshold;
    if (!ack_hit && !brk_hit)
        return 0;
    /* Prefer the higher-scoring symbol so the two are told apart cleanly. */
    if (brk_hit && (!ack_hit || brk_score >= ack_score))
    {
        if (is_break) *is_break = 1;
        return 1;
    }
    if (is_break) *is_break = 0;
    return 1;
}

static void mfsk_be_harq_reset(void *ctx)
{
    mfsk_modem_t *h = (mfsk_modem_t *)ctx;
    if (!h || !h->llr_acc || !h->code) return;
    memset(h->llr_acc, 0, (size_t)h->code->N * sizeof(float));
    h->harq_copies = 0;
    h->harq_first_ms = 0;
}

static void mfsk_be_set_harq(void *ctx, int enabled)
{
    mfsk_modem_t *h = (mfsk_modem_t *)ctx;
    if (!h) return;
    h->harq_on = enabled ? 1 : 0;
    if (!enabled) mfsk_be_harq_reset(ctx);
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
    .bandwidth_hz     = mfsk_be_bandwidth_hz,
    .get_mode         = mfsk_be_get_mode,
    .frames_per_burst = mfsk_be_frames_per_burst,
    .preamble_tx      = mfsk_be_preamble_tx,
    .rawdata_tx       = mfsk_be_rawdata_tx,
    .postamble_tx     = mfsk_be_postamble_tx,
    .nin              = mfsk_be_nin,
    .rawdata_rx       = mfsk_be_rawdata_rx,
    .get_stats        = mfsk_be_get_stats,
    .get_rx_status    = mfsk_be_get_rx_status,
    .harq_reset       = mfsk_be_harq_reset,
    .set_harq         = mfsk_be_set_harq,
};
