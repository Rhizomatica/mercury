#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bench_common.h"
#include "kiss_fft.h"

/* ---- RNG (xorshift64*, same family as the tools' original stream) ---- */

void bench_rng_seed(bench_rng *r, unsigned long long seed) {
  r->s = seed ? seed : 1ULL;
}

double bench_rng_uniform(bench_rng *r) {
  r->s = r->s * 6364136223846793005ULL + 1442695040888963407ULL;
  return (double)((r->s >> 11) & 0x1FFFFFFFFFFFFFULL) / (double)0x20000000000000ULL;
}

double bench_rng_gauss(bench_rng *r) {
  double u1 = bench_rng_uniform(r), u2 = bench_rng_uniform(r);
  if (u1 < 1e-300) u1 = 1e-300;
  return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ---- Fixed-length sequences ---- */

void bench_seq_init(bench_seq *s, bench_rng *rng, int len) {
  s->x = malloc(sizeof(double) * (size_t)len);
  s->len = len;
  s->pos = 0;
  if (s->x == NULL) {
    s->len = 0;
    return;
  }
  for (int i = 0; i < len; i++) s->x[i] = bench_rng_gauss(rng);
}

void bench_seq_free(bench_seq *s) {
  free(s->x);
  s->x = NULL;
  s->len = 0;
  s->pos = 0;
}

double bench_seq_next(bench_seq *s) {
  if (s->pos >= s->len) {
    /* The caller sized the sequence for the worst case (tail + 8192);
     * clamp rather than draw, so consumption stays receiver-independent. */
    s->pos = s->len;
    return 0.0;
  }
  return s->x[s->pos++];
}

/* ---- Burst construction ---- */

/* Fill `payload` with random bytes and the trailing CRC16, drawn from
 * `rng` -- the caller's stream A, so the count stays fixed per trial
 * ((nbytes - 2) draws). */
void bench_fill_payload(unsigned char *payload, int nbytes, bench_rng *rng) {
  for (int i = 0; i < nbytes - 2; i++)
    payload[i] = (unsigned char)(bench_rng_uniform(rng) * 256.0);
  unsigned short crc16 = freedv_gen_crc16(payload, nbytes - 2);
  payload[nbytes - 2] = (unsigned char)(crc16 >> 8);
  payload[nbytes - 1] = (unsigned char)(crc16 & 0xff);
}

/* preamble + payload + postamble, for a payload the caller already
 * prepared with bench_fill_payload.  Returns the burst length. */
int bench_make_burst(struct freedv *tx, short *burst,
                     const unsigned char *payload) {
  int n = 0;
  n += freedv_rawdatapreambletx(tx, burst + n);
  freedv_rawdatatx(tx, burst + n, (unsigned char *)payload);
  n += freedv_get_n_tx_modem_samples(tx);
  n += freedv_rawdatapostambletx(tx, burst + n);
  return n;
}

/* ---- Receiver loop ---- */

void bench_run_burst(struct freedv *rx, const short *samples, int n, int tail,
                     double sigma, unsigned char *out, bench_noise_fn noise,
                     void *noise_ctx, int *saw_sync, int *got_frame,
                     float *snr_est_sum, int *snr_est_n) {
  freedv_set_sync(rx, FREEDV_SYNC_UNSYNC);
  int pos = 0;
  *saw_sync = 0;
  *got_frame = 0;
  while (pos < n + tail) {
    int nin = freedv_nin(rx);
    short chunk[8192];
    if (nin > (int)(sizeof(chunk) / sizeof(chunk[0]))) break;
    for (int i = 0; i < nin; i++) {
      if (pos + i < n) {
        chunk[i] = samples[pos + i];
      } else {
        double v = sigma * noise(noise_ctx);
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        chunk[i] = (short)lrint(v);
      }
    }
    pos += nin;

    int nb = (int)freedv_rawdatarx(rx, out, chunk);
    int st = freedv_get_rx_status(rx);
    if (st & FREEDV_RX_SYNC) {
      *saw_sync = 1;
      if (snr_est_sum && snr_est_n) {
        int sync_i = 0;
        float se = 0.0f;
        freedv_get_modem_stats(rx, &sync_i, &se);
        *snr_est_sum += se;
        (*snr_est_n)++;
      }
    }
    if (nb > 0 && !(st & FREEDV_RX_BIT_ERRORS)) *got_frame = 1;
  }
}

void bench_run_noise_only(struct freedv *rx, double sigma, int n, int tail,
                          bench_seq *seq, int *saw_sync) {
  freedv_set_sync(rx, FREEDV_SYNC_UNSYNC);
  int pos = 0;
  *saw_sync = 0;
  while (pos < n + tail) {
    int nin = freedv_nin(rx);
    short chunk[8192];
    if (nin > (int)(sizeof(chunk) / sizeof(chunk[0]))) break;
    for (int i = 0; i < nin; i++) {
      double v = sigma * bench_seq_next(seq);
      if (v > 32767.0) v = 32767.0;
      if (v < -32768.0) v = -32768.0;
      chunk[i] = (short)lrint(v);
    }
    pos += nin;
    (void)freedv_rawdatarx(rx, NULL, chunk);
    int st = freedv_get_rx_status(rx);
    if (st & FREEDV_RX_SYNC) *saw_sync = 1;
  }
}

/* ---- Analytic frequency shift ---- */

#define SHIFT_GUARD 4096

void bench_freq_shift_real(short *buf, int n, double foff_hz, double fs) {
  if (foff_hz == 0.0 || n <= 0) return;

  int nfft = SHIFT_GUARD * 2 + n;
  int p = 8;
  while ((1 << p) < nfft) p++;
  nfft = 1 << p;

  /* Cache the plan: bursts within one tool run share a length, and the
   * allocation dominates at these sizes. */
  static kiss_fft_cfg fwd = NULL, inv = NULL;
  static int cached_nfft = 0;
  static kiss_fft_cpx *bufs = NULL; /* a, A, work: 3 x nfft */
  if (cached_nfft != nfft) {
    free(fwd);
    free(inv);
    free(bufs);
    fwd = kiss_fft_alloc(nfft, 0, NULL, NULL);
    inv = kiss_fft_alloc(nfft, 1, NULL, NULL);
    bufs = malloc(sizeof(kiss_fft_cpx) * 3 * (size_t)nfft);
    if (!fwd || !inv || !bufs) {
      free(fwd);
      free(inv);
      free(bufs);
      fwd = inv = NULL;
      bufs = NULL;
      cached_nfft = 0;
      return; /* caller keeps the unshifted burst */
    }
    cached_nfft = nfft;
  }

  kiss_fft_cpx *a = bufs, *A = bufs + nfft, *w = A + nfft;
  memset(a, 0, sizeof(kiss_fft_cpx) * (size_t)nfft);
  for (int i = 0; i < n; i++) {
    a[SHIFT_GUARD + i].r = (float)buf[i];
    a[SHIFT_GUARD + i].i = 0.0f;
  }

  kiss_fft(fwd, a, A);

  /* Analytic signal: zero the negative-frequency bins. */
  memset(&A[nfft / 2 + 1], 0, sizeof(kiss_fft_cpx) * (size_t)(nfft / 2 - 1));

  /* Circular shift by foff: delta bins = f * nfft / fs.  Shifted energy
   * that would wrap past DC lands in the zeroed negative half -- the zero
   * padding makes the wrap invisible to the signal band. */
  int dk = (int)lrint((double)foff_hz * (double)nfft / fs);
  for (int k = 0; k <= nfft / 2; k++) {
    int src = k - dk;
    if (src < 0 || src > nfft / 2) {
      w[k].r = 0.0f;
      w[k].i = 0.0f;
    } else {
      w[k] = A[src];
    }
  }

  kiss_fft(inv, w, a);

  double inv_scale = 1.0 / (double)nfft;
  for (int i = 0; i < n; i++) {
    double v = (double)a[SHIFT_GUARD + i].r * inv_scale;
    if (v > 32767.0) v = 32767.0;
    if (v < -32768.0) v = -32768.0;
    buf[i] = (short)lrint(v);
  }
}

/* ---- Mode lookup ---- */

int bench_mode_from_name(const char *s) {
  if (!strcmp(s, "DATAC16") || !strcmp(s, "datac16")) return FREEDV_MODE_DATAC16;
  if (!strcmp(s, "DATAC15") || !strcmp(s, "datac15")) return FREEDV_MODE_DATAC15;
  if (!strcmp(s, "DATAC13") || !strcmp(s, "datac13")) return FREEDV_MODE_DATAC13;
  if (!strcmp(s, "DATAC4") || !strcmp(s, "datac4")) return FREEDV_MODE_DATAC4;
  if (!strcmp(s, "DATAC3") || !strcmp(s, "datac3")) return FREEDV_MODE_DATAC3;
  if (!strcmp(s, "DATAC1") || !strcmp(s, "datac1")) return FREEDV_MODE_DATAC1;
  if (!strcmp(s, "DATAC0") || !strcmp(s, "datac0")) return FREEDV_MODE_DATAC0;
  if (!strcmp(s, "DATAC17") || !strcmp(s, "datac17")) return FREEDV_MODE_DATAC17;
  if (!strcmp(s, "QAM16C2") || !strcmp(s, "qam16c2")) return FREEDV_MODE_QAM16C2;
  return -1;
}
