#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nanorq.h"
#include "nanorq_core.h"
#include "nanorq_ops.h"
#include "partition.h"
#include "util.h"

#define NANORQ_TSNC_MAX_MATRIX_BYTES (256U * 1024U * 1024U)

static inline size_t div_ceil(size_t a, size_t b) {
  return a / b + (a % b != 0);
}

struct oti_common {
  size_t F;  /* input size in bytes */
  size_t T;  /* the symbol size in octets, which MUST be a multiple of Al */
  size_t Al; /* byte alignment, 0 < Al <= 8, 4 is recommended */
};

struct oti_scheme {
  size_t Z;  /* number of source blocks */
  size_t N;  /* number of sub-blocks in each source block */
  size_t Kt; /* the total number of symbols required to represent input */
};

struct source_block {
  size_t sbloc;
  size_t part_tot;
  struct partition part;
};

typedef struct {
  uint32_t esi;
  uint8_t *row;
  uint8_t *coefs; // Used for TSNC recoded symbols
} repair_sym;

typedef struct {
  repair_sym *a;
  size_t n;
  size_t m;
} repair_vec;

typedef struct {
  uint32_t *words;
  size_t size;
} compat_bitmask;

struct block_encoder {
  uint16_t K;
  bool loaded;
  bool inverted;

  // Core low level solver:
  nanorq_core core;

  // Dynamic scratch memory for core solver:
  uint8_t *prep_mem;
  size_t prep_len;
  uint8_t *work_mem;
  size_t work_len;

  uint8_t *D;      // Matrix data
  uint32_t stride; // Recommended row stride
  uint8_t *mix_buf;

  schedule S; // Operations schedule

  repair_vec repair_bin;
  compat_bitmask repair_mask;
};

struct nanorq {
  struct oti_common common;
  struct oti_scheme scheme;
  struct partition src_part; /* (KL, KS, ZL, ZS) = Partition[Kt, Z] */
  struct partition sub_part; /* (TL, TS, NL, NS) = Partition[T/Al, N] */
  params P;
  uint32_t max_esi;
  struct block_encoder *encoders[Z_max];

  // Cached precalculated components
  nanorq_core *precalc_core;
  uint8_t *precalc_prep_mem;
  uint8_t *precalc_work_mem;
  schedule *precalc_S;
};

static bool repair_vec_push(repair_vec *v, repair_sym val) {
  if (v->n >= v->m) {
    if (v->m > SIZE_MAX / 2)
      return false;
    size_t new_m = v->m == 0 ? 8 : v->m * 2;
    if (new_m > SIZE_MAX / sizeof(repair_sym))
      return false;
    repair_sym *new_a = (repair_sym *)realloc(v->a, new_m * sizeof(repair_sym));
    if (!new_a)
      return false;
    v->a = new_a;
    v->m = new_m;
  }
  v->a[v->n++] = val;
  return true;
}

static void repair_vec_free(repair_vec *v) {
  if (v->a) {
    for (size_t i = 0; i < v->n; i++) {
      obl_free(v->a[i].row);
      if (v->a[i].coefs) {
        obl_free(v->a[i].coefs);
      }
    }
    free(v->a);
  }
  v->a = NULL;
  v->n = v->m = 0;
}

static compat_bitmask compat_bitmask_new(size_t size) {
  compat_bitmask b = {0};
  if (size > SIZE_MAX - 31)
    return b;
  size_t num_words = (size + 31) / 32;
  if (num_words > SIZE_MAX / sizeof(uint32_t))
    return b;
  b.words = (uint32_t *)calloc(num_words, sizeof(uint32_t));
  if (b.words)
    b.size = size;
  return b;
}

static void compat_bitmask_free(compat_bitmask *b) {
  free(b->words);
  b->words = NULL;
  b->size = 0;
}

static void compat_bitmask_set(compat_bitmask *b, size_t bit) {
  if (bit < b->size) {
    b->words[bit / 32] |= (1U << (bit % 32));
  }
}

static bool compat_bitmask_check(compat_bitmask *b, size_t bit) {
  if (bit < b->size) {
    return (b->words[bit / 32] & (1U << (bit % 32))) != 0;
  }
  return false;
}

static void compat_bitmask_reset(compat_bitmask *b) {
  size_t num_words = (b->size + 31) / 32;
  if (num_words > 0 && b->words)
    memset(b->words, 0, num_words * sizeof(uint32_t));
}

static size_t compat_bitmask_gaps(compat_bitmask *b, size_t limit) {
  size_t gaps = 0;
  size_t check_limit = limit < b->size ? limit : b->size;
  size_t limit_words = check_limit / 32;
  size_t limit_rem = check_limit % 32;

  for (size_t i = 0; i < limit_words; i++) {
    gaps += 32 - __builtin_popcount(b->words[i]);
  }

  if (limit_rem > 0) {
    uint32_t mask = (1U << limit_rem) - 1;
    uint32_t last_word = b->words[limit_words] & mask;
    gaps += limit_rem - __builtin_popcount(last_word);
  }

  if (limit > b->size) {
    gaps += limit - b->size;
  }

  return gaps;
}

static struct oti_scheme gen_scheme_specific(struct oti_common *common, int K,
                                             int Z) {
  size_t Kn = K;
  struct oti_scheme ret = {0};
  ret.Kt = div_ceil(common->F, common->T);

  if (K == 0) {
    Kn = ret.Kt;
    if (Z == 0) {
      Z = 16; // if num_sbn's not specified default to at least this
      while (div_ceil(ret.Kt, Z) > K_max)
        Z++;
      while (div_ceil(ret.Kt, Z) < 10 && Z > 1)
        Z--;
    }
  }
  if (Z > 0 && K == 0) {
    Kn = div_ceil(ret.Kt, Z);
  }
  ret.Z = div_ceil(ret.Kt, Kn);
  ret.N = 1; // disable interleaving

  return ret;
}

