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
       (Rhizomatica/codec2), on the 2.0 geometry DATAC17 shares: 36 carriers
       (2250 Hz, the 2300 Hz channel), a 4 ms guard and a pilot every 8
       symbols.  Against 33 carriers and a pilot every 5: 3286 bit/s, not
       2941, from the same 14 dB on white noise and 21 dB on CCIR Moderate
       (utils/rung_sweep, 30 bursts a point).  1185-byte frames. */
    config->ns = 8;
    config->np = 16;
    config->tcp = 0.004;
    config->ts = 0.016;
    config->nc = 36;
    /* No edge pilots: with them the 36 carriers would be 38, 312.5 to 2625
       Hz, and under CCIR fading they made no difference that 40 bursts a
       point could tell (34 carriers with them against 36 without). */
    config->edge_pilots = 0;
    config->bps = 4;
    config->txtbits = 0;
    config->nuwbits = 42 * 4;
    assert(config->nuwbits <= MAX_UW_BITS);
    config->bad_uw_errors = 50;
    config->ftwindowwidth = 80;
    config->state_machine = "data";
    config->amp_est_mode = 1;
    config->tx_bpf_en = true;
    /* The preamble detector's threshold, as DATAC1, DATAC3 and DATAC17: at
       the default 0.30 a fade on the preamble and the postamble lost the
       frame, at any S/N (utils/rung_sweep, 40 bursts: CCIR Poor 33/40
       found, CCIR Moderate 37/40, the unique word never the cause).  At
       0.10 all 40 are found; QAM16C2W, QAM16N1 and QAM16N2 take it from
       here.  No trigger on 600 s of white noise or on DATAC1/3/4/16/17
       frames. */
    config->timing_mx_thresh = 0.10f;
    /* Clipped, as every other data mode: unclipped it peaks 11.4 dB over its
       average, so a peak-limited transmitter sent it 4.7 dB weaker than
       DATAC17 at the same setting.  Drive 2.0 and no make-up gain bring the
       peak-to-average ratio to 6.7 dB for about 1 dB of decoder loss
       (docs/CLIPPED-QAM16C2-FINDINGS.md); harder drives lose under fading.
       A receiver decodes either, but its SNR reading is mapped by
       SNR_MAP_QAM16C2 (freedv_700.c), fitted on frames clipped at this
       drive: a change of clip_gain1 or clip_gain2 needs the map refitted. */
    config->clip_en = true;
    config->clip_gain1 = 2.0;
    config->clip_gain2 = 1.0;
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
    config->tx_bpf_proto = filtP1150S1350;
    config->tx_bpf_proto_n = sizeof(filtP1150S1350) / sizeof(float);
  } else if (strcmp(mode, "datac17") == 0) {
    /* Mercury custom mode: intermediate-SNR fast payload mode.  Same
       big rate-0.6 H_16200_9720 codeword as qam16c2 but QPSK with TX
       clipping.  The 2.0 geometry: 36 carriers (2250 Hz), a 4 ms guard
       (6 before) and a pilot every 8 symbols (every 5 before), each lever
       measured alone and together on white noise and CCIR Moderate and
       Poor (utils/rung_sweep): 1740 bit/s, not 1362, at the same 8 dB on
       white noise and 12 to 13 dB faded.  A 160-bit unique word makes its
       frame 1186 bytes, distinct from qam16c2's 1185 for the ARQ
       mode-inference-by-size path. */
    config->ns = 8;
    config->np = 32;
    config->tcp = 0.004;
    config->ts = 0.016;
    config->nc = 36;
    config->edge_pilots = 0;
    config->txtbits = 0;
    config->state_machine = "data";
    config->ftwindowwidth = 80;
    config->timing_mx_thresh = 0.10;
    config->codename = "H_16200_9720";
    config->amp_est_mode = 1;
    config->nuwbits = 160;
    assert(config->nuwbits <= MAX_UW_BITS);
    config->bad_uw_errors = 57;
    uint8_t uw17[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1,
                      0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    memset(config->tx_uw, 0, config->nuwbits);
    memcpy(config->tx_uw, uw17, sizeof(uw17));
    memcpy(&config->tx_uw[config->nuwbits - sizeof(uw17)], uw17, sizeof(uw17));
    config->data_mode = "streaming";
    config->amp_scale = 145E3;
    config->clip_gain1 = 2.7;
    config->clip_gain2 = 0.8;
    /* Its 36 carriers span 375-2562.5 Hz: filtP1150S1350 holds the outer
       pair within 0.1 dB, where filtP1100S1300 took 1 dB off the lowest
       (and filtP900S1100 6.5 dB off the outer pair of the 33 carriers
       before). */
    config->tx_bpf_proto = filtP1150S1350;
    config->tx_bpf_proto_n = sizeof(filtP1150S1350) / sizeof(float);
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
    /* 0.40, down from the inherited 0.45: buys ~1.5-2 dB of preamble
       acquisition and delivered frames at ~0% noise-only false syncs.
       Measured on v1.9.22 with the corrected instrument (see
       docs/ACQ-TUNING-FINDINGS.md for the tables, the 0.35/0.30 knee, and
       the offset negative result). */
    config->timing_mx_thresh = 0.40;
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
    /* 0.40, down from the inherited 0.45: buys ~1.5-2 dB of preamble
       acquisition and delivered frames at ~0% noise-only false syncs.
       Measured on v1.9.22 with the corrected instrument (see
       docs/ACQ-TUNING-FINDINGS.md for the tables, the 0.35/0.30 knee, and
       the offset negative result). */
    config->timing_mx_thresh = 0.40;
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
  } else if (strcmp(mode, "datac17w") == 0 || strcmp(mode, "qam16c2w") == 0) {
    /* BW2750's top rungs: DATAC17 and QAM16C2 on 44 carriers, 125 to 2812.5
       Hz, for radios whose transmit passband is wider than 300-2700 Hz (an
       FT-710's 50-3050).  2089 and 3897 bit/s, 20 % over their BW2300
       versions, for 1 dB on white noise and CCIR Moderate and 2 on CCIR Poor
       (utils/rung_sweep x17:44,26 and xq:44,13, 40 bursts a point). */
    bool qam = strcmp(mode, "qam16c2w") == 0;
    ofdm_init_mode(qam ? "qam16c2" : "datac17", config);
    strcpy(config->mode, mode);
    config->nc = 44;
    config->np = qam ? 13 : 26;
    config->nuwbits = qam ? 168 : 160;                 /* 1169 / 1170 bytes */
    config->bad_uw_errors = config->nuwbits / 4;
    uint8_t uw[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1,
                    0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    memset(config->tx_uw, 0, MAX_UW_BITS);
    memcpy(config->tx_uw, uw, sizeof(uw));
    memcpy(&config->tx_uw[config->nuwbits - sizeof(uw)], uw, sizeof(uw));
    config->tx_bpf_proto = filtP1400S1600;
    config->tx_bpf_proto_n = sizeof(filtP1400S1600) / sizeof(float);
  } else if (strcmp(mode, "datan1") == 0 || strcmp(mode, "qam16n1") == 0 ||
             strcmp(mode, "qam16n2") == 0) {
    /* BW500's rungs above DATAC3: DATAC3's 9 carriers (562 Hz), a 4 ms guard
       and a pilot every 8 symbols, on 3072-bit repeat-accumulate codes
       (utils/ldpc_ira_peg.py).  QPSK at 2/3 (467 bit/s), 16-QAM at 1/2 and
       2/3 (639, 868), against DATAC3's 296; white noise from 2, 6 and 8 dB.
       In 500 Hz a fade takes the whole band: CCIR Moderate needs 10, 14 and
       21 dB (utils/rung_sweep x:9,24 and xq:9,12, 40 bursts a point). */
    bool qam = strcmp(mode, "datan1") != 0;
    ofdm_init_mode(qam ? "qam16c2" : "datac1", config);
    strcpy(config->mode, mode);
    config->nc = 9;
    config->np = qam ? 12 : 24;
    config->ns = 8;
    config->tcp = 0.004;
    config->edge_pilots = 0;
    config->codename = strcmp(mode, "qam16n1") == 0 ? "H_IRA_3072_1536" : "H_IRA_3072_2048";
    /* 243-byte payloads for datan1, 179 qam16n1, 242 qam16n2 (its word 8
       bits longer): every frame size distinct, as Mercury tells modes apart
       by it. */
    config->nuwbits = strcmp(mode, "qam16n2") == 0 ? 48 : 40;
    config->bad_uw_errors = config->nuwbits == 48 ? 8 : 6;
    uint8_t uw[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1,
                    0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    memset(config->tx_uw, 0, MAX_UW_BITS);
    memcpy(config->tx_uw, uw, sizeof(uw));
    config->tx_bpf_proto = filtP400S600;
    config->tx_bpf_proto_n = sizeof(filtP400S600) / sizeof(float);
    config->amp_scale = 300E3;
    config->clip_gain1 = 2.2;
  } else {
    assert(0);
  }
  config->rs = 1.0f / config->ts;
}
