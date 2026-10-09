/*---------------------------------------------------------------------------*\

  FILE........: freedv_700.c
  AUTHOR......: David Rowe
  DATE CREATED: May 2020

  Functions that implement the various FreeDV 700 modes, and more generally
  OFDM data modes.

\*---------------------------------------------------------------------------*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec2.h"
#include "codec2_fdmdv.h"
#include "codec2_ofdm.h"
#include "comp_prim.h"
#include "debug_alloc.h"
#include "filter.h"
#include "fmfsk.h"
#include "freedv_api.h"
#include "freedv_api_internal.h"
#include "fsk.h"
#include "gp_interleaver.h"
#include "interldpc.h"
#include "ldpc_codes.h"
#include "mpdecode_core.h"
#include "ofdm_internal.h"
#include "varicode.h"

extern char *ofdm_statemode[];

void freedv_700c_open(struct freedv *f) {
  f->snr_squelch_thresh = 0.0;
  f->squelch_en = false;

  f->cohpsk = cohpsk_create();
  f->nin = f->nin_prev = COHPSK_NOM_SAMPLES_PER_FRAME;
  f->n_nat_modem_samples =
      COHPSK_NOM_SAMPLES_PER_FRAME;  // native modem samples as used by the
                                     // modem
  f->n_nom_modem_samples = f->n_nat_modem_samples * FREEDV_FS_8000 /
                           COHPSK_FS;  // number of samples after native samples
                                       // are interpolated to 8000 sps
  f->n_max_modem_samples =
      COHPSK_MAX_SAMPLES_PER_FRAME * FREEDV_FS_8000 / COHPSK_FS + 1;
  f->modem_sample_rate =
      FREEDV_FS_8000;  // note weird sample rate tamed by resampling
  f->clip_en = true;
  f->sz_error_pattern = cohpsk_error_pattern_size();
  f->test_frames_diversity = 1;

  f->ptFilter7500to8000 =
      (struct quisk_cfFilter *)MALLOC(sizeof(struct quisk_cfFilter));
  f->ptFilter8000to7500 =
      (struct quisk_cfFilter *)MALLOC(sizeof(struct quisk_cfFilter));
  quisk_filt_cfInit(f->ptFilter8000to7500, quiskFilt120t480,
                    sizeof(quiskFilt120t480) / sizeof(float));
  quisk_filt_cfInit(f->ptFilter7500to8000, quiskFilt120t480,
                    sizeof(quiskFilt120t480) / sizeof(float));

  f->speech_sample_rate = FREEDV_FS_8000;
  f->codec2 = codec2_create(CODEC2_MODE_700C);
  assert(f->codec2 != NULL);

  f->n_codec_frames = 2;
  f->n_speech_samples = f->n_codec_frames * codec2_samples_per_frame(f->codec2);
  f->bits_per_codec_frame = codec2_bits_per_frame(f->codec2);
  f->bits_per_modem_frame =
      f->n_codec_frames * codec2_bits_per_frame(f->codec2);
  assert(f->bits_per_modem_frame == COHPSK_BITS_PER_FRAME);

  f->tx_payload_bits =
      (uint8_t *)MALLOC(f->bits_per_modem_frame * sizeof(char));
  assert(f->tx_payload_bits != NULL);
  f->rx_payload_bits =
      (uint8_t *)MALLOC(f->bits_per_modem_frame * sizeof(char));
  assert(f->rx_payload_bits != NULL);
}

void freedv_comptx_700c(struct freedv *f, COMP mod_out[]) {
  int i;
  COMP tx_fdm[f->n_nat_modem_samples];
  int tx_bits[COHPSK_BITS_PER_FRAME];

  /* earlier modems used one bit per int for unpacked bits */
  for (i = 0; i < COHPSK_BITS_PER_FRAME; i++)
    tx_bits[i] = f->tx_payload_bits[i];

  /* optionally overwrite the codec bits with test frames */
  if (f->test_frames) {
    cohpsk_get_test_bits(f->cohpsk, tx_bits);
  }

  /* cohpsk modulator */
  cohpsk_mod(f->cohpsk, tx_fdm, tx_bits, COHPSK_BITS_PER_FRAME);

  float gain = 1.0;
  if (f->clip_en) {
    cohpsk_clip(tx_fdm, COHPSK_CLIP, COHPSK_NOM_SAMPLES_PER_FRAME);
    gain = 2.5;
  }
  for (i = 0; i < f->n_nat_modem_samples; i++)
    mod_out[i] = fcmult(gain * COHPSK_SCALE, tx_fdm[i]);
  i = quisk_cfInterpDecim((complex float *)mod_out, f->n_nat_modem_samples,
                          f->ptFilter7500to8000, 16, 15);
}

