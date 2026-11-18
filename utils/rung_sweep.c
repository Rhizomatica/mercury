/* rung_sweep -- what would a rung carry, and at what radio setting?
 *
 * Measures one OFDM data mode, shipped or not yet built, as a peak-limited
 * transmitter sends it: every burst is normalised to one common peak, and the
 * axis is the S/N in 3 kHz of a steady carrier at that peak -- what an
 * operator has after setting the radio on a tune tone.  A mode with a higher
 * peak-to-average ratio goes out weaker for the same setting, so its row sits
 * further right; MFSK, one tone at a time, is the axis itself.
 *
 *   rung_sweep <mode> <trials> <lo> <hi> <step> [channel] [seed]
 *
 * <mode> is a shipped mode (datac1, datac3, datac17, qam16c2, ...) or a
 * candidate, built in codec2's custom slot with no change to the modem:
 *
 *   x:<carriers>,<np>,<LDPC code>[,<uw bits>]
 *
 * QPSK, 16 ms symbols and a 6 ms guard as DATAC1 and DATAC3; <np> modem
 * frames per packet.  The packet holds np * 4 * carriers * 2 bits: the unique
 * word, the code's parity bits, and as many of its data bits as are left (a
 * shortened code).  Its transmit filter follows the carrier count.  A
 * candidate keeps its base mode's drive into the clipper (amp_scale,
 * clip_gain1 and 2), except that one of 12 carriers or fewer gets the
 * narrow modes' (amp_scale 300E3, clip_gain1 2.2) on any base; so it is
 * clipped harder or softer than a shipped mode of its width would be.
 *
 * Per point: bursts delivered with their CRC, bursts acquired, the average
 * S/N the bursts had (the usual SNR3k), the S/N the modem reported for them
 * (what the rung ladder reads: a knob that distorts the frames, as the clip
 * does, shows here as a reading that falls away from the burst's S/N), and
 * the goodput: payload bits over the burst's airtime, times the share
 * delivered.  A candidate's reading is the raw estimate: the per-mode maps
 * (freedv_700.c) key on the mode, and the custom slot has none.
 *
 * Channels: awgn, or a chanutil preset (ccir-poor is MODES.md's MPP).
 *
 * RS_DIAG=1 adds, per point, how the receiver found the bursts: preamble
 * detections, postamble detections, and unique-word failures.  A burst never
 * detected at all is a detector miss; one detected and then dropped is the
 * unique word.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_common.h"
#include "chanutil.h"
#include "codec2_ofdm.h"
#include "filter.h"
#include "freedv_api.h"
#include "ldpc_codes.h"
#include "ofdm_internal.h"
#include "freedv_api_internal.h"

#define FS 8000.0
#define COMMON_PEAK 4000.0
/* A steady tone at the common peak, relative to which the axis is set. */
#define TONE_POWER (COMMON_PEAK * COMMON_PEAK / 2.0)

static bench_rng rngA, rngB;

static double seq_noise(void *ctx) { return bench_seq_next((bench_seq *)ctx); }

static struct OFDM_CONFIG custom;

/* x[17|q]:<nc>,<np>,<code>[,<nuw>[,<ns>[,<guard ms>[,<edge pilots>]]]] into codec2's custom
 * slot: on DATAC1's settings (x:), DATAC17's (x17:) or QAM16C2's (xq:, 16-QAM,
 * clipped, its estimator).  ns is the symbols from one pilot to the next
 * (5 in every shipped mode); the guard is the cyclic prefix (6 ms, QAM16C2's
 * 4); edge pilots 0 or 1 (an extra pilot carrier at each end, as QAM16C2
 * has).  Omitted (or 0 for ns and guard): the base mode's. */
