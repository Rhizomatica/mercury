/* play_recover: what the playback thread does about a device that stopped
 * taking audio (audioio/play_recover.h). */
#include "unity.h"
#include "play_recover.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RING_SIZE  4096
#define CHUNK      1024

static uint8_t ring_mem[RING_SIZE];
static cbuf_handle_t ring;
static uint8_t scratch[512];

void setUp(void)
{
    memset(ring_mem, 0, sizeof(ring_mem));
    ring = circular_buf_init(ring_mem, RING_SIZE);
}

void tearDown(void)
{
    circular_buf_free(ring);
}

/* The modem: keys PTT, then write_buffer()s its bursts, which blocks while
 * the ring has no room. */
static atomic_bool producer_done;
static void *producer(void *arg)
{
    int chunks = *(int *) arg;
    uint8_t chunk[CHUNK];
    memset(chunk, 0x5a, sizeof(chunk));
    for (int i = 0; i < chunks; i++)
        write_buffer(ring, chunk, sizeof(chunk));
    atomic_store(&producer_done, true);
    return NULL;
}

static bool never_stop(void) { return false; }
static bool stop_now(void)   { return true; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

/* A full buffer on a non-blocking stream takes nothing for a period or two:
 * not a stall. */
void test_short_gap_is_not_a_stall(void)
{
    play_recover_t r = {0};
    TEST_ASSERT_FALSE(play_recover_on_stuck(&r, 5000));
    TEST_ASSERT_FALSE(play_recover_on_stuck(&r, 5000 + PLAY_STALL_MS - 1));
    TEST_ASSERT_EQUAL_INT(0, play_recover_on_write(&r));
    /* The window starts again after the device takes something. */
    TEST_ASSERT_FALSE(play_recover_on_stuck(&r, 5000 + PLAY_STALL_MS + 10));
}

/* Issue #591: a stream that fails every write is reopened after the window,
 * not written to for ever. */
void test_stuck_past_window_reopens(void)
{
    play_recover_t r = {0};
    TEST_ASSERT_FALSE(play_recover_on_stuck(&r, 100));
    TEST_ASSERT_TRUE(play_recover_on_stuck(&r, 100 + PLAY_STALL_MS));
}

/* A reopen gives the device a whole window again. */
void test_reopen_restarts_window(void)
{
    play_recover_t r = {0};
    play_recover_on_stuck(&r, 0);
    TEST_ASSERT_TRUE(play_recover_on_stuck(&r, PLAY_STALL_MS));
    TEST_ASSERT_FALSE(play_recover_on_reopen(&r));
    TEST_ASSERT_FALSE(play_recover_on_stuck(&r, PLAY_STALL_MS + 1));
    TEST_ASSERT_FALSE(play_recover_on_stuck(&r, 2 * PLAY_STALL_MS));
    TEST_ASSERT_TRUE(play_recover_on_stuck(&r, 2 * PLAY_STALL_MS + 1));
}

/* A reopen that cures it is said once and never called dead. */
void test_cured_reopen_reports_back_not_alive(void)
{
    play_recover_t r = {0};
    TEST_ASSERT_FALSE(play_recover_on_reopen(&r));
    TEST_ASSERT_EQUAL_INT(PLAY_REC_BACK, play_recover_on_write(&r));
    TEST_ASSERT_EQUAL_INT(0, play_recover_on_write(&r));
}

/* Reopens that bring nothing: dead at the third, reported once; the first
 * audio after that is running again. */
void test_dead_after_dry_reopens_reported_once(void)
{
    play_recover_t r = {0};
    for (int i = 1; i < PLAY_DRY_REOPENS_DEAD; i++)
        TEST_ASSERT_FALSE(play_recover_on_reopen(&r));
    TEST_ASSERT_TRUE(play_recover_on_reopen(&r));
    for (int i = 0; i < 20; i++)
        TEST_ASSERT_FALSE(play_recover_on_reopen(&r));
    TEST_ASSERT_EQUAL_INT(PLAY_REC_BACK | PLAY_REC_ALIVE, play_recover_on_write(&r));
    TEST_ASSERT_FALSE(r.dead);
    /* ...and a later failure counts from zero. */
    for (int i = 1; i < PLAY_DRY_REOPENS_DEAD; i++)
        TEST_ASSERT_FALSE(play_recover_on_reopen(&r));
    TEST_ASSERT_TRUE(play_recover_on_reopen(&r));
}

void test_drop_queued_empties_ring(void)
{
    uint8_t data[3000];
    memset(data, 1, sizeof(data));
    write_buffer(ring, data, sizeof(data));
    TEST_ASSERT_EQUAL_size_t(3000, play_drop_queued(ring, scratch, sizeof(scratch)));
    TEST_ASSERT_EQUAL_size_t(0, size_buffer(ring));
    TEST_ASSERT_EQUAL_size_t(0, play_drop_queued(ring, scratch, sizeof(scratch)));
}

/* #617 review: while the device cannot be reopened, the modem goes on
 * queueing bursts.  Four times what the ring holds must not leave it blocked
 * in write_buffer(), which on air is the TX thread stuck with PTT keyed. */
void test_backoff_wait_keeps_producer_unblocked(void)
{
    int chunks = 4 * RING_SIZE / CHUNK;
    atomic_store(&producer_done, false);
    pthread_t t;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, producer, &chunks));

    /* Two backoff waits, as two failed reopen attempts make. */
    play_backoff_wait(ring, scratch, sizeof(scratch), 400, never_stop);
    play_backoff_wait(ring, scratch, sizeof(scratch), 800, never_stop);

    bool done = atomic_load(&producer_done);
    if (!done)
    {
        /* Free the producer before failing, so the test process can end. */
        while (!atomic_load(&producer_done))
        {
            play_drop_queued(ring, scratch, sizeof(scratch));
            usleep(1000);
        }
    }
    pthread_join(t, NULL);
    TEST_ASSERT_TRUE_MESSAGE(done, "the producer stayed blocked on a full ring");
    TEST_ASSERT_EQUAL_size_t(0, size_buffer(ring));
}

/* A shutdown during the backoff ends it at once, and what was queued is
 * still dropped. */
void test_backoff_wait_stops_early_and_drains(void)
{
    uint8_t data[CHUNK];
    memset(data, 2, sizeof(data));
    write_buffer(ring, data, sizeof(data));
    uint64_t t0 = now_ms();
    play_backoff_wait(ring, scratch, sizeof(scratch), 5000, stop_now);
    TEST_ASSERT_LESS_THAN_UINT64(100, now_ms() - t0);
    TEST_ASSERT_EQUAL_size_t(0, size_buffer(ring));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_gap_is_not_a_stall);
    RUN_TEST(test_stuck_past_window_reopens);
    RUN_TEST(test_reopen_restarts_window);
    RUN_TEST(test_cured_reopen_reports_back_not_alive);
    RUN_TEST(test_dead_after_dry_reopens_reported_once);
    RUN_TEST(test_drop_queued_empties_ring);
    RUN_TEST(test_backoff_wait_keeps_producer_unblocked);
    RUN_TEST(test_backoff_wait_stops_early_and_drains);
    return UNITY_END();
}
