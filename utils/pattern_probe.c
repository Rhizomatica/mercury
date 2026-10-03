/* pattern_probe: how well the MFSK pattern detector sees a pattern, and how
 * often it sees one that is not there.
 *
 * The keydown NAV header (a pattern at the start of every in-session keydown)
 * rests on these numbers, so they are measured on the real detector:
 *
 *   curve      detection probability against SNR3k, AWGN, at 0 and +25 Hz
 *              offset (two radios are never on the same frequency);
 *   fading     the miss probability under flat Rayleigh fading, from the AWGN
 *              curve: at 0.5 Hz Doppler the channel is coherent for ~0.85 s,
 *              longer than the 0.64 s pattern, so a pattern sees one fade
 *              level and P_miss(mean) = E[P_miss(mean + 10log10 |h|^2)];
 *   noise      false detections per hour of noise, scanned as the RX loop
 *              does (a two-burst window, once per burst of new audio);
 *   signals    false detections on every data mode's bursts (DATAC*, QAM16C2,
 *              MFSK) -- the peer's own frames are what the detector hears most;
 *   cpu        cost of the scan per second of audio.
 *
 * usage: pattern_probe [curve|fading|noise HOURS|signals|cpu|all]
 */
#include <complex.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "modem_mfsk.h"
#include "modem_backend.h"
#include "freedv_api.h"

static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static double urand(void) { rs = rs * 6364136223846793005ULL + 1442695040888963407ULL; return ((rs >> 11) + 0.5) / 9007199254740992.0; }
static double gauss(void) { return sqrt(-2.0 * log(urand())) * cos(2.0 * M_PI * urand()); }