// fill_partition extracted to lib/partition.c

static struct source_block get_source_block(nanorq *rq, uint8_t sbn,
                                            uint16_t symbol_size) {
  struct source_block blk;
  blk.part = rq->sub_part;
  blk.sbloc = 0;
  blk.part_tot = rq->sub_part.IL * rq->sub_part.JL;

  if (sbn < rq->src_part.JL) {
    blk.sbloc = sbn * rq->src_part.IL * symbol_size;
  } else if (sbn - rq->src_part.JL < rq->src_part.JS) {
    blk.sbloc = (rq->src_part.IL * rq->src_part.JL) * symbol_size +
                (sbn - rq->src_part.JL) * rq->src_part.IS * symbol_size;
  }

  return blk;
}

static size_t get_symbol_offset(struct source_block *blk, size_t pos,
                                uint16_t K, uint32_t esi) {
  size_t i;
  if (pos < blk->part_tot) {
    size_t sub_blk_id = pos / blk->part.IL;
    i = blk->sbloc + sub_blk_id * K * blk->part.IL + esi * blk->part.IL +
        pos % blk->part.IL;
  } else {
    size_t pos_part2 = pos - blk->part_tot;
    size_t sub_blk_id = pos_part2 / blk->part.IS;
    i = blk->sbloc + (blk->part_tot * K) + sub_blk_id * K * blk->part.IS +
        esi * blk->part.IS + pos_part2 % blk->part.IS;
  }
  return i;
}

static size_t transfer_esi(nanorq *rq, uint8_t sbn, uint32_t esi, uint16_t K,
                           uint8_t *ptr, size_t len, struct ioctx *io,
                           int out) {
  if (!rq || !ptr || !io || !io->seek || len < rq->common.T ||
      (out && !io->write) || (!out && !io->read))
    return SIZE_MAX;
  size_t transfer = 0;
  size_t col = 0;
  size_t symbol_size = rq->common.T / rq->common.Al;
  struct source_block blk = get_source_block(rq, sbn, symbol_size);
  for (size_t i = 0; i < symbol_size;) {
    size_t offset = get_symbol_offset(&blk, i, K, esi) * rq->common.Al;
    size_t sublen = (i < blk.part_tot) ? blk.part.IL : blk.part.IS;
    if (sublen == 0 || sublen > SIZE_MAX / rq->common.Al)
      return SIZE_MAX;
    size_t segment_len = sublen * rq->common.Al;
    size_t stride = segment_len;
    i += sublen;

    if (col > len || segment_len > len - col)
      return SIZE_MAX;
    if (offset >= rq->common.F) {
      col += segment_len;
      continue;
    }
    if (stride > rq->common.F - offset)
      stride = rq->common.F - offset;
    if (!io->seek(io, offset))
      return SIZE_MAX;
    size_t done = out ? io->write(io, ptr + col, stride)
                      : io->read(io, ptr + col, stride);
    if (done != stride)
      return SIZE_MAX;
    transfer += done;
    col += segment_len;
  }
  return transfer;
}

static struct block_encoder *get_block_encoder(nanorq *rq, uint8_t sbn) {
  if (!rq || sbn >= nanorq_blocks(rq))
    return NULL;
  if (rq->encoders[sbn])
    return rq->encoders[sbn];

  struct block_encoder *enc =
      (struct block_encoder *)calloc(1, sizeof(struct block_encoder));
  if (!enc)
    return NULL;
  enc->K = nanorq_block_symbols(rq, sbn);
  if (enc->K == 0) {
    free(enc);
    return NULL;
  }

  enc->repair_mask = compat_bitmask_new(enc->K);
  if (!enc->repair_mask.words) {
    free(enc);
    return NULL;
  }
  enc->stride = nanorq_core_recommended_stride(rq->common.T);
  if (enc->stride == 0) {
    compat_bitmask_free(&enc->repair_mask);
    free(enc);
    return NULL;
  }

  rq->encoders[sbn] = enc;
  return enc;
}

nanorq *nanorq_encoder_new_ex(size_t len, uint16_t T, uint16_t K, uint16_t Z,
                              uint8_t Al) {
  uint8_t alignments[] = {1, 2, 4, 8};

  if (len == 0 || len > NANORQ_MAX_TRANSFER)
    return NULL;

  for (int a = sizeof(alignments) - 1; a >= 0; a--) {
    if (Al >= alignments[a]) {
      Al = alignments[a];
      break;
    }
  }
  if (Al == 0)
    Al = 1;

  if (T < Al) {
    T = Al;
  } else {
    T -= T % Al;
  }

  const size_t max_symbols = (size_t)Z_max * K_max;
  if (div_ceil(len, T) > max_symbols) {
    size_t needed = div_ceil(len, max_symbols);
    size_t rem = needed % Al;
    if (rem != 0) {
      if (needed > UINT16_MAX - (Al - rem))
        return NULL;
      needed += Al - rem;
    }
    if (needed > UINT16_MAX)
      return NULL;
    T = (uint16_t)needed;
  }

  nanorq_core_init();

  nanorq *rq = (nanorq *)calloc(1, sizeof(nanorq));
  if (!rq)
    return NULL;
  rq->common.F = len;
  rq->common.T = T;
  rq->common.Al = Al;

  rq->scheme = gen_scheme_specific(&rq->common, K, Z);

  if (rq->scheme.Z == 0 || rq->scheme.N == 0 || rq->scheme.Z > Z_max ||
      rq->scheme.Kt > SIZE_MAX / rq->common.T ||
      div_ceil(rq->scheme.Kt, rq->scheme.Z) > K_max) {
    free(rq);
    return NULL;
  }

  rq->src_part = partition_fill(rq->scheme.Kt, rq->scheme.Z);
  rq->sub_part = partition_fill(rq->common.T / rq->common.Al, rq->scheme.N);
  rq->P = params_init(nanorq_block_symbols(rq, 0));

  rq->max_esi = (1 << 24) - 1;

  return rq;
}

