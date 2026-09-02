/* HERMES Modem — broadcast file transmission (RaptorQ carousel)
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See bcast_file.h.  The frame layout here is dictated by hermes-broadcast's
 * receiver and must not drift from it.
 */

#include "bcast_file.h"
#include "bcast_modes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "raptorq/include/nanorq.h"
#include "raptorq/include/nanorq_io.h"

struct bcast_file_tx
{
    nanorq       *rq;
    struct ioctx *io;

    uint8_t  *file;        /* whole file in memory; see BCAST_FILE_MAX_BYTES */
    size_t    file_bytes;

    int       mode;
    uint32_t  frame_size;  /* every emitted frame is exactly this long */
    size_t    symbol_size; /* frame_size - BCAST_RQ_HEADER_SIZE          */

    int       blocks;      /* RaptorQ source blocks (sbn count) */
    uint32_t *esi;         /* next ESI per block                */

    uint8_t   config_body[BCAST_CONFIG_BODY_SIZE];  /* repeated in every frame */
    uint8_t   session_id;   /* 1..31, in the header's extension field */

    /* Carousel position.  A cycle is one symbol from each block: with the joint
     * frame there is no separate configuration frame to account for. */
    int       cycles_total;   /* 0 = endless */
    int       cycle_now;
    int       next_block;
    uint64_t  frames_sent;
    int       finished;
};

/* basename() without <libgen.h>: that header's basename() may modify its
 * argument, and the Windows build has no libgen at all. */
static const char *bundle_basename(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

static void set_err(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
}

uint8_t *bcast_bundle_build(const char *path, size_t *out_len,
                            char *err, size_t errlen)
{
    FILE *fp = NULL;
    uint8_t *bundle = NULL;
    long fsz;

    if (!path || !*path) { set_err(err, errlen, "no file given"); return NULL; }

    const char *name = bundle_basename(path);
    size_t namelen = strlen(name);
    if (namelen == 0)                     { set_err(err, errlen, "file has no name"); return NULL; }
    if (namelen > BCAST_BUNDLE_NAME_MAX)  { set_err(err, errlen, "file name is too long"); return NULL; }

    fp = fopen(path, "rb");
    if (!fp) { set_err(err, errlen, "cannot open file"); return NULL; }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); set_err(err, errlen, "cannot size file"); return NULL; }
    fsz = ftell(fp);
    rewind(fp);

    if (fsz <= 0) { fclose(fp); set_err(err, errlen, "file is empty"); return NULL; }

    size_t body   = namelen + 1 + (size_t)fsz;   /* what the size field counts */
    size_t total  = 4 + body;

    if (total > BCAST_FILE_MAX_BYTES)
    {
        char m[160];
        snprintf(m, sizeof(m),
                 "file plus its name comes to %zu bytes; the limit is %u",
                 total, BCAST_FILE_MAX_BYTES);
        fclose(fp);
        set_err(err, errlen, m);
        return NULL;
    }

    bundle = malloc(total);
    if (!bundle) { fclose(fp); set_err(err, errlen, "out of memory"); return NULL; }

    /* Explicit little-endian; see the note in bcast_file.h. */
    uint32_t sz = (uint32_t)body;
    bundle[0] = (uint8_t)(sz & 0xff);
    bundle[1] = (uint8_t)((sz >> 8) & 0xff);
    bundle[2] = (uint8_t)((sz >> 16) & 0xff);
    bundle[3] = (uint8_t)((sz >> 24) & 0xff);

    memcpy(bundle + 4, name, namelen);
    bundle[4 + namelen] = '\n';

    if (fread(bundle + 4 + namelen + 1, 1, (size_t)fsz, fp) != (size_t)fsz)
    {
        fclose(fp);
        free(bundle);
        set_err(err, errlen, "cannot read file");
        return NULL;
    }
    fclose(fp);

    if (out_len) *out_len = total;
    return bundle;
}

