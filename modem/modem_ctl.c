/* The control plane's codec: the control mode (FreeDV DATAC16) for
 * everything, and the short PSK control frame (MERCURY_MODE_PSKC) on receive
 * too, from the same audio.
 *
 * A sender puts its control frames on the short frame when its peer hears it
 * well (carousel.c, ctl_mode), so the decoder that hears the control mode has
 * to hear both, whatever the session: a frame it decodes from either is a
 * control frame of the control mode's size (14 bytes and the CRC).  It
 * transmits only the control mode; the short frame goes out from its own
 * pool instance (modem_backend_psk).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "modem_ctl.h"
#include "modem_freedv.h"
#include "modem_psk.h"

#define CTL_MAX_IN 8192             /* more than any nin of the control mode */
#define CTL_FRAME  64               /* more than the control frame's bytes */

typedef struct {
    void *fdv;                      /* the control mode, modem_backend_freedv */
    void *psk;                      /* the short frame, modem_backend_psk */
    int16_t in[CTL_MAX_IN];         /* audio the short frame's receiver has not had */
    int nin;
    uint8_t held[CTL_FRAME];        /* a short frame decoded with a control-mode one */
    int nheld;
    int last;                       /* 0: the last frame was the control mode's, 1: the short one's */
} ctl_t;

#define F (&modem_backend_freedv)
#define P (&modem_backend_psk)

static void *ctl_open(int mode)
{
    ctl_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->fdv = F->open(mode);
    h->psk = P->open(MERCURY_MODE_PSKC);
    if (!h->fdv || !h->psk) {
        if (h->fdv) F->close(h->fdv);
        if (h->psk) P->close(h->psk);
        free(h);
        return NULL;
    }
    return h;
}

static void ctl_close(void *ctx)
{
    ctl_t *h = ctx;
    if (!h) return;
    F->close(h->fdv);
    P->close(h->psk);
    free(h);
}

#define FC(ctx) (((ctl_t *)(ctx))->fdv)
static void ctl_configure(void *ctx, int fpb, int v) { F->configure(FC(ctx), fpb, v); }
static int ctl_bits_per_frame(void *ctx)   { return F->bits_per_frame(FC(ctx)); }
static int ctl_n_tx_samples(void *ctx)     { return F->n_tx_samples(FC(ctx)); }
static int ctl_n_max_rx_samples(void *ctx) { return F->n_max_rx_samples(FC(ctx)); }
static int ctl_n_nom_samples(void *ctx)    { return F->n_nom_samples(FC(ctx)); }
static int ctl_sample_rate(void *ctx)      { return F->sample_rate(FC(ctx)); }
static int ctl_bandwidth_hz(void *ctx)     { return F->bandwidth_hz(FC(ctx)); }
static int ctl_get_mode(void *ctx)         { return F->get_mode(FC(ctx)); }
static int ctl_frames_per_burst(void *ctx) { return F->frames_per_burst(FC(ctx)); }
static int ctl_preamble_tx(void *ctx, int16_t *out)  { return F->preamble_tx(FC(ctx), out); }
static int ctl_postamble_tx(void *ctx, int16_t *out) { return F->postamble_tx(FC(ctx), out); }
static int ctl_rawdata_tx(void *ctx, int16_t *out, const uint8_t *frame)
{
    return F->rawdata_tx(FC(ctx), out, frame);
}
static int ctl_nin(void *ctx) { return F->nin(FC(ctx)); }

/* The control mode takes its nin samples; the short frame's receiver gets the
 * same samples in its own steps.  A frame from either comes out; one of each
 * in the same call (they cannot overlap on the air) keeps the short one for
 * the next call. */