nanorq *nanorq_encoder_new(size_t len, uint16_t T, uint8_t Al) {
  return nanorq_encoder_new_ex(len, T, 0, 0, Al);
}

void nanorq_free(nanorq *rq) {
  if (!rq)
    return;
  size_t num_sbn = nanorq_blocks(rq);
  for (size_t sbn = 0; sbn < num_sbn; sbn++)
    nanorq_encoder_cleanup(rq, sbn);

  if (rq->precalc_core) {
    if (rq->precalc_prep_mem)
      free(rq->precalc_prep_mem);
    if (rq->precalc_work_mem)
      free(rq->precalc_work_mem);
    if (rq->precalc_S) {
      if (rq->precalc_S->ops.a)
        free(rq->precalc_S->ops.a);
      free(rq->precalc_S);
    }
    free(rq->precalc_core);
  }

  free(rq);
}

uint64_t nanorq_oti_common(nanorq *rq) {
  if (!rq)
    return 0;
  uint64_t ret = 0;
  ret |= ((uint64_t)rq->common.F) << 24;
  ret |= (rq->common.T - 1) & 0xffff;
  return ret;
}

/* HERMES local addition -- see nanorq.h.  Same fields as nanorq_oti_common(),
 * written as 5 bytes little-endian instead of packed into a 64-bit word. */
uint8_t *nanorq_oti_common_reduced(nanorq *rq, uint8_t *buffer) {
  buffer[0] = rq->common.F & 0xff;
  buffer[1] = (rq->common.F >> 8) & 0xff;
  buffer[2] = (rq->common.F >> 16) & 0xff;
  buffer[3] = (rq->common.T - 1) & 0xff;
  buffer[4] = ((rq->common.T - 1) >> 8) & 0xff;
  return buffer;
}

uint32_t nanorq_oti_scheme_specific(nanorq *rq) {
  if (!rq)
    return 0;
  uint32_t ret = 0;
  ret |= (rq->scheme.Z - 1) << 24;
  ret |= (rq->scheme.N - 1) << 8;
  ret |= rq->common.Al;
  return ret;
}

/* HERMES local addition -- see nanorq.h.  Al is always 1 here, so it is not
 * transmitted; the receiver ORs it back in. */
uint8_t *nanorq_oti_scheme_specific_align1(nanorq *rq, uint8_t *buffer) {
  buffer[0] = rq->scheme.Z - 1;
  buffer[1] = (rq->scheme.N - 1) & 0xff;
  buffer[2] = ((rq->scheme.N - 1) >> 8) & 0xff;
  return buffer;
}

size_t nanorq_transfer_length(nanorq *rq) { return rq ? rq->common.F : 0; }

size_t nanorq_symbol_size(nanorq *rq) { return rq ? rq->common.T : 0; }

nanorq *nanorq_decoder_new(uint64_t common, uint32_t scheme) {
  uint64_t F = common >> 24;
  uint32_t encoded_T = (uint32_t)(common & 0xffff) + 1U;

  if (F == 0 || F > NANORQ_MAX_TRANSFER || F > SIZE_MAX ||
      (common & 0xff0000U) != 0 || encoded_T > UINT16_MAX)
    return NULL;

  nanorq_core_init();

  nanorq *rq = (nanorq *)calloc(1, sizeof(nanorq));
  if (!rq)
    return NULL;
  rq->common.F = F;
  rq->common.T = encoded_T;

  rq->scheme.Z = ((scheme >> 24) & 0x00ff) + 1;
  rq->scheme.N = ((scheme >> 8) & 0xffff) + 1;
  rq->common.Al = scheme & 0xff;
  rq->scheme.Kt = div_ceil(rq->common.F, rq->common.T);

  if (rq->scheme.Z == 0)
    rq->scheme.Z = Z_max;

  if (rq->scheme.N == 0) {
    rq->scheme.N = 1;
  }

  if (rq->common.Al == 0 || rq->common.Al > 8 ||
      (rq->common.Al & (rq->common.Al - 1)) != 0 ||
      rq->common.T < rq->common.Al || rq->common.T % rq->common.Al != 0) {
    free(rq);
    return NULL;
  }
  size_t symbols_per_subblock = rq->common.T / rq->common.Al;
  if (rq->scheme.N > UINT16_MAX || rq->scheme.N > symbols_per_subblock ||
      rq->scheme.Z > rq->scheme.Kt || rq->scheme.Kt > SIZE_MAX / rq->common.T ||
      div_ceil(div_ceil(rq->common.F, rq->common.T), rq->scheme.Z) > K_max) {
    free(rq);
    return NULL;
  }

  rq->src_part = partition_fill(rq->scheme.Kt, rq->scheme.Z);
  rq->sub_part = partition_fill(rq->common.T / rq->common.Al, rq->scheme.N);
  rq->P = params_init(nanorq_block_symbols(rq, 0));

  rq->max_esi = (1 << 24) - 1;
  return rq;
}

size_t nanorq_block_symbols(nanorq *rq, uint8_t sbn) {
  if (!rq)
    return 0;
  if (sbn < rq->src_part.JL)
    return rq->src_part.IL;
  if (sbn - rq->src_part.JL < rq->src_part.JS)
    return rq->src_part.IS;
  return 0;
}

size_t nanorq_max_blocks(nanorq *rq) {
  (void)rq;
  return Z_max;
}

size_t nanorq_blocks(nanorq *rq) {
  return rq ? (size_t)(rq->src_part.JL + rq->src_part.JS) : 0;
}

