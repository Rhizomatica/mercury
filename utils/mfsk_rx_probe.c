/* mfsk_rx_probe: the MFSK receiver as a listening station runs it -- frames
 * arriving at random times in continuous noise, fed in nin() chunks -- for
 * two numbers that trade against each other:
 *
 *   sens   frames decoded against SNR3k, at 0 and +25 Hz offset;
 *   cpu    receiver CPU per second of audio on noise alone (the idle station).
 *
 * Any change that makes the receiver cheaper must leave `sens` where it was.
 *
 * usage: mfsk_rx_probe [sens N | cpu SECONDS | all]
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

static uint64_t rs = 0x2545F4914F6CDD1DULL;
static double urand(void) { rs = rs * 6364136223846793005ULL + 1442695040888963407ULL; return ((rs >> 11) + 0.5) / 9007199254740992.0; }
static double gauss(void) { return sqrt(-2.0 * log(urand())) * cos(2.0 * M_PI * urand()); }

/* In-place radix-2 FFT (n a power of two); inverse when inv. */
static void fft(double complex *x, int n, int inv)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double complex t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * M_PI / len * (inv ? 1 : -1);
        double complex wl = cexp(I * ang);
        for (int i = 0; i < n; i += len) {
            double complex w = 1;
            for (int k = 0; k < len / 2; k++) {
                double complex u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v; x[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inv) for (int i = 0; i < n; i++) x[i] /= n;
}

/* Shift a real signal by df Hz through its analytic signal. */
static void shift(const int16_t *in, int n, double df, double *out)
{
    int N = 1; while (N < n) N <<= 1;
    double complex *X = calloc((size_t)N, sizeof *X);
    for (int i = 0; i < n; i++) X[i] = in[i];
    fft(X, N, 0);
    for (int k = 1; k < N / 2; k++) X[k] *= 2.0;
    for (int k = N / 2 + 1; k < N; k++) X[k] = 0;
    fft(X, N, 1);
    for (int i = 0; i < n; i++) out[i] = creal(X[i] * cexp(2.0 * I * M_PI * df * i / 8000.0));
    free(X);
}

static const modem_backend_t *be = &modem_backend_mfsk;

/* Feed `n` samples to the receiver in nin() chunks; returns frames decoded. */
static int feed(void *ctx, const int16_t *x, long n, uint8_t *out)
{
    int got = 0;
    long i = 0;
    while (i < n) {
        int c = be->nin(ctx);
        if (c <= 0) c = 160;
        if (i + c > n) break;
        if (be->rawdata_rx(ctx, out, x + i) > 0) got++;
        i += c;
    }
    return got;
}

static void sens(int frames)
{
    void *tx = be->open(MERCURY_MODE_MFSK);
    int ntx = be->n_tx_samples(tx), nb = be->bits_per_frame(tx) / 8;
    int16_t *f = calloc((size_t)ntx + 8000, sizeof *f);
    double *fs = calloc((size_t)ntx + 8000, sizeof *fs);
    uint8_t *pl = calloc((size_t)nb, 1), *out = calloc((size_t)nb + 16, 1);
    /* A random payload with FreeDV's CRC16 trailer (big-endian), which is what
     * the receiver checks before it reports a frame. */
    for (int b = 0; b < nb - 2; b++) pl[b] = (uint8_t)(urand() * 256);
    uint16_t crc = freedv_gen_crc16(pl, nb - 2);
    pl[nb - 2] = (uint8_t)(crc >> 8); pl[nb - 1] = (uint8_t)crc;
    /* A burst is preamble, payload and postamble: rawdata_tx is only the middle. */
    int n = be->preamble_tx(tx, f);
    n += be->rawdata_tx(tx, f + n, pl);
    n += be->postamble_tx(tx, f + n);
    double ps = 0; for (int i = 0; i < n; i++) ps += (double)f[i] * f[i]; ps /= n;
    printf("# sens: frames decoded of %d, frames at random times in continuous noise\n", frames);
    printf("# snr3k   off0   off+25\n");
    for (int snr = -8; snr >= -14; snr--) {
        int got[2];
        for (int oi = 0; oi < 2; oi++) {
            void *rx = be->open(MERCURY_MODE_MFSK);
            shift(f, n, oi ? 25.0 : 0.0, fs);
            double sigma = sqrt(ps / pow(10.0, snr / 10.0) * 4000.0 / 3000.0);
            got[oi] = 0;
            int16_t *buf = malloc(sizeof *buf * (size_t)(n + 40000));
            for (int k = 0; k < frames; k++) {
                int gap = 8000 + (int)(urand() * 24000);        /* 1..4 s of noise first */
                long tot = gap + n + 4000;
                for (long t = 0; t < tot; t++) {
                    double v = sigma * gauss() + ((t >= gap && t < gap + n) ? fs[t - gap] : 0);
                    buf[t] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
                got[oi] += feed(rx, buf, tot, out);
            }
            free(buf);
            be->close(rx);
        }
        printf("%6d   %3d    %3d\n", snr, got[0], got[1]);
        fflush(stdout);
    }
    be->close(tx);
    free(f); free(fs); free(pl); free(out);
}

static void cpu(double seconds)
{
    void *rx = be->open(MERCURY_MODE_MFSK);
    long n = (long)(8000 * seconds);
    int16_t *x = malloc((size_t)n * sizeof *x);
    uint8_t *out = calloc(256, 1);
    for (long t = 0; t < n; t++) x[t] = (int16_t)(2000.0 * gauss());
    clock_t c0 = clock();
    int got = feed(rx, x, n, out);
    double s = (double)(clock() - c0) / CLOCKS_PER_SEC;
    printf("# cpu: %.2f s for %.0f s of noise = %.2f %% of one core (%d false frames)\n", s, seconds, 100.0 * s / seconds, got);
    be->close(rx);
    free(x); free(out);
}

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "all";
    if (!strcmp(what, "cpu") || !strcmp(what, "all")) cpu(argc > 2 && strcmp(what, "all") ? atof(argv[2]) : 120.0);
    if (!strcmp(what, "sens") || !strcmp(what, "all")) sens(argc > 2 && strcmp(what, "all") ? atoi(argv[2]) : 30);
    return 0;
}