int bcast_bundle_parse(const uint8_t *buf, size_t len,
                       char *name, size_t namelen,
                       const uint8_t **payload, size_t *payload_len)
{
    if (!buf || len < 6)   /* size field + at least one name byte + '\n' */
        return -1;

    uint32_t body = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                    ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    if ((size_t)body + 4 != len)
        return -1;               /* truncated or over-long: not our bundle */

    /* The name runs to the first '\n'.  Bounded by the buffer, so a bundle
     * with no terminator is rejected rather than read off the end. */
    size_t n = 0;
    while (4 + n < len && buf[4 + n] != '\n')
        n++;
    if (4 + n >= len || n == 0 || n > BCAST_BUNDLE_NAME_MAX)
        return -1;

    /* Refuse anything that is not a bare filename: a receiver must never be
     * talked into writing outside its own directory. */
    for (size_t i = 0; i < n; i++)
        if (buf[4 + i] == '/' || buf[4 + i] == '\\' || buf[4 + i] == 0)
            return -1;
    if (buf[4] == '.' && (n == 1 || (n == 2 && buf[5] == '.')))
        return -1;               /* "." and ".." */

    if (name && namelen)
    {
        if (n + 1 > namelen) return -1;
        memcpy(name, buf + 4, n);
        name[n] = 0;
    }
    if (payload)     *payload = buf + 4 + n + 1;
    if (payload_len) *payload_len = len - (4 + n + 1);
    return 0;
}

int bcast_file_mode_frame_size(int mode)
{
    if (mode < 0 || mode > BCAST_MODE_MAX)
        return 0;
    return (int)bcast_frame_size[mode];
}

const char *bcast_file_mode_name(int mode)
{
    if (mode < 0 || mode > BCAST_MODE_MAX)
        return "";
    return bcast_mode_name[mode];
}

int bcast_file_mode_usable(int mode)
{
    int fs = bcast_file_mode_frame_size(mode);
    /* Every frame carries the header, the config body and the tag, so a frame
     * must have room for all of that plus at least one symbol byte.  DATAC14's
     * 3 bytes are the case this rejects; without the check the symbol size
     * underflows. */
    return fs > BCAST_FRAME_OVERHEAD;
}