// open function for OFDM voice modes
void freedv_ofdm_voice_open(struct freedv *f, char *mode) {
  f->snr_squelch_thresh = 0.0;
  f->squelch_en = false;
  struct OFDM_CONFIG *ofdm_config =
      (struct OFDM_CONFIG *)calloc(1, sizeof(struct OFDM_CONFIG));
  assert(ofdm_config != NULL);
  ofdm_init_mode(mode, ofdm_config);

  f->ofdm = ofdm_create(ofdm_config);
  assert(f->ofdm != NULL);
  free(ofdm_config);

  ofdm_config = ofdm_get_config_param(f->ofdm);
  f->ofdm_bitsperpacket = ofdm_get_bits_per_packet(f->ofdm);
  f->ofdm_bitsperframe = ofdm_get_bits_per_frame(f->ofdm);
  f->ofdm_nuwbits = ofdm_config->nuwbits;
  f->ofdm_ntxtbits = ofdm_config->txtbits;

  f->ldpc = (struct LDPC *)MALLOC(sizeof(struct LDPC));
  assert(f->ldpc != NULL);

  ldpc_codes_setup(f->ldpc, f->ofdm->codename);
  ldpc_mode_specific_setup(f->ofdm, f->ldpc);
#ifdef __EMBEDDED__
  f->ldpc->max_iter = LDPC_MAX_ITER_DEFAULT; /* early exit on parity; a deadline bounds slow hosts */
#endif
  int Nsymsperpacket = ofdm_get_bits_per_packet(f->ofdm) / f->ofdm->bps;
  f->rx_syms = (COMP *)MALLOC(sizeof(COMP) * Nsymsperpacket);
  assert(f->rx_syms != NULL);
  f->rx_amps = (float *)MALLOC(sizeof(float) * Nsymsperpacket);
  assert(f->rx_amps != NULL);
  for (int i = 0; i < Nsymsperpacket; i++) {
    f->rx_syms[i].real = f->rx_syms[i].imag = 0.0;
    f->rx_amps[i] = 0.0;
  }

  f->nin = f->nin_prev = ofdm_get_samples_per_frame(f->ofdm);
  f->n_nat_modem_samples = ofdm_get_samples_per_frame(f->ofdm);
  f->n_nom_modem_samples = ofdm_get_samples_per_frame(f->ofdm);
  f->n_max_modem_samples = ofdm_get_max_samples_per_frame(f->ofdm);
  f->modem_sample_rate = f->ofdm->config.fs;
  f->clip_en = false;
  f->sz_error_pattern = f->ofdm_bitsperframe;

  f->tx_bits = NULL; /* not used for 700D */

  f->speech_sample_rate = FREEDV_FS_8000;
  f->codec2 = codec2_create(CODEC2_MODE_700C);
  assert(f->codec2 != NULL);
  /* should be exactly an integer number of Codec 2 frames in a OFDM modem frame
   */
  assert((f->ldpc->data_bits_per_frame % codec2_bits_per_frame(f->codec2)) ==
         0);

  f->n_codec_frames =
      f->ldpc->data_bits_per_frame / codec2_bits_per_frame(f->codec2);
  f->n_speech_samples = f->n_codec_frames * codec2_samples_per_frame(f->codec2);
  f->bits_per_codec_frame = codec2_bits_per_frame(f->codec2);
  f->bits_per_modem_frame = f->n_codec_frames * f->bits_per_codec_frame;

  f->tx_payload_bits = (unsigned char *)MALLOC(f->bits_per_modem_frame);
  assert(f->tx_payload_bits != NULL);
  f->rx_payload_bits = (unsigned char *)MALLOC(f->bits_per_modem_frame);
  assert(f->rx_payload_bits != NULL);

  /* attenuate audio 12dB as channel noise isn't that pleasant */
  f->passthrough_gain = 0.25;

  /* should all add up to a complete frame */
  assert((ofdm_config->ns - 1) * ofdm_config->nc * ofdm_config->bps ==
         f->ldpc->coded_bits_per_frame + ofdm_config->txtbits +
             f->ofdm_nuwbits);
}

// open function for OFDM data modes, TODO consider moving to a new
// (freedv_ofdm_data.c) file
void freedv_ofdm_data_open(struct freedv *f, struct freedv_advanced *adv) {
  struct OFDM_CONFIG ofdm_config;
  char mode[32];
  if (f->mode == FREEDV_MODE_DATAC0) strcpy(mode, "datac0");
  if (f->mode == FREEDV_MODE_DATAC1) strcpy(mode, "datac1");
  if (f->mode == FREEDV_MODE_DATAC3) strcpy(mode, "datac3");
  if (f->mode == FREEDV_MODE_DATAC4) strcpy(mode, "datac4");
  if (f->mode == FREEDV_MODE_DATAC13) strcpy(mode, "datac13");
  if (f->mode == FREEDV_MODE_DATAC14) strcpy(mode, "datac14");
  if (f->mode == FREEDV_MODE_DATAC15) strcpy(mode, "datac15");
  if (f->mode == FREEDV_MODE_DATAC16) strcpy(mode, "datac16");
  if (f->mode == FREEDV_MODE_DATAC17) strcpy(mode, "datac17");
  if (f->mode == FREEDV_MODE_QAM16C2) strcpy(mode, "qam16c2");
  if (f->mode == FREEDV_MODE_DATA_CUSTOM) {
    assert(adv != NULL);
    assert(adv->config != NULL);
    memcpy(&ofdm_config, (struct OFDM_CONFIG *)adv->config,
           sizeof(struct OFDM_CONFIG));
  } else {
    ofdm_init_mode(mode, &ofdm_config);
  }
  f->ofdm = ofdm_create(&ofdm_config);
  assert(f->ofdm != NULL);

  // LDPC set up
  f->ldpc = (struct LDPC *)MALLOC(sizeof(struct LDPC));
  assert(f->ldpc != NULL);
  ldpc_codes_setup(f->ldpc, f->ofdm->codename);
  ldpc_mode_specific_setup(f->ofdm, f->ldpc);
#ifdef __EMBEDDED__
  f->ldpc->max_iter = LDPC_MAX_ITER_DEFAULT; /* early exit on parity; a deadline bounds slow hosts */
#endif

  // useful constants
  f->ofdm_bitsperpacket = ofdm_get_bits_per_packet(f->ofdm);
  f->ofdm_bitsperframe = ofdm_get_bits_per_frame(f->ofdm);
  f->ofdm_nuwbits = ofdm_config.nuwbits;
  f->ofdm_ntxtbits = ofdm_config.txtbits;

  /* payload bits per FreeDV API "frame".  In OFDM modem nomenclature this is
     the number of payload data bits per packet, or the number of data bits in
     a LDPC codeword */
  f->bits_per_modem_frame = f->ldpc->data_bits_per_frame;

  // buffers for received symbols for one packet/LDPC codeword - may span many
  // OFDM modem frames
  int Nsymsperpacket = ofdm_get_bits_per_packet(f->ofdm) / f->ofdm->bps;
  f->rx_syms = (COMP *)MALLOC(sizeof(COMP) * Nsymsperpacket);
  assert(f->rx_syms != NULL);
  f->rx_amps = (float *)MALLOC(sizeof(float) * Nsymsperpacket);
  assert(f->rx_amps != NULL);
  for (int i = 0; i < Nsymsperpacket; i++) {
    f->rx_syms[i].real = f->rx_syms[i].imag = 0.0;
    f->rx_amps[i] = 0.0;
  }

  f->nin = f->nin_prev = ofdm_get_nin(f->ofdm);
  f->n_nat_modem_samples = ofdm_get_samples_per_packet(f->ofdm);
  f->n_nom_modem_samples = ofdm_get_samples_per_frame(f->ofdm);
  /* in burst mode we might jump a preamble frame */
  f->n_max_modem_samples = 2 * ofdm_get_max_samples_per_frame(f->ofdm);
  f->modem_sample_rate = f->ofdm->config.fs;
  f->sz_error_pattern = f->ofdm_bitsperpacket;

  // Note inconsistency: freedv API modem "frame" is a OFDM modem packet
  f->tx_payload_bits = (unsigned char *)MALLOC(f->bits_per_modem_frame);
  assert(f->tx_payload_bits != NULL);
  f->rx_payload_bits = (unsigned char *)MALLOC(f->bits_per_modem_frame);
  assert(f->rx_payload_bits != NULL);

  /* HARQ soft-combining buffer (sized to the full coded packet; the data
   * payload LLR count is always <= ofdm_bitsperpacket).  Disabled by default. */
  f->harq_llr = (float *)MALLOC(sizeof(float) * f->ofdm_bitsperpacket);
  assert(f->harq_llr != NULL);
  f->harq_llr_nbits = 0;
  f->harq_enable = 0;
  f->ldpc_budget_ms = 0.0f;
  f->llr_calibrated = 1;
  f->llr_esno_db_used = f->ofdm->EsNodB;
  f->harq_valid = 0;
  f->harq_ncopies = 0;
}