/* Shift a real signal by df Hz (analytic signal via DFT; n is small). */
static void shift(const int16_t *in, int n, double df, int16_t *out)
{
    if (df == 0) { memcpy(out, in, (size_t)n * sizeof *in); return; }
    double complex *X = calloc((size_t)n, sizeof *X);
    for (int k = 0; k <= n / 2; k++) {
        double complex s = 0;
        for (int t = 0; t < n; t++) s += in[t] * cexp(-2.0 * I * M_PI * k * t / n);
        X[k] = (k == 0 || k == n / 2) ? s : 2.0 * s;
    }
    for (int t = 0; t < n; t++) {
        double complex s = 0;
        for (int k = 0; k <= n / 2; k++) s += X[k] * cexp(2.0 * I * M_PI * k * t / n);
        double v = creal(s / n * cexp(2.0 * I * M_PI * df * t / 8000.0));
        out[t] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    free(X);
}

static double power(const int16_t *x, int n) { double p = 0; for (int i = 0; i < n; i++) p += (double)x[i] * x[i]; return p / n; }

/* Noise power for a given SNR in 3 kHz: the noise spans fs/2 = 4 kHz. */
static double sigma_for(double ps, double snr3k) { return sqrt(ps / pow(10.0, snr3k / 10.0) * 4000.0 / 3000.0); }

#define SNR_LO (-18)
#define SNR_HI (0)
static double g_curve[2][SNR_HI - SNR_LO + 1];   /* [offset][snr] P_detect */

static void curve(int trials)
{
    int n_pat = mfsk_pattern_max_tx_samples();
    int16_t *pat = calloc((size_t)n_pat, sizeof *pat), *sh = calloc((size_t)n_pat, sizeof *sh);
    int16_t *bpat = calloc((size_t)n_pat, sizeof *bpat);
    int n = mfsk_pattern_tx(pat, 0);
    mfsk_pattern_tx(bpat, 1);
    double ps = power(pat, n);
    int total = n + 2 * 2000;
    int16_t *rx = calloc((size_t)total, sizeof *rx);
    printf("# curve: P(detect ACK) and P(read as BREAK), %d trials per point\n", trials);
    printf("# snr3k   off0    off+25  confused\n");
    for (int snr = SNR_LO; snr <= SNR_HI; snr++) {
        double pd[2]; int conf = 0;
        for (int oi = 0; oi < 2; oi++) {
            shift(pat, n, oi ? 25.0 : 0.0, sh);
            double sigma = sigma_for(ps, snr);
            int hit = 0;
            for (int i = 0; i < trials; i++) {
                for (int t = 0; t < total; t++) {
                    double v = sigma * gauss() + ((t >= 2000 && t < 2000 + n) ? sh[t - 2000] : 0);
                    rx[t] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
                int isb = 0;
                if (mfsk_pattern_detect(rx, total, &isb)) { if (!isb) hit++; else conf++; }
            }
            pd[oi] = (double)hit / trials;
            g_curve[oi][snr - SNR_LO] = pd[oi];
        }
        printf("%6d  %6.3f  %6.3f  %d\n", snr, pd[0], pd[1], conf);
        fflush(stdout);
    }
    free(pat); free(sh); free(bpat); free(rx);
}

static double p_detect_awgn(int oi, double snr)
{
    if (snr <= SNR_LO) return g_curve[oi][0];
    if (snr >= SNR_HI) return g_curve[oi][SNR_HI - SNR_LO];
    int i = (int)floor(snr) - SNR_LO; double f = snr - floor(snr);
    return g_curve[oi][i] * (1 - f) + g_curve[oi][i + 1] * f;
}

/* Flat Rayleigh: |h|^2 ~ Exp(1).  Integrate numerically over the fade. */
static void fading(void)
{
    printf("# fading: P(miss) under flat Rayleigh (one fade level per pattern), offset +25 Hz\n");
    printf("# mean_snr3k  P_miss_header  P_miss_two_independent\n");
    for (int mean = -12; mean <= 0; mean++) {
        double pm = 0; int N = 20000;
        for (int i = 0; i < N; i++) {
            double g = -log(1.0 - (i + 0.5) / N);          /* quantiles of Exp(1) */
            pm += 1.0 - p_detect_awgn(1, mean + 10.0 * log10(g));
        }
        pm /= N;
        printf("%8d      %6.3f         %6.4f\n", mean, pm, pm * pm);
    }
}

/* Scan audio the way the RX loop does: a window of two bursts, scanned once
 * per burst of new audio.  Returns detections. */
static long scan_stream(const int16_t *x, long n, long *scans)
{
    mfsk_pattern_window_t w = {0};
    long hits = 0, chunk = 160;
    for (long i = 0; i + chunk <= n; i += chunk) {
        int isb;
        if (mfsk_pattern_window_push(&w, x + i, (int)chunk, &isb)) hits++;
    }
    if (scans) *scans = n / mfsk_pattern_max_tx_samples();
    mfsk_pattern_window_free(&w);
    return hits;
}

static void noise(double hours)
{
    long n = (long)(8000.0 * 60.0);   /* a minute at a time */
    int16_t *x = malloc((size_t)n * sizeof *x);
    long hits = 0, minutes = (long)(hours * 60.0 + 0.5);
    for (long m = 0; m < minutes; m++) {
        for (long t = 0; t < n; t++) x[t] = (int16_t)(3000.0 * gauss());
        hits += scan_stream(x, n, NULL);
    }
    printf("# noise: %ld false detections in %.2f h of noise (%.3f per hour)\n", hits, hours, hits / hours);
    free(x);
}

static void signals(void)
{
    static const struct { int mode; const char *name; } modes[] = {
        { FREEDV_MODE_DATAC16, "DATAC16" }, { FREEDV_MODE_DATAC15, "DATAC15" }, { FREEDV_MODE_DATAC4, "DATAC4" },
        { FREEDV_MODE_DATAC3, "DATAC3" },   { FREEDV_MODE_DATAC1, "DATAC1" },   { FREEDV_MODE_DATAC17, "DATAC17" },
        { FREEDV_MODE_QAM16C2, "QAM16C2" },
    };
    printf("# signals: bursts that read as a pattern (clean, then +5 dB SNR noise)\n");
    for (unsigned mi = 0; mi < sizeof modes / sizeof modes[0]; mi++) {
        struct freedv *f = freedv_open(modes[mi].mode);
        if (!f) { printf("%-8s open failed\n", modes[mi].name); continue; }
        int npre = freedv_get_n_tx_preamble_modem_samples(f), nmod = freedv_get_n_tx_modem_samples(f);
        int npost = freedv_get_n_tx_postamble_modem_samples(f);
        int nbytes = freedv_get_bits_per_modem_frame(f) / 8;
        int frames = 60;
        long n = (long)frames * (npre + nmod + npost + 800);
        int16_t *x = calloc((size_t)n, sizeof *x), *y = calloc((size_t)n, sizeof *y);
        uint8_t *pl = malloc((size_t)nbytes);
        long k = 0;
        for (int i = 0; i < frames; i++) {
            for (int b = 0; b < nbytes; b++) pl[b] = (uint8_t)(urand() * 256);
            k += freedv_rawdatapreambletx(f, x + k);
            freedv_rawdatatx(f, x + k, pl); k += nmod;
            k += freedv_rawdatapostambletx(f, x + k);
            k += 800;
        }
        double ps = power(x, k);
        double sigma = sigma_for(ps, 5.0);
        for (long t = 0; t < k; t++) { double v = x[t] + sigma * gauss(); y[t] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
        long h0 = scan_stream(x, k, NULL), h1 = scan_stream(y, k, NULL);
        printf("%-8s %d frames (%.0f s): %ld clean, %ld noisy\n", modes[mi].name, frames, k / 8000.0, h0, h1);
        free(x); free(y); free(pl);
        freedv_close(f);
    }
    /* MFSK data */
    const modem_backend_t *be = &modem_backend_mfsk;
    void *ctx = be->open(MERCURY_MODE_MFSK);
    int ntx = be->n_tx_samples(ctx), nb = be->bits_per_frame(ctx) / 8;
    int frames = 40;
    long n = (long)frames * (ntx + 800);
    int16_t *x = calloc((size_t)n, sizeof *x);
    uint8_t *pl = calloc((size_t)nb, 1);
    long k = 0;
    for (int i = 0; i < frames; i++) {
        for (int b = 0; b < nb; b++) pl[b] = (uint8_t)(urand() * 256);
        k += be->rawdata_tx(ctx, x + k, pl);
        k += 800;
        if (k + ntx + 800 > n) break;
    }
    printf("%-8s %d frames (%.0f s): %ld clean\n", "MFSK", frames, k / 8000.0, scan_stream(x, k, NULL));
    free(x); free(pl);
}

static void cpu(void)
{
    long n = 8000L * 120;
    int16_t *x = malloc((size_t)n * sizeof *x);
    for (long t = 0; t < n; t++) x[t] = (int16_t)(3000.0 * gauss());
    clock_t c0 = clock();
    scan_stream(x, n, NULL);
    double s = (double)(clock() - c0) / CLOCKS_PER_SEC;
    printf("# cpu: %.3f s of CPU for %.0f s of audio (%.1f %% of real time)\n", s, n / 8000.0, 100.0 * s / (n / 8000.0));
    free(x);
}

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "all";
    int all = !strcmp(what, "all");
    if (all || !strcmp(what, "cpu")) cpu();
    if (all || !strcmp(what, "curve") || !strcmp(what, "fading")) {
        curve(argc > 2 && strcmp(what, "curve") == 0 ? atoi(argv[2]) : 200);
        fading();
    }
    if (all || !strcmp(what, "signals")) signals();
    if (all || !strcmp(what, "noise")) noise(argc > 2 && !all ? atof(argv[2]) : 1.0);
    return 0;
}
