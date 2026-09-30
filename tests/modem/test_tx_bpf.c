/*
 * OFDM TX band-pass filters: flat on every carrier, and filtering
 *
 * Each OFDM mode names the prototype its TX filter is built from, by hand,
 * next to a carrier layout set elsewhere in the same config.  Nothing tied the
 * two together, and both ways of getting it wrong shipped: qam16c2 went out
 * with no filter at all (a skirt 30 dB down across the whole 0-4 kHz band),
 * and datac17 with one whose -6 dB points fell inside its carrier span, 6.5 dB
 * off its outermost data carriers.
 *
 * So, for every mode Mercury transmits: the filter exists, its response on
 * each carrier the mode sends (data carriers 1..nc, plus the edge pilots when
 * enabled) is within 1 dB of its peak, and 400 Hz beyond the outermost
 * carrier it is at least 20 dB down.  The response is taken from the filter
 * as ofdm_create() tunes it, not from the prototype.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "ofdm_internal.h"
#include "filter.h"

void setUp(void) {}
void tearDown(void) {}

static const char *MODES[] = { "datac0",  "datac1",  "datac3",  "datac4",  "datac13",
                               "datac14", "datac15", "datac16", "datac17", "qam16c2" };

static double resp_db(const struct quisk_cfFilter *f, double hz, double fs)
{
    double complex acc = 0;
    for (int n = 0; n < f->nTaps; n++)
        acc += f->cpxCoefs[n] * cexp(-2.0 * I * M_PI * hz / fs * n);
    return 20.0 * log10(cabs(acc));
}

static double peak_db(const struct quisk_cfFilter *f, double fs)
{
    double peak = -1e9;
    for (double hz = 0; hz < fs / 2; hz += 1.0) {
        double r = resp_db(f, hz, fs);
        if (r > peak) peak = r;
    }
    return peak;
}

static struct OFDM *open_mode(const char *mode)
{
    struct OFDM_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    ofdm_init_mode((char *)mode, &cfg);
    return ofdm_create(&cfg);
}

static void test_every_carrier_is_in_the_passband(void)
{
    for (size_t m = 0; m < sizeof(MODES) / sizeof(MODES[0]); m++) {
        struct OFDM *o = open_mode(MODES[m]);
        char what[96];
        snprintf(what, sizeof(what), "%s has a TX filter", MODES[m]);
        TEST_ASSERT_NOT_NULL_MESSAGE(o, MODES[m]);
        TEST_ASSERT_TRUE_MESSAGE(o->tx_bpf_en && o->tx_bpf, what);

        double peak = peak_db(o->tx_bpf, o->fs);
        int lo = o->edge_pilots ? 0 : 1, hi = o->edge_pilots ? o->nc + 1 : o->nc;
        for (int c = lo; c <= hi; c++) {
            double hz = (o->tx_nlower + c) * o->rs;
            double r = resp_db(o->tx_bpf, hz, o->fs) - peak;
            snprintf(what, sizeof(what), "%s carrier %d at %.1f Hz: %.2f dB", MODES[m], c, hz, r);
            TEST_ASSERT_TRUE_MESSAGE(r >= -1.0, what);
        }
        ofdm_destroy(o);
    }
}

static void test_the_filter_filters(void)
{
    for (size_t m = 0; m < sizeof(MODES) / sizeof(MODES[0]); m++) {
        struct OFDM *o = open_mode(MODES[m]);
        TEST_ASSERT_NOT_NULL_MESSAGE(o, MODES[m]);
        TEST_ASSERT_NOT_NULL_MESSAGE(o->tx_bpf, MODES[m]);

        double peak = peak_db(o->tx_bpf, o->fs);
        int lo = o->edge_pilots ? 0 : 1, hi = o->edge_pilots ? o->nc + 1 : o->nc;
        double edges[2] = { (o->tx_nlower + lo) * o->rs - 400.0, (o->tx_nlower + hi) * o->rs + 400.0 };
        for (int e = 0; e < 2; e++) {
            char what[96];
            double r = resp_db(o->tx_bpf, edges[e], o->fs) - peak;
            snprintf(what, sizeof(what), "%s at %.1f Hz: %.2f dB", MODES[m], edges[e], r);
            TEST_ASSERT_TRUE_MESSAGE(r <= -20.0, what);
        }
        ofdm_destroy(o);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_carrier_is_in_the_passband);
    RUN_TEST(test_the_filter_filters);
    return UNITY_END();
}
