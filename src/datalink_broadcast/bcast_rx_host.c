/* The host's broadcast receiver: every decoded broadcast frame, and the files
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A host program (hermes-radio-daemon) gets each broadcast frame Mercury
 * decodes (on_frame) and each file the frames complete (on_object: its bytes
 * and the name it was sent under), without a broadcast TCP client.  The
 * modem's RX thread only copies the frame into this module's ring
 * (broadcast_set_rx_tap); a thread of its own runs the callbacks and the
 * RaptorQ decode (bcast_file_rx), so neither delays the modem.  The ring's
 * write waits when full: no frame is dropped.
 *
 * The decoder writes what it completes into a private directory; the file is
 * read back, handed over and removed -- the host files it where it wants.
 * It decodes at the listen mode, reopening when the mode changes, and only
 * where the mode carries files.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bcast_rx_host.h"
#include "bcast_file.h"
#include "modem.h"
#include "mercury_cli.h"
#include "ring_buffer_posix.h"
#include "tcp_interfaces.h"
#include "hermes_log.h"
#include "os_interop.h"
#include "defines_modem.h"

extern _Atomic bool mercury_stop;

#define RXQ_SIZE   (256 * 1024)
#define REC_HDR    10                 /* uint16 len, int32 mode, float snr */

static uint8_t          s_q_mem[RXQ_SIZE];
static cbuf_handle_t    s_q = NULL;
static pthread_mutex_t  s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t        s_thread;
static bool             s_live = false;
static _Atomic bool     s_stop = false;
static bcast_host_object_fn s_on_object = NULL;
static bcast_host_frame_fn  s_on_frame = NULL;
static void            *s_user = NULL;
static char             s_dir[256];

static void tap(const uint8_t *frame, size_t len, int mode, float snr)
{
    uint8_t rec[REC_HDR + 2048];
    if (len > sizeof(rec) - REC_HDR)
        return;
    uint16_t l = (uint16_t)len;
    int32_t  m = mode;
    memcpy(rec, &l, 2);
    memcpy(rec + 2, &m, 4);
    memcpy(rec + 6, &snr, 4);
    memcpy(rec + REC_HDR, frame, len);
    write_buffer(s_q, rec, REC_HDR + len);       /* waits while full: nothing dropped */
}

static int listen_index(void)
{
    int mode = modem_get_listen_mode();
    for (int i = 0; i < mercury_cli_mode_count(); i++)
        if (freedv_modes[i] == mode)
            return i;
    return -1;
}

/* Hand a completed file to the host, and remove it here. */
static void deliver(bcast_file_rx_t *rx)
{
    const char *path = bcast_file_rx_last_path(rx);
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *data = n > 0 ? malloc((size_t)n) : NULL;
    if (data && fread(data, 1, (size_t)n, f) == (size_t)n && s_on_object)
        s_on_object(data, (size_t)n, bcast_file_rx_last_name(rx), false, s_user);
    fclose(f);
    free(data);
    remove(path);
}

static void *worker(void *arg)
{
    (void)arg;
    uint8_t rec[REC_HDR + 2048];
    bcast_file_rx_t *rx = NULL;
    int rx_mode = -1;
    char err[160];

    while (!atomic_load(&s_stop) && !mercury_stop)
    {
        if (size_buffer(s_q) < REC_HDR)
        {
            msleep(5);
            continue;
        }
        read_buffer(s_q, rec, REC_HDR);
        uint16_t len;
        int32_t mode;
        float snr;
        memcpy(&len, rec, 2);
        memcpy(&mode, rec + 2, 4);
        memcpy(&snr, rec + 6, 4);
        read_buffer(s_q, rec + REC_HDR, len);     /* the writer put it whole */
        const uint8_t *frame = rec + REC_HDR;

        if (s_on_frame)
            s_on_frame(frame, len, mode, snr, s_user);
        if (!s_on_object)
            continue;

        int idx = listen_index();
        if (idx != rx_mode)
        {
            if (rx) bcast_file_rx_close(rx);
            rx = bcast_file_mode_usable(idx) ? bcast_file_rx_open(idx, s_dir, err, sizeof(err)) : NULL;
            rx_mode = idx;
        }
        if (rx && bcast_file_rx_frame(rx, frame, len) == BCAST_RX_COMPLETE)
            deliver(rx);
    }
    if (rx)
        bcast_file_rx_close(rx);
    return NULL;
}

static void stop_locked(void)
{
    if (!s_live)
        return;
    broadcast_set_rx_tap(NULL);
    atomic_store(&s_stop, true);
    pthread_join(s_thread, NULL);
    s_live = false;
    clear_buffer(s_q);           /* also wakes a tap still waiting on a full ring */
    rmdir(s_dir);
}

int bcast_rx_host_set(bcast_host_object_fn on_object, bcast_host_frame_fn on_frame, void *user)
{
    pthread_mutex_lock(&s_lock);
    stop_locked();
    if (!on_object && !on_frame)
    {
        pthread_mutex_unlock(&s_lock);
        return 0;
    }
    if (!s_q)
        s_q = circular_buf_init(s_q_mem, sizeof(s_q_mem));
    clear_buffer(s_q);           /* nothing of a previous run's in-flight frame */
    const char *tmp = getenv("TMPDIR");
    snprintf(s_dir, sizeof(s_dir), "%s/mercury-bcast-rx-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(s_dir))
    {
        HLOGW("bcast-rx", "cannot make a directory for received files (%s)", s_dir);
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    s_on_object = on_object;
    s_on_frame = on_frame;
    s_user = user;
    atomic_store(&s_stop, false);
    if (pthread_create(&s_thread, NULL, worker, NULL) != 0)
    {
        rmdir(s_dir);
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    s_live = true;
    broadcast_set_rx_tap(tap);
    pthread_mutex_unlock(&s_lock);
    return 0;
}
