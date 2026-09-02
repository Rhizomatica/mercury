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
/* Offsets are daemon.c's: config body at [1..8], tag at [9..11], symbol at 12. */
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
static uint8_t frame_session(const uint8_t *f)
{
    return (uint8_t)(f[0] & BCAST_FRAME_EXT_MASK);
}

/* Drive a whole transfer through a lossy channel and require the file back.
 *
 * loss_pct is applied pseudo-randomly.  See
 * test_periodic_loss_does_not_starve_a_block for why periodic loss is also
 * tested separately. */
static void transfer_case(size_t nbytes, int mode, int loss_pct, unsigned seed)
{
    char err[160] = {0};
    write_file(TMP, nbytes, seed);
    srand(seed * 7919u + 13u);   /* loss pattern, independent of the payload */

    bcast_file_tx_t *tx = bcast_file_tx_open(TMP, mode, 0 /* endless */, 0, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(tx, err);

    int fs = bcast_file_tx_frame_size(tx);
    TEST_ASSERT_EQUAL_INT(bcast_file_mode_frame_size(mode), fs);

    uint8_t *frame = malloc((size_t)fs);
    uint8_t *src = malloc(nbytes);
    FILE *f = fopen(TMP, "rb"); TEST_ASSERT_EQUAL_size_t(nbytes, fread(src, 1, nbytes, f)); fclose(f);

    /* The transfer carries the bundle, which is longer than the file. */
    size_t bundle_len = 0;
    uint8_t *ref = bcast_bundle_build(TMP, &bundle_len, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(ref, err);
    free(ref);
    size_t outsz = bundle_len;
    uint8_t *out = malloc(outsz);

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

        TEST_ASSERT_EQUAL_UINT8(BCAST_PACKET_RQ_CONFIG, frame_type(frame));

        /* Every frame carries the OTI, so the receiver can start on any of
         * them -- including the very first one it hears. */
        if (!dec)
        {
            dec = nanorq_decoder_new(rx_oti_common(frame), rx_oti_scheme(frame));
            TEST_ASSERT_NOT_NULL_MESSAGE(dec, "decoder rejected the frame's OTI");
            TEST_ASSERT_EQUAL_size_t(outsz, nanorq_transfer_length(dec));
            rio = ioctx_from_file(outf, 0);
            TEST_ASSERT_NOT_NULL(rio);
        }

        const uint8_t *tagp = frame + 1 + BCAST_CONFIG_BODY_SIZE;
        uint32_t tag = ((uint32_t)tagp[0] << 24) | tagp[1] | ((uint32_t)tagp[2] << 8);
        nanorq_decoder_add_symbol(dec, frame + BCAST_FRAME_OVERHEAD, tag, rio);

        done = 1;
        for (int b = 0; b < nanorq_blocks(dec); b++)
            if (!nanorq_repair_block(dec, rio, b)) { done = 0; break; }
    }
    TEST_ASSERT_TRUE_MESSAGE(done, "receiver never decoded the file");

    rio->destroy(rio);
    f = fopen(outf, "rb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_size_t(outsz, fread(out, 1, outsz, f));
    fclose(f);
    /* What came back is the bundle, so unwrap it the way a receiver does and
     * require BOTH the original name and the original bytes. */
    char gotname[BCAST_BUNDLE_NAME_MAX + 1] = {0};
    const uint8_t *payload = NULL;
    size_t paylen = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0,
        bcast_bundle_parse(out, (size_t)outsz, gotname, sizeof(gotname), &payload, &paylen),
        "recovered data is not a valid bundle");
    TEST_ASSERT_EQUAL_STRING(".mercury_bcast_test.bin", gotname);
    TEST_ASSERT_EQUAL_size_t(nbytes, paylen);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(src, payload, nbytes, "recovered file differs");

    nanorq_free(dec);
    bcast_file_tx_close(tx);
    free(frame); free(src); free(out);
    remove(TMP); remove(outf);
}

void test_clean_channel_recovers_the_file(void)      { transfer_case(20000, 0,  0, 1); }
void test_lossy_channel_recovers_the_file(void)      { transfer_case(20000, 0, 20, 2); }
void test_small_robust_mode_recovers_the_file(void)  { transfer_case(1500,  8, 25, 3); }
void test_fast_mode_recovers_the_file(void)          { transfer_case(60000, 10, 30, 4); }

/* A receiver that tunes in late must still be able to start.  With the joint
 * frame that is not "the config is repeated often enough" but the stronger
 * property that EVERY frame carries it -- so this checks the OTI can be read
 * out of an arbitrary later frame, not just the first. */
void test_every_frame_is_self_describing(void)
{
    char err[160] = {0};
    write_file(TMP, 8000, 7);
    bcast_file_tx_t *tx = bcast_file_tx_open(TMP, 0, 0, 0, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(tx, err);
    int fs = bcast_file_tx_frame_size(tx);
    uint8_t *frame = malloc((size_t)fs);

    size_t bytes; int blocks;
    bcast_file_tx_source(tx, &bytes, &blocks);
    TEST_ASSERT_TRUE(bytes > 8000);  /* the bundle, not the bare file */

    uint8_t session = 0;
    for (int i = 0; i < 3 * blocks + 2; i++)
    {
        TEST_ASSERT_EQUAL_INT(fs, bcast_file_tx_next(tx, frame, (size_t)fs));
        TEST_ASSERT_EQUAL_UINT8(BCAST_PACKET_RQ_CONFIG, frame_type(frame));

        /* the OTI in THIS frame must describe the file on its own */
        nanorq *d = nanorq_decoder_new(rx_oti_common(frame), rx_oti_scheme(frame));
        TEST_ASSERT_NOT_NULL(d);
        TEST_ASSERT_TRUE(nanorq_transfer_length(d) > 8000); /* file + bundle header */
        nanorq_free(d);

        /* and the session id must be stable across the whole file */
        if (i == 0) { session = frame_session(frame); TEST_ASSERT_NOT_EQUAL_UINT8(0, session); }
        else TEST_ASSERT_EQUAL_UINT8(session, frame_session(frame));
    }

    free(frame);
    bcast_file_tx_close(tx);
    remove(TMP);
}

/* A bounded run must stop on its own; an endless one must not. */
void test_cycle_budget_is_honoured(void)
{
    char err[160] = {0};
    write_file(TMP, 8000, 9);

    bcast_file_tx_t *tx = bcast_file_tx_open(TMP, 0, 2, 0, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(tx, err);
    int fs = bcast_file_tx_frame_size(tx);
    uint8_t *frame = malloc((size_t)fs);
    size_t bytes; int blocks;
    bcast_file_tx_source(tx, &bytes, &blocks);

    int n = 0;
    while (bcast_file_tx_next(tx, frame, (size_t)fs) > 0) { n++; TEST_ASSERT_LESS_THAN_INT(10000, n); }
    TEST_ASSERT_EQUAL_INT(2 * blocks, n);

    int cyc, tot; uint64_t sentf;
    bcast_file_tx_stats(tx, &cyc, &tot, &sentf);
    TEST_ASSERT_EQUAL_INT(2, cyc);
    TEST_ASSERT_EQUAL_INT(2, tot);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)n, sentf);
    bcast_file_tx_close(tx);

    /* endless: still going long after a bounded run would have stopped */
    tx = bcast_file_tx_open(TMP, 0, 0, 0, err, sizeof(err));
    TEST_ASSERT_NOT_NULL(tx);
    for (int i = 0; i < 5 * blocks; i++)
        TEST_ASSERT_EQUAL_INT(fs, bcast_file_tx_next(tx, frame, (size_t)fs));
    bcast_file_tx_close(tx);

    free(frame);
    remove(TMP);
}

void test_oversized_file_is_refused(void)
{
    char err[160] = {0};
    write_file(TMP, BCAST_FILE_MAX_BYTES + 1, 11);
    TEST_ASSERT_NULL(bcast_file_tx_open(TMP, 0, 1, 0, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "limit"));
    remove(TMP);
}

void test_empty_and_missing_files_are_refused(void)
{
    char err[160] = {0};
    FILE *f = fopen(TMP, "wb"); fclose(f);          /* zero bytes */
    TEST_ASSERT_NULL(bcast_file_tx_open(TMP, 0, 1, 0, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "empty"));
    remove(TMP);
    TEST_ASSERT_NULL(bcast_file_tx_open("/nonexistent/nope", 0, 1, 0, err, sizeof(err)));
}

/* DATAC14 carries 3 bytes; the 9-byte config packet cannot fit, and without
 * the guard the symbol size underflows. */
void test_modes_too_small_for_broadcast_are_refused(void)
{
    char err[160] = {0};
    write_file(TMP, 4000, 13);
    TEST_ASSERT_FALSE(bcast_file_mode_usable(5));
    TEST_ASSERT_NULL(bcast_file_tx_open(TMP, 5, 1, 0, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "more than"));

    TEST_ASSERT_NULL(bcast_file_tx_open(TMP, 11, 1, 0, err, sizeof(err)));
    TEST_ASSERT_NULL(bcast_file_tx_open(TMP, -1, 1, 0, err, sizeof(err)));
    remove(TMP);
}

/* ---- the bundle itself ---- */

void test_bundle_round_trips_name_and_contents(void)
{
    char err[160] = {0};
    write_file(TMP, 777, 21);

    size_t len = 0;
    uint8_t *b = bcast_bundle_build(TMP, &len, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(b, err);

    /* Layout is mercury-connector's: LE size, then "name\n", then contents. */
    uint32_t body = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(len - 4), body);

    char name[BCAST_BUNDLE_NAME_MAX + 1] = {0};
    const uint8_t *pay = NULL; size_t paylen = 0;
    TEST_ASSERT_EQUAL_INT(0, bcast_bundle_parse(b, len, name, sizeof(name), &pay, &paylen));
    TEST_ASSERT_EQUAL_STRING(".mercury_bcast_test.bin", name);
    TEST_ASSERT_EQUAL_size_t(777, paylen);

    uint8_t *src = malloc(777);
    FILE *f = fopen(TMP, "rb"); TEST_ASSERT_EQUAL_size_t(777, fread(src, 1, 777, f)); fclose(f);
    TEST_ASSERT_EQUAL_MEMORY(src, pay, 777);

    free(src); free(b);
    remove(TMP);
}

/* Only the basename travels, so a receiver cannot be steered out of its
 * directory by the sender's path. */
void test_bundle_sends_only_the_basename(void)
{
    char err[160] = {0};
    write_file("/tmp/.mercury_bcast_test.bin", 32, 22);
    size_t len = 0;
    uint8_t *b = bcast_bundle_build("/tmp/.mercury_bcast_test.bin", &len, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(b, err);
    char name[BCAST_BUNDLE_NAME_MAX + 1] = {0};
    TEST_ASSERT_EQUAL_INT(0, bcast_bundle_parse(b, len, name, sizeof(name), NULL, NULL));
    TEST_ASSERT_EQUAL_STRING(".mercury_bcast_test.bin", name);
    TEST_ASSERT_NULL(strchr(name, '/'));
    free(b);
    remove(TMP);
}

/* A bundle arriving off the air is untrusted input.  Malformed ones must be
 * rejected, and a name that would escape the download directory must never be
 * handed back to the caller. */
void test_bundle_parse_rejects_malformed_and_hostile_input(void)
{
    char name[BCAST_BUNDLE_NAME_MAX + 1];
    uint8_t buf[64];

    TEST_ASSERT_EQUAL_INT(-1, bcast_bundle_parse(NULL, 32, name, sizeof(name), NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, bcast_bundle_parse(buf, 3, name, sizeof(name), NULL, NULL));

    /* size field disagrees with the buffer */
    memset(buf, 0, sizeof(buf));
    buf[0] = 99; memcpy(buf + 4, "a\nxy", 4);
    TEST_ASSERT_EQUAL_INT(-1, bcast_bundle_parse(buf, 8, name, sizeof(name), NULL, NULL));

    /* no '\n' terminator anywhere */
    memset(buf, 'A', sizeof(buf));
    buf[0] = 12; buf[1] = buf[2] = buf[3] = 0;
    TEST_ASSERT_EQUAL_INT(-1, bcast_bundle_parse(buf, 16, name, sizeof(name), NULL, NULL));

    /* empty name */
    memset(buf, 0, sizeof(buf));
    buf[0] = 5; memcpy(buf + 4, "\nabcd", 5);
    TEST_ASSERT_EQUAL_INT(-1, bcast_bundle_parse(buf, 9, name, sizeof(name), NULL, NULL));

    /* path traversal, in both separator flavours, and the dot names */
    const char *hostile[] = { "../etc/passwd", "/etc/passwd", "..\\windows", "..", "." };
    for (unsigned i = 0; i < sizeof(hostile)/sizeof(hostile[0]); i++)
    {
        size_t n = strlen(hostile[i]);
        size_t body = n + 1 + 2;
        memset(buf, 0, sizeof(buf));
        buf[0] = (uint8_t)(body & 0xff); buf[1] = (uint8_t)(body >> 8);
        memcpy(buf + 4, hostile[i], n);
        buf[4 + n] = '\n';
        buf[4 + n + 1] = 'h'; buf[4 + n + 2] = 'i';
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1,
            bcast_bundle_parse(buf, 4 + body, name, sizeof(name), NULL, NULL),
            hostile[i]);
    }
}

/* Periodic loss must not be able to starve the transfer.
 *
 * RaptorQ codes each source block independently, so a symbol for one block does
 * nothing for another.  With the default Z=16 partitioning, a loss pattern
 * whose period matches the carousel hits the same block every cycle and that
 * block receives NOTHING -- measured: 0 symbols out of 15000 dropped, never
 * decoding after 60000 frames.  Encoding as a single block removes the failure
 * mode outright, because then there is only one block to feed and any K+e
 * symbols decode it.
 *
 * This pins that.  It fails if the encoder ever goes back to multi-block for a
 * file this size. */
void test_periodic_loss_does_not_starve_a_block(void)
{
    char err[160] = {0};
    write_file(TMP, 20000, 31);
    bcast_file_tx_t *tx = bcast_file_tx_open(TMP, 0, 0, 0, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(tx, err);

    size_t bundle_bytes; int blocks;
    bcast_file_tx_source(tx, &bundle_bytes, &blocks);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, blocks,
        "a file this size must encode as ONE source block");

    int fs = bcast_file_tx_frame_size(tx);
    uint8_t *frame = malloc((size_t)fs);
    nanorq *dec = NULL; struct ioctx *rio = NULL;
    const char *outf = "/tmp/.mercury_bcast_periodic.bin";
    int sent = 0, done = 0;

    /* Drop on a strict period -- the pattern that starved a block before. */
    for (int i = 0; i < 20000 && !done; i++)
    {
        TEST_ASSERT_EQUAL_INT(fs, bcast_file_tx_next(tx, frame, (size_t)fs));
        sent++;
        if (sent % 4 == 0) continue;

        if (!dec)
        {
            dec = nanorq_decoder_new(rx_oti_common(frame), rx_oti_scheme(frame));
            TEST_ASSERT_NOT_NULL(dec);
            rio = ioctx_from_file(outf, 0);
        }
        const uint8_t *tagp = frame + 1 + BCAST_CONFIG_BODY_SIZE;
        uint32_t tag = ((uint32_t)tagp[0] << 24) | tagp[1] | ((uint32_t)tagp[2] << 8);
        nanorq_decoder_add_symbol(dec, frame + BCAST_FRAME_OVERHEAD, tag, rio);
        done = 1;
        for (int b = 0; b < nanorq_blocks(dec); b++)
            if (!nanorq_repair_block(dec, rio, b)) { done = 0; break; }
    }
    TEST_ASSERT_TRUE_MESSAGE(done, "periodic loss starved the transfer");

    rio->destroy(rio);
    nanorq_free(dec);
    bcast_file_tx_close(tx);
    free(frame);
    remove(TMP); remove(outf);
}

/* ---- TX straight into RX ---- */

/* The pair must work together, joining mid-carousel and surviving loss, and the
 * receiver must write the file under the name the sender gave it. */
void test_tx_and_rx_complete_a_transfer(void)
{
    char err[192] = {0};
    const size_t N = 5000;
    write_file(TMP, N, 41);

    char dir[] = "/tmp/.mercury_bcast_rxdirXXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));

    bcast_file_tx_t *tx = bcast_file_tx_open(TMP, 0, 0, 0, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(tx, err);
    bcast_file_rx_t *rx = bcast_file_rx_open(0, dir, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(rx, err);

    int fs = bcast_file_tx_frame_size(tx);
    uint8_t *frame = malloc((size_t)fs);
    srand(4242);

    /* Skip the first few frames outright: a receiver tunes in mid-carousel. */
    for (int i = 0; i < 3; i++)
        TEST_ASSERT_EQUAL_INT(fs, bcast_file_tx_next(tx, frame, (size_t)fs));

    int done = 0;
    for (int i = 0; i < 5000 && !done; i++)
    {
        TEST_ASSERT_EQUAL_INT(fs, bcast_file_tx_next(tx, frame, (size_t)fs));
        if (rand() % 100 < 20) continue;                 /* 20% loss */
        switch (bcast_file_rx_frame(rx, frame, (size_t)fs))
        {
        case BCAST_RX_COMPLETE: done = 1; break;
        case BCAST_RX_ERROR:    TEST_FAIL_MESSAGE(bcast_file_rx_error(rx)); break;
        default: break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(done, "receiver never completed the file");
    TEST_ASSERT_EQUAL_STRING(".mercury_bcast_test.bin", bcast_file_rx_last_name(rx));

    /* contents must match the original exactly */
    uint8_t *a = malloc(N), *b = malloc(N);
    FILE *f = fopen(TMP, "rb"); TEST_ASSERT_EQUAL_size_t(N, fread(a, 1, N, f)); fclose(f);
    f = fopen(bcast_file_rx_last_path(rx), "rb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_size_t(N, fread(b, 1, N, f));
    fclose(f);
    TEST_ASSERT_EQUAL_MEMORY(a, b, N);

    /* The sender keeps going; that must not restart the same file. */
    for (int i = 0; i < 20; i++)
    {
        TEST_ASSERT_EQUAL_INT(fs, bcast_file_tx_next(tx, frame, (size_t)fs));
        TEST_ASSERT_EQUAL_INT(BCAST_RX_IGNORED, bcast_file_rx_frame(rx, frame, (size_t)fs));
    }

    free(a); free(b); free(frame);
    bcast_file_tx_close(tx);
    bcast_file_rx_close(rx);
    remove(TMP);
}

/* Traffic from other users of the broadcast plane -- chat, a different mode --
 * must be passed over, not mistaken for corruption. */
void test_rx_ignores_frames_that_are_not_ours(void)
{
    char err[192] = {0};
    char dir[] = "/tmp/.mercury_bcast_rxdir2XXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    bcast_file_rx_t *rx = bcast_file_rx_open(0, dir, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(rx, err);

    uint8_t buf[510];
    memset(buf, 0, sizeof(buf));

    /* wrong length (a chat frame) */
    TEST_ASSERT_EQUAL_INT(BCAST_RX_IGNORED, bcast_file_rx_frame(rx, buf, 40));
    /* right length, wrong packet type */
    buf[0] = (uint8_t)(0x01 << BCAST_PACKET_TYPE_SHIFT) | 3;
    TEST_ASSERT_EQUAL_INT(BCAST_RX_IGNORED, bcast_file_rx_frame(rx, buf, sizeof(buf)));
    /* right type, session 0 is "no session" */
    buf[0] = (uint8_t)(BCAST_PACKET_RQ_CONFIG << BCAST_PACKET_TYPE_SHIFT);
    TEST_ASSERT_EQUAL_INT(BCAST_RX_IGNORED, bcast_file_rx_frame(rx, buf, sizeof(buf)));

    bcast_file_rx_close(rx);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tx_and_rx_complete_a_transfer);
    RUN_TEST(test_rx_ignores_frames_that_are_not_ours);
    RUN_TEST(test_periodic_loss_does_not_starve_a_block);
    RUN_TEST(test_bundle_round_trips_name_and_contents);
    RUN_TEST(test_bundle_sends_only_the_basename);
    RUN_TEST(test_bundle_parse_rejects_malformed_and_hostile_input);
    RUN_TEST(test_clean_channel_recovers_the_file);
    RUN_TEST(test_lossy_channel_recovers_the_file);
    RUN_TEST(test_small_robust_mode_recovers_the_file);
    RUN_TEST(test_fast_mode_recovers_the_file);
    RUN_TEST(test_every_frame_is_self_describing);
    RUN_TEST(test_cycle_budget_is_honoured);
    RUN_TEST(test_oversized_file_is_refused);
    RUN_TEST(test_empty_and_missing_files_are_refused);
    RUN_TEST(test_modes_too_small_for_broadcast_are_refused);
    return UNITY_END();
}
