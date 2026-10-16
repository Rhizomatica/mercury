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
#include <pthread.h>
#include <stdatomic.h>

/* Passband geometry — matches the validated offline harness (docs/MFSK-PORT.md). */
#define MFSK_FS       8000.0
#define MFSK_NFFT     256          /* the floor's; the 500 Hz modes vary it */
#define MFSK_NFFT_MAX 512
/* Carrier.  The 32 tones are the middle 32 of the 50 DC-centred subcarriers,
 * bins -16..+15 at baseband.  mfsk_synth takes the real part against the
 * conjugate carrier, which mirrors the spectrum: on the air they are bins
 * -15..+16 around the carrier, centred half a bin (15.625 Hz) ABOVE it.  Half
 * a bin below 1500 Hz therefore puts them on 1500 Hz, where every codec2 mode
 * is centred: 1015.6..1984.4 Hz.  Through 1.9.18 the carrier was 2000 Hz,
 * carried over from the offline harness, which put the tones on 1531..2500 Hz
 * -- off-centre in the radio's passband, and away from every other mode.
 *
 * MFSK_FC / MFSK_FS = 95/512, so the carrier phasor repeats every
 * MFSK_FC_PERIOD samples and is read from a table (fc_phasor): no
 * trigonometry per sample, which on a Pi 4 is a sizeable share of what a
 * listening station spends.  The period must stay a power of two holding a
 * whole number of carrier cycles, or the phase jumps every period and no
 * burst decodes (test_mfsk_modem's round trips). */
#define MFSK_FC        (1500.0 - 0.5 * MFSK_FS / MFSK_NFFT)   /* 1484.375 Hz */
/* 1024 holds a whole number of cycles of both carriers: 1484.375 Hz (190
 * cycles) and, for the 15.625 Hz-spaced mode, 1492.1875 Hz (191). */
#define MFSK_FC_PERIOD 1024
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
/* Symbol-boundary crossfade, in samples (see mfsk_synth).  Must not exceed
 * the guard interval (MFSK_GI * MFSK_NFFT = 64), which the receiver drops. */
#define MFSK_XFADE    32
/* SNR3k from per-bin energies: one bin is MFSK_FS/MFSK_NFFT = 31.25 Hz wide,
 * the reference band 3 kHz, so 10*log10(31.25/3000) = -19.82 dB.  The
 * correction is fitted against codec2's ch: test-TX frames at SNR3k +9.0,
 * +4.0, -1.0, -5.2, -8.0, -10.0, -11.0, -12.0 dB read 1.1..1.4 dB high,
 * linear over the range, frame-to-frame spread ~0.1 dB. */
#define MFSK_SNR_BIN_TO_3K_DB  (-19.82)
#ifndef MFSK_SNR_CAL_DB
#define MFSK_SNR_CAL_DB        (-1.3)
#endif
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
/* At half the spacing the same +/-94 Hz takes twice the hypotheses. */
#define MFSK_FREQ_HYPS_MAX 25

#define MFSK_LPF_TAPS 63
/* The 500 Hz modes' payload decoder filters to their tones (see mfsk_be_open). */
#define MFSK_LPF_NARROW_FC   400.0
#define MFSK_LPF_NARROW_TAPS 191
#define MFSK_LPF_TAPS_MAX    MFSK_LPF_NARROW_TAPS
#define MFSK_LPF_FC   1000.0

/* Sample-clock error the payload decoder searches, as receiver samples per
 * transmitted sample: 1 +/- MFSK_RATE_MAX in MFSK_RATE_STEP steps.
 *
 * The burst is read symbol by symbol at the nominal spacing, so a clock
 * error walks the FFT window off its symbols: past about +/-0.2 % the last
 * symbols of a 13 s burst sit a whole guard interval out and no frame
 * decodes.  A sound card is normally within 100 ppm, but a station's audio
 * path need not be: on the bench both sBitx stations' ALSA loopback ran 1.5 %
 * slow (radiod's RTP path did not), so one station on RTP and one on the
 * loopback lost every MFSK frame both ways, while the OFDM modes, which track
 * timing, carried on.  Acquisition still finds the burst -- each preamble
 * symbol is correlated on its own, and the shift of 1500 Hz the clock causes
 * (23 Hz at 1.5 %) is just another dial offset to the frequency search -- so
 * only the payload needs the ratio, and it has hundreds of symbols to measure
 * it with.  (At 1.5 % the floor still needs about 1 dB more than on a good
 * clock, most of it the frequency grid's: 23 Hz is midway between two
 * hypotheses, and a dial 23 Hz off costs the same.)
 * The step is under the +/-0.2 % a payload tolerates, and a finer pass
 * halves it. */
#define MFSK_RATE_MAX  0.02
#define MFSK_RATE_STEP 0.0025

typedef struct {
    int mode;
    int nfft;                       /* MFSK_NFFT, or 512 for 15.625 Hz tones */
    int fcg;                        /* carrier table: 0 = MFSK_FC, 1 = the 512-point one */
    int nhyp, hyp_lo;               /* frequency hypotheses, in half-bins */
    double snr_bin_to_3k_db;        /* one bin's width against 3 kHz */
    mfsk_t m;
    ofdm_frame_t o;
    const mfsk_ldpc_code_t *code;   /* rate 8/16 */
    mfsk_ldpc_ws_t *ldpc;           /* this decoder's own LDPC workspace */
    int Nofdm, P, bps, NPAY;
    size_t frame_bytes;             /* K/8 (payload + CRC16) */

    double lpf[MFSK_LPF_TAPS_MAX];
    int    lpf_taps;
    long tx_n;                      /* TX carrier phase counter (per burst) */
    double complex tx_tail[MFSK_XFADE]; /* last symbol's faded-out suffix */
    int      tx_have_tail;          /* ...to add to the next symbol's head */

    /* RX sliding window (int16 passband) */
    int16_t *rxbuf;
    int      rxlen, rxcap;
    int      rxhold;                /* what the window keeps when it slides */
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
    double complex *preT_hyp[MFSK_FREQ_HYPS_MAX];
    int             freq_hb;        /* latched hypothesis, in HALF subcarriers */
    int             freq_locked;    /* a decode confirmed freq_hb            */
    /* Receiver samples per transmitted sample (see MFSK_RATE_MAX): the last
     * frame's, which the next burst is read at first, 1.0 until one is. */
    double          rate;
    /* The decode last attempted (mfsk_decode_at): its anchor (an absolute
     * index, -1 for none), sample ratio and dial offset in half subcarriers.
     * Its LLRs are in llr_di and its SNR sums in dm_snr_s/_n. */
    long            dm_key;
    double          dm_rate, dm_hb, dm_snr_s, dm_snr_n;
    long            pst_tried_abs;  /* furthest postamble whose payload was tried */
    /* The last frame's offset on air, Hz (positive: above nominal), for
     * get_freq_offset. */
    float           foff_hz;
    int             foff_valid;
    double          preE[MFSK_MAX_PREAMBLE_SYMB], pstE[MFSK_MAX_PREAMBLE_SYMB];
    int             preN, pstN;

    long  tried_abs;       /* anchor whose payload we have already decoded once */
    long  reject_abs;      /* furthest anchor whose resident payload failed CRC;
                            * the next search restarts past it so one false peak
                            * cannot mask the real burst behind it */
    long  anchor_abs;      /* absolute sample index of a located preamble, or
                            * -1: lets a burst be tracked without re-correlating
                            * the whole window on every attempt */
    long  carrier_abs;     /* a preamble whose payload is still arriving, or -1 */
    long  pre_scan_abs;    /* preamble offsets below this were searched: none, or -1 */
    long  pst_scan_abs;    /* postamble offsets below this were searched: none, or -1 */
    long  carrier_scan_abs;/* carrier sense has searched up to here, or -1 */
    int   last_sync;
    float last_snr;

    /* Carousel session seed, as freedv_set_crc_seed: every frame's CRC16 is
     * XORed with it, so an earlier or foreign session's frames fail. */
    uint16_t crc_seed;
    int      crc_accept_plain;  /* also accept an unseeded CRC */
    int      rx_seeded;         /* the last frame matched the seed */
} mfsk_modem_t;