bool nanorq_precalculate(nanorq *rq) {
  if (!rq)
    return false;
  if (rq->precalc_core)
    return true; // Already precalculated

  uint16_t K = nanorq_block_symbols(rq, 0);
  nanorq_core *core = (nanorq_core *)calloc(1, sizeof(nanorq_core));
  uint8_t *prep_mem = NULL;
  uint8_t *work_mem = NULL;
  schedule *S = NULL;
  void *sched_mem = NULL;
  if (!core || !nanorq_core_encoder_new(K, 0, core))
    goto fail;

  size_t prep_len = nanorq_core_calculate_prepare_memory(core);
  prep_mem = (uint8_t *)malloc(prep_len);
  if (!prep_mem || !nanorq_core_prepare(core, prep_mem, prep_len))
    goto fail;

  size_t work_len = nanorq_core_calculate_work_memory(core);
  work_mem = (uint8_t *)malloc(work_len);
  S = (schedule *)calloc(1, sizeof(schedule));
  size_t sched_bytes = ops_estimate_schedule_bytes(K);
  sched_mem = malloc(sched_bytes);
  if (!work_mem || !S || !schedule_init(S, sched_mem, sched_bytes))
    goto fail;

  nanorq_core_set_op_callback(core, S, ops_push);
  if (!nanorq_core_precalculate(core, work_mem, work_len) || S->overflowed ||
      S->cpidx != 2)
    goto fail;

  rq->precalc_core = core;
  rq->precalc_prep_mem = prep_mem;
  rq->precalc_work_mem = work_mem;
  rq->precalc_S = S;
  return true;

fail:
  free(sched_mem);
  free(S);
  free(work_mem);
  free(prep_mem);
  free(core);
  return false;
}

bool nanorq_generate_symbols(nanorq *rq, uint8_t sbn, struct ioctx *io) {
  if (!rq || !io)
    return false;
  struct block_encoder *enc = get_block_encoder(rq, sbn);
  if (enc == NULL)
    return false;

  if (enc->inverted)
    return true;

  if (!enc->loaded) {
    if (!enc->D) {
      if (!nanorq_core_encoder_new(enc->K, 0, &enc->core)) {
        return false;
      }
      enc->D = (uint8_t *)obl_alloc(nanorq_core_get_pc_rows(&enc->core),
                                    enc->stride, nanorq_oblas.align_size);
      if (!enc->D) {
        return false;
      }
    }

    uint8_t *tmp_buf = (uint8_t *)malloc(rq->common.T);
    if (!tmp_buf)
      return false;

    for (int esi = 0; esi < enc->K; esi++) {
      memset(tmp_buf, 0, rq->common.T);
      size_t got =
          transfer_esi(rq, sbn, esi, enc->K, tmp_buf, rq->common.T, io, 0);
      if (got == SIZE_MAX) {
        free(tmp_buf);
        return false;
      }
      nanorq_core_place_symbol(&enc->core, enc->D, enc->stride, esi, tmp_buf,
                               rq->common.T);
    }
    free(tmp_buf);
    enc->loaded = true;
  }

  if (enc->K == nanorq_block_symbols(rq, 0) && rq->precalc_core) {
    ops_run(rq->precalc_core, enc->D, enc->stride, rq->precalc_S);
    enc->inverted = true;
    return true;
  }

  if (!nanorq_core_encoder_new(enc->K, 0, &enc->core))
    return false;
  size_t prep_len = nanorq_core_calculate_prepare_memory(&enc->core);
  uint8_t *prep_mem = (uint8_t *)malloc(prep_len);
  if (!prep_mem || !nanorq_core_prepare(&enc->core, prep_mem, prep_len)) {
    free(prep_mem);
    return false;
  }

  size_t work_len = nanorq_core_calculate_work_memory(&enc->core);
  uint8_t *work_mem = (uint8_t *)malloc(work_len);
  size_t sched_bytes = ops_estimate_schedule_bytes(enc->K);
  void *sched_mem = malloc(sched_bytes);
  schedule new_S = {0};
  if (!work_mem || !schedule_init(&new_S, sched_mem, sched_bytes)) {
    free(sched_mem);
    free(work_mem);
    free(prep_mem);
    return false;
  }

  nanorq_core_set_op_callback(&enc->core, &new_S, ops_push);
  if (!nanorq_core_precalculate(&enc->core, work_mem, work_len) ||
      new_S.overflowed || new_S.cpidx != 2) {
    free(sched_mem);
    free(work_mem);
    free(prep_mem);
    return false;
  }

  free(enc->prep_mem);
  free(enc->work_mem);
  free(enc->S.ops.a);
  enc->prep_mem = prep_mem;
  enc->prep_len = prep_len;
  enc->work_mem = work_mem;
  enc->work_len = work_len;
  enc->S = new_S;

  ops_run(&enc->core, enc->D, enc->stride, &enc->S);
  enc->inverted = true;

  return true;
}

size_t nanorq_encode(nanorq *rq, void *data, uint32_t esi, uint8_t sbn,
                     struct ioctx *io) {
  if (!rq || !data || !io)
    return 0;
  struct block_encoder *enc = get_block_encoder(rq, sbn);
  if (enc == NULL)
    return 0;

  if (esi < enc->K) {
    if (enc->inverted) {
      if (!enc->mix_buf)
        enc->mix_buf =
            (uint8_t *)obl_alloc(1, enc->stride, nanorq_oblas.align_size);
      if (!enc->mix_buf)
        return 0;
      ops_mix(&enc->core, enc->D, enc->stride, esi, enc->mix_buf);
      memcpy(data, enc->mix_buf, rq->common.T);
      return rq->common.T;
    } else {
      memset(data, 0, rq->common.T);
      if (transfer_esi(rq, sbn, esi, enc->K, (uint8_t *)data, rq->common.T, io,
                       0) == SIZE_MAX)
        return 0;
      return rq->common.T;
    }
  } else {
    if (esi > ((1 << 24) - 1))
      return 0;
    if (!enc->inverted) {
      if (!nanorq_generate_symbols(rq, sbn, io)) {
        return 0;
      }
    }
    if (!enc->mix_buf)
      enc->mix_buf =
          (uint8_t *)obl_alloc(1, enc->stride, nanorq_oblas.align_size);
    if (!enc->mix_buf)
      return 0;
    ops_mix(&enc->core, enc->D, enc->stride, esi, enc->mix_buf);
    memcpy(data, enc->mix_buf, rq->common.T);
    return rq->common.T;
  }
}