/* speech or raw data, complex OFDM modulation out */
void freedv_comptx_ofdm(struct freedv *f, COMP mod_out[]) {
  int i, k;
  int nspare;

  /* Generate Varicode txt bits (if used), waren't protected by FEC */
  nspare = f->ofdm_ntxtbits;
  /* Mercury's custom data modes carry no text bits (ofdm_ntxtbits == 0); a
   * zero-length VLA is undefined behaviour (UBSan aborts on it).  Size to at
   * least 1 — the loop below and ofdm_ldpc_interleave_tx() only touch the
   * first nspare elements, so txt_bits[0] is never read when nspare == 0. */
  uint8_t txt_bits[nspare > 0 ? nspare : 1];

  for (k = 0; k < nspare; k++) {
    if (f->nvaricode_bits == 0) {
      /* get new char and encode */
      char s[2];
      if (f->freedv_get_next_tx_char != NULL) {
        s[0] = (*f->freedv_get_next_tx_char)(f->callback_state);
        f->nvaricode_bits =
            varicode_encode(f->tx_varicode_bits, s, VARICODE_MAX_BITS, 1,
                            f->varicode_dec_states.code_num);
        f->varicode_bit_index = 0;
      }
    }
    if (f->nvaricode_bits) {
      txt_bits[k] = f->tx_varicode_bits[f->varicode_bit_index++];
      f->nvaricode_bits--;
    } else
      txt_bits[k] = 0;
  }

  /* optionally replace payload bits with test frames known to rx */
  if (f->test_frames) {
    uint8_t payload_data_bits[f->bits_per_modem_frame];
    ofdm_generate_payload_data_bits(payload_data_bits, f->bits_per_modem_frame);

    for (i = 0; i < f->bits_per_modem_frame; i++) {
      f->tx_payload_bits[i] = payload_data_bits[i];
    }
  }

  /* OK now ready to LDPC encode, interleave, and OFDM modulate */
  ofdm_ldpc_interleave_tx(f->ofdm, f->ldpc, (complex float *)mod_out,
                          f->tx_payload_bits, txt_bits);
}