/* Windowed-sinc low-pass (matches the harness). */
static void mklpf_n(double *lpf, double fc, int taps)
{
    double s = 0.0;
    for (int i = 0; i < taps; i++)
    {
        int k = i - taps / 2;
        double h = (k == 0) ? 2.0 * M_PI * fc / MFSK_FS
                            : sin(2.0 * M_PI * fc / MFSK_FS * k) / (M_PI * k);
        h *= 0.54 - 0.46 * cos(2.0 * M_PI * i / (taps - 1));
        lpf[i] = h; s += h;
    }
    for (int i = 0; i < taps; i++) lpf[i] /= s;
}

static void mklpf(double *lpf, double fc) { mklpf_n(lpf, fc, MFSK_LPF_TAPS); }

/* The carrier phasor e^(j*2*pi*fc*n/MFSK_FS), one period of it, for the
 * 256-point geometry (g 0) and the 512-point one (g 1).  Either way the tones
 * are centred on 1500 Hz: fc sits half a bin below it. */
static double complex g_fc_ph[2][MFSK_FC_PERIOD];
static pthread_once_t g_fc_once = PTHREAD_ONCE_INIT;

static void fc_table_init(void)
{
    for (int g = 0; g < 2; g++)
    {
        double fc = 1500.0 - 0.5 * MFSK_FS / (double)(MFSK_NFFT << g);
        for (int n = 0; n < MFSK_FC_PERIOD; n++)
        {
            double ph = 2.0 * M_PI * fc / MFSK_FS * (double)n;
            g_fc_ph[g][n] = cos(ph) + I * sin(ph);
        }
    }
}

/* n is an absolute sample index (never negative). */
static inline double complex fc_phasor_g(int g, long n)
{
    return g_fc_ph[g][n & (MFSK_FC_PERIOD - 1)];
}
static inline double complex fc_phasor(long n) { return fc_phasor_g(0, n); }

/* ---- lifecycle -------------------------------------------------------- */

static void mfsk_be_close(void *ctx);

static void *mfsk_be_open(int mode)
{
    mfsk_modem_t *h = (mfsk_modem_t *)calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->mode = mode;
    int M = MFSK_M;
    h->nfft = MFSK_NFFT;
    if (mode == MERCURY_MODE_MFSK_N16) M = 16;
    if (mode == MERCURY_MODE_MFSK_N32) h->nfft = 2 * MFSK_NFFT;
    h->fcg = h->nfft == MFSK_NFFT ? 0 : 1;
    h->hyp_lo = MFSK_FREQ_HYP_LO * (h->nfft / MFSK_NFFT);
    h->nhyp = 1 - 2 * h->hyp_lo;
    h->snr_bin_to_3k_db = MFSK_SNR_BIN_TO_3K_DB - 10.0 * log10((double)h->nfft / MFSK_NFFT);
    mfsk_init(&h->m, M, MFSK_NCAR, 1);
    ofdm_frame_init(&h->o, h->nfft, MFSK_NCAR, MFSK_GI, 0);
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
    pthread_once(&g_fc_once, fc_table_init);
    /* The 500 Hz modes filter to their tones.  The sync statistic divides
     * by the energy the filter passes (mfsk_sync_search), so noise outside the
     * tones costs acquisition: at 15.625 Hz spacing, where the code needs 3 dB
     * less than the floor's, acquisition was the limit, and passing 2 kHz of
     * noise for 500 Hz of tones cost 2.5 dB (AWGN 50 % at -12.3 dB SNR3k, -14.6
     * filtered to 360 Hz).  400 Hz keeps the outer tones of a burst at the
     * edge of the +/-94 Hz dial search, and 191 taps keep the band edge sharp:
     * it measured as well as 360 Hz over 127 taps on frequency, and better
     * (37/40 against 31/40 at 500 Hz and 0/40 unfiltered, -13 dB) 90 Hz off.
     * A narrower band raises what noise alone scores; 360 Hz still gave 0
     * false syncs in 1296 s of noise. */
    h->lpf_taps = MFSK_LPF_TAPS;
    if (h->m.M * MFSK_FS / h->nfft <= 500.0)
    {
        h->lpf_taps = MFSK_LPF_NARROW_TAPS;
        mklpf_n(h->lpf, MFSK_LPF_NARROW_FC, h->lpf_taps);
    }
    else
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
    /* The window keeps two bursts and a margin (rxhold).  It slides by
     * memmove, three buffers of it -- at 2 x 3.6 MB of complex doubles, 25
     * times a second, ~190 MB/s, a quarter of a Pi 4 core spent copying while
     * idle.  So it is allocated a burst larger, and slides only when that
     * slack is used up: about once per burst instead of once per chunk.  The
     * searches resume where they stopped, so the longer window costs them
     * nothing. */
    h->rxhold = (2 * (h->P + h->NPAY) + h->P + 8) * h->Nofdm;
    h->rxcap  = h->rxhold + (h->P + h->NPAY) * h->Nofdm;
    h->rxbuf = (int16_t *)malloc((size_t)h->rxcap * sizeof(int16_t));
    h->bf    = (double complex *)malloc((size_t)h->rxcap * sizeof(double complex));
    h->bb    = (double complex *)malloc((size_t)h->rxcap * sizeof(double complex));
    h->bb_len = h->bf_len = 0;
    h->n_abs  = 0;
    h->anchor_abs = -1;
    h->tried_abs  = -1;
    h->reject_abs = -1;
    h->carrier_abs = h->carrier_scan_abs = -1;
    h->pre_scan_abs = h->pst_scan_abs = -1;
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
    h->ldpc  = mfsk_ldpc_ws_new();
    h->preT  = (double complex *)malloc((size_t)h->P * h->Nofdm * sizeof(double complex));
    h->pstT  = (double complex *)malloc((size_t)h->P * h->Nofdm * sizeof(double complex));
    if (!h->rxbuf || !h->bf || !h->rb || !h->llr || !h->llr_di || !h->llr_acc || !h->ilv ||
        !h->info || !h->ldpc || !h->preT || !h->pstT)
    {
        free(h->rxbuf); free(h->bf); free(h->rb); free(h->llr); free(h->llr_di); free(h->llr_acc);
        free(h->ilv); free(h->info); mfsk_ldpc_ws_free(h->ldpc); free(h->preT); free(h->pstT); free(h);
        return NULL;
    }
    mfsk_interleave_init(h->ilv, h->nb);
    h->preN = mfsk_sync_build_template(&h->m, &h->o, h->preT, h->preE);
    /* One template per hypothesis: rotating the TEMPLATE by +k bins matches a
     * received signal shifted by +k bins, and costs nothing at run time beyond
     * the extra correlations. Rotation is continuous across the whole template
     * because the received burst is. */
    for (int hy = 0; hy < h->nhyp; hy++)
    {
        int hb = h->hyp_lo + hy;          /* half-bins */
        h->preT_hyp[hy] = (double complex *)malloc((size_t)h->preN * h->Nofdm *
                                                   sizeof(double complex));
        if (!h->preT_hyp[hy]) { mfsk_be_close(h); return NULL; }
        for (int n = 0; n < h->preN * h->Nofdm; n++)
        {
            double ph = M_PI * (double)hb * (double)n / (double)h->nfft;
            h->preT_hyp[hy][n] = h->preT[n] * (cos(ph) + I * sin(ph));
        }
    }
    h->freq_hb = 0;
    h->freq_locked = 0;
    h->rate = 1.0;
    h->dm_key = -1;
    h->pst_tried_abs = -1;
    h->pstN = mfsk_sync_build_postamble_template(&h->m, &h->o, h->pstT, h->pstE);
    return h;
}