void nanorq_encoder_cleanup(nanorq *rq, uint8_t sbn) {
  if (!rq || sbn >= nanorq_blocks(rq))
    return;
  if (!rq->encoders[sbn])
    return;
  struct block_encoder *enc = rq->encoders[sbn];
  if (enc->D) {
    obl_free(enc->D);
  }
  obl_free(enc->mix_buf);
  if (enc->prep_mem) {
    free(enc->prep_mem);
  }
  if (enc->work_mem) {
    free(enc->work_mem);
  }
  if (enc->S.ops.a) {
    free(enc->S.ops.a);
  }
  repair_vec_free(&enc->repair_bin);
  compat_bitmask_free(&enc->repair_mask);
  free(enc);
  rq->encoders[sbn] = NULL;
}

void nanorq_encoder_reset(nanorq *rq, uint8_t sbn) {
  if (!rq || sbn >= nanorq_blocks(rq))
    return;
  if (!rq->encoders[sbn])
    return;
  struct block_encoder *enc = rq->encoders[sbn];
  enc->loaded = false;
  enc->inverted = false;
  if (enc->core.overhead != 0) {
    nanorq_core base_core = {0};
    if (nanorq_core_encoder_new(enc->K, 0, &base_core))
      enc->core = base_core;
  }
  if (enc->D) {
    nanorq_core_init_matrix(&enc->core, enc->D, enc->stride);
  }
  if (enc->prep_mem) {
    free(enc->prep_mem);
    enc->prep_mem = NULL;
  }
  if (enc->work_mem) {
    free(enc->work_mem);
    enc->work_mem = NULL;
  }
  if (enc->S.ops.a) {
    free(enc->S.ops.a);
    enc->S.ops.a = NULL;
  }
  repair_vec_free(&enc->repair_bin);
  compat_bitmask_reset(&enc->repair_mask);
}

bool nanorq_set_max_esi(nanorq *rq, uint32_t max_esi) {
  if (!rq || max_esi >= (1 << 24) || max_esi < rq->P.Kprime)
    return false;
  rq->max_esi = max_esi;
  return true;
}

static bool ensure_block_matrix(struct block_encoder *enc) {
  if (enc->D)
    return true;
  if (!nanorq_core_encoder_new(enc->K, 0, &enc->core))
    return false;
  enc->D = (uint8_t *)obl_alloc(nanorq_core_get_pc_rows(&enc->core),
                                enc->stride, nanorq_oblas.align_size);
  return enc->D != NULL;
}

int nanorq_decoder_add_symbol(nanorq *rq, void *data, uint32_t tag,
                              struct ioctx *io) {
  if (!rq || !data || !io)
    return NANORQ_SYM_ERR;
  uint8_t sbn = (tag >> 24) & 0xff;
  uint32_t esi = (tag & 0x00ffffff);

  struct block_encoder *dec = get_block_encoder(rq, sbn);
  if (dec == NULL || esi >= (1 << 24) || esi > rq->max_esi)
    return NANORQ_SYM_ERR;

  if (compat_bitmask_gaps(&dec->repair_mask, dec->K) == 0) {
    return NANORQ_SYM_IGN;
  }

  if (esi < dec->K) {
    if (compat_bitmask_check(&dec->repair_mask, esi))
      return NANORQ_SYM_DUP;
  } else {
    for (size_t i = 0; i < dec->repair_bin.n; i++) {
      if (dec->repair_bin.a[i].esi == esi) {
        return NANORQ_SYM_DUP;
      }
    }
  }

  if (!ensure_block_matrix(dec))
    return NANORQ_SYM_ERR;

  if (esi < dec->K) {
    if (transfer_esi(rq, sbn, esi, dec->K, (uint8_t *)data, rq->common.T, io,
                     1) == SIZE_MAX)
      return NANORQ_SYM_ERR;
    if (!dec->inverted)
      nanorq_core_place_symbol(&dec->core, dec->D, dec->stride, esi,
                               (const uint8_t *)data, rq->common.T);
    compat_bitmask_set(&dec->repair_mask, esi);
  } else {
    repair_sym rs = {0};
    rs.esi = esi;
    rs.row = (uint8_t *)obl_alloc(1, dec->stride, nanorq_oblas.align_size);
    if (!rs.row)
      return NANORQ_SYM_ERR;
    memcpy(rs.row, data, rq->common.T);
    if (!repair_vec_push(&dec->repair_bin, rs)) {
      obl_free(rs.row);
      return NANORQ_SYM_ERR;
    }
  }

  return NANORQ_SYM_ADDED;
}

