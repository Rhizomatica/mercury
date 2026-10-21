/* The host's broadcast carousel (datalink_broadcast/bcast_carousel.c): its
 * thread feeds the broadcast TX ring a frame at a time, a receiver decodes
 * the file from what it feeds, ARQ stops it, and one runs at a time.
 * The modem is a consumer thread here; ARQ and the listen mode are stubs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "bcast_carousel.h"
#include "bcast_status.h"
#include "bcast_file.h"
#include "ring_buffer_posix.h"

/* ---- what the carousel uses ---- */
_Atomic bool mercury_stop = false;
cbuf_handle_t data_tx_buffer_broadcast;
static uint8_t ring_mem[16384];
static _Atomic bool s_arq = false;
bool arq_session_active(void) { return atomic_load(&s_arq); }
int  freedv_modes[] = { 1001, 1002 };           /* index 0: DATAC1 for bcast_file */
char *freedv_mode_names[] = { "DATAC1", "DATAC3" };
int  mercury_cli_mode_count(void) { return 2; }
int  modem_get_listen_mode(void) { return 1001; }
void msleep(int ms) { usleep((useconds_t)ms * 1000); }

/* the status registry, as broadcast.c keeps it */
static pthread_mutex_t s_car_lock = PTHREAD_MUTEX_INITIALIZER;
static bcast_car_status_t s_car;
void bcast_carousel_publish(const bcast_car_status_t *st) { pthread_mutex_lock(&s_car_lock); s_car = *st; pthread_mutex_unlock(&s_car_lock); }
void bcast_carousel_get(bcast_car_status_t *o) { pthread_mutex_lock(&s_car_lock); *o = s_car; pthread_mutex_unlock(&s_car_lock); }
bool bcast_carousel_sending(void) { bcast_car_status_t s; bcast_carousel_get(&s); return s.state == BCAST_CAR_SENDING; }
const char *bcast_car_state_name(bcast_car_state_t st) { (void)st; return ""; }

/* ---- the modem: takes frames off the ring, as the TX thread would ---- */
static _Atomic bool s_consume = true;
static _Atomic int  s_taken = 0;
static size_t s_fs;
static bcast_file_rx_t *s_rx;
static _Atomic bool s_done = false;
static size_t s_max_waiting = 0;

static void *consumer(void *arg)
{
    (void)arg;
    uint8_t f[2048];
    while (atomic_load(&s_consume))
    {
        size_t n = size_buffer(data_tx_buffer_broadcast);
        if (n > s_max_waiting) s_max_waiting = n;
        if (n >= s_fs)
        {
            read_buffer(data_tx_buffer_broadcast, f, s_fs);
            atomic_fetch_add(&s_taken, 1);
            if (s_rx && bcast_file_rx_frame(s_rx, f, s_fs) == BCAST_RX_COMPLETE)
                atomic_store(&s_done, true);
        }
        usleep(1000);
    }
    return NULL;
}

static pthread_t s_cons;
static uint8_t *s_data;
#define N 2000

void setUp(void)
{
    data_tx_buffer_broadcast = circular_buf_init(ring_mem, sizeof(ring_mem));
    s_fs = (size_t)bcast_file_mode_frame_size(0);
    s_data = malloc(N);
    for (int i = 0; i < N; i++) s_data[i] = (uint8_t)(i * 13 + 5);
    atomic_store(&s_arq, false); atomic_store(&s_taken, 0); atomic_store(&s_done, false);
    atomic_store(&s_consume, true);
    s_max_waiting = 0;
    memset(&s_car, 0, sizeof(s_car));
    s_rx = NULL;
}

void tearDown(void)
{
    bcast_carousel_stop();
    atomic_store(&s_consume, false);
    pthread_join(s_cons, NULL);
    if (s_rx) bcast_file_rx_close(s_rx);
    free(s_data);
    circular_buf_free(data_tx_buffer_broadcast);
}

static bcast_car_status_t wait_not_sending(int ms)
{
    bcast_car_status_t st;
    for (int i = 0; i < ms / 10; i++)
    {
        bcast_carousel_get(&st);
        if (st.state != BCAST_CAR_SENDING) return st;
        usleep(10000);
    }
    bcast_carousel_get(&st);
    return st;
}

