/*
 * Polyphase FIR resampler — see resampler.h.
 *
 * Prototype filter: windowed-sinc low-pass, fc = 3400 Hz (passband flat past
 * the widest modem waveform ~2.5 kHz, stopband by the 8 kHz Nyquist of 4 kHz),
 * Hamming window, L*RESAMP_TAPS_PER_PHASE taps, normalised to unity DC gain.
 * The same prototype is the anti-imaging filter on upsample and the
 * anti-aliasing filter on downsample.
 *
 * The cutoff is normalised against the DEVICE rate (8000*L), so the filter
 * keeps the same 3400 Hz corner at every supported ratio; the tap count grows
 * with L so the impulse response spans a constant amount of TIME.
 *
 * L == 1 (an 8 kHz device: no rate change) is a pass-through — filtering there
 * would only add delay and passband droop for nothing.
 *
 * Rates that are NOT integer multiples of 8 kHz are handled by the rational
 * engine at the bottom of this file (44100 -> 8000 is 80/441).  It exists
 * because Windows and macOS have no equivalent of ALSA's plug layer: a card
 * left at 44.1 kHz in the OS sound settings could not be used at all, and
 * failed with an error suggesting an ALSA device string (issue #193).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "resampler.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FILT_FC    3400.0

/* h_up[p][t] = L * proto[p + L*t] — polyphase subfilter for output phase p.
 * h_down[k]  = proto[k]           — flat prototype for decimation.
 * The two directions are independent: playback and capture can negotiate
 * different device rates, so each keeps its own ratio and table. */
static float h_up[RESAMP_L_MAX][RESAMP_TAPS_PER_PHASE];
static float h_down[RESAMP_NTAPS_MAX];
static int   g_l_up   = 0;    /* 0 = not built yet */
static int   g_l_down = 0;

static inline int32_t clamp_i32(double v)
{
    if (v >  2147483647.0) return  2147483647;
    if (v < -2147483648.0) return (int32_t)(-2147483648.0);
    return (int32_t)v;
}

int resampler_ratio_for_rate(int device_rate_hz)
{
    if (device_rate_hz <= 0)
        return 0;
    if (device_rate_hz % RESAMP_MODEM_FS != 0)
        return 0;                       /* 44100 and friends */
    int L = device_rate_hz / RESAMP_MODEM_FS;
    if (L < 1 || L > RESAMP_L_MAX)
        return 0;
    return L;
}

/* Windowed-sinc prototype for ratio L, normalised to unity DC gain.
 * proto must hold L*RESAMP_TAPS_PER_PHASE doubles. */
static void build_proto(int L, double *proto)
{
    const int    N   = L * RESAMP_TAPS_PER_PHASE;
    const double fs  = (double)RESAMP_MODEM_FS * (double)L;
    const double wc  = 2.0 * FILT_FC / fs;    /* normalised cutoff (xNyquist) */
    const double mid = (N - 1) / 2.0;
    double sum = 0.0;

    for (int n = 0; n < N; n++) {
        double x = n - mid;
        double sinc = (fabs(x) < 1e-9) ? wc
                                       : sin(M_PI * wc * x) / (M_PI * x);
        double ham = 0.54 - 0.46 * cos(2.0 * M_PI * n / (N - 1));
        proto[n] = sinc * ham;
        sum += proto[n];
    }
    for (int n = 0; n < N; n++)
        proto[n] /= sum;
}

void resampler_init_up(int L)
{
    if (L < 1 || L > RESAMP_L_MAX) return;
    if (L == 1) { g_l_up = 1; return; }      /* pass-through, no table */

    double proto[RESAMP_NTAPS_MAX];
    const int N = L * RESAMP_TAPS_PER_PHASE;
    build_proto(L, proto);

    /* Polyphase decomposition for the interpolator.  Output sample
     * (i*L + p) = L * sum_t proto[p + L*t] * x[i - t].  The L gain
     * compensates the zero-stuffing energy loss; fold it into the table. */
    for (int p = 0; p < L; p++)
        for (int t = 0; t < RESAMP_TAPS_PER_PHASE; t++) {
            int idx = p + L * t;
            h_up[p][t] = (idx < N) ? (float)(L * proto[idx]) : 0.0f;
        }
    g_l_up = L;
}