int nanorq_decoder_add_recoded_symbol(nanorq *rq, void *data, uint32_t tag,
                                      const uint8_t *coefs, struct ioctx *io) {
  (void)io;
  if (!rq || !data)
    return NANORQ_SYM_ERR;
  uint8_t sbn = (tag >> 24) & 0xff;
  uint32_t esi = (tag & 0x00ffffff);

  struct block_encoder *dec = get_block_encoder(rq, sbn);
  if (dec == NULL || esi >= (1 << 24) || esi > rq->max_esi || !coefs)
    return NANORQ_SYM_ERR;

  if (compat_bitmask_gaps(&dec->repair_mask, dec->K) == 0) {
    return NANORQ_SYM_IGN;
  }

  for (size_t i = 0; i < dec->repair_bin.n; i++) {
    if (dec->repair_bin.a[i].esi == esi) {
      return NANORQ_SYM_DUP;
    }
  }

  repair_sym rs = {0};
  rs.esi = esi;
  rs.row = (uint8_t *)obl_alloc(1, dec->stride, nanorq_oblas.align_size);
  if (!rs.row)
    return NANORQ_SYM_ERR;
  memcpy(rs.row, data, rq->common.T);

  uint32_t coefs_stride = nanorq_core_recommended_stride(dec->K);
  rs.coefs = (uint8_t *)obl_alloc(1, coefs_stride, nanorq_oblas.align_size);
  if (!rs.coefs) {
    obl_free(rs.row);
    return NANORQ_SYM_ERR;
  }
  memcpy(rs.coefs, coefs, dec->K);

  if (!repair_vec_push(&dec->repair_bin, rs)) {
    obl_free(rs.coefs);
    obl_free(rs.row);
    return NANORQ_SYM_ERR;
  }

  return NANORQ_SYM_ADDED;
}

#include "rand.h"
#include "tuple.h"
extern const u32 degree_dist[];
extern const u16 degree_dist_size;

static uint32_t nanorq_deg(uint32_t v, uint32_t K) {
  for (uint32_t d = 0; d < degree_dist_size; d++) {
    if (v < degree_dist[d]) {
      return (d < K) ? d : K;
    }
  }
  return K;
}

bool nanorq_generate_recoded_symbol(nanorq *rq, struct ioctx *io, uint8_t sbn,
                                    uint32_t esi, uint8_t *out_coefs,
                                    void *out_payload) {
  if (!rq || !out_coefs || !out_payload)
    return false;

  struct block_encoder *enc = get_block_encoder(rq, sbn);
  if (!enc || enc->K == 0)
    return false;

  memset(out_coefs, 0, enc->K);
  memset(out_payload, 0, rq->common.T);

  if (io) {
    if (!io->seek || !io->read)
      return false;
    /* sparse initial encode (acts as source node) */
    uint32_t v = rnd_get(esi, 0, 1048576);
    uint32_t d = nanorq_deg(v, enc->K);
    if (d == 0)
      d = 1;

    uint32_t a = enc->K == 1 ? 1 : 1 + rnd_get(esi, 1, enc->K - 1);
    uint32_t b = rnd_get(esi, 2, enc->K);

    for (uint32_t j = 0; j < d; j++) {
      uint32_t idx = b % enc->K;
      uint8_t coef;

      if (rnd_get(esi, 3 + j, 100) < 90) {
        coef = 1;
      } else {
        coef = rnd_get(esi, 3 + j, 254) + 2; /* [2, 255] */
      }

      out_coefs[idx] = coef;
      b = (b + a) % enc->K;
    }

    uint8_t *tmp = (uint8_t *)malloc(rq->common.T);
    if (!tmp)
      return false;
    for (uint32_t i = 0; i < enc->K; i++) {
      if (out_coefs[i] > 0) {
        memset(tmp, 0, rq->common.T);
        if (transfer_esi(rq, sbn, i, enc->K, tmp, rq->common.T, io, 0) ==
            SIZE_MAX) {
          free(tmp);
          return false;
        }
        nanorq_oblas.axpy((uint8_t *)out_payload, tmp, out_coefs[i],
                          rq->common.T);
      }
    }
    free(tmp);
  } else {
    /* dense network recode */
    repair_sym *fallback = NULL;
    for (size_t i = 0; i < enc->repair_bin.n; i++) {
      repair_sym *rs = &enc->repair_bin.a[i];
      if (!rs->coefs)
        continue;
      bool valid = false;
      for (uint32_t j = 0; j < enc->K; j++)
        valid |= rs->coefs[j] != 0;
      if (!valid)
        continue;
      if (!fallback)
        fallback = rs;
      uint8_t c;
      uint32_t r = rnd_get(esi, (uint32_t)(3 * i), 100);
      if (r < 80) {
        c = 1;
      } else if (r < 90) {
        c = 0;
      } else {
        c = (uint8_t)(rnd_get(esi, (uint32_t)(3 * i + 1), 254) + 2);
      }

      if (c > 0) {
        nanorq_oblas.axpy(out_coefs, rs->coefs, c, enc->K);
        nanorq_oblas.axpy((uint8_t *)out_payload, rs->row, c, rq->common.T);
      }
    }
    if (!fallback)
      return false;
    bool nonzero = false;
    for (uint32_t i = 0; i < enc->K; i++)
      nonzero |= out_coefs[i] != 0;
    if (!nonzero) {
      memcpy(out_coefs, fallback->coefs, enc->K);
      memcpy(out_payload, fallback->row, rq->common.T);
    }
  }
  return true;
}

size_t nanorq_num_missing(nanorq *rq, uint8_t sbn) {
  struct block_encoder *dec = get_block_encoder(rq, sbn);
  if (dec == NULL)
    return 0;
  return compat_bitmask_gaps(&dec->repair_mask, dec->K);
}

size_t nanorq_num_repair(nanorq *rq, uint8_t sbn) {
  struct block_encoder *dec = get_block_encoder(rq, sbn);
  if (dec == NULL)
    return 0;
  return dec->repair_bin.n;
}

uint32_t nanorq_tag(uint8_t sbn, uint32_t esi) {
  uint32_t ret = (uint32_t)(sbn) << 24;
  ret |= esi & 0x00ffffff;
  return ret;
}