bcast_file_tx_t *bcast_file_tx_open(const char *path, int mode, int cycles,
                                    int session_id, char *err, size_t errlen)
{
    bcast_file_tx_t *tx = NULL;

    if (!path || !*path) { set_err(err, errlen, "no file given"); return NULL; }
    if (cycles < 0)      { set_err(err, errlen, "cycles cannot be negative"); return NULL; }

    if (!bcast_file_mode_usable(mode))
    {
        char m[128];
        int fs = bcast_file_mode_frame_size(mode);
        if (fs == 0)
            snprintf(m, sizeof(m), "mode %d does not exist (valid range 0..%d)",
                     mode, BCAST_MODE_MAX);
        else
            snprintf(m, sizeof(m),
                     "mode %d carries only %d bytes per frame; broadcast needs more than %d",
                     mode, fs, BCAST_FRAME_OVERHEAD);
        set_err(err, errlen, m);
        return NULL;
    }

    /* What goes over the air is the BUNDLE -- the file with its name -- not the
     * bare file, so the receiver can write it back under the right name. */
    size_t bundle_len = 0;
    uint8_t *bundle = bcast_bundle_build(path, &bundle_len, err, errlen);
    if (!bundle)
        return NULL;

    tx = calloc(1, sizeof(*tx));
    if (!tx) { free(bundle); set_err(err, errlen, "out of memory"); return NULL; }

    tx->file       = bundle;
    tx->file_bytes = bundle_len;

    tx->mode         = mode;
    tx->frame_size   = (uint32_t)bcast_file_mode_frame_size(mode);
    tx->symbol_size  = tx->frame_size - BCAST_FRAME_OVERHEAD;
    tx->cycles_total = cycles;
    tx->next_block   = 0;

    /* Read-only memory context: no mmap, so this works on Windows too, and the
     * file is capped small enough that holding it is cheap. */
    tx->io = ioctx_from_mem_ro(tx->file, tx->file_bytes);
    if (!tx->io) { set_err(err, errlen, "cannot wrap file"); bcast_file_tx_close(tx); return NULL; }

    /* ONE source block wherever it fits.
     *
     * RaptorQ partitions into Z source blocks and codes each independently, so
     * a symbol for one block does nothing for another.  nanorq defaults to
     * Z=16, which costs twice over:
     *
     *   - the fountain's overhead is paid PER BLOCK, so 16 blocks need ~5-17%
     *     more frames than one does for the same file (measured across modes 0,
     *     1 and 10 at 1.44 MB), and
     *   - a loss pattern that happens to be periodic in the carousel starves
     *     one block completely, and no amount of further transmission recovers
     *     it.  With Z=1 there is no such pattern: any K+e symbols decode.
     *
     * Decode cost is essentially unchanged (0.04s vs 0.04s at 1.44 MB), so this
     * is a straight win where it is legal.  It is not always legal: a block
     * holds at most K_max symbols, so a tiny symbol size on a large file needs
     * more than one.  Fall back to the SMALLEST legal Z rather than nanorq's
     * default 16, which keeps the per-block overhead as low as the constraint
     * allows. */
    {
        const uint16_t k_max = 56403;   /* RFC 6330 */
        size_t kt = (tx->file_bytes + tx->symbol_size - 1) / tx->symbol_size;
        size_t z  = (kt + k_max - 1) / k_max;
        if (z < 1) z = 1;
        if (z > 256) z = 256;           /* Z_max */
        tx->rq = nanorq_encoder_new_ex(tx->file_bytes, (uint16_t)tx->symbol_size,
                                       0, (uint16_t)z, 1);
        /* If the explicit choice is refused for any reason, let nanorq pick:
         * a working transfer beats an optimal one that will not start. */
        if (!tx->rq)
            tx->rq = nanorq_encoder_new(tx->file_bytes, tx->symbol_size, 1);
    }
    if (!tx->rq) { set_err(err, errlen, "cannot initialise RaptorQ encoder"); bcast_file_tx_close(tx); return NULL; }

    nanorq_set_max_esi(tx->rq, BCAST_MAX_ESI);

    tx->blocks      = nanorq_blocks(tx->rq);
    tx->symbol_size = nanorq_symbol_size(tx->rq);
    if (tx->blocks <= 0) { set_err(err, errlen, "RaptorQ produced no blocks"); bcast_file_tx_close(tx); return NULL; }

    tx->esi = calloc((size_t)tx->blocks, sizeof(*tx->esi));
    if (!tx->esi) { set_err(err, errlen, "out of memory"); bcast_file_tx_close(tx); return NULL; }

    for (int b = 0; b < tx->blocks; b++)
        nanorq_generate_symbols(tx->rq, b, tx->io);

    /* The reduced OTI, carried in EVERY frame so a receiver can begin on
     * whichever one it happens to hear first. */
    memset(tx->config_body, 0, sizeof(tx->config_body));
    nanorq_oti_common_reduced(tx->rq, tx->config_body);
    nanorq_oti_scheme_specific_align1(tx->rq, tx->config_body + 5);

    /* Per-file id in the header's 5-bit extension field, so a receiver can tell
     * a new file from the one it just finished.  Never 0: the daemon treats 0
     * as "no session". */
    if (session_id > 0)
        tx->session_id = (uint8_t)(session_id & BCAST_FRAME_EXT_MASK);
    if (tx->session_id == 0)
        tx->session_id = (uint8_t)(1 + (rand() % (int)BCAST_FRAME_EXT_MASK));

    return tx;
}

