/*
 * #579 regression: the TCP reactor never waits on the session lock
 *
 * The event loop delivers a session's received bytes into data_rx_buffer_arq
 * under g_sess_lock, and write_buffer waits on a full ring for its reader,
 * the TCP reactor.  Before #590 the reactor took g_sess_lock for its BUFFER
 * report (arq_get_runtime_snapshot, arq_is_link_connected) before draining
 * the ring, so a delivery larger than the ring's free room at that moment
 * left both threads waiting on each other for good: on the #579 bench the
 * receiver's log ended on three 2300-byte car_rx lines, the fourth never
 * fitting the 8 kB ring of the time.
 *
 * This holds the real g_sess_lock in the real cb_deliver_rx_data, blocked on
 * a full ring, and calls what the reactor calls (arq_buffer_bytes_nowait,
 * arq_link_connected_now, arq_call_in_progress, arq_discard_stale_rx) before
 * it drains the ring.  Each must return; if one waits on the lock, the test
 * reports the deadlock and stops.
 *
 * arq.c is included, as test_tcp_interfaces.c includes tcp_interfaces.c, for
 * its static lock and callbacks.  No ARQ thread is started.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../../datalink_arq/arq.c"

#include <stdio.h>
#include <unistd.h>

#include "unity.h"

/* ---- what arq.c takes from the modem and its threads ---- */

cbuf_handle_t data_rx_buffer_arq;
cbuf_handle_t data_tx_buffer_arq;
cbuf_handle_t data_tx_buffer_arq_control;

int  arq_modem_queue_init(size_t capacity) { (void)capacity; return 0; }
void arq_modem_queue_shutdown(void) { }
int  arq_modem_enqueue(const arq_action_t *action) { (void)action; return 0; }
bool arq_modem_dequeue(arq_action_t *action, int timeout_ms) { (void)action; (void)timeout_ms; return false; }
void arq_modem_set_event_fn(void (*fn)(int mode, bool ptt_on)) { (void)fn; }
bool arq_modem_channel_busy(void) { return false; }
void arq_modem_crc_seed(uint16_t seed) { (void)seed; }
void arq_modem_floor_mode(int mode) { (void)mode; }
void arq_modem_ptt_off(void) { }
void arq_modem_cut_tx(void) { }
int  arq_modem_preferred_rx_mode(const arq_session_t *sess) { (void)sess; return 0; }
int  arq_modem_preferred_tx_mode(const arq_session_t *sess) { (void)sess; return 0; }

/* ---- the #579 shape ---- */

#define FRAME   2300                                  /* a full carousel frame's payload */
#define NFRAMES (DATA_RX_BUFFER_SIZE / FRAME + 4)     /* more than the ring holds */
#define TOTAL   ((size_t)FRAME * NFRAMES)

static uint8_t       *received;
static atomic_bool    delivered_all;
static atomic_bool    reactor_done;
static atomic_int     reactor_step;                   /* the call it is in, for the report */
static bool           saw_connected_answer, saw_call_in_progress;
static int            buffer_figure = -1;

static const char *const step_name[] = {
    "start", "arq_buffer_bytes_nowait", "arq_link_connected_now",
    "arq_call_in_progress", "arq_discard_stale_rx", "drain", "done",
};

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void pause_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint8_t pattern_byte(size_t i) { return (uint8_t)(i * 31u + 7u); }

/* The event loop: one dispatch delivering a carousel burst, under the lock. */
static void *event_loop(void *arg)
{
    (void)arg;
    uint8_t frame[FRAME];
    pthread_mutex_lock(&g_sess_lock);
    for (int f = 0; f < NFRAMES; f++)
    {
        for (size_t i = 0; i < FRAME; i++)
            frame[i] = pattern_byte((size_t)f * FRAME + i);
        cb_deliver_rx_data(frame, FRAME);         /* waits on a full ring */
    }
    pthread_mutex_unlock(&g_sess_lock);
    atomic_store(&delivered_all, true);
    return NULL;
}