int freedv_comprx_700c(struct freedv *f, COMP demod_in_8kHz[]) {
  int i;
  int sync;

  int rx_status = 0;

  // quisk_cfInterpDecim() modifies input data so lets make a copy just in
  // case there is no sync and we need to echo input to output

  // freedv_nin(f): input samples at Fs=8000 Hz
  // f->nin: input samples at Fs=7500 Hz

  COMP demod_in[freedv_nin(f)];

  for (i = 0; i < freedv_nin(f); i++) demod_in[i] = demod_in_8kHz[i];

  i = quisk_cfInterpDecim((complex float *)demod_in, freedv_nin(f),
                          f->ptFilter8000to7500, 15, 16);

  for (i = 0; i < f->nin; i++)
    demod_in[i] = fcmult(1.0 / COHPSK_SCALE, demod_in[i]);

  float rx_soft_bits[COHPSK_BITS_PER_FRAME];

  cohpsk_demod(f->cohpsk, rx_soft_bits, &sync, demod_in, &f->nin);

  for (i = 0; i < f->bits_per_modem_frame; i++)
    f->rx_payload_bits[i] = rx_soft_bits[i] < 0.0f;

  f->sync = sync;
  cohpsk_get_demod_stats(f->cohpsk, &f->stats);
  f->snr_est = f->stats.snr_est;

  if (sync) {
    rx_status = FREEDV_RX_SYNC;
    if (f->test_frames == 0) {
      rx_status |= FREEDV_RX_BITS;
    } else {
      if (f->test_frames_diversity) {
        /* normal operation - error pattern on frame after diveristy
         * combination
         */
        short error_pattern[COHPSK_BITS_PER_FRAME];
        int bit_errors;

        /* test data, lets see if we can sync to the test data sequence */

        char rx_bits_char[COHPSK_BITS_PER_FRAME];
        for (i = 0; i < COHPSK_BITS_PER_FRAME; i++)
          rx_bits_char[i] = rx_soft_bits[i] < 0.0;
        cohpsk_put_test_bits(f->cohpsk, &f->test_frame_sync_state,
                             error_pattern, &bit_errors, rx_bits_char, 0);
        if (f->test_frame_sync_state) {
          f->total_bit_errors += bit_errors;
          f->total_bits += COHPSK_BITS_PER_FRAME;
          if (f->freedv_put_error_pattern != NULL) {
            (*f->freedv_put_error_pattern)(f->error_pattern_callback_state,
                                           error_pattern,
                                           COHPSK_BITS_PER_FRAME);
          }
        }
      } else {
        /* calculate error pattern on uncombined carriers - test mode to spot
           any carrier specific issues like tx passband filtering */

        short error_pattern[2 * COHPSK_BITS_PER_FRAME];
        char rx_bits_char[COHPSK_BITS_PER_FRAME];
        int bit_errors_lower, bit_errors_upper;

        /* lower group of carriers */

        float *rx_bits_lower = cohpsk_get_rx_bits_lower(f->cohpsk);
        for (i = 0; i < COHPSK_BITS_PER_FRAME; i++) {
          rx_bits_char[i] = rx_bits_lower[i] < 0.0;
        }
        cohpsk_put_test_bits(f->cohpsk, &f->test_frame_sync_state,
                             error_pattern, &bit_errors_lower, rx_bits_char, 0);

        /* upper group of carriers */

        float *rx_bits_upper = cohpsk_get_rx_bits_upper(f->cohpsk);
        for (i = 0; i < COHPSK_BITS_PER_FRAME; i++) {
          rx_bits_char[i] = rx_bits_upper[i] < 0.0;
        }
        cohpsk_put_test_bits(f->cohpsk, &f->test_frame_sync_state_upper,
                             &error_pattern[COHPSK_BITS_PER_FRAME],
                             &bit_errors_upper, rx_bits_char, 1);

        /* combine total errors and call callback */

        if (f->test_frame_sync_state && f->test_frame_sync_state_upper) {
          f->total_bit_errors += bit_errors_lower + bit_errors_upper;
          f->total_bits += 2 * COHPSK_BITS_PER_FRAME;
          if (f->freedv_put_error_pattern != NULL) {
            (*f->freedv_put_error_pattern)(f->error_pattern_callback_state,
                                           error_pattern,
                                           2 * COHPSK_BITS_PER_FRAME);
          }
        }
      }
    }
  }

  return rx_status;
}

/* Per-mode SNR-estimate calibration.
 *
 * ofdm_esno_est_calc() estimates Es/No from the spread of each symbol's
 * minor axis.  Its error depends on the mode, so the faster modes read low
 * while DATAC15 reads close to the truth; the ARQ_SNR_MIN_* thresholds are
 * defined on the true SNR3k scale.
 *
 * The error is not a constant: fixed offsets fitted at ~15 dB over-read
 * DATAC3 by 4-5 dB and DATAC1 by 3-4 dB at 0..4 dB, where the ARQ decides
 * most (on the bench a 1 dB link read 4.5 dB on DATAC3 and ~1 dB on DATAC4,
 * so the rung chosen changed what the SNR looked like).  So each mode gets a
 * line, true = a * raw + b, fitted against the codec2 ch.c AWGN reference
 * (SNR3k = -No - 14.82), 60 s of test frames per point, median estimate of
 * the frames that decoded:
 *
 *   mode      a       b      fitted over true   max residual
 *   DATAC15   1       0      (reference; -7..11 dB within 0.3)
 *   DATAC16   1.045  +0.29   -7.2..10.8 dB       0.38 dB
 *   DATAC4    1.071  +0.33   -5.8..10.2 dB       0.40 dB
 *   DATAC3    1.154  +0.37   -2.1.. 9.9 dB       0.16 dB
 *   DATAC1    1.220  -0.42    1.4.. 9.4 dB       0.38 dB
 *   DATAC17   1.604  -0.66    5.1..11.1 dB       0.11 dB
 *
 * Above ~12 dB the estimate stops rising.  On the same reference, up to
 * 22 dB (snr_calib_sweep.sh, 2026-10):
 *
 *   true SNR3k    DATAC15  DATAC16  DATAC4  DATAC3  DATAC17  DATAC1
 *   ~14 dB        14.3     13.9     15.1    12.6    14.4     14.3
 *   ~18 dB        17.7     16.3     17.1    14.6    15.3     18.5
 *   ~22 dB        20.2     17.3      --     15.5    16.4     22.0
 *
 * DATAC3 read 6 dB low at 22 dB.  Readings off DATAC17 never passed ~16.5
 * dB, so QAM16C2 (13 dB, doubted up to 16) was doubted on every link.
 * Above 10 dB a table of what each line's output read against the truth maps
 * it back.  The table is steep where the estimate is flat: one DATAC3 frame
 * at 22 dB read 13.6..17.9, about +-5 dB once mapped.  The carousel's moving
 * average narrows that, and the map stops at 25 dB.
 *
 * QAM16C2 uses ofdm_esno_est_dd() (decision-directed) instead.  On the
 * minor-axis estimator 16-QAM's own amplitude levels count as noise: it read
 * ~9 dB low raw and flattened past 20 dB (21.3 at 27.6).  The
 * decision-directed estimate rises to 27.6 dB and is mapped by a table of
 * its own.  It compresses near 8 dB, where QAM16C2 barely decodes.
 */