int bcast_file_tx_next(bcast_file_tx_t *tx, uint8_t *buf, size_t buflen)
{
    if (!tx || !buf) return -1;
    if (tx->finished)  return 0;
    if (buflen < tx->frame_size) return -1;

    int sbn = tx->next_block;
    uint32_t esi = tx->esi[sbn];

    /* One self-describing frame: header, config body, tag, symbol. */
    memset(buf, 0, tx->frame_size);
    if (nanorq_encode(tx->rq, buf + BCAST_FRAME_OVERHEAD, esi, (uint8_t)sbn, tx->io)
            != tx->symbol_size)
        return -1;

    memcpy(buf + 1, tx->config_body, BCAST_CONFIG_BODY_SIZE);
    nanorq_tag_reduced((uint8_t)sbn, esi, buf + 1 + BCAST_CONFIG_BODY_SIZE);
    bcast_write_frame_header(buf, BCAST_PACKET_RQ_CONFIG, tx->session_id);

    tx->esi[sbn]++;
    tx->frames_sent++;

    /* Advance the carousel. */
    tx->next_block++;
    if (tx->next_block >= tx->blocks)
    {
        tx->next_block = 0;
        tx->cycle_now++;
        if (tx->cycles_total > 0 && tx->cycle_now >= tx->cycles_total)
            tx->finished = 1;
    }

    /* The reduced tag only has 16 bits of ESI.  Running past that would wrap
     * and start re-sending symbols the receiver already rejected as duplicates,
     * so stop instead of silently going in circles. */
    if (tx->esi[sbn] > BCAST_MAX_ESI)
        tx->finished = 1;

    return (int)tx->frame_size;
}

int bcast_file_tx_frame_size(const bcast_file_tx_t *tx)
{
    return tx ? (int)tx->frame_size : 0;
}

void bcast_file_tx_stats(const bcast_file_tx_t *tx, int *cycle_now,
                         int *cycles_total, uint64_t *frames_sent)
{
    if (!tx) return;
    if (cycle_now)    *cycle_now    = tx->cycle_now;
    if (cycles_total) *cycles_total = tx->cycles_total;
    if (frames_sent)  *frames_sent  = tx->frames_sent;
}

void bcast_file_tx_source(const bcast_file_tx_t *tx, size_t *file_bytes, int *blocks)
{
    if (!tx) return;
    if (file_bytes) *file_bytes = tx->file_bytes;
    if (blocks)     *blocks     = tx->blocks;
}

void bcast_file_tx_close(bcast_file_tx_t *tx)
{
    if (!tx) return;
    if (tx->rq) nanorq_free(tx->rq);
    if (tx->io) tx->io->destroy(tx->io);
    free(tx->esi);
    free(tx->file);
    free(tx);
}

/* ======================================================================
 * Receiving
 * ====================================================================== */

struct bcast_file_rx
{
    int       mode;
    uint32_t  frame_size;
    size_t    symbol_size;
    char      dir[512];

    nanorq       *rq;
    struct ioctx *io;
    char      tmp_path[600];      /* decode target, renamed once complete */
    int       session;            /* -1 = no session yet */
    int       last_done_session;  /* ignore a repeat of what we just finished */
    uint64_t  symbols;
    size_t    expect_bytes;

    char      last_path[600];
    char      last_name[BCAST_BUNDLE_NAME_MAX + 1];
    char      err[192];
};

static void rx_reset(bcast_file_rx_t *rx)
{
    if (rx->rq) { nanorq_free(rx->rq); rx->rq = NULL; }
    if (rx->io) { rx->io->destroy(rx->io); rx->io = NULL; }
    rx->session = -1;
    rx->symbols = 0;
    rx->expect_bytes = 0;
    if (rx->tmp_path[0]) remove(rx->tmp_path);
}