static struct freedv *open_candidate(const char *spec) {
  static char code[64];
  int nc = 0, np = 0, nuw = 0, ns = 0, edge = -1;
  float guard = 0.0f;
  char *base = "datac1";
  const char *args = spec + 2;
  if (!strncmp(spec, "x17:", 4)) base = "datac17", args = spec + 4;
  else if (!strncmp(spec, "xq:", 3)) base = "qam16c2", args = spec + 3;
  if (sscanf(args, "%d,%d,%63[^,],%d,%d,%f,%d", &nc, &np, code, &nuw, &ns, &guard, &edge) < 3 || nc < 1 || np < 1)
    return NULL;

  ofdm_init_mode(base, &custom);
  strcpy(custom.mode, "custom");
  custom.nc = nc;
  custom.np = np;
  custom.codename = code;
  if (ns > 1) custom.ns = ns;
  if (guard > 0.0f) custom.tcp = guard / 1000.0f;
  if (edge == 0 || edge == 1) custom.edge_pilots = edge;

  /* What the library would assert on, as a usage error instead: an unknown
   * code, a packet too short for the code's parity and a CRC, or data bits
   * that are not whole bytes. */
  int ci = -1;   /* ldpc_codes_find asserts on a name it does not know */
  for (int i = 0; i < ldpc_codes_num(); i++)
    if (!strcmp(ldpc_codes[i].name, code)) ci = i;
  if (ci < 0) {
    fprintf(stderr, "unknown LDPC code '%s'\n", code);
    return NULL;
  }

  /* The unique word takes what the code leaves; the base mode's length (16
   * bits on DATAC1) unless told otherwise.  The 24-bit word goes at both
   * ends when there is room. */
  if (nuw <= 0) nuw = custom.nuwbits;
  if (nuw > MAX_UW_BITS || (nuw % custom.bps) != 0) return NULL;
  custom.nuwbits = nuw;
  custom.bad_uw_errors = nuw < 24 ? 6 : nuw / 4;
  int data = np * (custom.ns - 1) * nc * custom.bps - nuw - custom.txtbits - ldpc_codes[ci].NumberParityBits;
  int room = ldpc_codes[ci].CodeLength - ldpc_codes[ci].NumberParityBits;
  if (data <= 16 || data > room || data % 8) {
    fprintf(stderr, "%s: %d data bits in the packet (need 17 to %d, whole bytes)\n", spec, data, room);
    return NULL;
  }
  static const uint8_t uw[] = {1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1,
                               0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0};
  memset(custom.tx_uw, 0, MAX_UW_BITS);
  if (nuw >= 48) {
    memcpy(custom.tx_uw, uw, sizeof(uw));
    memcpy(&custom.tx_uw[nuw - (int)sizeof(uw)], uw, sizeof(uw));
  } else {
    memcpy(custom.tx_uw, uw, (size_t)(nuw < 24 ? nuw : 24));
  }

  /* Carriers are 62.5 Hz apart; the filters are named by half their
   * passband. */
  double half = nc * 62.5 / 2.0;
  if (half <= 400.0) {
    custom.tx_bpf_proto = filtP400S600;
    custom.tx_bpf_proto_n = 100;
    custom.amp_scale = 300E3;
    custom.clip_gain1 = 2.2;
  } else if (half <= 650.0) {
    custom.tx_bpf_proto = filtP650S900;
    custom.tx_bpf_proto_n = 100;
  } else if (half <= 900.0) {
    custom.tx_bpf_proto = filtP900S1100;
    custom.tx_bpf_proto_n = 100;
  } else if (half <= 1100.0) {
    custom.tx_bpf_proto = filtP1100S1300;
    custom.tx_bpf_proto_n = 100;
  } else if (half <= 1125.0) {
    custom.tx_bpf_proto = filtP1150S1350;
    custom.tx_bpf_proto_n = 100;
  } else {
    custom.tx_bpf_proto = filtP1400S1600;
    custom.tx_bpf_proto_n = 240;
  }