void resampler_init_down(int L)
{
    if (L < 1 || L > RESAMP_L_MAX) return;
    if (L == 1) { g_l_down = 1; return; }    /* pass-through, no table */

    double proto[RESAMP_NTAPS_MAX];
    const int N = L * RESAMP_TAPS_PER_PHASE;
    build_proto(L, proto);

    for (int n = 0; n < N; n++)
        h_down[n] = (float)proto[n];
    g_l_down = L;
}

void resampler_global_init(void)
{
    resampler_init_up(RESAMP_L);
    resampler_init_down(RESAMP_L);
}

/* ---------------- Upsampler 8k -> 8000*L ---------------- */

void resamp_up_reset(resamp_up_t *r)
{
    memset(r->hist, 0, sizeof(r->hist));
    r->w = RESAMP_TAPS_PER_PHASE - 1;
}

int resamp_up_process(resamp_up_t *r, const int32_t *in, int n_in, int32_t *out)
{
    if (!g_l_up) resampler_init_up(RESAMP_L);
    const int L = g_l_up;

    if (L == 1) {                       /* device already at the modem rate */
        memcpy(out, in, (size_t)n_in * sizeof(int32_t));
        return n_in;
    }

    const int K = RESAMP_TAPS_PER_PHASE;
    int o = 0;
    for (int i = 0; i < n_in; i++) {
        /* Advance the circular head and write the sample at both mirrors. */
        if (++r->w >= K) r->w = 0;
        r->hist[r->w]     = in[i];
        r->hist[r->w + K] = in[i];

        /* hist_logical[t] (t=0 newest) == hist[w + K - t]: a contiguous run,
         * walked in the SAME order as the old shift version so the
         * accumulation is bit-identical. */
        const int32_t *win = &r->hist[r->w + K];
        for (int p = 0; p < L; p++) {
            double acc = 0.0;
            const float *hp = h_up[p];
            for (int t = 0; t < K; t++)
                acc += (double)hp[t] * (double)win[-t];
            out[o++] = clamp_i32(acc);
        }
    }
    return o;
}

/* ---------------- Downsampler 8000*L -> 8k ---------------- */

void resamp_down_reset(resamp_down_t *r)
{
    memset(r->hist, 0, sizeof(r->hist));
    r->w     = 0;
    r->phase = 0;
}

int resamp_down_process(resamp_down_t *r, const int32_t *in, int n_in,
                        int32_t *out)
{
    if (!g_l_down) resampler_init_down(RESAMP_L);
    const int L = g_l_down;

    if (L == 1) {                       /* device already at the modem rate */
        memcpy(out, in, (size_t)n_in * sizeof(int32_t));
        return n_in;
    }

    const int ntaps = L * RESAMP_TAPS_PER_PHASE;
    int o = 0;
    for (int i = 0; i < n_in; i++) {
        /* Advance the circular head and write the sample at both mirrors.
         * The old code shifted all `ntaps` words here for every input sample;
         * at a 48 kHz device rate that was 8.6 M word-moves a second to feed a
         * filter that only fires once every L samples. */
        if (++r->w >= ntaps) r->w = 0;
        r->hist[r->w]         = in[i];
        r->hist[r->w + ntaps] = in[i];

        if (++r->phase >= L) {
            r->phase = 0;
            /* hist_logical[t] (t=0 newest) == hist[w + ntaps - t]; same walk
             * order as before, so the sum is bit-identical. */
            const int32_t *win = &r->hist[r->w + ntaps];
            double acc = 0.0;
            for (int t = 0; t < ntaps; t++)
                acc += (double)h_down[t] * (double)win[-t];
            out[o++] = clamp_i32(acc);
        }
    }
    return o;
}

