/* Mercury audio interface (audioio/audioio.h), daemon build
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The host owns the sound device and the sample clock; it hands Mercury each
 * audio block through mercury_host_audio_exchange().  This file keeps only
 * the two rings modem.c reads and writes (int32 samples at 8 kHz, as every
 * audioio backend fills them), and answers the rest of audioio.h the way a
 * build with no choice of sound system would.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "audioio.h"
#include "defines_modem.h"
#include "hermes_log.h"
#include "mercury_host.h"
#include "ring_buffer_posix.h"

cbuf_handle_t capture_buffer = NULL;
cbuf_handle_t playback_buffer = NULL;

/* Guards the rings' lifetime against the host's audio thread: taken there
 * with trylock, so the exchange never waits, and held by init and deinit
 * while they create or free the rings. */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_running = false;

#define EXCHANGE_CHUNK 256

static void free_rings(void)
{
    if (capture_buffer)
    {
        free(capture_buffer->buffer);
        circular_buf_free(capture_buffer);
    }
    if (playback_buffer)
    {
        free(playback_buffer->buffer);
        circular_buf_free(playback_buffer);
    }
    capture_buffer = NULL;
    playback_buffer = NULL;
}

int audioio_init_buffers(void)
{
    if (capture_buffer && playback_buffer)
        return 0;

    uint8_t *cap = malloc(SIGNAL_BUFFER_SIZE);
    uint8_t *play = malloc(SIGNAL_BUFFER_SIZE);
    if (cap)
        capture_buffer = circular_buf_init(cap, SIGNAL_BUFFER_SIZE);
    if (play)
        playback_buffer = circular_buf_init(play, SIGNAL_BUFFER_SIZE);
    if (!capture_buffer || !playback_buffer)
    {
        if (!capture_buffer) free(cap);
        if (!playback_buffer) free(play);
        free_rings();
        return -1;
    }
    return 0;
}

void audioio_deinit_buffers(void)
{
    free_rings();
}

int audioio_init_internal(char *capture_dev, char *playback_dev, int audio_subsys,
                          int capture_channel_layout, pthread_t *radio_capture,
                          pthread_t *radio_playback)
{
    (void) capture_dev; (void) playback_dev; (void) audio_subsys;
    (void) capture_channel_layout; (void) radio_capture; (void) radio_playback;

    pthread_mutex_lock(&s_lock);
    int rc = audioio_init_buffers();
    s_running = rc == 0;
    pthread_mutex_unlock(&s_lock);
    if (rc == 0)
        HLOGI("audio-host", "Audio from the host program (8 kHz)");
    return rc;
}

int audioio_deinit(pthread_t *radio_capture, pthread_t *radio_playback)
{
    (void) radio_capture; (void) radio_playback;

    pthread_mutex_lock(&s_lock);
    s_running = false;
    free_rings();
    pthread_mutex_unlock(&s_lock);
    return 0;
}

size_t mercury_host_audio_exchange(const int16_t *rx8k, int16_t *tx8k, size_t n)
{
    int32_t ring[EXCHANGE_CHUNK];
    size_t got = 0;

    if (pthread_mutex_trylock(&s_lock) == 0)
    {
        if (s_running)
        {
            /* One producer of capture and one consumer of playback (this
             * thread), so the room or data checked here can only grow
             * before the write or read: neither ever waits. */
            for (size_t i = 0; rx8k && i < n; i += EXCHANGE_CHUNK)
            {
                size_t k = n - i < EXCHANGE_CHUNK ? n - i : EXCHANGE_CHUNK;
                for (size_t j = 0; j < k; j++)
                    ring[j] = (int32_t) ((uint32_t) (int16_t) rx8k[i + j] << 16);
                if (circular_buf_free_size(capture_buffer) >= k * sizeof(int32_t))
                    write_buffer(capture_buffer, (uint8_t *) ring, k * sizeof(int32_t));
            }

            size_t have = size_buffer(playback_buffer) / sizeof(int32_t);
            if (have > n)
                have = n;
            while (tx8k && got < have)
            {
                size_t k = have - got < EXCHANGE_CHUNK ? have - got : EXCHANGE_CHUNK;
                read_buffer(playback_buffer, (uint8_t *) ring, k * sizeof(int32_t));
                for (size_t j = 0; j < k; j++)
                    tx8k[got + j] = (int16_t) (ring[j] >> 16);
                got += k;
            }
        }
        pthread_mutex_unlock(&s_lock);
    }

    if (tx8k && got < n)
        memset(tx8k + got, 0, (n - got) * sizeof(int16_t));
    return got;
}

/* ---- the rest of audioio.h: one sound system, the host's ---- */

bool audioio_rings_external(int audio_system)
{
    (void) audio_system;
    return false;
}

int audioio_pick_default_subsystem(void)
{
    return AUDIO_SUBSYSTEM_NULL;
}

int audioio_available_subsystems(int *subsystems, int max)
{
    (void) subsystems; (void) max;
    return 0;
}

int audioio_restart(const char *capture_dev, const char *playback_dev,
                    int audio_subsys, int capture_channel_layout)
{
    (void) capture_dev; (void) playback_dev; (void) audio_subsys;
    (void) capture_channel_layout;
    HLOGW("audio-host", "Audio belongs to the host program; not restarted");
    return -1;
}

audio_health_t audioio_capture_health(void)
{
    return s_running ? AUDIO_HEALTH_RUNNING : AUDIO_HEALTH_STOPPED;
}

audio_health_t audioio_playback_health(void)
{
    return audioio_capture_health();
}

void audioio_health_reason(char *buf, size_t buflen)
{
    if (buf && buflen)
        buf[0] = '\0';
}

bool audioio_health_ok(char *reason, size_t reasonlen)
{
    audioio_health_reason(reason, reasonlen);
    return true;
}

void audioio_health_reset(void)
{
}

int audioio_wait_healthy(int timeout_ms)
{
    (void) timeout_ms;
    return s_running ? 0 : -2;
}

void list_soundcards(int audio_system)
{
    (void) audio_system;
    printf("Audio comes from the host program; there are no sound cards to list.\n");
}

int get_soundcard_list(int audio_system, int mode,
                       char ids[][AUDIO_DEV_STR_MAX], char dev_names[][AUDIO_DEV_STR_MAX],
                       int max_count)
{
    (void) audio_system; (void) mode; (void) ids; (void) dev_names; (void) max_count;
    return 0;
}
