/* Mercury modem-backend abstraction — a waveform-codec vtable.
 *
 * Mercury's datalink/ARQ layer is codec-agnostic: it hands the modem opaque
 * frames (payload + 2-byte CRC16) and gets frames back only when the CRC is
 * valid.  Historically the modem layer called codec2/FreeDV directly.  This
 * interface lets more than one modem architecture coexist behind one seam: a
 * backend turns frame bytes into int16 passband audio (TX) and audio back into
 * frame bytes + a sync/detect flag (RX).  FreeDV is one backend; the
 * non-coherent MFSK weak-signal mode is another.
 *
 * A backend is selected per mode (a plain mode int).  The two DSP funnels in
 * modem.c (send_modulated_data on TX, rx_decoder_consume_chunk on RX) dispatch
 * through the vtable; nothing above the modem layer knows which backend runs.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_MODEM_BACKEND_H
#define MERCURY_MODEM_BACKEND_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Per-mode codec instance: a backend + its opaque context (e.g. a struct
 * freedv * for the FreeDV backend, an mfsk_modem_t * for MFSK). */
typedef struct modem_backend modem_backend_t;

typedef struct {
    const modem_backend_t *be;
    void                  *ctx;
} modem_codec_t;

/* Every function takes the instance context. TX builders write int16 passband
 * samples and return the sample count; rawdata_rx returns decoded frame bytes
 * (0 when no CRC-valid frame is available yet). All rates are 8 kHz passband.
 *
 * The RX contract mirrors FreeDV's streaming model so the RX funnel is codec-
 * agnostic: nin() reports how many samples the backend wants next, rawdata_rx()
 * is fed that many and may buffer internally (a burst codec accumulates a whole
 * preamble+payload+postamble window before returning bytes). */
struct modem_backend {
    const char *name;

    /* lifecycle */
    void *(*open)(int mode);                 /* create instance for mode, or NULL */
    void  (*close)(void *ctx);
    void  (*configure)(void *ctx, int frames_per_burst, int verbosity);

    /* geometry */
    int   (*bits_per_frame)(void *ctx);      /* payload+CRC bits (bytes = /8) */
    int   (*n_tx_samples)(void *ctx);        /* samples per data frame */
    int   (*n_max_rx_samples)(void *ctx);    /* RX demod buffer sizing */
    int   (*n_nom_samples)(void *ctx);       /* nominal samples (settle time) */
    int   (*sample_rate)(void *ctx);         /* Hz (8000) */
    int   (*get_mode)(void *ctx);            /* the mode int */
    int   (*frames_per_burst)(void *ctx);

    /* TX: build int16 passband audio */
    int   (*preamble_tx)(void *ctx, int16_t *out);
    int   (*rawdata_tx)(void *ctx, int16_t *out, const uint8_t *frame);
    int   (*postamble_tx)(void *ctx, int16_t *out);

    /* RX */
    int   (*nin)(void *ctx);
    int   (*rawdata_rx)(void *ctx, uint8_t *bytes_out, const int16_t *demod_in);
    void  (*get_stats)(void *ctx, int *sync, float *snr);
    int   (*get_rx_status)(void *ctx);

    /* HARQ soft-combining across retransmissions (optional: leave NULL if
     * unsupported; the modem layer null-checks before calling). */
    void  (*harq_reset)(void *ctx);
    void  (*set_harq)(void *ctx, int enabled);

    /* Windowed ARQ: re-anchor the burst RX state machine from a just-decoded
     * frame's self-described "frames remaining in this keydown" (0 = burst
     * ends now).  Called by the modem RX after every decoded frame on a
     * burst-mode instance so a multi-frame keydown exits sync exactly at
     * end-of-burst.  Optional: NULL for backends without multi-frame bursts
     * (e.g. MFSK, which stays one frame per keydown). */
    void  (*set_frames_remaining)(void *ctx, int remaining);
};

static inline bool modem_codec_valid(const modem_codec_t *c)
{
    return c && c->be && c->ctx;
}

#endif /* MERCURY_MODEM_BACKEND_H */