static int ctl_rawdata_rx(void *ctx, uint8_t *bytes_out, const int16_t *demod_in)
{
    ctl_t *h = ctx;
    int n = F->nin(h->fdv);
    int got = F->rawdata_rx(h->fdv, bytes_out, demod_in);
    if (got > 0) h->last = 0;
    if (n > 0 && h->nin + n <= CTL_MAX_IN) {
        memcpy(h->in + h->nin, demod_in, sizeof(int16_t) * (size_t)n);
        h->nin += n;
    }
    int step = P->nin(h->psk), off = 0;
    while (h->nin - off >= step && !h->nheld) {
        uint8_t frame[CTL_FRAME];
        int k = P->rawdata_rx(h->psk, frame, h->in + off);
        off += step;
        if (k > 0 && k <= CTL_FRAME) {
            memcpy(h->held, frame, (size_t)k);
            h->nheld = k;
            /* The control mode's detector may have locked onto the short
             * frame's middle carrier: drop it, or its sync outlives the
             * frame and holds the channel for a frame of its own. */
            F->unsync(h->fdv);
        }
    }
    if (off) memmove(h->in, h->in + off, sizeof(int16_t) * (size_t)(h->nin - off));
    h->nin -= off;
    if (!got && h->nheld) {
        memcpy(bytes_out, h->held, (size_t)h->nheld);
        got = h->nheld;
        h->nheld = 0;
        h->last = 1;
    }
    return got;
}

static void ctl_get_stats(void *ctx, int *sync, float *snr)
{
    ctl_t *h = ctx;
    int s1 = 0, s2 = 0;
    float r1 = 0.0f, r2 = 0.0f;
    F->get_stats(h->fdv, &s1, &r1);
    P->get_stats(h->psk, &s2, &r2);
    if (sync) *sync = s1 || s2;
    if (snr) *snr = h->last ? r2 : r1;
}

static int ctl_get_rx_status(void *ctx) { return F->get_rx_status(FC(ctx)); }
static void ctl_harq_reset(void *ctx)   { F->harq_reset(FC(ctx)); }
static void ctl_set_harq(void *ctx, int on) { F->set_harq(FC(ctx), on); }
static void ctl_unsync(void *ctx)
{
    ctl_t *h = ctx;
    F->unsync(h->fdv);
    P->unsync(h->psk);
    h->nin = h->nheld = 0;
}
static void ctl_set_crc_seed(void *ctx, uint16_t seed, int accept_plain)
{
    ctl_t *h = ctx;
    F->set_crc_seed(h->fdv, seed, accept_plain);
    P->set_crc_seed(h->psk, seed, accept_plain);
}
static int ctl_rx_crc_seeded(void *ctx)
{
    ctl_t *h = ctx;
    return h->last ? P->rx_crc_seeded(h->psk) : F->rx_crc_seeded(h->fdv);
}
static int ctl_get_freq_offset(void *ctx, float *hz)
{
    ctl_t *h = ctx;
    return h->last ? 0 : F->get_freq_offset(h->fdv, hz);
}

const modem_backend_t modem_backend_ctl = {
    .name             = "control",
    .open             = ctl_open,
    .close            = ctl_close,
    .configure        = ctl_configure,
    .bits_per_frame   = ctl_bits_per_frame,
    .n_tx_samples     = ctl_n_tx_samples,
    .n_max_rx_samples = ctl_n_max_rx_samples,
    .n_nom_samples    = ctl_n_nom_samples,
    .sample_rate      = ctl_sample_rate,
    .bandwidth_hz     = ctl_bandwidth_hz,
    .get_mode         = ctl_get_mode,
    .frames_per_burst = ctl_frames_per_burst,
    .preamble_tx      = ctl_preamble_tx,
    .rawdata_tx       = ctl_rawdata_tx,
    .postamble_tx     = ctl_postamble_tx,
    .nin              = ctl_nin,
    .rawdata_rx       = ctl_rawdata_rx,
    .get_stats        = ctl_get_stats,
    .get_rx_status    = ctl_get_rx_status,
    .harq_reset       = ctl_harq_reset,
    .set_harq         = ctl_set_harq,
    .unsync           = ctl_unsync,
    .set_crc_seed     = ctl_set_crc_seed,
    .rx_crc_seeded    = ctl_rx_crc_seeded,
    .get_freq_offset  = ctl_get_freq_offset,
};
