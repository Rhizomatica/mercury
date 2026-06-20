/*---------------------------------------------------------------------------*\

  FILE........: ofdm_mode.c
  AUTHORS.....: David Rowe & Steve Sampson
  DATE CREATED: July 2020

  Mode specific configuration for OFDM modem.

\*---------------------------------------------------------------------------*/

#include <assert.h>
#include <string.h>

#include "codec2_ofdm.h"
#include "ofdm_internal.h"

void ofdm_init_mode(char mode[], struct OFDM_CONFIG *config) {
  assert(mode != NULL);
  assert(config != NULL);

  assert(strlen(mode) < 16);
  strcpy(config->mode, mode);

  /* Fill in default values - 700D */

  config->nc = 17; /* Number of carriers */
  config->np = 1;
  config->ns = 8; /* Number of Symbols per modem frame */
  config->ts = 0.018f;
  config->tcp = .002f;         /* Cyclic Prefix duration */
  config->tx_centre = 1500.0f; /* TX Carrier Frequency */
  config->rx_centre = 1500.0f; /* RX Carrier Frequency */
  config->fs = 8000.0f;        /* Sample rate */
  config->txtbits = 4;
  config->bps = 2; /* Bits per Symbol */
  config->nuwbits =
      5 * config->bps; /* default is 5 symbols of Unique Word bits */
  config->bad_uw_errors = 3;
  config->ftwindowwidth = 32;
  config->timing_mx_thresh = 0.30f;
  config->edge_pilots = 1;
  config->state_machine = "voice1";
  config->data_mode = "";
  config->codename = "HRA_112_112";
  config->clip_gain1 = 2.5;
  config->clip_gain2 = 0.8;
  config->clip_en = true;
  config->tx_bpf_en = true;
  config->tx_bpf_proto = filtP650S900;
  config->tx_bpf_proto_n = sizeof(filtP650S900) / sizeof(float);
  config->rx_bpf_en = false;
  config->amp_scale = 245E3;
  config->foff_limiter = false;
  config->EsNodB = 3.0;
  memset(config->tx_uw, 0, MAX_UW_BITS);

  if (strcmp(mode, "700D") == 0) {
  } else if (strcmp(mode, "700E") == 0) {
    config->ts = 0.014;
    config->tcp = 0.006;
    config->nc = 21;
    config->ns = 4;
    config->edge_pilots = 0;
    config->nuwbits = 12;
    config->bad_uw_errors = 3;
    config->txtbits = 2;
    config->state_machine = "voice2";
    config->amp_est_mode = 1;
    config->ftwindowwidth = 80;
    config->codename = "HRA_56_56";
    config->foff_limiter = true;
    config->amp_scale = 155E3;
    config->clip_gain1 = 3;
    config->clip_gain2 = 0.8;
    config->tx_bpf_proto = filtP900S1100;
    config->tx_bpf_proto_n = sizeof(filtP900S1100) / sizeof(float);
  } else if ((strcmp(mode, "2020") == 0)) {
    config->ts = 0.0205;
    config->nc = 31;
    config->codename = "HRAb_396_504";
    config->amp_scale = 167E3;
    config->clip_gain1 = 2.5;
    config->clip_gain2 = 0.8;
    config->tx_bpf_proto = filtP900S1100;
    config->tx_bpf_proto_n = sizeof(filtP900S1100) / sizeof(float);
  } else if (strcmp(mode, "2020B") == 0) {
    config->ts = 0.014;
    config->tcp = 0.004;
    config->nc = 29;
    config->ns = 5;
    config->codename = "HRA_56_56";
    config->txtbits = 4;
    config->nuwbits = 8 * 2;
    config->bad_uw_errors = 5;
    config->amp_scale = 130E3;
    config->clip_gain1 = 2.5;
    config->clip_gain2 = 0.8;
    config->edge_pilots = 0;
    config->state_machine = "voice2";
    config->ftwindowwidth = 64;
    config->foff_limiter = true;
    config->tx_bpf_proto = filtP1100S1300;
    config->tx_bpf_proto_n = sizeof(filtP1100S1300) / sizeof(float);
  } else if (strcmp(mode, "qam16c2") == 0) {
    /* QAM16 high-SNR data mode, ported from upstream dr-qam16-cport
       (Rhizomatica/codec2).  ~3100 bps payload in ~2.1 kHz, target
       90/100 MPP at +15 dB SNR. */
    config->ns = 5;
    config->np = 31;
    config->tcp = 0.004;
    config->ts = 0.016;
    config->nc = 33;
    config->bps = 4;
    config->txtbits = 0;
    config->nuwbits = 42 * 4;
    assert(config->nuwbits <= MAX_UW_BITS);
    config->bad_uw_errors = 50;
    config->ftwindowwidth = 80;
    config->state_machine = "data";
    config->amp_est_mode = 1;
    config->tx_bpf_en = false;
    config->clip_en = false;
    config->data_mode = "streaming";
    config->amp_scale = 135E3;
    config->rx_bpf_en = false;
    uint8_t uw[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1,
                    0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    memset(config->tx_uw, 0, config->nuwbits);
    memcpy(config->tx_uw, uw, sizeof(uw));
    memcpy(&config->tx_uw[config->nuwbits - sizeof(uw)], uw, sizeof(uw));
    config->EsNodB = 10;
    config->codename = "H_16200_9720";
    config->tx_bpf_proto = filtP1100S1300;
    config->tx_bpf_proto_n = sizeof(filtP1100S1300) / sizeof(float);
  } else if (strcmp(mode, "datac17") == 0) {
    /* Mercury custom mode: intermediate-SNR fast payload mode.  Same
       big rate-0.6 H_16200_9720 codeword as qam16c2 but QPSK with TX
       clipping — about 2x DATAC1 ARQ goodput at roughly +9 dB.  np=61
       shortens the codeword (1180 payload bytes, not 1213) so the
       frame size stays distinct from qam16c2 for the ARQ
       mode-inference-by-size path. */
    config->ns = 5;
    config->np = 61;
    config->tcp = 0.006;
    config->ts = 0.016;
    config->nc = 33;
    config->edge_pilots = 0;
    config->txtbits = 0;
    config->state_machine = "data";
    config->ftwindowwidth = 80;
    config->timing_mx_thresh = 0.10;
    config->codename = "H_16200_9720";
    config->amp_est_mode = 1;
    config->nuwbits = 168;
    assert(config->nuwbits <= MAX_UW_BITS);
    config->bad_uw_errors = 60;
    uint8_t uw17[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1,
                      0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    memset(config->tx_uw, 0, config->nuwbits);
    memcpy(config->tx_uw, uw17, sizeof(uw17));
    memcpy(&config->tx_uw[config->nuwbits - sizeof(uw17)], uw17, sizeof(uw17));
    config->data_mode = "streaming";
    config->amp_scale = 145E3;
    config->clip_gain1 = 2.7;
    config->clip_gain2 = 0.8;
    config->tx_bpf_proto = filtP900S1100;
    config->tx_bpf_proto_n = sizeof(filtP900S1100) / sizeof(float);
  } else if (strcmp(mode, "qam16") == 0) {
    /* not in use yet */
    config->ns = 5;
    config->np = 5;
    config->tcp = 0.004;
    config->ts = 0.016;
    config->nc = 33;
    config->bps = 4;
    config->txtbits = 0;
    config->nuwbits = 15 * 4;
    config->bad_uw_errors = 5;
    config->ftwindowwidth = 32;
    config->state_machine = "data";
    config->amp_est_mode = 1;
    config->tx_bpf_en = false;
    config->clip_en = false;
    config->data_mode = "streaming";
    config->tx_bpf_proto = NULL;
    config->tx_bpf_proto_n = 0;
  } else if (strcmp(mode, "datac0") == 0) {
    config->ns = 5;
    config->np = 4;
    config->tcp = 0.006;
    config->ts = 0.016;
    config->nc = 9;
    config->edge_pilots = 0;
    config->txtbits = 0;
    config->nuwbits = 32;
    config->bad_uw_errors = 9;
    config->state_machine = "data";
    config->amp_est_mode = 1;
    config->ftwindowwidth = 80;
    config->codename = "H_128_256_5";
    uint8_t uw[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    memcpy(config->tx_uw, uw, sizeof(uw));
    config->timing_mx_thresh = 0.08f;
    config->data_mode = "streaming";
    config->amp_scale = 300E3;
    config->clip_gain1 = 2.2;
    config->clip_gain2 = 0.85;
    config->tx_bpf_proto = filtP400S600;
    config->tx_bpf_proto_n = sizeof(filtP400S600) / sizeof(float);
  } else if (strcmp(mode, "datac1") == 0) {
    config->ns = 5;
    config->np = 38;
    config->tcp = 0.006;
    config->ts = 0.016;
    config->nc = 27;
    config->edge_pilots = 0;
    config->txtbits = 0;
    config->nuwbits = 16;
    config->bad_uw_errors = 6;
    config->state_machine = "data";
    config->amp_est_mode = 1;
    config->ftwindowwidth = 80;
    config->codename = "H_4096_8192_3d";
    uint8_t uw[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    assert(sizeof(uw) == config->nuwbits);
    memcpy(config->tx_uw, uw, config->nuwbits);
    config->timing_mx_thresh = 0.10f;
    config->data_mode = "streaming";
    config->amp_scale = 145E3;
    config->clip_gain1 = 2.7;
    config->clip_gain2 = 0.8;
    config->tx_bpf_proto = filtP900S1100;
    config->tx_bpf_proto_n = sizeof(filtP900S1100) / sizeof(float);
  } else if (strcmp(mode, "datac3") == 0) {
    config->ns = 5;
    config->np = 29;
    config->tcp = 0.006;
    config->ts = 0.016;
    config->nc = 9;
    config->edge_pilots = 0;
    config->txtbits = 0;
    config->state_machine = "data";
    config->ftwindowwidth = 80;
    config->timing_mx_thresh = 0.10;
    config->codename = "H_1024_2048_4f";
    config->amp_est_mode = 1;
    config->nuwbits = 40;
    config->bad_uw_errors = 10;
    uint8_t uw[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1,
                    0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    assert(sizeof(uw) <= MAX_UW_BITS);
    memcpy(config->tx_uw, uw, sizeof(uw));
    memcpy(&config->tx_uw[config->nuwbits - sizeof(uw)], uw, sizeof(uw));
    config->data_mode = "streaming";
    config->amp_scale = 300E3;
    config->clip_gain1 = 2.2;
    config->clip_gain2 = 0.8;
    config->tx_bpf_proto = filtP400S600;
    config->tx_bpf_proto_n = sizeof(filtP400S600) / sizeof(float);
  } else if (strcmp(mode, "datac4") == 0) {
    config->ns = 5;
    config->np = 47;
    config->tcp = 0.006;
    config->ts = 0.016;
    config->nc = 4;
    config->edge_pilots = 0;
    config->txtbits = 0;
    config->state_machine = "data";
    config->ftwindowwidth = 80;
    config->timing_mx_thresh = 0.5;
    config->codename = "H_1024_2048_4f";
    config->amp_est_mode = 1;
    config->nuwbits = 32;
    config->bad_uw_errors = 12;
    uint8_t uw[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1,
                    0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    assert(sizeof(uw) <= MAX_UW_BITS);
    memcpy(config->tx_uw, uw, sizeof(uw));
    memcpy(&config->tx_uw[config->nuwbits - sizeof(uw)], uw, sizeof(uw));
    config->data_mode = "streaming";
    config->amp_scale = 2 * 300E3;
    config->clip_gain1 = 1.2;
    config->clip_gain2 = 1.0;
    config->rx_bpf_en = true;
    config->tx_bpf_proto = filtP200S400;
    config->tx_bpf_proto_n = sizeof(filtP200S400) / sizeof(float);
  } else if (strcmp(mode, "datac13") == 0) {
    config->ns = 5;
    config->np = 18;
    config->tcp = 0.006;
    config->ts = 0.016;
    config->nc = 3;
    config->edge_pilots = 0;
    config->txtbits = 0;
    config->state_machine = "data";
    config->ftwindowwidth = 80;
    config->timing_mx_thresh = 0.45;
    config->codename = "H_256_512_4";
    config->amp_est_mode = 1;
    config->nuwbits = 48;
    config->bad_uw_errors = 18;
    uint8_t uw[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1,
                    0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    assert(sizeof(uw) <= MAX_UW_BITS);
    memcpy(config->tx_uw, uw, sizeof(uw));
    memcpy(&config->tx_uw[config->nuwbits - sizeof(uw)], uw, sizeof(uw));
    config->data_mode = "streaming";
    config->amp_scale = 2.5 * 300E3;
    config->clip_gain1 = 1.2;
    config->clip_gain2 = 1.0;
    config->rx_bpf_en = true;
    config->tx_bpf_proto = filtP200S400;
    config->tx_bpf_proto_n = sizeof(filtP200S400) / sizeof(float);
  } else if (strcmp(mode, "datac15") == 0) {
    /* Mercury custom mode: most robust payload mode.  Same narrowband
       geometry as datac13 but a full rate 1/3 H_256_768_22 codeword,
       targeting operation a few dB below datac4 (30 payload bytes). */
    config->ns = 5;
    config->np = 34;
    config->tcp = 0.006;
    config->ts = 0.016;
    config->nc = 3;
    config->edge_pilots = 0;
    config->txtbits = 0;
    config->state_machine = "data";
    config->ftwindowwidth = 80;
    config->timing_mx_thresh = 0.45;
    config->codename = "H_256_768_22";
    config->amp_est_mode = 1;
    config->nuwbits = 48;
    config->bad_uw_errors = 18;
    uint8_t uw[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1,
                    0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    assert(sizeof(uw) <= MAX_UW_BITS);
    memcpy(config->tx_uw, uw, sizeof(uw));
    memcpy(&config->tx_uw[config->nuwbits - sizeof(uw)], uw, sizeof(uw));
    config->data_mode = "streaming";
    config->amp_scale = 2.5 * 300E3;
    config->clip_gain1 = 1.2;
    config->clip_gain2 = 1.0;
    config->rx_bpf_en = true;
    config->tx_bpf_proto = filtP200S400;
    config->tx_bpf_proto_n = sizeof(filtP200S400) / sizeof(float);
  } else if (strcmp(mode, "datac16") == 0) {
    /* Mercury custom mode: most robust control mode (replaces datac13).
       H_256_768_22 shortened to 128 data + 512 parity bits (effective
       rate 0.2) — exactly 14 usable payload bytes like datac13. */
    config->ns = 5;
    config->np = 28;
    config->tcp = 0.006;
    config->ts = 0.016;
    config->nc = 3;
    config->edge_pilots = 0;
    config->txtbits = 0;
    config->state_machine = "data";
    config->ftwindowwidth = 80;
    config->timing_mx_thresh = 0.45;
    config->codename = "H_256_768_22";
    config->amp_est_mode = 1;
    config->nuwbits = 32;
    config->bad_uw_errors = 12;
    uint8_t uw[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1,
                    0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    assert(sizeof(uw) <= MAX_UW_BITS);
    memcpy(config->tx_uw, uw, sizeof(uw));
    memcpy(&config->tx_uw[config->nuwbits - sizeof(uw)], uw, sizeof(uw));
    config->data_mode = "streaming";
    config->amp_scale = 2.5 * 300E3;
    config->clip_gain1 = 1.2;
    config->clip_gain2 = 1.0;
    config->rx_bpf_en = true;
    config->tx_bpf_proto = filtP200S400;
    config->tx_bpf_proto_n = sizeof(filtP200S400) / sizeof(float);
  } else if (strcmp(mode, "datac14") == 0) {
    config->ns = 5;
    config->np = 4;
    config->tcp = 0.005;
    config->ts = 0.018;
    config->nc = 4;
    config->edge_pilots = 0;
    config->txtbits = 0;
    config->state_machine = "data";
    config->ftwindowwidth = 80;
    config->timing_mx_thresh = 0.45;
    config->codename = "HRA_56_56";
    config->amp_est_mode = 1;
    config->nuwbits = 32;
    config->bad_uw_errors = 12;
    uint8_t uw[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1,
                    0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    assert(sizeof(uw) <= MAX_UW_BITS);
    memcpy(config->tx_uw, uw, sizeof(uw));
    memcpy(&config->tx_uw[config->nuwbits - sizeof(uw)], uw, sizeof(uw));
    config->data_mode = "streaming";
    config->amp_scale = 2.0 * 300E3;
    config->clip_gain1 = 2.0;
    config->clip_gain2 = 1.0;
    config->rx_bpf_en = true;
    config->tx_bpf_proto = filtP200S400;
    config->tx_bpf_proto_n = sizeof(filtP200S400) / sizeof(float);
  } else {
    assert(0);
  }
  config->rs = 1.0f / config->ts;
}