typedef struct { float x, y; } snr_knot_t;
static const snr_knot_t SNR_MAP_DATAC15[] = {{10.77f, 10.77f}, {14.34f, 14.90f}, {17.66f, 18.90f}, {20.17f, 22.90f}};
static const snr_knot_t SNR_MAP_DATAC16[] = {{10.72f, 10.72f}, {13.94f, 14.80f}, {16.34f, 18.80f}, {17.28f, 22.80f}};
static const snr_knot_t SNR_MAP_DATAC4[]  = {{12.00f, 12.00f}, {15.05f, 16.20f}, {17.07f, 20.20f}};
static const snr_knot_t SNR_MAP_DATAC3[]  = {{9.75f, 9.75f}, {12.64f, 13.87f}, {14.58f, 17.87f}, {15.49f, 21.87f}};
static const snr_knot_t SNR_MAP_DATAC1[]  = {{9.64f, 9.64f}, {14.33f, 13.43f}, {18.54f, 17.43f}, {22.04f, 21.43f}};
static const snr_knot_t SNR_MAP_DATAC17[] = {{11.60f, 11.60f}, {13.23f, 13.52f}, {14.41f, 15.52f}, {15.32f, 17.52f}, {16.40f, 21.52f}};
/* QAM16C2: the decision-directed estimate (raw) against the truth. */
static const snr_knot_t SNR_MAP_QAM16C2[] = {{8.62f, 6.58f}, {8.97f, 7.58f}, {9.41f, 8.58f}, {9.95f, 9.58f},
                                             {10.62f, 10.58f}, {11.38f, 11.58f}, {12.20f, 12.58f}, {13.10f, 13.58f},
                                             {14.98f, 15.58f}, {19.83f, 20.58f}, {26.37f, 27.58f}};
#define SNR_MAP_MAX_SLOPE 4.0f
#define SNR_MAP_MAX_DB    25.0f

/* Below the first knot: unit slope through it.  Between knots: linear.
 * Above the last: its last segment, no steeper than SNR_MAP_MAX_SLOPE,
 * and never past SNR_MAP_MAX_DB. */
static float snr_map(const snr_knot_t *k, int n, float x) {
  if (x <= k[0].x) return x + (k[0].y - k[0].x);
  for (int i = 1; i < n; i++)
    if (x <= k[i].x)
      return k[i - 1].y + (x - k[i - 1].x) * (k[i].y - k[i - 1].y) / (k[i].x - k[i - 1].x);
  float slope = (k[n - 1].y - k[n - 2].y) / (k[n - 1].x - k[n - 2].x);
  if (slope > SNR_MAP_MAX_SLOPE) slope = SNR_MAP_MAX_SLOPE;
  float y = k[n - 1].y + (x - k[n - 1].x) * slope;
  return y > SNR_MAP_MAX_DB ? SNR_MAP_MAX_DB : y;
}
#define SNR_MAP(t, x) snr_map((t), (int)(sizeof(t) / sizeof((t)[0])), (x))

static float freedv_snr_calib(int mode, float snr_raw) {
  switch (mode) {
    case FREEDV_MODE_DATAC16: return SNR_MAP(SNR_MAP_DATAC16, 1.045f * snr_raw + 0.29f);
    case FREEDV_MODE_DATAC4:  return SNR_MAP(SNR_MAP_DATAC4,  1.071f * snr_raw + 0.33f);
    case FREEDV_MODE_DATAC3:  return SNR_MAP(SNR_MAP_DATAC3,  1.154f * snr_raw + 0.37f);
    case FREEDV_MODE_DATAC1:  return SNR_MAP(SNR_MAP_DATAC1,  1.220f * snr_raw - 0.42f);
    case FREEDV_MODE_DATAC17: return SNR_MAP(SNR_MAP_DATAC17, 1.604f * snr_raw - 0.66f);
    case FREEDV_MODE_QAM16C2: return SNR_MAP(SNR_MAP_QAM16C2, snr_raw);
    case FREEDV_MODE_DATAC15: return SNR_MAP(SNR_MAP_DATAC15, snr_raw);
    default:                  return snr_raw;
  }
}

/*
  OFDM demod function that can support complex (float) or real (short)
  samples.  The real short samples are useful for low memory platforms such as
  the SM1000.
*/