float resampler_down_tap(int t)
{
    if (t < 0 || t >= RESAMP_NTAPS_MAX) return 0.0f;
    return h_down[t];
}

/* ======================================================================
 * Rational resampler
 * ======================================================================
 *
 * Same polyphase idea as the integer engine, generalised: convert by L/M in
 * lowest terms, so 44100 -> 8000 is 80/441 rather than "not a multiple of
 * 8000, refuse".  Output sample n is taken at input position n*M/L, which
 * splits into a base sample index and a filter phase; both advance by exact
 * integer arithmetic, so there is no drift however long the stream runs.
 *
 * The prototype spans a fixed amount of TIME rather than a fixed tap count,
 * the same rule the integer engine follows: at 8 kHz in that is 30 taps, at
 * 44.1 kHz in it is 166.  A tap count fixed across rates would make the
 * anti-aliasing filter progressively shorter in time as the input rate rose,
 * which is exactly where it is needed most.
 */

#define RAT_SPAN_MS      3.75    /* impulse-response span, input time */
#define RAT_MIN_TAPS     30
#define RAT_MAX_TAPS     768
#define RAT_MAX_PHASES   512     /* L; 441 is the largest the rate table needs */

struct resamp_rat {
    int      L, M;          /* out/in = L/M, lowest terms                  */
    int      K;             /* taps per phase (input samples spanned)      */
    float   *h;             /* L*K coefficients, phase-major               */
    int32_t *hist;          /* doubled circular history, 2*hist_len words  */
    int      hist_len;
    int      w;             /* index of the newest sample                  */
    int      phase;         /* (n*M) mod L                                 */
    int      lag;           /* newest input index minus the next output's
                             * base index; an output is due while >= 0     */
};

