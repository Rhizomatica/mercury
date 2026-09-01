/* Tests for datalink_broadcast/bcast_file.c
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The frames this produces are consumed by hermes-broadcast's receiver on
 * another station, so the property that matters is not "the encoder ran" but
 * "a receiver implementing that wire format recovers the file byte-for-byte".
 * So the decode side here is written from the RECEIVER's point of view --
 * parsing the reduced OTI and tag exactly as receiver.c does -- rather than by
 * calling back into our own encoder's helpers.  If the layout drifts, this
 * fails instead of quietly agreeing with itself.
 */
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bcast_file.h"
#include "bcast_modes.h"
#include "raptorq/include/nanorq.h"
#include "raptorq/include/io.h"

void setUp(void) {}
void tearDown(void) {}

static const char *TMP = "/tmp/.mercury_bcast_test.bin";

static void write_file(const char *path, size_t n, unsigned seed)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    srand(seed);
    for (size_t i = 0; i < n; i++) fputc(rand() & 0xff, f);
    fclose(f);
}

/* ---- receiver-side parsing, mirroring hermes-broadcast/receiver.c ---- */
static uint64_t rx_oti_common(const uint8_t *p)
{
    uint64_t c = 0;
    c |= (uint64_t)(p[1] & 0xff) << 24;
    c |= (uint64_t)(p[2] & 0xff) << 32;
    c |= (uint64_t)(p[3] & 0xff) << 40;
    c |= p[4] & 0xff;
    c |= (uint64_t)(p[5] & 0xff) << 8;
    return c;
}
static uint32_t rx_oti_scheme(const uint8_t *p)
{
    uint32_t s = 0;
    s |= (uint32_t)(p[6] & 0xff) << 24;
    s |= (uint32_t)(p[7] & 0xff) << 8;
    s |= (uint32_t)(p[8] & 0xff) << 16;
    s |= 1;
    return s;
}
static uint8_t frame_type(const uint8_t *f)
{
    return (uint8_t)((f[0] >> BCAST_PACKET_TYPE_SHIFT) & BCAST_PACKET_TYPE_MASK);
}

/* Drive a whole transfer through a lossy channel and require the file back.
 *
 * loss_pct is applied pseudo-randomly, NOT as "drop every Nth frame".  A fixed
 * period aliases with the carousel: the cycle is 1 config + one symbol per
 * block, so dropping every Nth frame where N is the cycle length starves the
 * same block every time and it can never decode.  Real HF loss is bursty, not
 * periodic, and a test that models it periodically fails for a reason that has
 * nothing to do with the code under test. */
