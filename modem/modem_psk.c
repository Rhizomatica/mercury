/* PSK modem backend -- see modem_psk.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "modem_psk.h"
#include "psk/mpsk.h"

/* The codec2 modes come out of their modulators at this peak; this one is
 * scaled to it, so every rung drives the radio to the same peak. */
#define PSK_TX_PEAK 16384.0
/* Samples the receiver takes a call: 20 ms. */
#define PSK_NIN 160

typedef struct {
    mpsk_t *m;
    int mode;
    float snr;         /* the last frame's S/N, the burst's in 3 kHz */
} psk_modem_t;

/* The marker estimate (mpsk_rx's *snr_db, from the markers' phase noise;
 * the control frame's, on the same carriers, read the same way)
 * against the burst's S/N in 3 kHz on white noise (utils/psk_sweep psk3, 80
 * frames a point; the burst goes out 1.4 dB under a carrier at its peak),
 * so the reading is on the scale the OFDM rungs report.  Outside the knots,
 * the nearest knot's offset. */
static const float RAW_KNOT[]   = { 5.9f, 6.6f, 7.3f, 8.0f, 8.7f, 9.5f, 10.4f, 11.2f, 12.1f, 13.0f, 13.9f, 14.8f, 15.7f, 16.6f };
static const float BURST_KNOT[] = { -6.4f, -5.4f, -4.4f, -3.4f, -2.4f, -1.4f, -0.4f, 0.6f, 1.6f, 2.6f, 3.6f, 4.6f, 5.6f, 6.6f };
#define NKNOT ((int)(sizeof(RAW_KNOT) / sizeof(RAW_KNOT[0])))

static float burst_snr(float raw)
{
    if (raw <= RAW_KNOT[0]) return raw + BURST_KNOT[0] - RAW_KNOT[0];
    if (raw >= RAW_KNOT[NKNOT - 1]) return raw + BURST_KNOT[NKNOT - 1] - RAW_KNOT[NKNOT - 1];
    int i = 1;
    while (RAW_KNOT[i] < raw) i++;
    float t = (raw - RAW_KNOT[i - 1]) / (RAW_KNOT[i] - RAW_KNOT[i - 1]);
    return BURST_KNOT[i - 1] + t * (BURST_KNOT[i] - BURST_KNOT[i - 1]);
}

static void *psk_be_open(int mode)
{
    if (!mercury_mode_is_psk(mode)) return NULL;
    psk_modem_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->mode = mode;
    h->m = mpsk_open(mpsk_level_index(mode == MERCURY_MODE_PSKC ? "pskc" : "psk3"));
    if (!h->m) { free(h); return NULL; }
    mpsk_rx_reset(h->m);
    return h;
}

static void psk_be_close(void *ctx)
{
    psk_modem_t *h = ctx;
    if (!h) return;
    mpsk_close(h->m);
    free(h);
}

static void psk_be_configure(void *ctx, int frames_per_burst, int verbosity)
{
    (void)ctx; (void)frames_per_burst; (void)verbosity;   /* one frame a burst */
}

static int psk_be_bits_per_frame(void *ctx)    { return 8 * mpsk_frame_bytes(((psk_modem_t *)ctx)->m); }
static int psk_be_n_tx_samples(void *ctx)      { return mpsk_tx_samples(((psk_modem_t *)ctx)->m); }
static int psk_be_n_max_rx_samples(void *ctx)  { (void)ctx; return 2 * PSK_NIN; }
static int psk_be_n_nom_samples(void *ctx)     { return psk_be_n_tx_samples(ctx); }
static int psk_be_sample_rate(void *ctx)       { (void)ctx; return MPSK_FS; }
static int psk_be_get_mode(void *ctx)          { return ((psk_modem_t *)ctx)->mode; }
static int psk_be_frames_per_burst(void *ctx)  { (void)ctx; return 1; }

/* The outer carriers' centres, plus a symbol rate. */
static int psk_be_bandwidth_hz(void *ctx)
{
    const mpsk_level_t *l = mpsk_level(((psk_modem_t *)ctx)->m);
    return l->fc[l->nc - 1] - l->fc[0] + MPSK_FS / MPSK_SPS;
}

static int psk_be_preamble_tx(void *ctx, int16_t *out)  { (void)ctx; (void)out; return 0; }
static int psk_be_postamble_tx(void *ctx, int16_t *out) { (void)ctx; (void)out; return 0; }

static int psk_be_rawdata_tx(void *ctx, int16_t *out, const uint8_t *frame)
{
    return mpsk_modulate_raw(((psk_modem_t *)ctx)->m, frame, out, PSK_TX_PEAK);
}

static int psk_be_nin(void *ctx) { (void)ctx; return PSK_NIN; }

static int psk_be_rawdata_rx(void *ctx, uint8_t *bytes_out, const int16_t *demod_in)
{
    psk_modem_t *h = ctx;
    int found = 0;
    float raw = 0.0f;
    int ok = mpsk_rx(h->m, demod_in, PSK_NIN, bytes_out, &found, &raw);
    if (found) h->snr = burst_snr(raw);
    return ok ? mpsk_frame_bytes(h->m) : 0;
}

/* Never in sync: a frame is found only once it has ended, so the found
 * flag says nothing about what is on the air now, and the modem takes a
 * sync that rises without a frame for a preamble caught -- a hold of a
 * whole frame's airtime on a channel that was already clear (on the
 * control plane, 2 s of a probe's answer slot lost, Go harness). */
static void psk_be_get_stats(void *ctx, int *sync, float *snr)
{
    psk_modem_t *h = ctx;
    if (sync) *sync = 0;
    if (snr) *snr = h->snr;
}

static int psk_be_get_rx_status(void *ctx) { (void)ctx; return 0; }

static void psk_be_unsync(void *ctx) { mpsk_rx_reset(((psk_modem_t *)ctx)->m); }

static void psk_be_set_crc_seed(void *ctx, uint16_t seed, int accept_plain)
{
    mpsk_set_crc_seed(((psk_modem_t *)ctx)->m, seed, accept_plain);
}

static int psk_be_rx_crc_seeded(void *ctx) { return mpsk_rx_seeded(((psk_modem_t *)ctx)->m); }

const modem_backend_t modem_backend_psk = {
    .name             = "psk",
    .open             = psk_be_open,
    .close            = psk_be_close,
    .configure        = psk_be_configure,
    .bits_per_frame   = psk_be_bits_per_frame,
    .n_tx_samples     = psk_be_n_tx_samples,
    .n_max_rx_samples = psk_be_n_max_rx_samples,
    .n_nom_samples    = psk_be_n_nom_samples,
    .sample_rate      = psk_be_sample_rate,
    .bandwidth_hz     = psk_be_bandwidth_hz,
    .get_mode         = psk_be_get_mode,
    .frames_per_burst = psk_be_frames_per_burst,
    .preamble_tx      = psk_be_preamble_tx,
    .rawdata_tx       = psk_be_rawdata_tx,
    .postamble_tx     = psk_be_postamble_tx,
    .nin              = psk_be_nin,
    .rawdata_rx       = psk_be_rawdata_rx,
    .get_stats        = psk_be_get_stats,
    .get_rx_status    = psk_be_get_rx_status,
    .unsync           = psk_be_unsync,
    .set_crc_seed     = psk_be_set_crc_seed,
    .rx_crc_seeded    = psk_be_rx_crc_seeded,
};