int freedv_comp_short_rx_ofdm(struct freedv *f, void *demod_in_8kHz,
                              int demod_in_is_short, float gain) {
  int i, k;
  int n_ascii;
  char ascii_out;
  struct OFDM *ofdm = f->ofdm;
  struct LDPC *ldpc = f->ldpc;

  /* useful constants */
  int Nbitsperframe = ofdm_get_bits_per_frame(ofdm);
  int Nbitsperpacket = ofdm_get_bits_per_packet(ofdm);
  int Nsymsperframe = Nbitsperframe / ofdm->bps;
  int Nsymsperpacket = Nbitsperpacket / ofdm->bps;
  int Npayloadbitsperpacket = Nbitsperpacket - ofdm->nuwbits - ofdm->ntxtbits;
  int Npayloadsymsperpacket = Npayloadbitsperpacket / ofdm->bps;
  int Ndatabitsperpacket = ldpc->data_bits_per_frame;

  complex float *rx_syms = (complex float *)f->rx_syms;
  float *rx_amps = f->rx_amps;

  int rx_bits[Nbitsperframe];
  /* Text-free data modes have ofdm_ntxtbits == 0; avoid a zero-length VLA
   * (UB) — only the first ofdm_ntxtbits elements are ever accessed. */
  short txt_bits[f->ofdm_ntxtbits > 0 ? f->ofdm_ntxtbits : 1];
  COMP payload_syms[Npayloadsymsperpacket];
  float payload_amps[Npayloadsymsperpacket];

  int Nerrs_raw = 0;
  int Nerrs_coded = 0;
  int iter = 0;
  int parityCheckCount = 0;
  uint8_t rx_uw[f->ofdm_nuwbits];

  float new_gain = gain / f->ofdm->amp_scale;

  assert((demod_in_is_short == 0) || (demod_in_is_short == 1));

  int rx_status = 0;
  float EsNo = pow(10.0, ofdm->EsNodB / 10);
  f->sync = 0;

  /* looking for OFDM modem sync */
  if (ofdm->sync_state == search) {
    if (demod_in_is_short)
      ofdm_sync_search_shorts(f->ofdm, (short *)demod_in_8kHz, new_gain);
    else
      ofdm_sync_search(f->ofdm, (COMP *)demod_in_8kHz);
    f->snr_est = -5.0;
  }

  if ((ofdm->sync_state == synced) || (ofdm->sync_state == trial)) {
    /* OK we have OFDM modem sync */
    rx_status |= FREEDV_RX_SYNC;
    if (ofdm->sync_state == trial) rx_status |= FREEDV_RX_TRIAL_SYNC;
    if (demod_in_is_short)
      ofdm_demod_shorts(ofdm, rx_bits, (short *)demod_in_8kHz, new_gain);
    else
      ofdm_demod(ofdm, rx_bits, (COMP *)demod_in_8kHz);

    /* accumulate a buffer of data symbols for this packet */
    for (i = 0; i < Nsymsperpacket - Nsymsperframe; i++) {
      rx_syms[i] = rx_syms[i + Nsymsperframe];
      rx_amps[i] = rx_amps[i + Nsymsperframe];
    }
    memcpy(&rx_syms[Nsymsperpacket - Nsymsperframe], ofdm->rx_np,
           sizeof(complex float) * Nsymsperframe);
    memcpy(&rx_amps[Nsymsperpacket - Nsymsperframe], ofdm->rx_amp,
           sizeof(float) * Nsymsperframe);

    /* look for UW as frames enter packet buffer, note UW may span several
     * modem frames */
    int st_uw = Nsymsperpacket - ofdm->nuwframes * Nsymsperframe;
    ofdm_extract_uw(ofdm, &rx_syms[st_uw], &rx_amps[st_uw], rx_uw);

    // update some FreeDV API level stats
    f->sync = 1;

    if (ofdm->modem_frame == (ofdm->np - 1)) {
      /* we have received enough modem frames to complete packet and run LDPC
       * decoder */
      int txt_sym_index = 0;
      ofdm_disassemble_psk_modem_packet_with_text_amps(
          ofdm, rx_syms, rx_amps, payload_syms, payload_amps, txt_bits,
          &txt_sym_index);

      COMP payload_syms_de[Npayloadsymsperpacket];
      float payload_amps_de[Npayloadsymsperpacket];
      gp_deinterleave_comp(payload_syms_de, payload_syms,
                           Npayloadsymsperpacket);
      gp_deinterleave_float(payload_amps_de, payload_amps,
                            Npayloadsymsperpacket);

      float llr[Npayloadbitsperpacket];
      float llr_raw[Npayloadbitsperpacket];
      /* run_ldpc_decoder() writes the ENTIRE codeword (ldpc->CodeLength bytes)
       * into decoded_codeword, not just the payload; for Mercury's shortened
       * custom-mode LDPC codes CodeLength > Npayloadbitsperpacket, so a buffer
       * sized to the payload overflows (caught by ASan in run_ldpc_decoder).
       * Size to the codeword length — Mercury still reads only the first
       * Ndatabitsperpacket, so decode output is unchanged. */
      int codeword_len = ldpc->CodeLength > Npayloadbitsperpacket
                             ? ldpc->CodeLength : Npayloadbitsperpacket;
      uint8_t decoded_codeword[codeword_len];
      /* LLR scale.  The mode constant EsNodB (3 dB, 10 dB for the 16200-bit
       * modes) was right at one operating point only; the sum-product decoder
       * is scale-sensitive and the measured loss was 0.15-0.6 dB (tests/matlab
       * TestLdpc).  Use the receiver's Es/No estimate, passed through the
       * per-mode SNR calibration above and mapped back from SNR3k to Es/No by
       * subtracting the bandwidth + cyclic-prefix term, clamped to a sane range. */
      float EsNo_used = EsNo;
      if (f->llr_calibrated) {
        float raw_esno_db = (ofdm->bps == 4)
                                ? ofdm_esno_est_dd(ofdm->bps, rx_syms, rx_amps, Nsymsperpacket)
                                : ofdm_esno_est_calc(rx_syms, Nsymsperpacket);
        float snr3k_cal = freedv_snr_calib(f->mode, ofdm_snr_from_esno(ofdm, raw_esno_db));
        float esno_db = snr3k_cal - ofdm_snr_from_esno(ofdm, 0.0f);
        if (esno_db < -3.0f) esno_db = -3.0f;
        if (esno_db > 20.0f) esno_db = 20.0f;
        f->llr_esno_db_used = esno_db;
        EsNo_used = powf(10.0f, esno_db / 10.0f);
      } else {
        f->llr_esno_db_used = ofdm->EsNodB;
      }
      symbols_to_llrs(llr, payload_syms_de, payload_amps_de, EsNo_used,
                      ofdm->mean_amp, ofdm->bps, Npayloadsymsperpacket);
      /* Save this-transmission-only LLRs.  HARQ combining is applied BELOW only
       * as a fallback if the single-shot decode fails, so retained soft info
       * can never corrupt a frame that would have decoded on its own. */
      memcpy(llr_raw, llr, sizeof(float) * Npayloadbitsperpacket);
      ldpc->deadline_ns = f->ldpc_budget_ms > 0.0f
                              ? ldpc_now_ns() + (uint64_t)(f->ldpc_budget_ms * 1e6f)
                              : 0;
      ldpc_decode_frame(ldpc, &parityCheckCount, &iter, decoded_codeword, llr);
      memcpy(f->rx_payload_bits, decoded_codeword, Ndatabitsperpacket);

      if (strlen(ofdm->data_mode)) {
        int crc_class = freedv_crc16_class(f, f->rx_payload_bits, Ndatabitsperpacket);
        int crc_ok = crc_class != 0;
        /* HARQ Chase combining FALLBACK: only when the single-shot decode
         * failed, add the retained running sum of prior (same-frame) LLRs and
         * retry.  Independent noise realisations of the same codeword add
         * coherently, lifting effective Es/No; running this only on single-shot
         * failure means stale/cross-frame retained LLRs can never break an
         * otherwise-good frame (the cause of the on-air HARQ data regression). */
        if (!crc_ok && f->harq_enable && f->harq_valid &&
            f->harq_llr_nbits == Npayloadbitsperpacket) {
          float llr_comb[Npayloadbitsperpacket];
          /* Average (normalize by copy count) — do NOT raw-sum.  codec2 derives
           * LLRs from a fixed EsNodB (an uncalibrated scale), so summing N
           * copies inflates the magnitude ~Nx and over-drives the scale-
           * sensitive sum-product (phi0) LDPC decoder, losing combining gain on
           * the large codes (H_256_768_22, H_16200_9720) that every Mercury ARQ
           * mode uses.  Averaging keeps the magnitude in the decoder's
           * calibrated range while the independent per-copy noise still averages
           * down (~3 dB per doubling).  harq_llr holds the sum of harq_ncopies
           * prior copies; llr_raw is this copy, so divide by ncopies + 1. */
          /* With calibrated LLRs the right combine is the SUM (independent
           * observations of the same bit add in the log-likelihood domain);
           * averaging was a workaround for the uncalibrated fixed-EsNo scale
           * and is kept only for that case. */
          float norm = f->llr_calibrated ? 1.0f : 1.0f / (float)(f->harq_ncopies + 1);
          for (int i = 0; i < Npayloadbitsperpacket; i++)
            llr_comb[i] = (llr_raw[i] + f->harq_llr[i]) * norm;
          ldpc->deadline_ns = f->ldpc_budget_ms > 0.0f
                                  ? ldpc_now_ns() + (uint64_t)(f->ldpc_budget_ms * 1e6f)
                                  : 0;
          ldpc_decode_frame(ldpc, &parityCheckCount, &iter, decoded_codeword,
                            llr_comb);
          /* Integrity gate on the COMBINED decode — do NOT accept on CRC16
           * alone.  A Chase combine that has NOT converged still emits a
           * codeword, and that codeword still has a ~2^-16 chance of passing
           * CRC16 by itself.  On the single-shot path that stray pass is just a
           * rare undetected bit-error; on the combine path it is reached on
           * PURPOSE at the fade cliff (roughly half the combines fail to
           * converge there by design), and once burst_frames>1 lets the RX
           * admit more than one DATA frame per session such a forgery parses
           * in-window and is delivered as new data rather than retransmitted.
           * A converged LDPC decode satisfies essentially all of its
           * mother-code parity checks; a non-converged one sits near the ~50%
           * binomial floor.  So require the parity-check count to clear a wide
           * margin (90% of NumberParityBits) IN ADDITION to CRC16 before
           * adopting a combined result.  Deliberately NOT a strict all-checks
           * gate: at the marginal combining point residual errors can live in
           * parity bits, so a genuine decode need not reach 100% (measured e.g.
           * 4094/4096 on the OpenARQ reference) — 90% cleanly separates the
           * converged and garbage populations.  Single-shot decodes above are
           * untouched, so clean-channel goodput is unchanged. */
          if (ldpc_harq_combine_parity_ok(parityCheckCount,
                                          ldpc->NumberParityBits)) {
            memcpy(f->rx_payload_bits, decoded_codeword, Ndatabitsperpacket);
            crc_class = freedv_crc16_class(f, f->rx_payload_bits, Ndatabitsperpacket);
            crc_ok = crc_class != 0;
          }
        }
        if (!crc_ok && f->verbose) {
          int bytes_per_frame = (Ndatabitsperpacket + 7) / 8;
          uint8_t rx_bytes[bytes_per_frame];
          freedv_pack(rx_bytes, f->rx_payload_bits, Ndatabitsperpacket);
          uint16_t rx_crc16 =
              ((uint16_t)rx_bytes[bytes_per_frame - 2] << 8) |
              rx_bytes[bytes_per_frame - 1];
          uint16_t calc_crc16 =
              freedv_gen_crc16(rx_bytes, bytes_per_frame - 2);
          fprintf(stderr,
                  "OFDM data CRC fail: mode=%s bytes=%d rx_crc=%04x calc_crc=%04x "
                  "pcc=%d/%d iter=%d euw=%d foff=%4.1f\n",
                  ofdm->mode, bytes_per_frame, rx_crc16, calc_crc16,
                  parityCheckCount, ldpc->NumberParityBits, iter, ofdm->uw_errors,
                  (double)ofdm->foff_est_hz);
          if (f->verbose >= 2) {
            int n = bytes_per_frame < 16 ? bytes_per_frame : 16;
            fprintf(stderr, "  payload[0:%d] =", n);
            for (int bi = 0; bi < n; bi++) fprintf(stderr, " %02x", rx_bytes[bi]);
            fprintf(stderr, "\n");
          }
        } else if (crc_ok && f->verbose >= 2) {
          int bytes_per_frame = (Ndatabitsperpacket + 7) / 8;
          uint8_t rx_bytes[bytes_per_frame];
          freedv_pack(rx_bytes, f->rx_payload_bits, Ndatabitsperpacket);
          fprintf(stderr, "OFDM data OK: mode=%s bytes=%d pcc=%d/%d iter=%d\n",
                  ofdm->mode, bytes_per_frame, parityCheckCount,
                  ldpc->NumberParityBits, iter);
          int n = bytes_per_frame < 16 ? bytes_per_frame : 16;
          fprintf(stderr, "  payload[0:%d] =", n);
          for (int bi = 0; bi < n; bi++) fprintf(stderr, " %02x", rx_bytes[bi]);
          fprintf(stderr, "\n");
        }
        // we need a valid CRC to declare a data packet valid
        f->rx_crc_seeded = crc_class == 2;
        if (crc_ok)
          rx_status |= FREEDV_RX_BITS;
        else
          rx_status |= FREEDV_RX_BIT_ERRORS;

        /* HARQ: on success drop the retained soft info; on failure retain the
         * (already-combined) LLRs so the next retransmission combines with all
         * prior copies of this frame. */
        if (f->harq_enable) {
          if (crc_ok) {
            f->harq_valid = 0;
            f->harq_ncopies = 0;
          } else {
            /* Retain the running SUM of single-shot LLRs plus the copy count, so
             * the next attempt's fallback averages all prior copies of this
             * frame (see the averaging rationale at the combine site above). */
            if (f->harq_valid && f->harq_llr_nbits == Npayloadbitsperpacket) {
              for (int i = 0; i < Npayloadbitsperpacket; i++)
                f->harq_llr[i] += llr_raw[i];
              f->harq_ncopies++;
            } else {
              memcpy(f->harq_llr, llr_raw,
                     sizeof(float) * Npayloadbitsperpacket);
              f->harq_llr_nbits = Npayloadbitsperpacket;
              f->harq_ncopies = 1;
            }
            f->harq_valid = 1;
          }
        }
      } else {
        // voice modes aren't as strict - pass everything through to the
        // speech decoder, but flag frame with possible errors
        rx_status |= FREEDV_RX_BITS;
        if (parityCheckCount != ldpc->NumberParityBits)
          rx_status |= FREEDV_RX_BIT_ERRORS;
      }

      if (f->test_frames) {
        /* est uncoded BER from payload bits */
        Nerrs_raw =
            count_uncoded_errors(ldpc, &f->ofdm->config, payload_syms_de,
                                 payload_amps_de, strlen(ofdm->data_mode));
        f->total_bit_errors += Nerrs_raw;
        f->total_bits += Npayloadbitsperpacket;

        /* coded errors from decoded bits */
        uint8_t payload_data_bits[Ndatabitsperpacket];
        ofdm_generate_payload_data_bits(payload_data_bits, Ndatabitsperpacket);
        if (strlen(ofdm->data_mode)) {
          uint16_t tx_crc16 =
              freedv_crc16_unpacked(payload_data_bits, Ndatabitsperpacket - 16);
          uint8_t tx_crc16_bytes[] = {tx_crc16 >> 8, tx_crc16 & 0xff};
          freedv_unpack(payload_data_bits + Ndatabitsperpacket - 16,
                        tx_crc16_bytes, 16);
        }
        Nerrs_coded = count_errors(payload_data_bits, f->rx_payload_bits,
                                   Ndatabitsperpacket);
        f->total_bit_errors_coded += Nerrs_coded;
        f->total_bits_coded += Ndatabitsperpacket;
        if (Nerrs_coded) f->total_packet_errors++;
        f->total_packets++;
      }

      /* decode txt bits (if used) */
      for (k = 0; k < f->ofdm_ntxtbits; k++) {
        if (k % 2 == 0 && (f->freedv_put_next_rx_symbol != NULL)) {
          (*f->freedv_put_next_rx_symbol)(f->callback_state_sym,
                                          rx_syms[txt_sym_index],
                                          rx_amps[txt_sym_index]);
          txt_sym_index++;
        }
        n_ascii = varicode_decode(&f->varicode_dec_states, &ascii_out,
                                  &txt_bits[k], 1, 1);
        if (n_ascii && (f->freedv_put_next_rx_char != NULL)) {
          (*f->freedv_put_next_rx_char)(f->callback_state, ascii_out);
        }
      }

      ofdm_get_demod_stats(ofdm, &f->stats, rx_syms, Nsymsperpacket);
      if (ofdm->bps == 4)
        f->stats.snr_est = ofdm_snr_from_esno(
            ofdm, ofdm_esno_est_dd(ofdm->bps, rx_syms, rx_amps, Nsymsperpacket));
      f->stats.snr_est = freedv_snr_calib(f->mode, f->stats.snr_est);
      f->snr_est = f->stats.snr_est;
    } /* complete packet */

    if ((ofdm->np == 1) && (ofdm->modem_frame == 0)) {
      /* add in UW bit errors, useful in non-testframe,
         single modem frame per packet modes */
      for (i = 0; i < f->ofdm_nuwbits; i++) {
        if (rx_uw[i] != ofdm->tx_uw[i]) {
          f->total_bit_errors++;
        }
      }
      f->total_bits += f->ofdm_nuwbits;
    }
  }

  /* iterate state machine and update nin for next call */

  f->nin = ofdm_get_nin(ofdm);
  ofdm_sync_state_machine(ofdm, rx_uw);

  int print_full = 0;
  int print_truncated = 0;
  if (f->verbose &&
      ((rx_status & FREEDV_RX_BITS) || (rx_status & FREEDV_RX_BIT_ERRORS)))
    print_full = 1;
  if ((f->verbose == 2) &&
      !((rx_status & FREEDV_RX_BITS) || (rx_status & FREEDV_RX_BIT_ERRORS)))
    print_truncated = 1;
  if (print_full) {
    fprintf(stderr,
            "%3d nin: %4d st: %-6s euw: %2d %2d mf: %2d f: %5.1f pbw: %d snr: "
            "%4.1f eraw: %4d ecdd: %4d iter: %3d "
            "pcc: %4d rxst: %s\n",
            f->frames++, ofdm->nin, ofdm_statemode[ofdm->last_sync_state],
            ofdm->uw_errors, ofdm->sync_counter, ofdm->modem_frame,
            (double)ofdm->foff_est_hz, ofdm->phase_est_bandwidth,
            (double)f->snr_est, Nerrs_raw, Nerrs_coded, iter, parityCheckCount,
            rx_sync_flags_to_text[rx_status]);
  }
  if (print_truncated) {
    fprintf(stderr,
            "%3d nin: %4d st: %-6s euw: %2d %2d mf: %2d f: %5.1f pbw: %d       "
            "                                 "
            "             rxst: %s\n",
            f->frames++, ofdm->nin, ofdm_statemode[ofdm->last_sync_state],
            ofdm->uw_errors, ofdm->sync_counter, ofdm->modem_frame,
            (double)ofdm->foff_est_hz, ofdm->phase_est_bandwidth,
            rx_sync_flags_to_text[rx_status]);
  }

  return rx_status;
}