static bool nanorq_repair_block_recoded(nanorq *rq, struct block_encoder *dec,
                                        uint8_t sbn, struct ioctx *io) {
  size_t num_gaps = compat_bitmask_gaps(&dec->repair_mask, dec->K);
  if (num_gaps == 0)
    return true;
  uint32_t coefs_stride = nanorq_core_recommended_stride(dec->K);
  if (coefs_stride == 0 || num_gaps > SIZE_MAX / coefs_stride ||
      num_gaps > SIZE_MAX / dec->stride)
    return false;
  size_t matrix_bytes = num_gaps * coefs_stride;
  size_t payload_bytes = num_gaps * dec->stride;
  if (matrix_bytes > NANORQ_TSNC_MAX_MATRIX_BYTES ||
      payload_bytes > NANORQ_TSNC_MAX_MATRIX_BYTES - matrix_bytes)
    return false;

  uint8_t *matrix =
      (uint8_t *)obl_alloc(num_gaps, coefs_stride, nanorq_oblas.align_size);
  uint8_t *payloads =
      (uint8_t *)obl_alloc(num_gaps, dec->stride, nanorq_oblas.align_size);
  int32_t *pivot_row = (int32_t *)calloc(dec->K, sizeof(*pivot_row));
  uint8_t *cur_coef = (uint8_t *)malloc(coefs_stride);
  uint8_t *cur_payload = (uint8_t *)malloc(dec->stride);
  bool success = false;

  if (!matrix || !payloads || !pivot_row || !cur_coef || !cur_payload)
    goto out;
  for (uint32_t i = 0; i < dec->K; i++)
    pivot_row[i] = -1;

  size_t rank = 0;
  for (size_t p = 0; p < dec->repair_bin.n && rank < num_gaps; p++) {
    repair_sym *rs = &dec->repair_bin.a[p];
    if (!rs->coefs)
      continue;

    memcpy(cur_coef, rs->coefs, coefs_stride);
    memcpy(cur_payload, rs->row, dec->stride);

    /* Move already-known source symbols to the right-hand side. */
    if (dec->D) {
      for (uint32_t i = 0; i < dec->K; i++) {
        if (cur_coef[i] != 0 && compat_bitmask_check(&dec->repair_mask, i)) {
          nanorq_oblas.axpy(
              cur_payload,
              nanorq_core_get_symbol_ptr(&dec->core, dec->D, dec->stride, i),
              cur_coef[i], rq->common.T);
          cur_coef[i] = 0;
        }
      }
    }

    /* fully reduce against existing pivots */
    for (uint32_t i = 0; i < dec->K; i++) {
      if (cur_coef[i] != 0 && pivot_row[i] >= 0) {
        uint8_t mult = cur_coef[i];
        size_t row = (size_t)pivot_row[i];
        nanorq_oblas.axpy(cur_coef, matrix + row * coefs_stride, mult, dec->K);
        nanorq_oblas.axpy(cur_payload, payloads + row * dec->stride, mult,
                          rq->common.T);
      }
    }

    int new_pivot = -1;
    for (uint32_t i = 0; i < dec->K; i++) {
      if (!compat_bitmask_check(&dec->repair_mask, i) && cur_coef[i] != 0) {
        new_pivot = (int)i;
        break;
      }
    }

    if (new_pivot != -1) {
      uint32_t i = (uint32_t)new_pivot;
      uint8_t inv = GF2_8_INV[cur_coef[i]];
      nanorq_oblas.scal(cur_coef, inv, dec->K);
      nanorq_oblas.scal(cur_payload, inv, rq->common.T);

      /* eliminate new pivot from established rows */
      for (size_t row = 0; row < rank; row++) {
        if (matrix[row * coefs_stride + i] != 0) {
          uint8_t mult = matrix[row * coefs_stride + i];
          nanorq_oblas.axpy(matrix + row * coefs_stride, cur_coef, mult,
                            dec->K);
          nanorq_oblas.axpy(payloads + row * dec->stride, cur_payload, mult,
                            rq->common.T);
        }
      }

      memcpy(matrix + rank * coefs_stride, cur_coef, coefs_stride);
      memcpy(payloads + rank * dec->stride, cur_payload, dec->stride);
      pivot_row[i] = (int32_t)rank;
      rank++;
    }
  }

  if (rank != num_gaps || !ensure_block_matrix(dec))
    goto out;

  for (uint32_t gap = 0; gap < dec->K; gap++) {
    if (!compat_bitmask_check(&dec->repair_mask, gap)) {
      int32_t row = pivot_row[gap];
      if (row < 0 || transfer_esi(rq, sbn, gap, dec->K,
                                  payloads + (size_t)row * dec->stride,
                                  rq->common.T, io, 1) == SIZE_MAX)
        goto out;
      nanorq_core_place_symbol(&dec->core, dec->D, dec->stride, gap,
                               payloads + (size_t)row * dec->stride,
                               rq->common.T);
      compat_bitmask_set(&dec->repair_mask, gap);
    }
  }
  success = true;

out:
  obl_free(matrix);
  obl_free(payloads);
  free(pivot_row);
  free(cur_coef);
  free(cur_payload);
  return success;
}

static bool write_recovered_block(nanorq *rq, struct block_encoder *dec,
                                  uint8_t sbn, struct ioctx *io,
                                  uint8_t *recovered) {
  for (uint32_t gap = 0; gap < dec->K; gap++) {
    if (compat_bitmask_check(&dec->repair_mask, gap))
      continue;
    ops_mix(&dec->core, dec->D, dec->stride, gap, recovered);
    if (transfer_esi(rq, sbn, gap, dec->K, recovered, rq->common.T, io, 1) ==
        SIZE_MAX)
      return false;
  }

  for (uint32_t gap = 0; gap < dec->K; gap++) {
    if (!compat_bitmask_check(&dec->repair_mask, gap))
      compat_bitmask_set(&dec->repair_mask, gap);
  }
  return true;
}

/* HERMES local addition -- see nanorq.h.  3-byte tag: sbn, then a 16-bit esi
 * little-endian.  Caps the ESI at 65535, which transmitter.c enforces. */
uint8_t *nanorq_tag_reduced(uint8_t sbn, uint32_t esi, uint8_t *buffer) {
  buffer[0] = sbn;
  buffer[1] = esi & 0xff;
  buffer[2] = (esi >> 8) & 0xff;
  return buffer;
}