/* Its cycles go out whole -- a cycle is one frame per RaptorQ block, so a
 * 2 KB file on DATAC1 (12 symbols of 41 B a frame) needs 5: 10 sent -- a
 * receiver decodes the file from them, and the thread never had more than
 * one frame waiting in the ring. */
void test_sends_its_cycles_and_a_receiver_decodes_them(void)
{
    char err[160], dir[] = "/tmp/.mercury_car_rxXXXXXX";
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    s_rx = bcast_file_rx_open(0, dir, err, sizeof(err));
    TEST_ASSERT_NOT_NULL_MESSAGE(s_rx, err);
    pthread_create(&s_cons, NULL, consumer, NULL);

    TEST_ASSERT_EQUAL_INT(0, bcast_carousel_send("note.txt", s_data, N, 10, err, sizeof(err)));
    TEST_ASSERT_EQUAL_INT(BCAST_CAROUSEL_BUSY, bcast_carousel_send("other", s_data, N, 1, err, sizeof(err)));
    bcast_car_status_t st = wait_not_sending(20000);
    TEST_ASSERT_EQUAL_INT(BCAST_CAR_DONE, st.state);
    TEST_ASSERT_EQUAL_INT(10, st.cycles_total);
    TEST_ASSERT_EQUAL_INT(10, st.cycle);
    TEST_ASSERT_EQUAL_UINT64(10, st.frames_sent);
    usleep(50000);
    TEST_ASSERT_TRUE(atomic_load(&s_done));
    TEST_ASSERT_EQUAL_STRING("note.txt", bcast_file_rx_last_name(s_rx));
    TEST_ASSERT_TRUE(s_max_waiting <= s_fs);              /* one frame at most */
    remove(bcast_file_rx_last_path(s_rx));
    rmdir(dir);
}

/* An ARQ session starting stops it, and no more frames follow. */
void test_an_arq_session_stops_it(void)
{
    char err[160];
    pthread_create(&s_cons, NULL, consumer, NULL);
    TEST_ASSERT_EQUAL_INT(0, bcast_carousel_send("note.txt", s_data, N, 0, err, sizeof(err)));
    usleep(100000);
    atomic_store(&s_arq, true);
    bcast_car_status_t st = wait_not_sending(2000);
    TEST_ASSERT_EQUAL_INT(BCAST_CAR_STOPPED_BY_ARQ, st.state);
    usleep(50000);
    int taken = atomic_load(&s_taken);
    usleep(200000);
    TEST_ASSERT_EQUAL_INT(taken, atomic_load(&s_taken));   /* nothing after */
    /* ...and none starts while the session is up */
    TEST_ASSERT_EQUAL_INT(BCAST_CAROUSEL_BUSY, bcast_carousel_send("x", s_data, N, 1, err, sizeof(err)));
}

/* Stopped by the host: at the next frame, state "stopped"; a new one may go. */
void test_stop_ends_it_and_frees_the_plane(void)
{
    char err[160];
    pthread_create(&s_cons, NULL, consumer, NULL);
    TEST_ASSERT_EQUAL_INT(0, bcast_carousel_send("note.txt", s_data, N, 0, err, sizeof(err)));
    usleep(50000);
    bcast_carousel_stop();
    bcast_car_status_t st;
    bcast_carousel_get(&st);
    TEST_ASSERT_EQUAL_INT(BCAST_CAR_STOPPED, st.state);
    TEST_ASSERT_EQUAL_INT(0, bcast_carousel_send("again.txt", s_data, N, 1, err, sizeof(err)));
}

/* A name with a path, or nothing to send, is refused before anything starts. */
void test_bad_input_is_refused(void)
{
    char err[160];
    pthread_create(&s_cons, NULL, consumer, NULL);
    TEST_ASSERT_EQUAL_INT(BCAST_CAROUSEL_BAD, bcast_carousel_send("../x", s_data, N, 1, err, sizeof(err)));
    TEST_ASSERT_EQUAL_INT(BCAST_CAROUSEL_BAD, bcast_carousel_send("x", s_data, 0, 1, err, sizeof(err)));
    TEST_ASSERT_FALSE(bcast_carousel_sending());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sends_its_cycles_and_a_receiver_decodes_them);
    RUN_TEST(test_an_arq_session_stops_it);
    RUN_TEST(test_stop_ends_it_and_frees_the_plane);
    RUN_TEST(test_bad_input_is_refused);
    return UNITY_END();
}