bcast_file_rx_t *bcast_file_rx_open(int mode, const char *dir,
                                    char *err, size_t errlen)
{
    if (!bcast_file_mode_usable(mode))
    {
        set_err(err, errlen, "mode cannot carry broadcast");
        return NULL;
    }
    if (!dir || !*dir) { set_err(err, errlen, "no directory given"); return NULL; }

    bcast_file_rx_t *rx = calloc(1, sizeof(*rx));
    if (!rx) { set_err(err, errlen, "out of memory"); return NULL; }

    rx->mode        = mode;
    rx->frame_size  = (uint32_t)bcast_file_mode_frame_size(mode);
    rx->symbol_size = rx->frame_size - BCAST_FRAME_OVERHEAD;
    rx->session     = -1;
    rx->last_done_session = -1;
    snprintf(rx->dir, sizeof(rx->dir), "%s", dir);
    snprintf(rx->tmp_path, sizeof(rx->tmp_path), "%s/.bcast_partial.tmp", dir);
    return rx;
}

/* Reassemble the standard OTI words from the reduced bytes every frame carries.
 * Mirrors hermes-broadcast's daemon so the two interoperate. */
static uint64_t rx_common(const uint8_t *f)
{
    uint64_t c = 0;
    c |= (uint64_t)(f[1] & 0xff) << 24;
    c |= (uint64_t)(f[2] & 0xff) << 32;
    c |= (uint64_t)(f[3] & 0xff) << 40;
    c |= (uint64_t)(f[4] & 0xff);
    c |= (uint64_t)(f[5] & 0xff) << 8;
    return c;
}
static uint32_t rx_scheme(const uint8_t *f)
{
    uint32_t s = 0;
    s |= (uint32_t)(f[6] & 0xff) << 24;
    s |= (uint32_t)(f[7] & 0xff) << 8;
    s |= (uint32_t)(f[8] & 0xff) << 16;
    s |= 1;                       /* Al, not transmitted */
    return s;
}