bool nanorq_repair_block(nanorq *rq, struct ioctx *io, uint8_t sbn) {
  if (!rq || !io || !io->seek || !io->write)
    return false;
  struct block_encoder *dec = get_block_encoder(rq, sbn);
  if (dec == NULL)
    return false;

  size_t conventional_count = 0;
  size_t recoded_count = 0;
  for (size_t i = 0; i < dec->repair_bin.n; i++) {
    if (dec->repair_bin.a[i].coefs)
      recoded_count++;
    else
      conventional_count++;
  }

  size_t num_gaps = compat_bitmask_gaps(&dec->repair_mask, dec->K);
  if (num_gaps == 0)
    return true;
  if (dec->inverted) {
    uint8_t *recovered = (uint8_t *)malloc(dec->stride);
    if (!recovered)
      return false;
    bool success = write_recovered_block(rq, dec, sbn, io, recovered);
    free(recovered);
    return success;
  }
  if (recoded_count >= num_gaps &&
      nanorq_repair_block_recoded(rq, dec, sbn, io))
    return true;
  if (conventional_count < num_gaps ||
      conventional_count - num_gaps > UINT32_MAX)
    return false;

  uint32_t overhead = (uint32_t)(conventional_count - num_gaps);
  nanorq_core core = {0};
  uint8_t *D = NULL;
  uint8_t *prep_mem = NULL;
  uint8_t *work_mem = NULL;
  void *sched_mem = NULL;
  uint8_t *recovered = NULL;
  schedule S = {0};
  bool matrix_modified = false;
  bool success = false;

  if (!ensure_block_matrix(dec) ||
      !nanorq_core_encoder_new(dec->K, overhead, &core))
    goto out;

  size_t rows = nanorq_core_get_pc_rows(&core);
  size_t base_rows = nanorq_core_get_pc_rows(&dec->core);
  if (base_rows > rows)
    goto out;
  D = (uint8_t *)obl_alloc(rows, dec->stride, nanorq_oblas.align_size);
  if (!D)
    goto out;
  memcpy(D, dec->D, base_rows * dec->stride);

  /*
   * the replacement matrix is still an exact copy of the canonical decoder
   * state. adopt it before solver setup so only one large matrix is resident;
   * failures below restore any temporary repair rows before returning.
   */
  uint8_t *old_D = dec->D;
  dec->D = D;
  D = NULL;
  obl_free(old_D);

  size_t prep_len = nanorq_core_calculate_prepare_memory(&core);
  prep_mem = (uint8_t *)malloc(prep_len);
  if (!prep_mem || !nanorq_core_prepare(&core, prep_mem, prep_len))
    goto out;

  size_t rep_idx = 0;
  matrix_modified = true;
  for (uint32_t gap = 0; gap < dec->K; gap++) {
    if (compat_bitmask_check(&dec->repair_mask, gap))
      continue;
    while (rep_idx < dec->repair_bin.n && dec->repair_bin.a[rep_idx].coefs)
      rep_idx++;
    if (rep_idx == dec->repair_bin.n)
      goto out;
    repair_sym *rs = &dec->repair_bin.a[rep_idx++];
    nanorq_core_replace_symbol(&core, gap, rs->esi);
    nanorq_core_place_symbol(&core, dec->D, dec->stride, gap, rs->row,
                             rq->common.T);
  }

  for (uint32_t extra = 0; extra < overhead; extra++) {
    while (rep_idx < dec->repair_bin.n && dec->repair_bin.a[rep_idx].coefs)
      rep_idx++;
    if (rep_idx == dec->repair_bin.n)
      goto out;
    repair_sym *rs = &dec->repair_bin.a[rep_idx++];
    uint32_t row = core.P.Kprime + extra;
    nanorq_core_replace_symbol(&core, row, rs->esi);
    nanorq_core_place_symbol(&core, dec->D, dec->stride, row, rs->row,
                             rq->common.T);
  }

  if (!nanorq_core_patch_matrix(&core))
    goto out;
  size_t work_len = nanorq_core_calculate_work_memory(&core);
  size_t sched_bytes = ops_estimate_schedule_bytes(dec->K);
  work_mem = (uint8_t *)malloc(work_len);
  sched_mem = malloc(sched_bytes);
  recovered = (uint8_t *)malloc(dec->stride);
  if (!work_mem || !recovered || !schedule_init(&S, sched_mem, sched_bytes))
    goto out;
  nanorq_core_set_op_callback(&core, &S, ops_push);
  if (!nanorq_core_precalculate(&core, work_mem, work_len) || S.overflowed ||
      S.cpidx != 2)
    goto out;

  free(dec->prep_mem);
  free(dec->work_mem);
  free(dec->S.ops.a);
  dec->core = core;
  dec->prep_mem = prep_mem;
  dec->prep_len = prep_len;
  dec->work_mem = work_mem;
  dec->work_len = work_len;
  dec->S = S;
  prep_mem = NULL;
  work_mem = NULL;
  sched_mem = NULL;

  ops_run(&dec->core, dec->D, dec->stride, &dec->S);
  dec->inverted = true;
  success = write_recovered_block(rq, dec, sbn, io, recovered);

out:
  if (matrix_modified && !dec->inverted) {
    for (uint32_t gap = 0; gap < dec->K; gap++) {
      if (!compat_bitmask_check(&dec->repair_mask, gap))
        memset(nanorq_core_get_symbol_ptr(&core, dec->D, dec->stride, gap), 0,
               dec->stride);
    }
    for (uint32_t extra = 0; extra < overhead; extra++)
      memset(nanorq_core_get_symbol_ptr(&core, dec->D, dec->stride,
                                        core.P.Kprime + extra),
             0, dec->stride);
  }
  free(recovered);
  free(sched_mem);
  free(work_mem);
  free(prep_mem);
  obl_free(D);
  return success;
}
