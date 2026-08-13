/*
 * OFDM burst acquisition: FFT search must equal the brute-force search
 *
 * The joint timing/frequency search in ofdm.c used to evaluate every
 * (frequency hypothesis, timing offset) pair with a direct dot product, and on
 * an idle receiver that measured 90 % of all RX CPU (issue #162).  It is now
 * computed as an FFT cross-correlation, which is the same quantity by a
 * different route.
 *
 * "Same quantity by a different route" is exactly the kind of claim that
 * rots.  Acquisition is what sets the fringe floor for every DATAC mode, so a
 * fast path that silently picks a different peak — or picks the right peak
 * only on clean signals — would cost sensitivity where we can least afford it
 * and would not show up in any functional test.
 *
 * So: run the real modem over the same audio twice, once with each path, and
 * require the outcomes to match exactly.  freedv_set_acq_fft_enable() exists
 * for this.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "freedv_api.h"

#define FS 8000

static uint64_t s_rng;
static double urand(void)
{
    s_rng = s_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((s_rng >> 11) & 0x1FFFFFFFFFFFFFULL) / (double)0x20000000000000ULL;
}
static double gauss(void)
{
    double u1 = urand(), u2 = urand();
    if (u1 < 1e-300) u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

void setUp(void)    { s_rng = 0xACC0; }
void tearDown(void) { }

/* Push one noisy burst through a decoder and report what happened.
 * acquired: reached full sync.  delivered: frame out with the CRC intact. */
static void run_one(int mode, bool fft_en, double snr_db, unsigned seed,
                    int *acquired, int *delivered)
{
    struct freedv *tx = freedv_open(mode);
    struct freedv *rx = freedv_open(mode);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_NOT_NULL(rx);
    freedv_set_frames_per_burst(tx, 1);
    freedv_set_frames_per_burst(rx, 1);
    freedv_set_acq_fft_enable(rx, fft_en);

    int nbytes = freedv_get_bits_per_modem_frame(tx) / 8;
    int ntx    = freedv_get_n_tx_modem_samples(tx);
    int npre   = freedv_get_n_tx_preamble_modem_samples(tx);
    int npost  = freedv_get_n_tx_postamble_modem_samples(tx);
    int n      = npre + ntx + npost;

    short *burst = malloc(sizeof(short) * (size_t)(n + 8));
    unsigned char *payload = malloc((size_t)nbytes);
    unsigned char *out = malloc((size_t)nbytes + 8);
    TEST_ASSERT_NOT_NULL(burst);

    s_rng = seed;
    for (int i = 0; i < nbytes - 2; i++)
        payload[i] = (unsigned char)(urand() * 256.0);
    unsigned short crc = freedv_gen_crc16(payload, nbytes - 2);
    payload[nbytes - 2] = (unsigned char)(crc >> 8);
    payload[nbytes - 1] = (unsigned char)(crc & 0xff);

    int k = 0;
    k += freedv_rawdatapreambletx(tx, burst + k);
    freedv_rawdatatx(tx, burst + k, payload);
    k += ntx;
    k += freedv_rawdatapostambletx(tx, burst + k);

    /* Normalise then add noise, so signal+noise cannot clip the int16 rail —
     * clipping would make the two paths differ for a reason unrelated to the
     * search (see utils/acquire_vs_decode). */
    double ps = 0.0, pk = 0.0;
    for (int i = 0; i < k; i++) {
        ps += (double)burst[i] * burst[i];
        if (fabs((double)burst[i]) > pk) pk = fabs((double)burst[i]);
    }
    ps /= k;
    double g = 4000.0 / pk;
    for (int i = 0; i < k; i++) burst[i] = (short)lrint(burst[i] * g);
    ps *= g * g;

    double sigma = sqrt(ps / (pow(10.0, snr_db / 10.0) * (3000.0 / (FS / 2.0))));
    for (int i = 0; i < k; i++) {
        double v = burst[i] + sigma * gauss();
        if (v >  32767.0) v =  32767.0;
        if (v < -32768.0) v = -32768.0;
        burst[i] = (short)lrint(v);
    }

    freedv_set_sync(rx, FREEDV_SYNC_UNSYNC);
    int pos = 0, sync = 0, got = 0;
    while (pos < k + n) {
        int nin = freedv_nin(rx);
        short chunk[8192];
        if (nin <= 0 || nin > (int)(sizeof(chunk) / sizeof(chunk[0]))) break;
        for (int i = 0; i < nin; i++) {
            if (pos + i < k) chunk[i] = burst[pos + i];
            else {
                double v = sigma * gauss();
                if (v >  32767.0) v =  32767.0;
                if (v < -32768.0) v = -32768.0;
                chunk[i] = (short)lrint(v);
            }
        }
        pos += nin;
        int nb = (int)freedv_rawdatarx(rx, out, chunk);
        int st = freedv_get_rx_status(rx);
        if (st & FREEDV_RX_SYNC) sync = 1;
        if (nb > 0 && !(st & FREEDV_RX_BIT_ERRORS)) got = 1;
    }

    *acquired = sync;
    *delivered = got;
    free(burst); free(payload); free(out);
    freedv_close(tx); freedv_close(rx);
}

/* Across modes and down to where acquisition is genuinely failing, the two
 * paths must agree burst for burst -- including on the bursts that FAIL.
 * Agreeing only where the signal is strong would prove nothing: the whole
 * question is whether the fast search holds up at the fringe. */
void test_acq_fft_matches_brute_force(void)
{
    const int modes[] = { FREEDV_MODE_DATAC16, FREEDV_MODE_DATAC15,
                          FREEDV_MODE_DATAC4,  FREEDV_MODE_DATAC3 };
    const double snrs[] = { -4.0, -9.0, -11.0, -13.0 };

    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        for (unsigned s = 0; s < sizeof(snrs) / sizeof(snrs[0]); s++) {
            for (unsigned trial = 0; trial < 4; trial++) {
                unsigned seed = 0x5EED + trial * 977u + s * 31u + m * 7u;
                int a_fft, d_fft, a_bf, d_bf;
                run_one(modes[m], true,  snrs[s], seed, &a_fft, &d_fft);
                run_one(modes[m], false, snrs[s], seed, &a_bf,  &d_bf);

                char msg[128];
                snprintf(msg, sizeof(msg),
                         "mode idx %u, SNR %.0f dB, trial %u: acquisition differs",
                         m, snrs[s], trial);
                TEST_ASSERT_EQUAL_INT_MESSAGE(a_bf, a_fft, msg);
                TEST_ASSERT_EQUAL_INT_MESSAGE(d_bf, d_fft, msg);
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_acq_fft_matches_brute_force);
    return UNITY_END();
}