  struct freedv_advanced adv;
  memset(&adv, 0, sizeof(adv));
  adv.config = &custom;
  return freedv_open_advanced(FREEDV_MODE_DATA_CUSTOM, &adv);
}

static struct freedv *open_mode(const char *name) {
  if (name[0] == 'x' && strchr(name, ':')) return open_candidate(name);
  int m = bench_mode_from_name(name);
  return m < 0 ? NULL : freedv_open(m);
}

/* One burst from stream A, scaled to the common peak.  Returns its length
 * and its average power after scaling. */
static int make_burst(struct freedv *tx, short *burst, unsigned char *payload,
                      int nbytes, double *power) {
  bench_fill_payload(payload, nbytes, &rngA);
  int n = bench_make_burst(tx, burst, payload);
  double ps = 0.0, pk = 0.0;
  for (int i = 0; i < n; i++) {
    double v = (double)burst[i];
    ps += v * v;
    if (fabs(v) > pk) pk = fabs(v);
  }
  double g = pk > 0.0 ? COMMON_PEAK / pk : 1.0;
  for (int i = 0; i < n; i++) burst[i] = (short)lrint((double)burst[i] * g);
  *power = ps / n * g * g;
  return n;
}

int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr,
            "usage: %s <mode | x[17|q]:carriers,np,code[,uwbits[,ns[,guard ms]]]> <trials> <lo> <hi> <step> [channel] [seed]\n"
            "  the axis is the S/N in 3 kHz of a steady carrier at the transmitter's peak\n"
            "  channel: awgn (default) or a chanutil preset (ccir-poor = MPP)\n",
            argv[0]);
    return 1;
  }
  const char *modename = argv[1];
  int trials = atoi(argv[2]);
  double lo = atof(argv[3]), hi = atof(argv[4]), step = atof(argv[5]);
  const char *channame = argc > 6 ? argv[6] : "awgn";
  unsigned long long seed = argc > 7 ? strtoull(argv[7], NULL, 10) : 1357911131517ULL;
  if (trials <= 0 || step <= 0.0) return 1;

  int chan = CHAN_AWGN;
  if (strcmp(channame, "awgn") != 0 && (chan = chanutil_preset_from_name(channame)) < 0) {
    fprintf(stderr, "unknown channel '%s'\n", channame);
    return 1;
  }

  struct freedv *tx = open_mode(modename), *rx = tx ? open_mode(modename) : NULL;   /* one error line */
  if (!tx || !rx) {
    fprintf(stderr, "cannot open mode '%s'\n", modename);
    return 1;
  }
  freedv_set_frames_per_burst(tx, 1);
  freedv_set_frames_per_burst(rx, 1);

  int nbytes = freedv_get_bits_per_modem_frame(tx) / 8;
  int nburst = freedv_get_n_tx_preamble_modem_samples(tx) + freedv_get_n_tx_modem_samples(tx) +
               freedv_get_n_tx_postamble_modem_samples(tx);
  double secs = (double)nburst / FS;
  double rate = (nbytes - 2) * 8.0 / secs;

  short *burst = malloc(sizeof(short) * (size_t)(nburst + 4));
  short *noisy = malloc(sizeof(short) * (size_t)(nburst + 4));
  unsigned char *payload = malloc((size_t)nbytes);
  unsigned char *out = malloc((size_t)nbytes + 8);
  if (!burst || !noisy || !payload || !out) return 1;

  /* How far under the carrier this mode goes out, over a few bursts; and for
   * a fading channel, the level constant of the carrier (measured S/N =
   * A - No, near noiseless as mfsk_channel_sweep.sh does).  Calibrated with
   * no fading: the model's paths have unit mean power, and through a fade a
   * short burst reads whatever the first seconds of the channel happen to be
   * (a 2.2 s candidate read 1.8 dB high on one channel, 1.0 dB low on the
   * next). */
  bench_rng_seed(&rngA, seed);
  double under = 0.0, a_tone = 0.0;
  for (int i = 0; i < 8; i++) {
    double p;
    int n = make_burst(tx, burst, payload, nbytes, &p);
    double u = 10.0 * log10(TONE_POWER / p);
    under += u / 8.0;
    if (chan != CHAN_AWGN) {
      chanutil_t *ck = chanutil_open(CHAN_AWGN, -30.0f, 1234u);
      float m0 = 0.0f;
      if (!ck) return 1;
      chanutil_run(ck, burst, n, &m0);
      chanutil_close(ck);
      a_tone += ((double)m0 - 30.0 + u) / 8.0;
    }
  }

  printf("%s: %d B payload, burst %.2f s, %.0f bit/s, %.1f dB under the carrier, channel %s, %d trials\n",
         modename, nbytes - 2, secs, rate, under,
         chan == CHAN_AWGN ? "awgn" : chanutil_preset_name(chan), trials);
  printf("  carrier S/N   burst S/N   %s   delivered   acquired   goodput\n",
         modename[0] == 'x' ? "  raw est. " : "modem reads");

  for (double s = lo; s <= hi + 0.001; s += step) {
    bench_rng_seed(&rngA, seed + 1);
    bench_rng_seed(&rngB, seed + 2);
    chanutil_t *ch = NULL;
    if (chan != CHAN_AWGN && !(ch = chanutil_open(chan, (float)(a_tone - s), 1234u))) return 1;

    int delivered = 0, acquired = 0, nest = 0;
    int pre0 = rx->ofdm->pre, post0 = rx->ofdm->post, uwf0 = rx->ofdm->uw_fails;
    double achieved = 0.0;
    float est = 0.0f;
    bench_seq tailseq = {0};
    for (int t = 0; t < trials; t++) {
      double p, sigma;
      int n = make_burst(tx, burst, payload, nbytes, &p);
      if (chan == CHAN_AWGN) {
        /* Noise in 3 kHz against the carrier: sigma^2 lies over FS/2. */
        sigma = sqrt(TONE_POWER / (pow(10.0, s / 10.0) * (3000.0 / (FS / 2.0))));
        for (int i = 0; i < n; i++) {
          double v = burst[i] + sigma * bench_rng_gauss(&rngA);
          noisy[i] = (short)lrint(v > 32767.0 ? 32767.0 : v < -32768.0 ? -32768.0 : v);
        }
        achieved += 10.0 * log10(p / (sigma * sigma * 3000.0 / (FS / 2.0)));
      } else {
        float m = 0.0f;
        chanutil_run(ch, burst, n, &m);
        memcpy(noisy, burst, sizeof(short) * (size_t)n);
        sigma = sqrt(p / (pow(10.0, m / 10.0) * (3000.0 / (FS / 2.0))));
        achieved += m;
      }
      bench_seq_init(&tailseq, &rngB, nburst + 8192);
      int saw, got;
      bench_run_burst(rx, noisy, n, nburst, sigma, out, seq_noise, &tailseq, &saw, &got, &est, &nest);
      bench_seq_free(&tailseq);
      acquired += saw ? 1 : 0;
      delivered += got ? 1 : 0;
    }
    if (ch) chanutil_close(ch);
    char reads[16] = "    -";
    if (nest) snprintf(reads, sizeof(reads), "%+7.1f", est / nest);
    printf("  %+7.1f     %+7.1f      %7s      %3d/%-3d     %3d/%-3d   %6.0f\n", s, achieved / trials, reads,
           delivered, trials, acquired, trials, rate * delivered / trials);
    if (getenv("RS_DIAG"))
      printf("      diag: pre %d post %d uw_fails %d\n", rx->ofdm->pre - pre0, rx->ofdm->post - post0,
             rx->ofdm->uw_fails - uwf0);
    fflush(stdout);
  }

  freedv_close(tx);
  freedv_close(rx);
  free(burst);
  free(noisy);
  free(payload);
  free(out);
  return 0;
}