static void transfer_case(size_t nbytes, int mode, int loss_pct, unsigned seed)
{
    char err[160] = {0};
    write_file(TMP, nbytes, seed);
    srand(seed * 7919u + 13u);   /* loss pattern, independent of the payload */

    bcast_file_tx_t *tx = bcast_file_tx_open(TMP, mode, 0 /* endless */, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(tx, err);

    int fs = bcast_file_tx_frame_size(tx);
    TEST_ASSERT_EQUAL_INT(bcast_file_mode_frame_size(mode), fs);

    uint8_t *frame = malloc((size_t)fs);
    uint8_t *src = malloc(nbytes), *out = malloc(nbytes);
    FILE *f = fopen(TMP, "rb"); TEST_ASSERT_EQUAL_size_t(nbytes, fread(src, 1, nbytes, f)); fclose(f);

    nanorq *dec = NULL;
    struct ioctx *rio = NULL;
    const char *outf = "/tmp/.mercury_bcast_out.bin";
    int sent = 0, done = 0;

    for (int i = 0; i < 400000 && !done; i++)
    {
        int n = bcast_file_tx_next(tx, frame, (size_t)fs);
        TEST_ASSERT_EQUAL_INT(fs, n);   /* endless: never returns 0 */
        sent++;
        if (loss_pct > 0 && (rand() % 100) < loss_pct) continue;   /* channel loss */

        if (frame_type(frame) == BCAST_PACKET_RQ_CONFIG)
        {
            if (!dec)
            {
                dec = nanorq_decoder_new(rx_oti_common(frame), rx_oti_scheme(frame));
                TEST_ASSERT_NOT_NULL_MESSAGE(dec, "decoder rejected our config packet");
                TEST_ASSERT_EQUAL_size_t(nbytes, nanorq_transfer_length(dec));
                rio = ioctx_from_file(outf, 0);
                TEST_ASSERT_NOT_NULL(rio);
            }
            continue;
        }
        if (!dec) continue;   /* nothing usable until the config arrives */

        /* reduced 3-byte tag -> the 32-bit tag nanorq wants */
        uint32_t tag = ((uint32_t)frame[1] << 24) | frame[2] | ((uint32_t)frame[3] << 8);
        nanorq_decoder_add_symbol(dec, frame + BCAST_RQ_HEADER_SIZE, tag, rio);

        done = 1;
        for (int b = 0; b < nanorq_blocks(dec); b++)
            if (!nanorq_repair_block(dec, rio, b)) { done = 0; break; }
    }
    TEST_ASSERT_TRUE_MESSAGE(done, "receiver never decoded the file");

    rio->destroy(rio);
    f = fopen(outf, "rb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_size_t(nbytes, fread(out, 1, nbytes, f));
    fclose(f);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(src, out, nbytes, "recovered file differs");

    nanorq_free(dec);
    bcast_file_tx_close(tx);
    free(frame); free(src); free(out);
    remove(TMP); remove(outf);
}

void test_clean_channel_recovers_the_file(void)      { transfer_case(20000, 0,  0, 1); }
void test_lossy_channel_recovers_the_file(void)      { transfer_case(20000, 0, 20, 2); }
void test_small_robust_mode_recovers_the_file(void)  { transfer_case(1500,  8, 25, 3); }
void test_fast_mode_recovers_the_file(void)          { transfer_case(60000, 10, 30, 4); }

/* A receiver that tunes in late must still be able to start: the configuration
 * packet is repeated every cycle, not sent once at the beginning. */
void test_config_packet_repeats_every_cycle(void)
{
    char err[160] = {0};
    write_file(TMP, 8000, 7);
    bcast_file_tx_t *tx = bcast_file_tx_open(TMP, 0, 0, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(tx, err);
    int fs = bcast_file_tx_frame_size(tx);
    uint8_t *frame = malloc((size_t)fs);

    size_t bytes; int blocks;
    bcast_file_tx_source(tx, &bytes, &blocks);
    TEST_ASSERT_EQUAL_size_t(8000, bytes);

    int configs = 0;
    /* three cycles' worth of frames */
    int total = 3 * (1 + blocks);
    for (int i = 0; i < total; i++)
    {
        TEST_ASSERT_EQUAL_INT(fs, bcast_file_tx_next(tx, frame, (size_t)fs));
        if (frame_type(frame) == BCAST_PACKET_RQ_CONFIG) configs++;
    }
    TEST_ASSERT_EQUAL_INT(3, configs);

    free(frame);
    bcast_file_tx_close(tx);
    remove(TMP);
}

/* A bounded run must stop on its own; an endless one must not. */
void test_cycle_budget_is_honoured(void)
{
    char err[160] = {0};
    write_file(TMP, 8000, 9);

    bcast_file_tx_t *tx = bcast_file_tx_open(TMP, 0, 2, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(tx, err);
    int fs = bcast_file_tx_frame_size(tx);
    uint8_t *frame = malloc((size_t)fs);
    size_t bytes; int blocks;
    bcast_file_tx_source(tx, &bytes, &blocks);

    int n = 0;
    while (bcast_file_tx_next(tx, frame, (size_t)fs) > 0) { n++; TEST_ASSERT_LESS_THAN_INT(10000, n); }
    TEST_ASSERT_EQUAL_INT(2 * (1 + blocks), n);

    int cyc, tot; uint64_t sentf;
    bcast_file_tx_stats(tx, &cyc, &tot, &sentf);
    TEST_ASSERT_EQUAL_INT(2, cyc);
    TEST_ASSERT_EQUAL_INT(2, tot);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)n, sentf);
    bcast_file_tx_close(tx);

    /* endless: still going long after a bounded run would have stopped */
    tx = bcast_file_tx_open(TMP, 0, 0, err, sizeof(err));
    TEST_ASSERT_NOT_NULL(tx);
    for (int i = 0; i < 5 * (1 + blocks); i++)
        TEST_ASSERT_EQUAL_INT(fs, bcast_file_tx_next(tx, frame, (size_t)fs));
    bcast_file_tx_close(tx);

    free(frame);
    remove(TMP);
}

void test_oversized_file_is_refused(void)
{
    char err[160] = {0};
    write_file(TMP, BCAST_FILE_MAX_BYTES + 1, 11);
    TEST_ASSERT_NULL(bcast_file_tx_open(TMP, 0, 1, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "limit"));
    remove(TMP);
}

void test_empty_and_missing_files_are_refused(void)
{
    char err[160] = {0};
    FILE *f = fopen(TMP, "wb"); fclose(f);          /* zero bytes */
    TEST_ASSERT_NULL(bcast_file_tx_open(TMP, 0, 1, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "empty"));
    remove(TMP);
    TEST_ASSERT_NULL(bcast_file_tx_open("/nonexistent/nope", 0, 1, err, sizeof(err)));
}

/* DATAC14 carries 3 bytes; the 9-byte config packet cannot fit, and without
 * the guard the symbol size underflows. */
void test_modes_too_small_for_broadcast_are_refused(void)
{
    char err[160] = {0};
    write_file(TMP, 4000, 13);
    TEST_ASSERT_FALSE(bcast_file_mode_usable(5));
    TEST_ASSERT_NULL(bcast_file_tx_open(TMP, 5, 1, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "at least"));

    TEST_ASSERT_NULL(bcast_file_tx_open(TMP, 11, 1, err, sizeof(err)));
    TEST_ASSERT_NULL(bcast_file_tx_open(TMP, -1, 1, err, sizeof(err)));
    remove(TMP);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_clean_channel_recovers_the_file);
    RUN_TEST(test_lossy_channel_recovers_the_file);
    RUN_TEST(test_small_robust_mode_recovers_the_file);
    RUN_TEST(test_fast_mode_recovers_the_file);
    RUN_TEST(test_config_packet_repeats_every_cycle);
    RUN_TEST(test_cycle_budget_is_honoured);
    RUN_TEST(test_oversized_file_is_refused);
    RUN_TEST(test_empty_and_missing_files_are_refused);
    RUN_TEST(test_modes_too_small_for_broadcast_are_refused);
    return UNITY_END();
}