static void mfsk_be_close(void *ctx)
{
    mfsk_modem_t *h = (mfsk_modem_t *)ctx;
    if (!h) return;
    /* bb was never freed here: it is rxcap complex doubles (~3.4 MB), leaked on
     * every open/close cycle. */
    for (int hy = 0; hy < h->nhyp; hy++) free(h->preT_hyp[hy]);
    free(h->rxbuf); free(h->bf); free(h->bb); free(h->rb);
    free(h->llr); free(h->llr_di); free(h->llr_acc); free(h->ilv);
    free(h->info); mfsk_ldpc_ws_free(h->ldpc); free(h->preT); free(h->pstT); free(h);
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
    double spacing = MFSK_FS / (double)((mfsk_modem_t *)ctx)->nfft;
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

/* Synthesize nsym freq-domain symbols as int16 passband.
 *
 * Each symbol is one IFFT (a tone per lit bin) behind its cyclic prefix.  Laid
 * end to end, every symbol boundary is a jump in phase and frequency -- a
 * click -- and a rectangular tone's spectrum falls off only as 1/f: measured
 * on this output, -27..-34 dB out of band against -50..-66 dB for codec2's
 * modes, and heard on air as spikes across the spectrum.  So the boundaries
 * are crossfaded (windowed overlap-add): a symbol continues cyclically for
 * MFSK_XFADE samples past its end, fading out under a raised cosine, while the
 * next fades in over the same samples.  Both sit inside the next symbol's
 * guard interval, which the receiver discards, so its FFT window sees the
 * symbol exactly as before.  A burst fades in at its start and, when last is
 * set, out over the end of its final symbol.
 *
 * *tx_n is the carrier phase counter (absolute sample index in the burst);
 * tail/have_tail carry the pending fade-out across calls, so preamble, data
 * and postamble join as smoothly as symbols do. */
static int mfsk_synth(const ofdm_frame_t *o, int g, int nofdm, const mfsk_cplx *syms, int nsym,
                      int16_t *out, long *tx_n, double complex *tail, int *have_tail, int last)
{
    static double rise[MFSK_XFADE];
    static int rise_init;
    if (!rise_init) {
        for (int j = 0; j < MFSK_XFADE; j++)
            rise[j] = 0.5 * (1.0 - cos(M_PI * (j + 0.5) / MFSK_XFADE));
        rise_init = 1;
    }
    int written = 0;
    for (int s = 0; s < nsym; s++)
    {
        double complex bins[MFSK_NCAR], pad[MFSK_NFFT_MAX], t[MFSK_NFFT_MAX], cp[MFSK_NFFT_MAX * 2];
        for (int k = 0; k < MFSK_NCAR; k++)
            bins[k] = syms[s * MFSK_NCAR + k].re + syms[s * MFSK_NCAR + k].im * I;
        ofdm_zero_padder(o, bins, pad);
        ofdm_ifft(o, pad, t);
        ofdm_gi_adder(o, t, cp);
        for (int j = 0; j < MFSK_XFADE; j++)
            cp[j] = cp[j] * rise[j] + (*have_tail ? tail[j] : 0);
        if (last && s == nsym - 1)
            for (int j = 0; j < MFSK_XFADE; j++)
                cp[nofdm - MFSK_XFADE + j] *= rise[MFSK_XFADE - 1 - j];
        /* The cyclic continuation past the symbol's end is its body's start. */
        for (int j = 0; j < MFSK_XFADE; j++)
            tail[j] = t[j] * rise[MFSK_XFADE - 1 - j];
        *have_tail = !(last && s == nsym - 1);
        for (int n = 0; n < nofdm; n++)
        {
            double complex p = fc_phasor_g(g, (*tx_n)++);
            double v = MFSK_TXAMP * (creal(cp[n]) * creal(p) + cimag(cp[n]) * cimag(p));
            if (v > 32767.0) v = 32767.0; else if (v < -32768.0) v = -32768.0;
            out[written++] = (int16_t)lrint(v);
        }
    }
    return written;
}

static int mfsk_emit(mfsk_modem_t *h, const mfsk_cplx *syms, int nsym, int16_t *out, int last)
{
    return mfsk_synth(&h->o, h->fcg, h->Nofdm, syms, nsym, out, &h->tx_n,
                      h->tx_tail, &h->tx_have_tail, last);
}

static int mfsk_be_preamble_tx(void *ctx, int16_t *out)
{
    mfsk_modem_t *h = ctx;
    h->tx_n = 0;   /* preamble is always first in a burst */
    h->tx_have_tail = 0;
    mfsk_cplx *pre = (mfsk_cplx *)calloc((size_t)h->P * MFSK_NCAR, sizeof(mfsk_cplx));
    if (!pre) return 0;
    mfsk_generate_preamble(&h->m, pre, h->P);
    int n = mfsk_emit(h, pre, h->P, out, 0);
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
    int n = mfsk_emit(h, dat, h->NPAY, out, 0);
    free(dat); free(cbits);
    return n;
}

static int mfsk_be_postamble_tx(void *ctx, int16_t *out)
{
    mfsk_modem_t *h = ctx;
    mfsk_cplx *pst = (mfsk_cplx *)calloc((size_t)h->P * MFSK_NCAR, sizeof(mfsk_cplx));
    if (!pst) return 0;
    mfsk_generate_postamble(&h->m, pst, h->P);
    int n = mfsk_emit(h, pst, h->P, out, 1);
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
        h->bb[i] = 2.0 * (double)h->rxbuf[i] * fc_phasor_g(h->fcg, h->n_abs + (long)i);
    h->bb_len = h->rxlen;

    /* bf[i] needs bb up to i + TAPS/2, so it is final only once that has
     * arrived.  Everything below bf_len is already final and is never
     * recomputed. */
    int last = h->bb_len - h->lpf_taps / 2;
    if (last > h->rxlen) last = h->rxlen;
    for (int i = h->bf_len; i < last; i++)
    {
        double complex a = 0;
        for (int k = 0; k < h->lpf_taps; k++)
        {
            int j = i - k + h->lpf_taps / 2;
            if (j >= 0 && j < h->bb_len) a += h->lpf[k] * h->bb[j];
        }
        h->bf[i] = a;
    }
    if (last > h->bf_len) h->bf_len = last;
}

/* bf at a fractional index: 4-point Lagrange interpolation.  The baseband is
 * filtered to 1 kHz or less at 8 kHz, so it is smooth enough at that order;
 * x - 1 and x + 2 must be in bf. */
static inline double complex bf_at(const mfsk_modem_t *h, double x)
{
    long i = (long)floor(x);
    double t = x - (double)i;
    const double complex *p = &h->bf[i - 1];
    if (t == 0.0) return p[1];
    return -t * (t - 1.0) * (t - 2.0) / 6.0 * p[0]
         + (t + 1.0) * (t - 1.0) * (t - 2.0) / 2.0 * p[1]
         - (t + 1.0) * t * (t - 2.0) / 2.0 * p[2]
         + (t + 1.0) * t * (t - 1.0) / 6.0 * p[3];
}

/* One symbol's depadded bins, read from bf at pos onwards, one sample every
 * `rate`, with a dial offset of hb half subcarriers undone so the tones land
 * back on their nominal bins.  The phase restarts each symbol, which is
 * harmless: the demod is non-coherent, only the frequency matters. */
static void mfsk_sym_bins(const mfsk_modem_t *h, double pos, double rate, double hb,
                          double complex *dep)
{
    double complex rmv[MFSK_NFFT_MAX], ff[MFSK_NFFT_MAX], sym[MFSK_NFFT_MAX * 2];
    const double complex *src = sym;
    double ph = -M_PI * hb / (double)h->nfft;
    double complex w = cos(ph) + I * sin(ph), p = 1.0;
    if (rate == 1.0 && pos == floor(pos))
    {
        int b = (int)pos;
        if (hb == 0.0)
            src = &h->bf[b];
        else
            for (int n = 0; n < h->Nofdm; n++, p *= w)
                sym[n] = h->bf[b + n] * p;
    }
    else
        for (int n = 0; n < h->Nofdm; n++, p *= w)
            sym[n] = bf_at(h, pos + (double)n * rate) * p;
    ofdm_gi_remover(&h->o, src, rmv);
    ofdm_fft(&h->o, rmv, ff);
    ofdm_zero_depadder(&h->o, ff, dep);
}

/* Where the payload starts at sample ratio `rate`.  It is anchored ref
 * symbols from payoff -- the preamble's start (ref P) or the postamble's
 * (ref -NPAY) -- and payoff is where it starts at the nominal rate, so the
 * burst stretches or shrinks about the anchor. */
static double mfsk_pay_start(const mfsk_modem_t *h, int payoff, int ref, double rate)
{
    return (double)payoff + (double)ref * h->Nofdm * (rate - 1.0);
}

/* Is the whole payload, read from start at `rate`, in bf (with the
 * interpolator's reach either side)? */
static int mfsk_pay_fits(const mfsk_modem_t *h, double start, double rate)
{
    return start >= 1.0 &&
           start + (double)(h->NPAY * h->Nofdm - 1) * rate + 2.0 < (double)h->bf_len;
}

/* Can every ratio be tried, i.e. is the payload in at the slowest clock? */
static int mfsk_pay_ready(const mfsk_modem_t *h, int payoff, int ref)
{
    double r = 1.0 + MFSK_RATE_MAX;
    double end = mfsk_pay_start(h, payoff, ref, r) + (double)(h->NPAY * h->Nofdm - 1) * r + 2.0;
    return end < (double)h->bf_len;
}

/* The payload's strongest tone per stream and symbol, summed, over every
 * `every`-th symbol: how well a ratio and offset line the tones up with the
 * bins. */
static double mfsk_pay_energy(const mfsk_modem_t *h, double start, double rate, double hb,
                              int every)
{
    double e = 0.0;
    for (int s = 0; s < h->NPAY; s += every)
    {
        double complex dep[MFSK_NCAR];
        mfsk_sym_bins(h, start + (double)s * h->Nofdm * rate, rate, hb, dep);
        for (int st = 0; st < h->m.nStreams; st++)
        {
            double emax = 0.0;
            for (int t = 0; t < h->m.M; t++)
            {
                double complex v = dep[h->m.stream_offsets[st] + t];
                double en = creal(v * conj(v));
                if (en > emax) emax = en;
            }
            e += emax;
        }
    }
    return e;
}

/* The dial offset to an eighth of a subcarrier.
 *
 * Acquisition picks among hypotheses half a subcarrier apart, which leaves up
 * to a quarter of one between the burst and the bins it is read on.  That
 * quarter costs a lot: the tone's energy leaks into its neighbours, and at
 * SNR3k -14 dB the 15.625 Hz geometry delivered 37/40 bursts on a hypothesis
 * and 14/40 between two (the 31.25 Hz one loses the same way at its own floor).
 * So the payload is read at the hypothesis and at a quarter and an eighth of
 * a subcarrier either side, and the offset whose strongest tones carry the
 * most energy over the whole payload wins: hundreds of symbols choose it
 * reliably where the preamble's few could not. */
static double mfsk_refine_hb(const mfsk_modem_t *h, double start, double rate, double hb0)
{
    double best_e = -1.0, best_hb = hb0;
    for (int k = -2; k <= 2; k++)
    {
        double hb = hb0 + 0.25 * k;
        double e = mfsk_pay_energy(h, start, rate, hb, 1);
        if (e > best_e) { best_e = e; best_hb = hb; }
    }
    return best_hb;
}

/* The sample ratio, from the payload: every MFSK_RATE_STEP over +/-
 * MFSK_RATE_MAX on every other symbol, then half a step either side of the
 * best, and the ratio last decoded at, on all of them.  The frequency stays
 * acquisition's: a clock error shifts 1500 Hz by far more than it spreads
 * the tones about it, and that shift is what acquisition found. */
static double mfsk_search_rate(const mfsk_modem_t *h, int payoff, int ref, long key)
{
    int K = (int)lround(MFSK_RATE_MAX / MFSK_RATE_STEP);
    double best_e = -1.0, best = 1.0;
    for (int k = -K; k <= K; k++)
    {
        double r = 1.0 + k * MFSK_RATE_STEP, st = mfsk_pay_start(h, payoff, ref, r);
        if (!mfsk_pay_fits(h, st, r)) continue;
        double e = mfsk_pay_energy(h, st, r, h->freq_hb * r, 2);
        if (e > best_e) { best_e = e; best = r; }
    }
    double cand[4] = { best - 0.5 * MFSK_RATE_STEP, best, best + 0.5 * MFSK_RATE_STEP,
                       h->dm_key == key ? h->dm_rate : best };
    best_e = -1.0;
    double pick = best;
    for (int c = 0; c < 4; c++)
    {
        double r = cand[c], st = mfsk_pay_start(h, payoff, ref, r);
        if (!mfsk_pay_fits(h, st, r)) continue;
        double e = mfsk_pay_energy(h, st, r, h->freq_hb * r, 1);
        if (e > best_e) { best_e = e; pick = r; }
    }
    return pick;
}

/* Info bits -> bytes (MSB-first, matching TX), then the CRC16 over the
 * payload (all but the last 2) against its trailer -- FreeDV's contract. */
static int mfsk_frame_crc_ok(mfsk_modem_t *h, const int *bits, uint8_t *bytes_out)
{
    size_t nbytes = h->frame_bytes;
    for (size_t by = 0; by < nbytes; by++)
    {
        uint8_t v = 0;
        for (int bit = 0; bit < 8; bit++)
            v = (uint8_t)((v << 1) | (bits[by * 8 + bit] & 1));
        bytes_out[by] = v;
    }
    uint16_t crc = freedv_gen_crc16(bytes_out, (int)(nbytes - 2));
    uint16_t got = (uint16_t)((bytes_out[nbytes - 2] << 8) | bytes_out[nbytes - 1]);
    if (h->crc_seed && (uint16_t)(crc ^ h->crc_seed) == got)
        h->rx_seeded = 1;
    else if ((!h->crc_seed || h->crc_accept_plain) && crc == got)
        h->rx_seeded = 0;
    else
        return 0;
    return 1;
}

/* Demod and decode the payload read from start at `rate`, the dial offset
 * refined about acquisition's.  Leaves the LLRs in llr_di and what it did in
 * dm_*; returns whether the frame passed its CRC (bytes_out filled). */
static int mfsk_decode_at(mfsk_modem_t *h, long key, double start, double rate,
                          uint8_t *bytes_out)
{
    double hb = mfsk_refine_hb(h, start, rate, h->freq_hb * rate);

    /* SNR: per symbol, the strongest tone bin of each stream (signal + noise)
     * against the mean of the bins outside every stream's band (noise). */
    double snr_s = 0.0, snr_n = 0.0;
    int band_lo = h->m.stream_offsets[0];
    int band_hi = h->m.stream_offsets[h->m.nStreams - 1] + h->m.M;
    for (int s = 0; s < h->NPAY; s++)
    {
        double complex dep[MFSK_NCAR];
        mfsk_sym_bins(h, start + (double)s * h->Nofdm * rate, rate, hb, dep);
        for (int k = 0; k < MFSK_NCAR; k++)
        {
            h->rb[s * MFSK_NCAR + k].re = creal(dep[k]);
            h->rb[s * MFSK_NCAR + k].im = cimag(dep[k]);
        }
        double nsum = 0.0;
        int nbins = 0;
        for (int k = 0; k < MFSK_NCAR; k++)
            if (k < band_lo || k >= band_hi) { nsum += creal(dep[k] * conj(dep[k])); nbins++; }
        double nbin = nbins ? nsum / nbins : 0.0;
        for (int st = 0; st < h->m.nStreams; st++)
        {
            double emax = 0.0;
            for (int t = 0; t < h->m.M; t++)
            {
                double complex v = dep[h->m.stream_offsets[st] + t];
                double e = creal(v * conj(v));
                if (e > emax) emax = e;
            }
            snr_s += emax - nbin;
            snr_n += nbin;
        }
    }
    mfsk_demod(&h->m, h->rb, h->nb, h->llr);
    for (int t = 0; t < h->nb; t++)
    {
        int j = h->ilv[t];
        if (j < h->code->N) h->llr_di[j] = h->llr[t];   /* undo the scatter */
    }
    h->dm_key = key;
    h->dm_rate = rate;
    h->dm_hb = hb;
    h->dm_snr_s = snr_s;
    h->dm_snr_n = snr_n;

    /* The CRC decides, not the LDPC's parity check: an unconverged decode
     * can still carry the right info bits. */
    int out_bits[MFSK_LDPC_MAXK];   /* not static: decoders run side by side */
    (void)mfsk_ldpc_decode_ws(h->ldpc, h->code, h->llr_di, out_bits, 50);
    return mfsk_frame_crc_ok(h, out_bits, bytes_out);
}

/* Demod one MFSK burst whose payload starts at bf[payoff] at the nominal
 * rate, anchored ref symbols from there (mfsk_pay_start), decode LDPC, verify
 * CRC16.  Returns frame_bytes on success (bytes_out filled), else 0.
 *
 * It is read first at the sample ratio the last frame came at, as soon as
 * that much of it is in, so a frame from a well-clocked peer costs what it
 * always did.  Only if that fails, once the burst is in at the slowest ratio
 * searched (mfsk_pay_ready -- the caller treats the anchor as settled from
 * then on), is the ratio searched and the frame read again at the best one
 * if that differs. */
static int mfsk_try_payload(mfsk_modem_t *h, int payoff, int ref, uint8_t *bytes_out)
{
    long key = h->n_abs + (long)payoff;
    int pass = 0;
    if (h->dm_key != key)
    {
        double st = mfsk_pay_start(h, payoff, ref, h->rate);
        if (!mfsk_pay_fits(h, st, h->rate))
            return 0;
        pass = mfsk_decode_at(h, key, st, h->rate, bytes_out);
    }
    if (!pass)
    {
        if (!mfsk_pay_ready(h, payoff, ref))
            return 0;
        double r = mfsk_search_rate(h, payoff, ref, key);
        if (h->dm_key != key || r != h->dm_rate)
            pass = mfsk_decode_at(h, key, mfsk_pay_start(h, payoff, ref, r), r, bytes_out);
    }

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
     * outage here (issue #223 lineage), so it is deliberate.  The copy summed
     * is the one read at the best sample ratio found (llr_di, dm_*).
     */
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
        if (!pass && h->harq_copies > 1)
        {
            /* Average, not raw sum: the min-sum decoder is scale-sensitive,
             * so a growing magnitude changes its behaviour as copies pile up. */
            float inv = 1.0f / (float)h->harq_copies;
            for (int i = 0; i < h->code->N; i++) h->llr_di[i] = h->llr_acc[i] * inv;
            int out_bits[MFSK_LDPC_MAXK];
            (void)mfsk_ldpc_decode_ws(h->ldpc, h->code, h->llr_di, out_bits, 50);
            pass = mfsk_frame_crc_ok(h, out_bits, bytes_out);
        }
        if (pass)
        {
            memset(h->llr_acc, 0, (size_t)h->code->N * sizeof(float));
            h->harq_copies = 0;
            h->harq_first_ms = 0;
        }
    }
    if (!pass)
        return 0;

    /* The SNR used to be the mean |LLR|, which saturates (it read +5 at -9 dB
     * and at +7 dB alike on the air).  Now: tone energy over bin noise,
     * summed over the streams' tones, referred to 3 kHz. */
    {
        double lin = h->dm_snr_n > 0.0 ? h->dm_snr_s / h->dm_snr_n * h->m.nStreams : 0.0;
        if (lin < 1e-4) lin = 1e-4;
        h->last_snr = (float)(10.0 * log10(lin) + h->snr_bin_to_3k_db + MFSK_SNR_CAL_DB);
    }
    /* The next burst is read at this one's ratio first.  The offset is
     * reported as it reached us: in the receiver's own samples, of the burst's
     * centre.  The spectrum is mirrored at baseband (see MFSK_FC), so a burst
     * above nominal reads as a negative hb. */
    h->rate = h->dm_rate;
    h->foff_hz = (float)(-h->dm_hb * 0.5 * MFSK_FS / (double)h->nfft / h->dm_rate + 0.0);
    h->foff_valid = 1;
    return (int)h->frame_bytes;
}

/* Carrier sense: is a burst on the air now?  The search above looks only at
 * preambles whose payload has all arrived -- nothing earlier can be decoded --
 * so on its own it notices a 13.5 s burst in its last few symbols, and the ARQ,
 * reading "no sync" as "nobody transmitting", keyed over the peer's frame (an
 * idle receiver answered its application mid-burst on air, and the two ends
 * then collided every round).  This looks where that search does not: at
 * preambles still followed by an incomplete payload.  Only the samples added
 * since the last look are new, so each look correlates a few symbols' worth,
 * and once a preamble is found it is tracked until its payload is resident,
 * where the search above takes over. */
#define CS_OVERLAP_SYMB 1
/* How far back an incremental search starts behind where the last one ended. */
#define SCAN_OVERLAP_SYMB 2
static int mfsk_carrier_sense(mfsk_modem_t *h, int search_len)
{
    int burst = (h->P + h->NPAY) * h->Nofdm;
    long end_abs = h->n_abs + h->bf_len;
    if (h->carrier_abs >= 0) {
        if (h->carrier_abs >= h->n_abs && h->carrier_abs + burst > end_abs)
            return 1;                        /* still arriving */
        h->carrier_abs = -1;                 /* resident or slid out */
    }
    /* From where the last look stopped, one symbol back so a preamble that
     * straddled it is seen whole; never below the resident-payload range. */
    int resident_end = search_len - h->P * h->Nofdm;   /* the search above ends here */
    long from_abs = h->n_abs + (resident_end > 0 ? resident_end : 0);
    if (h->carrier_scan_abs - CS_OVERLAP_SYMB * h->Nofdm > from_abs)
        from_abs = h->carrier_scan_abs - CS_OVERLAP_SYMB * h->Nofdm;
    int base = (int)(from_abs - h->n_abs);
    int len  = h->bf_len - base;
    if (len < (h->P + 1) * h->Nofdm)
        return 0;
    int best = -1;
    double best_metric = -1.0;
    for (int hy = 0; hy < h->nhyp; hy++) {
        if (h->freq_locked && hy != h->freq_hb - h->hyp_lo) continue;
        double m = 0.0;
        int o = mfsk_sync_search(h->bf + base, len, 1, h->preT_hyp[hy], h->preE,
                                 h->preN, h->Nofdm, 0, &m);
        if (o >= 0 && m > best_metric) { best_metric = m; best = o; }
    }
    /* Positions within P symbols of the end cannot be tested yet. */
    h->carrier_scan_abs = end_abs - (long)(h->P + 2) * h->Nofdm;
    if (best < 0) return 0;
    h->carrier_abs = from_abs + best;
    return h->carrier_abs + burst > end_abs;
}

static int mfsk_be_rawdata_rx(void *ctx, uint8_t *bytes_out, const int16_t *demod_in)
{
    mfsk_modem_t *h = ctx;
    int chunk = h->Nofdm;   /* == nin */

    /* Append this chunk to the sliding window (drop oldest if over capacity). */
    if (h->rxlen + chunk > h->rxcap)
    {
        int drop = h->rxlen + chunk - h->rxhold;
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

    /* Rate-limit the (heavy) downmix+search to a few times per burst. */
    int burst = (h->P + h->NPAY) * h->Nofdm;   /* preamble + data */
    if (h->since_search < 4 * h->Nofdm)
        return 0;
    h->since_search = 0;

    mfsk_downmix(h);

    /* Nothing can decode until a whole burst could be present, but a burst can
     * already be arriving: the window starts empty after every decode and on
     * open, and without this the carrier sense below stayed dark for a burst's
     * length (13 s) -- on air, a station keyed a repeated handover over the
     * peer's floor frame 1.3 s into it. */
    if (h->rxlen < burst)
    {
        h->last_sync = mfsk_carrier_sense(h, h->bf_len - h->NPAY * h->Nofdm);
        return 0;
    }

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
        /* Nor search again what an earlier look already found empty: the
         * window only grows at its end, and a correlation reads only the
         * samples under it, so an offset that held no preamble then holds
         * none now.  Searching the whole window every four symbols, on a
         * listening station that hears only noise, was most of its CPU.
         * SCAN_OVERLAP_SYMB back covers the refinement's reach and the
         * downmix filter's still-settling tail. */
        if (h->pre_scan_abs >= 0)
        {
            long rel = h->pre_scan_abs - h->n_abs - (long)SCAN_OVERLAP_SYMB * h->Nofdm;
            int s = rel > 0 ? (int)(rel / h->Nofdm) : 0;
            if (s > start_symb) start_symb = s;
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
            int hy = h->freq_hb - h->hyp_lo;
            if (hy >= 0 && hy < h->nhyp)
                off = mfsk_sync_search(h->bf, search_len, 1, h->preT_hyp[hy],
                                       h->preE, h->preN, h->Nofdm, start_symb,
                                       &metric);
            if (off < 0) h->freq_locked = 0;   /* stale lock: re-sweep below */
        }

        if (off < 0)
        {
            double best_metric = -1.0;
            int    best_off = -1, best_hb = h->freq_hb;
            for (int hy = 0; hy < h->nhyp; hy++)
            {
                double m = 0.0;
                int o = mfsk_sync_search(h->bf, search_len, 1, h->preT_hyp[hy],
                                         h->preE, h->preN, h->Nofdm,
                                         start_symb, &m);
                if (o >= 0 && m > best_metric)
                {
                    best_metric = m;
                    best_off    = o;
                    best_hb     = h->hyp_lo + hy;
                }
            }
            if (best_off >= 0)
            {
                off        = best_off;
                metric     = best_metric;
                h->freq_hb = best_hb;
            }
        }

        /* The search refines its candidate to sub-symbol precision, and can
         * refine it back onto the peak it was told to start past: a rejected
         * anchor must never be taken again, or it is tried once and then
         * sits there as "already tried" while the burst behind it goes by. */
        bool none_found = off < 0;   /* every hypothesis searched to the end */
        if (off >= 0 && h->reject_abs >= 0 && h->n_abs + (long)off <= h->reject_abs)
            off = -1;
        if (off >= 0)
            h->anchor_abs = h->n_abs + (long)off;
        else if (none_found)   /* every offset with a whole preamble in view */
            h->pre_scan_abs = h->n_abs + (long)(search_len / h->Nofdm - h->P) * h->Nofdm;
    }
    int payoff = (off >= 0) ? off + h->P * h->Nofdm : -1;
    /* Decode a given burst ONCE.  Once the anchor is fixed and the payload is
     * fully resident, the samples feeding the demod no longer change, so
     * re-running a 50-iteration LDPC decode every 4 symbols cannot produce a
     * different answer -- it just burns CPU in bursts, and a spike is exactly
     * what makes a real-time capture overrun and lose the NEXT burst.  (The
     * FIFO path blocks instead of dropping, which is why it completes transfers
     * while ALSA and PulseAudio both stall after one frame.)  "Resident"
     * means in at the slowest sample clock searched, which is when
     * mfsk_try_payload has tried everything it will. */
    int payload_resident = (payoff >= 0) && mfsk_pay_ready(h, payoff, h->P);
    int already_tried = (h->anchor_abs >= 0) && (h->tried_abs == h->anchor_abs);
    int nbytes = 0;
    if (payoff >= 0 && !already_tried)
    {
        nbytes = mfsk_try_payload(h, payoff, h->P, bytes_out);
        if ((payload_resident || nbytes > 0) && h->anchor_abs >= 0)
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
    int payload_was_resident = payload_resident;
    if (nbytes <= 0 && (off < 0 || payload_was_resident))
    {
        double pmetric = 0.0;
        int pstart = 0;
        if (h->pst_scan_abs >= 0)
        {
            long rel = h->pst_scan_abs - h->n_abs - (long)SCAN_OVERLAP_SYMB * h->Nofdm;
            pstart = rel > 0 ? (int)(rel / h->Nofdm) : 0;
        }
        int poff = mfsk_sync_search(h->bf, h->bf_len, 1, h->pstT, h->pstE,
                                    h->pstN, h->Nofdm, pstart, &pmetric);
        /* The search refines its candidate and can land back on a postamble
         * it was told to start past, as the preamble's can (above), and a
         * payload is tried once: re-reading a failed one every 4 symbols,
         * sample-clock search and all, would be most of the decoder's CPU. */
        if (poff >= 0 && h->n_abs + (long)poff <= h->pst_tried_abs)
        {
            h->pst_scan_abs = h->n_abs + (long)poff + (long)(SCAN_OVERLAP_SYMB + 1) * h->Nofdm;
        }
        else if (poff >= 0)
        {
            int ppayoff = poff - h->NPAY * h->Nofdm;
            int pn = mfsk_try_payload(h, ppayoff, -h->NPAY, bytes_out);
            h->pst_tried_abs = h->n_abs + (long)poff;
            if (pn > 0)
            {
                off = poff;
                nbytes = pn;
            }
            else   /* its payload failed, or is not in view: it will not change */
                h->pst_scan_abs = h->n_abs + (long)poff + (long)(SCAN_OVERLAP_SYMB + 1) * h->Nofdm;
        }
        else
            h->pst_scan_abs = h->n_abs + (long)(h->bf_len / h->Nofdm - h->P) * h->Nofdm;
    }

    /* Sync means "a burst is arriving": its preamble located, its payload not
     * all in yet.  The ARQ reads it as the peer being on the air.  It used to
     * stay set from a decode until the window refilled -- the early return
     * above leaves it untouched, and that is ~13 s after a burst had ended --
     * so the carousel's receiver deferred its answer to a round until the
     * sender, hearing nothing, had keyed the next one. */
    int arriving = off >= 0 && off + (h->P + h->NPAY) * h->Nofdm > h->bf_len;
    if (nbytes <= 0 && !arriving)
        arriving = mfsk_carrier_sense(h, search_len);
    h->last_sync = nbytes <= 0 && arriving;
    if (nbytes <= 0)
        return 0;
    h->carrier_abs = h->carrier_scan_abs = -1;
    h->pre_scan_abs = h->pst_scan_abs = -1;
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

static int mfsk_be_get_freq_offset(void *ctx, float *hz)
{
    mfsk_modem_t *h = ctx;
    if (!h->foff_valid) return 0;
    *hz = h->foff_hz;
    return 1;
}

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
static int          g_pat_nofdm;
static int          g_pat_fcg;      /* carrier table, as mfsk_modem_t.fcg */
static bool         g_pat_ready = false;
/* The ACK and BREAK lists in use: the session's while a carousel session
 * seeds the modem (mfsk_pattern_set_session), the global ones otherwise. */
static pthread_mutex_t g_pat_lists_mtx = PTHREAD_MUTEX_INITIALIZER;
static int          g_pat_ack[MFSK_MAX_ACK_TONES], g_pat_brk[MFSK_MAX_ACK_TONES];
static bool         g_pat_lists_set = false;
static _Atomic unsigned g_pat_gen = 1;     /* bumped on every change */

static void mfsk_pattern_init_geometry(int mode)
{
    int nfft = mode == MERCURY_MODE_MFSK_N32 ? 2 * MFSK_NFFT : MFSK_NFFT;
    mfsk_init(&g_pat_m, mode == MERCURY_MODE_MFSK_N16 ? 16 : MFSK_M, MFSK_NCAR, 1);
    ofdm_frame_init(&g_pat_o, nfft, MFSK_NCAR, MFSK_GI, 0);
    g_pat_nofdm = ofdm_frame_nofdm(&g_pat_o);
    g_pat_fcg = nfft == MFSK_NFFT ? 0 : 1;
    pthread_once(&g_fc_once, fc_table_init);
    mklpf(g_pat_lpf, MFSK_LPF_FC);
    g_pat_ready = true;
}

static void mfsk_pattern_lazy_init(void)
{
    if (g_pat_ready) return;
    mfsk_pattern_init_geometry(MERCURY_MODE_MFSK);
}

void mfsk_pattern_set_geometry(int mode)
{
    mfsk_pattern_init_geometry(mode);
    pthread_mutex_lock(&g_pat_lists_mtx);
    g_pat_lists_set = false;            /* back to this geometry's global lists */
    pthread_mutex_unlock(&g_pat_lists_mtx);
    atomic_fetch_add(&g_pat_gen, 1);
}

static void pat_lists(int *ack, int *brk)
{
    pthread_mutex_lock(&g_pat_lists_mtx);
    if (!g_pat_lists_set) {
        memcpy(g_pat_ack, g_pat_m.ack_tones, sizeof(g_pat_ack));
        memcpy(g_pat_brk, g_pat_m.break_tones, sizeof(g_pat_brk));
        g_pat_lists_set = true;
    }
    memcpy(ack, g_pat_ack, sizeof(g_pat_ack));
    memcpy(brk, g_pat_brk, sizeof(g_pat_brk));
    pthread_mutex_unlock(&g_pat_lists_mtx);
}

void mfsk_pattern_set_lists(const int *ack, const int *brk)
{
    mfsk_pattern_lazy_init();
    pthread_mutex_lock(&g_pat_lists_mtx);
    memcpy(g_pat_ack, ack ? ack : g_pat_m.ack_tones, sizeof(g_pat_ack));
    memcpy(g_pat_brk, brk ? brk : g_pat_m.break_tones, sizeof(g_pat_brk));
    g_pat_lists_set = true;
    pthread_mutex_unlock(&g_pat_lists_mtx);
    atomic_fetch_add(&g_pat_gen, 1);
}

bool mfsk_pattern_set_session(uint32_t key)
{
    mfsk_pattern_lazy_init();
    int ack[MFSK_MAX_ACK_TONES] = {0}, brk[MFSK_MAX_ACK_TONES] = {0};
    bool bound = mfsk_session_patterns(&g_pat_m, key, NULL, 0, ack, brk);
    pthread_mutex_lock(&g_pat_lists_mtx);
    memcpy(g_pat_ack, ack, sizeof(g_pat_ack));
    memcpy(g_pat_brk, brk, sizeof(g_pat_brk));
    g_pat_lists_set = true;
    pthread_mutex_unlock(&g_pat_lists_mtx);
    atomic_fetch_add(&g_pat_gen, 1);
    return bound;
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
    mfsk_t m = g_pat_m;      /* with the lists in use */
    pat_lists(m.ack_tones, m.break_tones);
    if (pattern_kind == 1)   /* ARQ_PATTERN_BREAK */
        mfsk_generate_break_pattern(&m, bins);
    else
        mfsk_generate_ack_pattern(&m, bins);

    long tx_n = 0;
    double complex tail[MFSK_XFADE];
    int have_tail = 0;
    int written = mfsk_synth(&g_pat_o, g_pat_fcg, g_pat_nofdm, bins, ns, out, &tx_n, tail, &have_tail, 1);
    free(bins);
    return written;
}

int mfsk_nav_tx(int16_t *out, int cls)
{
    mfsk_pattern_lazy_init();
    if (cls < 0) cls = 0;
    if (cls >= MFSK_NAV_CLASSES) cls = MFSK_NAV_CLASSES - 1;
    int ns = g_pat_m.ack_pattern_nsymb;
    mfsk_cplx *bins = (mfsk_cplx *)calloc((size_t)ns * MFSK_NCAR, sizeof(mfsk_cplx));
    if (!bins) return 0;
    mfsk_t m = g_pat_m;
    memcpy(m.ack_tones, mfsk_nav_tones[cls], sizeof(mfsk_nav_tones[cls]));
    mfsk_generate_ack_pattern(&m, bins);
    long tx_n = 0;
    double complex tail[MFSK_XFADE];
    int have_tail = 0;
    int written = mfsk_synth(&g_pat_o, g_pat_fcg, g_pat_nofdm, bins, ns, out, &tx_n, tail, &have_tail, 1);
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
        bb[i] = 2.0 * x * fc_phasor_g(g_pat_fcg, i);
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
    int ack[MFSK_MAX_ACK_TONES], brk[MFSK_MAX_ACK_TONES];
    pat_lists(ack, brk);
    const int *lists[2] = { ack, brk };
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

static void mfsk_be_set_crc_seed(void *ctx, uint16_t seed, int accept_plain)
{
    mfsk_modem_t *h = ctx;
    h->crc_seed = seed;
    h->crc_accept_plain = accept_plain;
}

static int mfsk_be_rx_crc_seeded(void *ctx)
{
    return ((mfsk_modem_t *)ctx)->rx_seeded;
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
    .set_crc_seed     = mfsk_be_set_crc_seed,
    .rx_crc_seeded    = mfsk_be_rx_crc_seeded,
    .get_freq_offset  = mfsk_be_get_freq_offset,
};


/* ----------------------------------------------------------------------
 * Sliding detection window -- see modem_mfsk.h for why it paces.
 * ---------------------------------------------------------------------- */
/* How long an event waits for a stronger overlapping one: two symbols. */
#define PAT_HOLD 640

int mfsk_pattern_window_events(mfsk_pattern_window_t *w, const int16_t *pcm, int n,
                               mfsk_pattern_ev_t *out, int maxev)
{
    if (!w || !pcm || n <= 0)
        return 0;
    mfsk_pattern_lazy_init();
    if (!w->det)
    {
        w->det = mfsk_stream_det_new(&g_pat_m, &g_pat_o, g_pat_m.ack_pattern_len,
                                     g_pat_m.ack_pattern_nsymb);
        if (!w->det)
            return 0;             /* no memory, no detection; never a crash */
        /* A NAV threshold a symbol above the ACK's: a session ACK or BREAK
         * at a few dB can cross-read as a NAV class at the ACK's threshold
         * (a window straddling two of its symbols matches either tone),
         * aligned a few symbols early, before the pattern itself is
         * complete -- and a false NAV is a hold of up to 32 s.  At 9 of 16
         * (pattern_probe): 0 cross-reads in 2048 clean session ACK/BREAK
         * bursts (3 at 8), 0 false headers in 10 h of noise (4 at 8), for
         * about 0.7 dB at -14 dB; 98 % detection at -12 dB still. */
        int nav_thr = g_pat_m.ack_match_threshold + 1;
        for (int k = 0; k < MFSK_NAV_CLASSES; k++)
            mfsk_stream_det_set_list(w->det, MFSK_PAT_NAV(k), mfsk_nav_tones[k], nav_thr);
        w->gen = 0;
    }
    unsigned gen = atomic_load(&g_pat_gen);
    if (w->gen != gen)
    {
        /* the session's lists changed (a session began or ended): what is in
         * the ring was scored against the old ones */
        int ack[MFSK_MAX_ACK_TONES], brk[MFSK_MAX_ACK_TONES];
        pat_lists(ack, brk);
        mfsk_stream_det_set_list(w->det, MFSK_PAT_ACK, ack, g_pat_m.ack_match_threshold);
        mfsk_stream_det_set_list(w->det, MFSK_PAT_BREAK, brk, g_pat_m.break_match_threshold);
        mfsk_stream_det_reset(w->det);
        w->gen = gen;
    }

    /* Downmix and low-pass, one sample at a time: the filter
     * mfsk_pattern_detect() applies to a whole buffer, causal here, so the
     * stream runs MFSK_LPF_TAPS/2 samples late. */
    int nout = 0;
    for (int i = 0; i < n; i++)
    {
        double complex x = 2.0 * (double)pcm[i] * fc_phasor_g(g_pat_fcg, w->n++);
        w->hist_re[w->hist_pos] = creal(x);
        w->hist_im[w->hist_pos] = cimag(x);
        double ar = 0.0, ai = 0.0;
        int h = w->hist_pos;
        for (int k = 0; k < MFSK_LPF_TAPS; k++)
        {
            ar += g_pat_lpf[k] * w->hist_re[h];
            ai += g_pat_lpf[k] * w->hist_im[h];
            h = h ? h - 1 : MFSK_LPF_TAPS - 1;
        }
        w->hist_pos = (w->hist_pos + 1) % MFSK_LPF_TAPS;
        double complex y = ar + I * ai;
        mfsk_stream_event_t ev[4];
        int nev = mfsk_stream_det_push(w->det, &y, 1, ev, 4);
        for (int e = 0; e < nev; e++)
        {
            /* Overlapping events: one burst read as more than one list.  The
             * strongest stands; a weaker one never reaches the caller. */
            long long st = ev[e].pos - MFSK_LPF_TAPS / 2, span = (long long)mfsk_pattern_max_tx_samples();
            if (w->last_score && llabs(st - w->last_start) < span && ev[e].score <= w->last_score)
                continue;
            bool drop = false;
            for (int q = 0; q < w->npend; q++)
            {
                if (llabs(st - w->pend[q].start) >= span) continue;
                if (ev[e].score <= w->pend[q].score) { drop = true; break; }
                w->pend[q] = w->pend[--w->npend];     /* the newcomer is stronger */
                q--;
            }
            if (drop || w->npend >= (int)(sizeof w->pend / sizeof w->pend[0]))
                continue;
            w->pend[w->npend].kind  = ev[e].list;
            w->pend[w->npend].start = st;
            w->pend[w->npend].score = ev[e].score;
            w->pend_due[w->npend]   = w->n + PAT_HOLD;
            w->npend++;
        }
        for (int q = 0; q < w->npend && nout < maxev; q++)
        {
            if (w->pend_due[q] > w->n) continue;
            out[nout++] = w->pend[q];
            w->last_start = w->pend[q].start;
            w->last_score = w->pend[q].score;
            w->pend[q] = w->pend[w->npend - 1];
            w->pend_due[q] = w->pend_due[w->npend - 1];
            w->npend--;
            q--;
        }
    }
    return nout;
}

long long mfsk_pattern_window_samples(const mfsk_pattern_window_t *w) { return w ? w->n : 0; }

int mfsk_pattern_window_push(mfsk_pattern_window_t *w, const int16_t *pcm, int n,
                             int *is_break)
{
    mfsk_pattern_ev_t ev[16];
    int nev = mfsk_pattern_window_events(w, pcm, n, ev, 16);
    int hit = 0, best = -1, brk = 0;
    for (int e = 0; e < nev; e++)
    {
        if (ev[e].kind != MFSK_PAT_ACK && ev[e].kind != MFSK_PAT_BREAK) continue;
        /* the higher score wins, as in mfsk_pattern_detect() */
        if (ev[e].score > best) { best = ev[e].score; brk = ev[e].kind == MFSK_PAT_BREAK; }
        hit = 1;
    }
    if (hit && is_break) *is_break = brk;
    return hit;
}

void mfsk_pattern_window_reset(mfsk_pattern_window_t *w)
{
    if (!w) return;
    if (w->det) mfsk_stream_det_reset(w->det);
    memset(w->hist_re, 0, sizeof(w->hist_re));
    memset(w->hist_im, 0, sizeof(w->hist_im));
    w->npend = 0;
    w->last_start = 0;
    w->last_score = 0;
    w->hist_pos = 0;
    w->n = 0;
}

void mfsk_pattern_window_free(mfsk_pattern_window_t *w)
{
    if (!w) return;
    mfsk_stream_det_free(w->det);
    memset(w, 0, sizeof(*w));
}