bcast_rx_result_t bcast_file_rx_frame(bcast_file_rx_t *rx,
                                      const uint8_t *frame, size_t len)
{
    if (!rx || !frame) return BCAST_RX_ERROR;

    /* Anything that is not exactly one of our frames is somebody else's
     * traffic on the broadcast plane -- chat, KISS from another client -- and
     * must be passed over silently rather than treated as corruption. */
    if (len != rx->frame_size) return BCAST_RX_IGNORED;
    if (((frame[0] >> BCAST_PACKET_TYPE_SHIFT) & BCAST_PACKET_TYPE_MASK)
            != BCAST_PACKET_RQ_CONFIG)
        return BCAST_RX_IGNORED;

    int session = frame[0] & BCAST_FRAME_EXT_MASK;
    if (session == 0) return BCAST_RX_IGNORED;

    /* The sender keeps transmitting after we have finished; do not start the
     * same file over again. */
    if (session == rx->last_done_session && rx->session < 0)
        return BCAST_RX_IGNORED;

    if (rx->session >= 0 && session != rx->session)
        rx_reset(rx);             /* a different file began */

    if (rx->session < 0)
    {
        uint64_t common = rx_common(frame);
        uint32_t scheme = rx_scheme(frame);
        rx->rq = nanorq_decoder_new(common, scheme);
        if (!rx->rq)
        {
            snprintf(rx->err, sizeof(rx->err), "frame carried an unusable OTI");
            return BCAST_RX_ERROR;
        }
        rx->expect_bytes = nanorq_transfer_length(rx->rq);
        if (rx->expect_bytes == 0 || rx->expect_bytes > BCAST_FILE_MAX_BYTES)
        {
            snprintf(rx->err, sizeof(rx->err), "announced size %zu is out of range",
                     rx->expect_bytes);
            rx_reset(rx);
            return BCAST_RX_ERROR;
        }
        rx->io = ioctx_from_file(rx->tmp_path, 0);
        if (!rx->io)
        {
            snprintf(rx->err, sizeof(rx->err), "cannot write into the download directory");
            rx_reset(rx);
            return BCAST_RX_ERROR;
        }
        rx->session = session;
        rx->symbols = 0;
    }

    const uint8_t *tagp = frame + 1 + BCAST_CONFIG_BODY_SIZE;
    uint32_t tag = ((uint32_t)tagp[0] << 24) | tagp[1] | ((uint32_t)tagp[2] << 8);
    nanorq_decoder_add_symbol(rx->rq, (void *)(frame + BCAST_FRAME_OVERHEAD),
                              tag, rx->io);
    rx->symbols++;

    int blocks = nanorq_blocks(rx->rq);
    for (int b = 0; b < blocks; b++)
        if (!nanorq_repair_block(rx->rq, rx->io, b))
            return BCAST_RX_PROGRESS;

    /* Complete: flush, read the bundle back, and unwrap it. */
    rx->io->destroy(rx->io);
    rx->io = NULL;

    uint8_t *buf = malloc(rx->expect_bytes);
    FILE *fp = buf ? fopen(rx->tmp_path, "rb") : NULL;
    size_t got = fp ? fread(buf, 1, rx->expect_bytes, fp) : 0;
    if (fp) fclose(fp);

    bcast_rx_result_t out = BCAST_RX_ERROR;
    char name[BCAST_BUNDLE_NAME_MAX + 1] = {0};
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    if (got != rx->expect_bytes)
        snprintf(rx->err, sizeof(rx->err), "decoded file could not be read back");
    else
    {
        if (bcast_bundle_parse(buf, got, name, sizeof(name), &payload, &payload_len) != 0)
        {
            /* Not one of our bundles.  hermes-broadcast's broadcast_daemon
             * transmits the bare file, and so may anything else on the air --
             * the bundle is OUR convention for carrying a name, not part of
             * the RaptorQ protocol.  The decode succeeded, so the data is
             * good; throwing it away because it lacks our wrapper would be
             * losing a file we already have.  Save it under a timestamped
             * name, which is what the daemon does with what it receives. */
            time_t now = time(NULL);
            struct tm tmv;
#ifdef _WIN32
            struct tm *tp = localtime(&now);
            if (tp) tmv = *tp; else memset(&tmv, 0, sizeof(tmv));
#else
            localtime_r(&now, &tmv);
#endif
            strftime(name, sizeof(name), "broadcast_%Y%m%d_%H%M%S.bin", &tmv);
            payload     = buf;
            payload_len = got;
        }

        char path[600];
        snprintf(path, sizeof(path), "%s/%s", rx->dir, name);
        FILE *of = fopen(path, "wb");
        if (!of)
            snprintf(rx->err, sizeof(rx->err), "cannot create the output file");
        else
        {
            size_t w = fwrite(payload, 1, payload_len, of);
            fclose(of);
            if (w != payload_len)
                snprintf(rx->err, sizeof(rx->err), "short write to the output file");
            else
            {
                snprintf(rx->last_path, sizeof(rx->last_path), "%s", path);
                snprintf(rx->last_name, sizeof(rx->last_name), "%s", name);
                out = BCAST_RX_COMPLETE;
            }
        }
    }

    free(buf);
    rx->last_done_session = rx->session;
    rx_reset(rx);
    return out;
}

const char *bcast_file_rx_last_path(const bcast_file_rx_t *rx)
{ return rx ? rx->last_path : ""; }
const char *bcast_file_rx_last_name(const bcast_file_rx_t *rx)
{ return rx ? rx->last_name : ""; }
const char *bcast_file_rx_error(const bcast_file_rx_t *rx)
{ return rx ? rx->err : ""; }

void bcast_file_rx_stats(const bcast_file_rx_t *rx, uint64_t *symbols,
                         size_t *expect_bytes)
{
    if (!rx) return;
    if (symbols)      *symbols      = rx->symbols;
    if (expect_bytes) *expect_bytes = rx->expect_bytes;
}

void bcast_file_rx_close(bcast_file_rx_t *rx)
{
    if (!rx) return;
    rx_reset(rx);
    free(rx);
}
