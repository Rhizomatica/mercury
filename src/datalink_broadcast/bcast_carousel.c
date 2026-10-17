/* The host's broadcast carousel: a file from memory, sent by Mercury itself
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A host program (hermes-radio-daemon) hands Mercury a message and the name
 * it is filed under; a thread of Mercury's own turns it into RaptorQ frames
 * (bcast_file_tx_open_mem) and feeds them to the broadcast TX ring at the
 * listen mode's frame size -- what a TCP client running bcast_file_tool does
 * through the broadcast port, without the port.
 *
 * One carousel at a time.  ARQ comes first: a session starting either way
 * (arq_session_active) stops the carousel, STOPPED_BY_ARQ, and it does not
 * resume -- the host decides whether to send again (same message, same
 * session id: a receiver keeps the symbols it has).  So that no stale tail
 * follows the session, the thread keeps at most one frame waiting in the
 * ring: the engine sends broadcast frames in bursts of one (init_modem in
 * mercury_engine.c), and the ring holds the next while one goes out.
 *
 * While it sends, the listen mode cannot change (modem_set_listen_mode answers
 * BUSY, and MODE with it); the state and progress are published
 * (bcast_carousel_publish) for Mercury's status and the host.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bcast_carousel.h"
#include "bcast_file.h"
#include "broadcast.h"
#include "arq.h"
#include "modem.h"
#include "mercury_cli.h"
#include "ring_buffer_posix.h"
#include "hermes_log.h"
#include "os_interop.h"

extern cbuf_handle_t data_tx_buffer_broadcast;
extern _Atomic bool mercury_stop;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       s_thread;
static bool            s_thread_live = false;   /* joinable */
static _Atomic bool    s_stop_req = false;
static bcast_file_tx_t *s_tx = NULL;
static bcast_car_status_t s_st;                  /* the thread's; published */

#define PUMP_POLL_MS 20

static int listen_index(void)
{
    int mode = modem_get_listen_mode();
    for (int i = 0; i < mercury_cli_mode_count(); i++)
        if (freedv_modes[i] == mode)
            return i;
    return -1;
}

static void say(char *err, size_t errlen, const char *why)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", why);
}

static void publish(void)
{
    bcast_carousel_publish(&s_st);
}

static void *pump(void *arg)
{
    (void)arg;
    uint8_t frame[2048];
    size_t fs = (size_t)bcast_file_tx_frame_size(s_tx);

    while (!atomic_load(&s_stop_req) && !mercury_stop)
    {
        if (arq_session_active())
        {
            s_st.state = BCAST_CAR_STOPPED_BY_ARQ;
            HLOGI("bcast-car", "'%s' stopped: an ARQ session started", s_st.name);
            break;
        }
        if (size_buffer(data_tx_buffer_broadcast) >= fs)
        {
            msleep(PUMP_POLL_MS);                 /* one frame waits: enough */
            continue;
        }
        int n = bcast_file_tx_next(s_tx, frame, sizeof(frame));
        if (n == 0)
        {
            s_st.state = BCAST_CAR_DONE;
            HLOGI("bcast-car", "'%s' done: %llu frames", s_st.name,
                  (unsigned long long)s_st.frames_sent);
            break;
        }
        if (n < 0 || (size_t)n != fs)
        {
            s_st.state = BCAST_CAR_ERROR;
            HLOGW("bcast-car", "'%s': the carousel failed", s_st.name);
            break;
        }
        write_buffer(data_tx_buffer_broadcast, frame, (size_t)n);
        int cycle = 0, total = 0;
        uint64_t sent = 0;
        bcast_file_tx_stats(s_tx, &cycle, &total, &sent);
        s_st.cycle = cycle;
        s_st.cycles_total = total;
        s_st.frames_sent = sent;
        publish();
    }
    if (s_st.state == BCAST_CAR_SENDING)
    {
        s_st.state = BCAST_CAR_STOPPED;
        HLOGI("bcast-car", "'%s' stopped", s_st.name);
    }
    publish();
    return NULL;
}

/* Join a carousel that has ended, freeing its encoder.  s_lock held. */
static void reap_locked(void)
{
    if (s_thread_live && !bcast_carousel_sending())
    {
        pthread_join(s_thread, NULL);
        s_thread_live = false;
    }
    if (!s_thread_live && s_tx)
    {
        bcast_file_tx_close(s_tx);
        s_tx = NULL;
    }
}

int bcast_carousel_send(const char *name, const uint8_t *data, size_t len, int cycles,
                        char *err, size_t errlen)
{
    pthread_mutex_lock(&s_lock);
    reap_locked();
    if (s_thread_live)
    {
        pthread_mutex_unlock(&s_lock);
        say(err, errlen, "a carousel is sending");
        return BCAST_CAROUSEL_BUSY;
    }
    if (arq_session_active())
    {
        pthread_mutex_unlock(&s_lock);
        say(err, errlen, "an ARQ session is up");
        return BCAST_CAROUSEL_BUSY;
    }
    int mode = listen_index();
    bcast_file_tx_t *tx = bcast_file_tx_open_mem(name, data, len, mode, cycles, 0, err, errlen);
    if (!tx)
    {
        pthread_mutex_unlock(&s_lock);
        return BCAST_CAROUSEL_BAD;
    }

    s_tx = tx;
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = BCAST_CAR_SENDING;
    s_st.mode = mode;
    s_st.cycles_total = cycles;
    s_st.bytes = len;
    snprintf(s_st.name, sizeof(s_st.name), "%s", name);
    publish();                    /* sending from here: MODE answers BUSY */
    atomic_store(&s_stop_req, false);
    if (pthread_create(&s_thread, NULL, pump, NULL) != 0)
    {
        s_st.state = BCAST_CAR_ERROR;
        publish();
        bcast_file_tx_close(s_tx);
        s_tx = NULL;
        pthread_mutex_unlock(&s_lock);
        say(err, errlen, "cannot start the carousel");
        return BCAST_CAROUSEL_BAD;
    }
    s_thread_live = true;
    HLOGI("bcast-car", "sending '%s' (%zu B) at mode %d, %d cycles", name, len, mode, cycles);
    pthread_mutex_unlock(&s_lock);
    return 0;
}

void bcast_carousel_stop(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_thread_live)
    {
        atomic_store(&s_stop_req, true);
        pthread_join(s_thread, NULL);
        s_thread_live = false;
    }
    reap_locked();
    pthread_mutex_unlock(&s_lock);
}