/* The reactor's pass: what it asks the ARQ before it drains the ring. */
static void *reactor(void *arg)
{
    (void)arg;
    atomic_store(&reactor_step, 1);
    buffer_figure = arq_buffer_bytes_nowait();
    atomic_store(&reactor_step, 2);
    bool connected = false;
    saw_connected_answer = arq_link_connected_now(&connected);
    atomic_store(&reactor_step, 3);
    saw_call_in_progress = arq_call_in_progress();
    atomic_store(&reactor_step, 4);
    arq_discard_stale_rx();                       /* gives up after ~1 s */
    atomic_store(&reactor_step, 5);

    size_t have = 0;
    uint8_t buf[4096];
    uint64_t until = mono_ms() + 10000;
    while (have < TOTAL && mono_ms() < until)
    {
        size_t n = size_buffer(data_rx_buffer_arq);
        if (n > sizeof(buf)) n = sizeof(buf);
        if (n > TOTAL - have) n = TOTAL - have;
        if (n > 0 && read_buffer(data_rx_buffer_arq, buf, n) == 0)
        {
            memcpy(received + have, buf, n);
            have += n;
        }
        else
            pause_ms(1);
    }
    atomic_store(&reactor_step, 6);
    atomic_store(&reactor_done, true);
    return NULL;
}

static uint8_t rx_storage[DATA_RX_BUFFER_SIZE];

void setUp(void)
{
    data_rx_buffer_arq = circular_buf_init(rx_storage, DATA_RX_BUFFER_SIZE);
    g_app_tx_buf = circular_buf_init(g_app_tx_storage, APP_TX_BUF_SIZE);
    g_initialized = true;
    g_sess.conn_state = ARQ_CONN_CONNECTED;
    received = calloc(1, TOTAL);
}

void tearDown(void)
{
    free(received);
    g_initialized = false;
}

void test_reactor_drains_while_a_delivery_waits_under_the_session_lock(void)
{
    atomic_store(&delivered_all, false);
    atomic_store(&reactor_done, false);
    atomic_store(&reactor_step, 0);

    pthread_t et, rt;
    pthread_create(&et, NULL, event_loop, NULL);

    /* The ring full and the event loop waiting on it, the lock held. */
    uint64_t until = mono_ms() + 5000;
    while (size_buffer(data_rx_buffer_arq) + FRAME <= DATA_RX_BUFFER_SIZE && mono_ms() < until)
        pause_ms(1);
    pause_ms(50);
    TEST_ASSERT_FALSE_MESSAGE(atomic_load(&delivered_all), "the delivery never had to wait");
    int busy = pthread_mutex_trylock(&g_sess_lock);
    if (busy == 0)
        pthread_mutex_unlock(&g_sess_lock);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, busy, "the event loop is not holding the session lock");

    pthread_create(&rt, NULL, reactor, NULL);
    until = mono_ms() + 15000;
    while (!(atomic_load(&reactor_done) && atomic_load(&delivered_all)) && mono_ms() < until)
        pause_ms(5);
    if (!atomic_load(&reactor_done) || !atomic_load(&delivered_all))
    {
        /* Both threads are stuck on the lock and the ring for good: nothing
         * after this can run beside them. */
        printf("%s:%d:%s:FAIL: the reactor waited on the session lock in %s "
               "while the event loop waited on it (#579 deadlock)\n",
               __FILE__, __LINE__, __func__, step_name[atomic_load(&reactor_step)]);
        fflush(stdout);
        _exit(1);
    }
    pthread_join(rt, NULL);
    pthread_join(et, NULL);

    TEST_ASSERT_FALSE(saw_connected_answer);      /* the lock was busy: no answer, no wait */
    TEST_ASSERT_TRUE(saw_call_in_progress);       /* busy reads as a call: the safe answer */
    TEST_ASSERT_EQUAL_INT(0, buffer_figure);      /* nothing of ours queued */
    for (size_t i = 0; i < TOTAL; i++)            /* the discard left a live session's bytes */
        if (received[i] != pattern_byte(i))
            TEST_FAIL_MESSAGE("received bytes differ from those delivered");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reactor_drains_while_a_delivery_waits_under_the_session_lock);
    return UNITY_END();
}