static int gcd_int(int a, int b)
{
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

/* Rates the modem can drive.  The 8 kHz multiples are what the integer engine
 * already handled; the 44.1 kHz family is what #193 is about.  192 kHz is here
 * too — it is an integer multiple (24) that the old RESAMP_L_MAX of 12 refused
 * for no reason other than the table size. */
static const int g_supported_rates[] = {
    8000, 11025, 16000, 22050, 24000, 32000,
    44100, 48000, 88200, 96000, 176400, 192000,
};

bool resampler_rate_supported(int device_rate_hz)
{
    for (size_t i = 0; i < sizeof(g_supported_rates)/sizeof(g_supported_rates[0]); i++)
        if (g_supported_rates[i] == device_rate_hz)
            return true;
    return false;
}

bool resampler_rational_for(int in_rate_hz, int out_rate_hz, int *L, int *M)
{
    if (!resampler_rate_supported(in_rate_hz) || !resampler_rate_supported(out_rate_hz))
        return false;
    int g = gcd_int(out_rate_hz, in_rate_hz);
    if (g <= 0)
        return false;
    int l = out_rate_hz / g;
    int m = in_rate_hz  / g;
    if (l > RAT_MAX_PHASES)
        return false;
    if (L) *L = l;
    if (M) *M = m;
    return true;
}

resamp_rat_t *resamp_rat_create(int in_rate_hz, int out_rate_hz)
{
    int L, M;
    if (!resampler_rational_for(in_rate_hz, out_rate_hz, &L, &M))
        return NULL;

    /* Span a fixed time, so the filter is as long as the job needs. */
    int K = (int)(RAT_SPAN_MS * 1e-3 * (double)in_rate_hz + 0.5);
    if (K < RAT_MIN_TAPS) K = RAT_MIN_TAPS;
    if (K > RAT_MAX_TAPS) K = RAT_MAX_TAPS;

    resamp_rat_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->L = L; r->M = M; r->K = K;

    /* History must reach back K taps from the base sample, plus however far
     * the base can lag the newest sample (one output step, ceil(M/L)). */
    r->hist_len = K + (M + L - 1) / L + 2;
    r->h    = calloc((size_t)L * (size_t)K, sizeof(*r->h));
    r->hist = calloc((size_t)2 * (size_t)r->hist_len, sizeof(*r->hist));
    if (!r->h || !r->hist) { resamp_rat_free(r); return NULL; }

    /* Prototype at the interpolated rate L*in_rate, cut at the lower of the
     * two Nyquists with a little margin -- on the way DOWN that is what stops
     * aliasing, on the way UP it is what stops images. */
    const int    N   = L * K;
    const double fs  = (double)in_rate_hz * (double)L;
    double fc = (in_rate_hz < out_rate_hz ? in_rate_hz : out_rate_hz) * 0.5 * 0.85;
    if (fc > FILT_FC) fc = FILT_FC;
    const double wc  = 2.0 * fc / fs;
    const double mid = (N - 1) / 2.0;

    double *proto = calloc((size_t)N, sizeof(*proto));
    if (!proto) { resamp_rat_free(r); return NULL; }

    double sum = 0.0;
    for (int n = 0; n < N; n++) {
        double x    = n - mid;
        double sinc = (fabs(x) < 1e-9) ? wc : sin(M_PI * wc * x) / (M_PI * x);
        double ham  = 0.54 - 0.46 * cos(2.0 * M_PI * n / (N - 1));
        proto[n] = sinc * ham;
        sum += proto[n];
    }
    /* Unity DC gain overall: each phase carries 1/L of the prototype, and the
     * L factor puts back what zero-stuffing takes out. */
    for (int n = 0; n < N; n++)
        proto[n] /= sum;

    for (int p = 0; p < L; p++)
        for (int t = 0; t < K; t++) {
            int idx = p + L * t;
            r->h[(size_t)p * K + t] = (idx < N) ? (float)(L * proto[idx]) : 0.0f;
        }
    free(proto);

    resamp_rat_reset(r);
    return r;
}

void resamp_rat_free(resamp_rat_t *r)
{
    if (!r) return;
    free(r->h);
    free(r->hist);
    free(r);
}

void resamp_rat_reset(resamp_rat_t *r)
{
    if (!r) return;
    memset(r->hist, 0, (size_t)2 * r->hist_len * sizeof(*r->hist));
    r->w     = r->hist_len - 1;
    r->phase = 0;
    r->lag   = -1;          /* nothing due until the first sample arrives */
}

void resamp_rat_ratio(const resamp_rat_t *r, int *L, int *M)
{
    if (!r) return;
    if (L) *L = r->L;
    if (M) *M = r->M;
}

int resamp_rat_max_out(const resamp_rat_t *r, int n_in)
{
    if (!r || n_in <= 0) return 0;
    /* +1 for the output that may already be due when the block starts. */
    return (int)(((int64_t)n_in * r->L) / r->M) + 2;
}

int resamp_rat_process(resamp_rat_t *r, const int32_t *in, int n_in,
                       int32_t *out)
{
    if (!r || !in || !out || n_in <= 0) return 0;

    const int L = r->L, M = r->M, K = r->K, HL = r->hist_len;
    int o = 0;

    for (int i = 0; i < n_in; i++) {
        if (++r->w >= HL) r->w = 0;
        r->hist[r->w]      = in[i];
        r->hist[r->w + HL] = in[i];

        /* The newest input just moved one ahead of the next output's base.
         * Emit every output whose base has now arrived: on the way DOWN that
         * is one output per M/L inputs, on the way UP several outputs share a
         * base and all of them are due at once. */
        r->lag++;

        while (r->lag >= 0) {
            const int32_t *win = &r->hist[r->w + HL];
            const float   *hp  = &r->h[(size_t)r->phase * K];
            const int      d   = r->lag;

            double acc = 0.0;
            for (int t = 0; t < K; t++)
                acc += (double)hp[t] * (double)win[-(d + t)];
            out[o++] = clamp_i32(acc);

            /* Exact integer step: no accumulated rounding however long the
             * stream runs, which is what a float cursor would give. */
            const int raw = r->phase + M;
            r->phase = raw % L;
            r->lag  -= raw / L;
        }
    }
    return o;
}
