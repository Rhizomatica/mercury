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
#include "raptorq/include/io.h"

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

static void set_err(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
}

int bcast_file_mode_frame_size(int mode)
{
    if (mode < 0 || mode > BCAST_MODE_MAX)
        return 0;
    return (int)bcast_frame_size[mode];
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
    FILE *fp = NULL;
    long sz;

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

    fp = fopen(path, "rb");
    if (!fp) { set_err(err, errlen, "cannot open file"); return NULL; }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); set_err(err, errlen, "cannot size file"); return NULL; }
    sz = ftell(fp);
    rewind(fp);

    if (sz <= 0)
    {
        fclose(fp);
        set_err(err, errlen, "file is empty");
        return NULL;
    }
    if ((size_t)sz > BCAST_FILE_MAX_BYTES)
    {
        char m[128];
        snprintf(m, sizeof(m), "file is %ld bytes; the limit is %u",
                 sz, BCAST_FILE_MAX_BYTES);
        fclose(fp);
        set_err(err, errlen, m);
        return NULL;
    }

    tx = calloc(1, sizeof(*tx));
    if (!tx) { fclose(fp); set_err(err, errlen, "out of memory"); return NULL; }

    tx->file_bytes = (size_t)sz;
    tx->file = malloc(tx->file_bytes);
    if (!tx->file || fread(tx->file, 1, tx->file_bytes, fp) != tx->file_bytes)
    {
        fclose(fp);
        set_err(err, errlen, "cannot read file");
        bcast_file_tx_close(tx);
        return NULL;
    }
    fclose(fp);

    tx->mode         = mode;
    tx->frame_size   = (uint32_t)bcast_file_mode_frame_size(mode);
    tx->symbol_size  = tx->frame_size - BCAST_FRAME_OVERHEAD;
    tx->cycles_total = cycles;
    tx->next_block   = 0;

    /* Read-only memory context: no mmap, so this works on Windows too, and the
     * file is capped small enough that holding it is cheap. */
    tx->io = ioctx_from_mem_ro(tx->file, tx->file_bytes);
    if (!tx->io) { set_err(err, errlen, "cannot wrap file"); bcast_file_tx_close(tx); return NULL; }

    tx->rq = nanorq_encoder_new(tx->file_bytes, tx->symbol_size, 1);
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
