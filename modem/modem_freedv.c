/* FreeDV modem backend — codec2/FreeDV behind the modem_backend_t vtable.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "modem_freedv.h"
#include "freedv_api.h"

/* ctx is the struct freedv *. */
#define FDV(ctx) ((struct freedv *)(ctx))

static void *fdv_open(int mode)
{
    /* FSK_LDPC needs an advanced open (was open_freedv_mode_locked in modem.c).
     * All OFDM data modes use the plain open. */
    if (mode == FREEDV_MODE_FSK_LDPC)
    {
        char codename[80] = "H_256_768_22";
        struct freedv_advanced adv = {0, 4, 50, 8000, 750, 250, codename};
        return freedv_open_advanced(mode, &adv);
    }
    return freedv_open(mode);
}

static void fdv_close(void *ctx)                 { if (ctx) freedv_close(FDV(ctx)); }

static void fdv_configure(void *ctx, int frames_per_burst, int verbosity)
{
    freedv_set_frames_per_burst(FDV(ctx), frames_per_burst);
    freedv_set_verbose(FDV(ctx), verbosity);
}

static int fdv_bits_per_frame(void *ctx)   { return freedv_get_bits_per_modem_frame(FDV(ctx)); }
static int fdv_n_tx_samples(void *ctx)     { return freedv_get_n_tx_modem_samples(FDV(ctx)); }
static int fdv_n_max_rx_samples(void *ctx) { return freedv_get_n_max_modem_samples(FDV(ctx)); }
static int fdv_n_nom_samples(void *ctx)    { return freedv_get_n_nom_modem_samples(FDV(ctx)); }
static int fdv_sample_rate(void *ctx)      { return freedv_get_modem_sample_rate(FDV(ctx)); }
static int fdv_get_mode(void *ctx)         { return freedv_get_mode(FDV(ctx)); }
static int fdv_frames_per_burst(void *ctx) { return freedv_get_frames_per_burst(FDV(ctx)); }

static int fdv_preamble_tx(void *ctx, int16_t *out)
{
    return freedv_rawdatapreambletx(FDV(ctx), out);
}

static int fdv_rawdata_tx(void *ctx, int16_t *out, const uint8_t *frame)
{
    freedv_rawdatatx(FDV(ctx), out, (unsigned char *)frame);
    return freedv_get_n_tx_modem_samples(FDV(ctx));
}

static int fdv_postamble_tx(void *ctx, int16_t *out)
{
    return freedv_rawdatapostambletx(FDV(ctx), out);
}

static int fdv_nin(void *ctx) { return freedv_nin(FDV(ctx)); }

static int fdv_rawdata_rx(void *ctx, uint8_t *bytes_out, const int16_t *demod_in)
{
    return (int)freedv_rawdatarx(FDV(ctx), bytes_out, (short *)demod_in);
}

static void fdv_get_stats(void *ctx, int *sync, float *snr)
{
    freedv_get_modem_stats(FDV(ctx), sync, snr);
}

static int  fdv_get_rx_status(void *ctx)      { return freedv_get_rx_status(FDV(ctx)); }
static void fdv_harq_reset(void *ctx)         { freedv_harq_reset(FDV(ctx)); }
static void fdv_set_harq(void *ctx, int on)   { freedv_set_harq(FDV(ctx), on); }
static void fdv_unsync(void *ctx)             { freedv_set_sync(FDV(ctx), FREEDV_SYNC_UNSYNC); }

const modem_backend_t modem_backend_freedv = {
    .name             = "freedv",
    .open             = fdv_open,
    .close            = fdv_close,
    .configure        = fdv_configure,
    .bits_per_frame   = fdv_bits_per_frame,
    .n_tx_samples     = fdv_n_tx_samples,
    .n_max_rx_samples = fdv_n_max_rx_samples,
    .n_nom_samples    = fdv_n_nom_samples,
    .sample_rate      = fdv_sample_rate,
    .get_mode         = fdv_get_mode,
    .frames_per_burst = fdv_frames_per_burst,
    .preamble_tx      = fdv_preamble_tx,
    .rawdata_tx       = fdv_rawdata_tx,
    .postamble_tx     = fdv_postamble_tx,
    .nin              = fdv_nin,
    .rawdata_rx       = fdv_rawdata_rx,
    .get_stats        = fdv_get_stats,
    .get_rx_status    = fdv_get_rx_status,
    .harq_reset       = fdv_harq_reset,
    .set_harq         = fdv_set_harq,
    .unsync           = fdv_unsync,
};
