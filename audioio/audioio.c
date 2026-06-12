/* Audio subsystem
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */


#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>
#include "os_interop.h"
#include <ffaudio/audio.h>
#include "std.h"
#ifndef FF_WIN
#include <time.h>
#endif

#include "ring_buffer_posix.h"
#include "shm_posix.h"
#include "defines_modem.h"

#include "audioio.h"
#include "hermes_log.h"

extern volatile bool shutdown_;

/* ------------------------------------------------------------------ */
/*  DirectSound GUID ↔ string helpers (Windows only)                  */
/* ------------------------------------------------------------------ */
#if defined(_WIN32)

/* Format a GUID as "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}".
 * buf must be at least 39 bytes (38 chars + NUL).                    */
static void guid_to_str(const GUID *g, char *buf, size_t bufsize)
{
    snprintf(buf, bufsize,
             "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             (unsigned long)g->Data1, g->Data2, g->Data3,
             g->Data4[0], g->Data4[1],
             g->Data4[2], g->Data4[3], g->Data4[4],
             g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* Parse a GUID string back into a GUID struct.  Returns 0 on success. */
static int str_to_guid(const char *s, GUID *g)
{
    unsigned long d1;
    unsigned int d2, d3;
    unsigned int d4[8];
    if (sscanf(s, "{%8lX-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X}",
               &d1, &d2, &d3,
               &d4[0], &d4[1], &d4[2], &d4[3],
               &d4[4], &d4[5], &d4[6], &d4[7]) != 11)
        return -1;
    g->Data1 = d1;
    g->Data2 = (unsigned short)d2;
    g->Data3 = (unsigned short)d3;
    for (int i = 0; i < 8; i++)
        g->Data4[i] = (unsigned char)d4[i];
    return 0;
}

#endif /* _WIN32 */

cbuf_handle_t capture_buffer;
cbuf_handle_t playback_buffer;

int audio_subsystem;
static int capture_input_channel_layout = LEFT;

// Internal state for restart support
static pthread_t s_radio_capture;
static pthread_t s_radio_playback;
static char s_capture_dev[256];
static char s_playback_dev[256];
static int s_buffers_initialized = 0;
static volatile bool audio_shutdown_ = false;  // local stop flag for audio threads

#define NULL_AUDIO_PERIOD_MS 20
#define NULL_AUDIO_SAMPLES_PER_PERIOD 160
#define FIFO_AUDIO_POLL_MS 10
#define FIFO_AUDIO_CHUNK_BYTES 4096

struct conf {
    const char *cmd;
    ffaudio_conf buf;
    uint8_t flags;
    uint8_t exclusive;
    uint8_t hwdev;
    uint8_t loopback;
    uint8_t nonblock;
    uint8_t wav;
};


static inline void ffthread_sleep(ffuint msec)
{
#ifdef FF_WIN
    Sleep(msec);
#else
    struct timespec ts = {
        .tv_sec = msec / 1000,
        .tv_nsec = (msec % 1000) * 1000000,
    };
    nanosleep(&ts, NULL);
#endif
}

static inline uint64_t audioio_monotonic_ms(void)
{
#ifdef FF_WIN
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
#endif
}

#if defined(__linux__)
static bool pulse_init_already_initialized(const ffaudio_init_conf *aconf)
{
    return aconf && aconf->error && strcmp(aconf->error, "already initialized") == 0;
}

static int pulse_shared_init(bool *did_init)
{
    ffaudio_interface *audio = (ffaudio_interface *) &ffpulse;
    ffaudio_init_conf aconf = {};
    aconf.app_name = "mercury";
    if (did_init)
        *did_init = false;

    if (audio->init(&aconf) != 0)
    {
        if (pulse_init_already_initialized(&aconf))
            return 0;
        HLOGE("audio-pulse", "Error initializing PulseAudio: %s",
              aconf.error ? aconf.error : "unknown");
        return -1;
    }

    if (did_init)
        *did_init = true;
    return 0;
}

static void pulse_shared_uninit(void)
{
    ffaudio_interface *audio = (ffaudio_interface *) &ffpulse;
    audio->uninit();
}
#endif

int audioio_pick_default_subsystem(void)
{
#if defined(__linux__)
    return AUDIO_SUBSYSTEM_ALSA;
#elif defined(_WIN32)
    return AUDIO_SUBSYSTEM_WASAPI;
#elif defined(__FREEBSD__)
    return AUDIO_SUBSYSTEM_OSS;
#elif defined(__APPLE__)
    return AUDIO_SUBSYSTEM_COREAUDIO;
#elif defined(__ANDROID__)
    return AUDIO_SUBSYSTEM_AAUDIO;
#else
    return AUDIO_SUBSYSTEM_ALSA;
#endif
}

static void *null_capture_thread(void *unused)
{
    (void) unused;
    int32_t silence[NULL_AUDIO_SAMPLES_PER_PERIOD] = {0};
    const size_t bytes = sizeof(silence);

    HLOGI("audio-null", "capture silence thread started");
    while (!shutdown_ && !audio_shutdown_)
    {
        if (circular_buf_free_size(capture_buffer) >= bytes)
            write_buffer(capture_buffer, (uint8_t *) silence, bytes);
        ffthread_sleep(NULL_AUDIO_PERIOD_MS);
    }
    HLOGI("audio-null", "capture silence thread exit");
    return NULL;
}

static void *null_playback_thread(void *unused)
{
    (void) unused;
    uint8_t discard[4096];

    HLOGI("audio-null", "playback discard thread started");
    while (!shutdown_ && !audio_shutdown_)
    {
        size_t bytes = size_buffer(playback_buffer);
        if (bytes > sizeof(discard))
            bytes = sizeof(discard);
        if (bytes > 0)
            read_buffer(playback_buffer, discard, bytes);
        else
            ffthread_sleep(NULL_AUDIO_PERIOD_MS);
    }
    HLOGI("audio-null", "playback discard thread exit");
    return NULL;
}

static int fifo_open_retry(const char *path, int flags, const char *log_tag)
{
    bool logged_wait = false;

    if (!path || path[0] == '\0')
    {
        HLOGE(log_tag, "missing FIFO path");
        return -1;
    }

    while (!shutdown_ && !audio_shutdown_)
    {
        int fd = open(path, flags);
        if (fd >= 0)
        {
            HLOGI(log_tag, "opened %s", path);
            return fd;
        }

        int err = errno;
        if (err == ENOENT || err == ENXIO)
        {
            if (!logged_wait)
            {
                HLOGW(log_tag, "waiting for FIFO %s: %s", path, strerror(err));
                logged_wait = true;
            }
        }
        else if (err != EINTR)
        {
            HLOGW(log_tag, "open(%s) failed: %s", path, strerror(err));
        }
        ffthread_sleep(FIFO_AUDIO_POLL_MS);
    }
    return -1;
}

static void *fifo_capture_thread(void *device_ptr)
{
    const char *path = (const char *) device_ptr;
    uint8_t buf[FIFO_AUDIO_CHUNK_BYTES];
    size_t buf_len = 0;
    bool logged_eof = false;

    int fd = fifo_open_retry(path, O_RDONLY | O_NONBLOCK, "audio-fifo-cap");
    if (fd < 0)
    {
        shutdown_ = true;
        return NULL;
    }

    while (!shutdown_ && !audio_shutdown_)
    {
        /* Try to consume aligned int32_t samples from buf first */
        if (buf_len >= sizeof(int32_t))
        {
            size_t aligned = buf_len - (buf_len % sizeof(int32_t));
            if (circular_buf_free_size(capture_buffer) >= aligned)
            {
                write_buffer(capture_buffer, buf, aligned);
                buf_len -= aligned;
                if (buf_len > 0)
                    memmove(buf, buf + aligned, buf_len);
                continue;
            }
            ffthread_sleep(FIFO_AUDIO_POLL_MS);
            continue;
        }

        /* Read more data from FIFO */
        ssize_t n = read(fd, buf + buf_len, sizeof(buf) - buf_len);
        if (n > 0)
        {
            logged_eof = false;
            buf_len += (size_t)n;
            continue;
        }

        if (n == 0)
        {
            if (!logged_eof)
            {
                HLOGI("audio-fifo-cap", "read(%s) reached EOF; reopening", path);
                logged_eof = true;
            }
            buf_len = 0;
            close(fd);
            fd = fifo_open_retry(path, O_RDONLY | O_NONBLOCK, "audio-fifo-cap");
            if (fd < 0)
                break;
            ffthread_sleep(FIFO_AUDIO_POLL_MS);
            continue;
        }

        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            HLOGW("audio-fifo-cap", "read(%s) failed: %s", path, strerror(errno));
            buf_len = 0;
            close(fd);
            fd = fifo_open_retry(path, O_RDONLY | O_NONBLOCK, "audio-fifo-cap");
            if (fd < 0)
                break;
        }
        ffthread_sleep(FIFO_AUDIO_POLL_MS);
    }

    close(fd);
    HLOGI("audio-fifo-cap", "capture FIFO thread exit");
    return NULL;
}

static void *fifo_playback_thread(void *device_ptr)
{
    const char *path = (const char *) device_ptr;
    uint8_t buf[FIFO_AUDIO_CHUNK_BYTES];
    size_t pending_off = 0;
    size_t pending_len = 0;

    int fd = fifo_open_retry(path, O_WRONLY | O_NONBLOCK, "audio-fifo-play");
    if (fd < 0)
    {
        shutdown_ = true;
        return NULL;
    }

    while (!shutdown_ && !audio_shutdown_)
    {
        if (pending_off >= pending_len)
        {
            pending_off = 0;
            pending_len = 0;
            size_t available = size_buffer(playback_buffer);
            if (available > sizeof(buf))
                available = sizeof(buf);
            available -= available % sizeof(int32_t);
            if (available > 0)
            {
                read_buffer(playback_buffer, buf, available);
                pending_len = available;
            }
            else
            {
                ffthread_sleep(FIFO_AUDIO_POLL_MS);
                continue;
            }
        }

        ssize_t n = write(fd, buf + pending_off, pending_len - pending_off);
        if (n > 0)
        {
            pending_off += (size_t)n;
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            ffthread_sleep(FIFO_AUDIO_POLL_MS);
            continue;
        }

        HLOGW("audio-fifo-play", "write(%s) failed: %s", path, strerror(errno));
        close(fd);
        fd = fifo_open_retry(path, O_WRONLY | O_NONBLOCK, "audio-fifo-play");
        if (fd < 0)
            break;
    }

    close(fd);
    HLOGI("audio-fifo-play", "playback FIFO thread exit");
    return NULL;
}

static void fifo_ignore_sigpipe(void)
{
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
}


void *radio_playback_thread(void *device_ptr)
{
    ffaudio_interface *audio;
    struct conf conf = {};
    conf.buf.app_name = "mercury_playback";
    conf.buf.format = FFAUDIO_F_INT32;
    conf.buf.sample_rate = 48000;
    conf.buf.channels = 2;
    conf.buf.device_id = (device_ptr && ((const char *)device_ptr)[0] != '\0')
                         ? (const char *) device_ptr : NULL;
    uint32_t period_ms;
    uint32_t period_bytes;


#if defined(_WIN32)
    conf.buf.buffer_length_msec = 40;
    period_ms = conf.buf.buffer_length_msec / 4;
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    conf.buf.buffer_length_msec = 80;
    period_ms = conf.buf.buffer_length_msec / 3;
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
#elif defined(__FREEBSD__)
    conf.buf.buffer_length_msec = 40;
    period_ms = conf.buf.buffer_length_msec / 4;
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = (ffaudio_interface *) &ffoss;
#elif defined(__APPLE__)
    conf.buf.buffer_length_msec = 40;
    period_ms = conf.buf.buffer_length_msec / 4;
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#endif

    period_bytes = conf.buf.sample_rate * sizeof(double) * period_ms / 1000;

    //printf("period_ms: %u\n", period_ms);
    //printf("period_size: %u\n", period_bytes);

#if defined(_WIN32)
    /* DirectSound device IDs are GUID strings from get_soundcard_list().
     * Convert back to binary GUID for the DirectSound API. */
    GUID play_guid;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND && conf.buf.device_id &&
        conf.buf.device_id[0] == '{' && str_to_guid(conf.buf.device_id, &play_guid) == 0)
        conf.buf.device_id = (const char *)&play_guid;
#endif

    conf.flags = FFAUDIO_PLAYBACK;
    ffaudio_init_conf aconf = {};
    aconf.app_name = "mercury_playback";

    int r;
    ffaudio_buf *b;
    ffaudio_conf *cfg;

    ffuint frame_size;
    ffuint msec_bytes;

    // input is int32_t (8kHz samples from playback_buffer)
    int32_t *input_buffer = (int32_t *) malloc(SIGNAL_BUFFER_SIZE * sizeof(int32_t));

    // upsampled buffer (48kHz mono)
    int32_t *buffer_upsampled = (int32_t *) malloc(SIGNAL_BUFFER_SIZE * sizeof(int32_t) * 6);

    // output is int32_t stereo (48kHz)
    int32_t *buffer_output_stereo = (int32_t *) malloc(SIGNAL_BUFFER_SIZE * sizeof(int32_t) * 2 * 6); // a big enough buffer

    ffuint total_written = 0;
    int ch_layout = STEREO;

    // Resampling ratio: 8kHz -> 48kHz = 1:6
    const int resample_ratio = 6;

    /* PulseAudio uses a single global context (gconn in pulse.c).
     * If init() returns "already initialized" it means the capture thread
     * already called init() successfully and we can proceed normally.
     * Track whether we initialized so we only uninit once.
     */
    bool did_init_play = false;
    r = audio->init(&aconf);
    if (r != 0)
    {
        if (aconf.error == NULL || strcmp(aconf.error, "already initialized") != 0)
        {
            HLOGE("audio-play", "Error in audio->init(): %s", aconf.error ? aconf.error : "unknown");
            goto finish_play;
        }
        // "already initialized" is fine - another thread owns the context
    }
    else
    {
        did_init_play = true;
    }

    // playback code...
    b = audio->alloc();
    if (b == NULL)
    {
        HLOGE("audio-play", "Error in audio->alloc()");
        goto finish_play;
    }

    cfg = &conf.buf;
    r = audio->open(b, cfg, conf.flags);
    if (r == FFAUDIO_EFORMAT)
        r = audio->open(b, cfg, conf.flags);
    if (r != 0)
    {
        HLOGE("audio-play", "error in audio->open(): %d: %s", r, audio->error(b));
        goto cleanup_play;
    }

    HLOGI("audio-play", "I/O playback (%s) %s / %dHz / %dch / %dms buffer",
          device_ptr ? (const char *)device_ptr : "default",
          cfg->format == FFAUDIO_F_FLOAT32 ? "float32" :
          cfg->format == FFAUDIO_F_INT32   ? "int32"   :
          cfg->format == FFAUDIO_F_INT24_4 ? "int24in32" :
          cfg->format == FFAUDIO_F_INT24   ? "int24"   :
          cfg->format == FFAUDIO_F_INT16   ? "int16"   : "unknown",
          cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);


    frame_size = cfg->channels * (cfg->format & 0xff) / 8;
    msec_bytes = cfg->sample_rate * frame_size / 1000;

#if 0 // TODO: parametrize this
    if (radio_type == RADIO_SBITX)
        ch_layout = RIGHT;
    if (radio_type == RADIO_STOCKHF)
        ch_layout = STEREO;
#endif
    ch_layout = STEREO;
    
    // period_bytes at 8kHz (input rate) - adjust for the lower sample rate
    uint32_t period_bytes_8k = period_bytes / resample_ratio;

    while (!shutdown_ && !audio_shutdown_)
    {
        ffssize n;
        size_t buffer_size = size_buffer(playback_buffer);
        if (buffer_size == 0)
        {
            ffthread_sleep(period_ms ? period_ms : 5);
            continue;
        }
        if (buffer_size >= period_bytes_8k)
        {
            read_buffer(playback_buffer, (uint8_t *) input_buffer, period_bytes_8k);
            n = period_bytes_8k;
        }
        else
        {
            // we just play zeros if there is nothing to play
            memset(input_buffer, 0, period_bytes_8k);
            if (buffer_size > 0)
                read_buffer(playback_buffer, (uint8_t *) input_buffer, buffer_size);
            n = buffer_size;
        }

        total_written = 0;

        int samples_read_8k = n / sizeof(int32_t);

        // Upsample from 8kHz to 48kHz using linear interpolation
        int samples_upsampled = samples_read_8k * resample_ratio;
        for (int i = 0; i < samples_read_8k; i++)
        {
            int32_t current = input_buffer[i];
            int32_t next = (i + 1 < samples_read_8k) ? input_buffer[i + 1] : current;

            for (int j = 0; j < resample_ratio; j++)
            {
                // Linear interpolation between current and next sample
                buffer_upsampled[i * resample_ratio + j] = current + (next - current) * j / resample_ratio;
            }
        }

        // Convert upsampled mono to stereo
        for (int i = 0; i < samples_upsampled; i++)
        {
            int idx = i * cfg->channels;
            if (ch_layout == LEFT)
            {
                buffer_output_stereo[idx] = buffer_upsampled[i];
                buffer_output_stereo[idx + 1] = 0;
            }

            if (ch_layout == RIGHT)
            {
                buffer_output_stereo[idx] = 0;
                buffer_output_stereo[idx + 1] = buffer_upsampled[i];
            }

            if (ch_layout == STEREO)
            {
                buffer_output_stereo[idx] = buffer_upsampled[i];
                buffer_output_stereo[idx + 1] = buffer_upsampled[i];
            }
        }

        n = samples_upsampled * frame_size;

        while (n >= frame_size)
        {
            if (audio_shutdown_) break;  // exit fast on restart

            r = audio->write(b, ((uint8_t *)buffer_output_stereo) + total_written, n);

            if (r == -FFAUDIO_ESYNC) {
                HLOGW("audio-play", "detected underrun");
                continue;
            }
            if (r < 0)
            {
                HLOGE("audio-play", "ffaudio.write: %s", audio->error(b));
            }
#if 0 // print time measurement
            else
            {
                printf(" %dms\n", r / msec_bytes);
            }
#endif
            total_written += r;
            n -= r;
        }
        // printf("n = %lld total written = %u\n", n, total_written);
    }
    // Only drain when doing a full shutdown, not a restart
    // audio->drain() blocks until all buffered data is played out
    // which can hang indefinitely during a device switch
    if (!audio_shutdown_) {
        r = audio->drain(b);
        if (r < 0)
            HLOGE("audio-play", "ffaudio.drain: %s", audio->error(b));
    }
    r = audio->stop(b);
    if (r != 0)
        HLOGE("audio-play", "ffaudio.stop: %s", audio->error(b));

    r = audio->clear(b);
    if (r != 0)
        HLOGE("audio-play", "ffaudio.clear: %s", audio->error(b));

cleanup_play:

    audio->free(b);

    // Only uninit if this thread was the one that initialized the PA context
    if (did_init_play)
        audio->uninit();

finish_play:

    free(input_buffer);
    free(buffer_upsampled);
    free(buffer_output_stereo);

    HLOGI("audio-play", "radio_playback_thread exit");

    // Only trigger global shutdown if this was NOT a restart-initiated stop
    if (!audio_shutdown_)
        shutdown_ = true;

    return NULL;
}


void *radio_capture_thread(void *device_ptr)
{
    ffaudio_interface *audio;
    struct conf conf = {};
    conf.buf.app_name = "mercury_capture";
    conf.buf.format = FFAUDIO_F_INT32;
    conf.buf.sample_rate = 48000;
    conf.buf.channels = 2;
    conf.buf.device_id = (device_ptr && ((const char *)device_ptr)[0] != '\0')
                         ? (const char *) device_ptr : NULL;

#if defined(_WIN32)
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI) {
        conf.buf.buffer_length_msec = 40;
        audio = (ffaudio_interface *) &ffwasapi;
    }
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND) {
        /* DSound on Win10/11 is emulated via WASAPI. A small looping buffer
         * causes the write cursor to lap our read position between polls,
         * losing most captured data.  Use 500ms (DSound's own default). */
        conf.buf.buffer_length_msec = 200;
        audio = (ffaudio_interface *) &ffdsound;
    }
#elif defined(__linux__)
    conf.buf.buffer_length_msec = 80;
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
#elif defined(__FREEBSD__)
    conf.buf.buffer_length_msec = 40;
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = (ffaudio_interface *) &ffoss;
#elif defined(__APPLE__)
    conf.buf.buffer_length_msec = 40;
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#endif

#if defined(_WIN32)
    /* DirectSound device IDs are GUID strings from get_soundcard_list().
     * Convert back to binary GUID for the DirectSound API. */
    GUID cap_guid;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND && conf.buf.device_id &&
        conf.buf.device_id[0] == '{' && str_to_guid(conf.buf.device_id, &cap_guid) == 0)
        conf.buf.device_id = (const char *)&cap_guid;
#endif

    conf.flags = FFAUDIO_CAPTURE;
    ffaudio_init_conf aconf = {};
    aconf.app_name = "mercury_capture";

    int r;
    ffaudio_buf *b;
    ffaudio_conf *cfg;

    ffuint frame_size;
    ffuint msec_bytes;

    int32_t *buffer = NULL;

    int ch_layout = STEREO;

    int32_t *buffer_output = NULL;
    int32_t *buffer_downsampled = NULL;

    // Resampling ratio: 48kHz -> 8kHz = 6:1
    const int resample_ratio = 6;

    /* PulseAudio uses a single global context (gconn in pulse.c).
     * If init() returns "already initialized" it means the playback thread
     * already called init() successfully and we can proceed normally.
     * Track whether we initialized so we only uninit once.
     */
    bool did_init_cap = false;
    r = audio->init(&aconf);
    if (r != 0)
    {
        if (aconf.error == NULL || strcmp(aconf.error, "already initialized") != 0)
        {
            HLOGE("audio-cap", "Error in audio->init(): %s", aconf.error ? aconf.error : "unknown");
            goto finish_cap;
        }
        // "already initialized" is fine - another thread owns the context
    }
    else
    {
        did_init_cap = true;
    }

    // capture code
    b = audio->alloc();
    if (b == NULL)
    {
        HLOGE("audio-cap", "Error in audio->alloc()");
        goto finish_cap;
    }

    cfg = &conf.buf;
    r = audio->open(b, cfg, conf.flags);
    if (r == FFAUDIO_EFORMAT)
        r = audio->open(b, cfg, conf.flags);
    if (r != 0)
    {
        HLOGE("audio-cap", "error in audio->open(): %d: %s", r, audio->error(b));
        goto cleanup_cap;
    }

    HLOGI("audio-cap", "I/O capture (%s) %s / %dHz / %dch / %dms buffer",
          device_ptr ? (const char *)device_ptr : "default",
          cfg->format == FFAUDIO_F_FLOAT32 ? "float32" :
          cfg->format == FFAUDIO_F_INT32   ? "int32"   :
          cfg->format == FFAUDIO_F_INT24_4 ? "int24in32" :
          cfg->format == FFAUDIO_F_INT24   ? "int24"   :
          cfg->format == FFAUDIO_F_INT16   ? "int16"   : "unknown",
          cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);

    frame_size = cfg->channels * (cfg->format & 0xff) / 8;
    msec_bytes = cfg->sample_rate * frame_size / 1000;

    bool capture_is_float = (cfg->format == FFAUDIO_F_FLOAT32);
    bool capture_is_int16 = (cfg->format == FFAUDIO_F_INT16);
    int  capture_channels = cfg->channels;

    if (!capture_is_float && !capture_is_int16 &&
        cfg->format != FFAUDIO_F_INT32 && cfg->format != FFAUDIO_F_INT24_4)
    {
        HLOGE("audio-cap", "Unsupported capture format %d, aborting", cfg->format);
        audio->free(b);
        return NULL;
    }

    buffer_output = (int32_t *) malloc(SIGNAL_BUFFER_SIZE * sizeof(int32_t) * 2);
    buffer_downsampled = (int32_t *) malloc(SIGNAL_BUFFER_SIZE * sizeof(int32_t));

#if 0 // TODO: parametrize this
    if (radio_type == RADIO_SBITX)
        ch_layout = LEFT;
    if (radio_type == RADIO_STOCKHF)
        ch_layout = STEREO;
#endif
    ch_layout = capture_input_channel_layout;

    static int resample_remainder = 0;  // Track fractional samples for accurate resampling

    /* --- Capture rate diagnostics (prints every ~5 seconds) --- */
    uint64_t diag_start_ms = audioio_monotonic_ms();
    uint64_t diag_total_48k_frames = 0;   /* frames read from audio device (48kHz) */
    uint64_t diag_total_8k_samples = 0;   /* samples after downsampling (8kHz) */
    uint32_t diag_read_calls = 0;
    uint32_t diag_read_errors = 0;
    uint32_t diag_buf_full_drops = 0;
    int      diag_last_read_bytes = 0;

    while (!shutdown_ && !audio_shutdown_)
    {
        r = audio->read(b, (const void **)&buffer);
        if (r < 0)
        {
            diag_read_errors++;
            HLOGE("audio-cap", "ffaudio.read: %s", audio->error(b));
            continue;
        }

        diag_read_calls++;
        diag_last_read_bytes = r;

        int frames_read = r / frame_size;
        int frames_to_write = frames_read;
        
        // Downsample from 48kHz to 8kHz with decimation
        // resample_remainder tracks position in decimation cycle (0 to resample_ratio-1)
        // When remainder is 0, we take a sample; otherwise skip
        int downsampled_frames = 0;
        for (int i = 0; i < frames_to_write; i++)
        {
            int32_t sample;

            if (capture_channels == 1)
            {
                // Mono: one sample per frame
                if (capture_is_float)
                {
                    float fsample = ((float *)buffer)[i];
                    if (fsample > 1.0f) fsample = 1.0f;
                    else if (fsample < -1.0f) fsample = -1.0f;
                    sample = (int32_t)(fsample * 2147483647.0f);
                }
                else if (capture_is_int16)
                {
                    sample = (int32_t)((int16_t *)buffer)[i] * 65536;
                }
                else
                {
                    sample = buffer[i];
                }
            }
            else
            {
                // Stereo: two samples per frame, extract based on ch_layout
                if (capture_is_float)
                {
                    float *fbuf = (float *)buffer;
                    float fl = fbuf[i*2];
                    float fr = fbuf[i*2 + 1];
                    float fs;
                    if (ch_layout == LEFT)
                        fs = fl;
                    else if (ch_layout == RIGHT)
                        fs = fr;
                    else
                        fs = (fl + fr) * 0.5f;
                    if (fs > 1.0f) fs = 1.0f;
                    else if (fs < -1.0f) fs = -1.0f;
                    sample = (int32_t)(fs * 2147483647.0f);
                }
                else if (capture_is_int16)
                {
                    int16_t *i16buf = (int16_t *)buffer;
                    if (ch_layout == LEFT)
                        sample = (int32_t)i16buf[i*2] * 65536;
                    else if (ch_layout == RIGHT)
                        sample = (int32_t)i16buf[i*2 + 1] * 65536;
                    else
                        sample = ((int32_t)i16buf[i*2] + (int32_t)i16buf[i*2 + 1]) * 32768;
                }
                else
                {
                    if (ch_layout == LEFT)
                        sample = buffer[i*2];
                    else if (ch_layout == RIGHT)
                        sample = buffer[i*2 + 1];
                    else
                        sample = (buffer[i*2] + buffer[i*2 + 1]) / 2;
                }
            }

            // Take every 6th sample (when remainder == 0)
            // Bounds check: ensure we don't overflow buffer_downsampled
            if (resample_remainder == 0 && downsampled_frames < (int)SIGNAL_BUFFER_SIZE)
            {
                buffer_downsampled[downsampled_frames++] = sample;
            }

            resample_remainder = (resample_remainder + 1) % resample_ratio;
        }

        if (downsampled_frames > 0)
        {
            if (circular_buf_free_size(capture_buffer) >= (size_t)(downsampled_frames * sizeof(int32_t)))
                write_buffer(capture_buffer, (uint8_t *)buffer_downsampled, downsampled_frames * sizeof(int32_t));
            else
            {
                diag_buf_full_drops += downsampled_frames;
                HLOGW("audio-cap", "Buffer full in capture buffer!");
            }
        }

        diag_total_48k_frames += frames_read;
        diag_total_8k_samples += downsampled_frames;

        /* Print diagnostics every ~5 seconds */
        uint64_t diag_now = audioio_monotonic_ms();
        uint64_t diag_elapsed = diag_now - diag_start_ms;
        if (diag_elapsed >= 5000)
        {
#ifdef DEBUG_IO
            double elapsed_sec = diag_elapsed / 1000.0;
            double rate_48k = diag_total_48k_frames / elapsed_sec;
            double rate_8k  = diag_total_8k_samples / elapsed_sec;
            size_t buf_used = size_buffer(capture_buffer);
            size_t buf_free = circular_buf_free_size(capture_buffer);
            HLOGD("audio-cap",
                  "DIAG: %.1fs | reads=%u errs=%u | 48kHz=%.0f Hz (expect 48000) | 8kHz=%.0f Hz (expect 8000) | last_read=%d B | ringbuf used=%zu free=%zu | drops=%u",
                  elapsed_sec, diag_read_calls, diag_read_errors,
                  rate_48k, rate_8k, diag_last_read_bytes,
                  buf_used, buf_free, diag_buf_full_drops);
#endif /* DEBUG_IO */
            /* reset counters */
            diag_start_ms = diag_now;
            diag_total_48k_frames = 0;
            diag_total_8k_samples = 0;
            diag_read_calls = 0;
            diag_read_errors = 0;
            diag_buf_full_drops = 0;
        }
    }

    r = audio->stop(b);
    if (r != 0)
        HLOGE("audio-cap", "ffaudio.stop: %s", audio->error(b));

    r = audio->clear(b);
    if (r != 0)
        HLOGE("audio-cap", "ffaudio.clear: %s", audio->error(b));

    free(buffer_output);
    free(buffer_downsampled);

cleanup_cap:

    audio->free(b);

    // Only uninit if this thread was the one that initialized the PA context
    if (did_init_cap)
        audio->uninit();

finish_cap:
    HLOGI("audio-cap", "radio_capture_thread exit");

    // Only trigger global shutdown if this was NOT a restart-initiated stop
    if (!audio_shutdown_)
        shutdown_ = true;

    return NULL;
}

int get_soundcard_list(int audio_system, int mode,
                       char ids[][64], char dev_names[][64], int max_count)
{
    ffaudio_interface *audio = NULL;
    int count = 0;
    bool did_init = false;

    if (audio_system == AUDIO_SUBSYSTEM_SHM ||
        audio_system == AUDIO_SUBSYSTEM_NULL ||
        audio_system == AUDIO_SUBSYSTEM_FIFO)
        return 0;

#if defined(_WIN32)
    if (audio_system == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_system == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    if (audio_system == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
    if (audio_system == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
#elif defined(__FREEBSD__)
    if (audio_system == AUDIO_SUBSYSTEM_OSS)
        audio = (ffaudio_interface *) &ffoss;
#elif defined(__APPLE__)
    if (audio_system == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#elif defined(__ANDROID__)
    if (audio_system == AUDIO_SUBSYSTEM_AAUDIO)
        audio = (ffaudio_interface *) &ffaaudio;
#endif

    if (!audio)
        return 0;

#if defined(__linux__)
    if (audio_system == AUDIO_SUBSYSTEM_PULSE)
    {
        if (pulse_shared_init(&did_init) != 0)
            return 0;
    }
    else
#endif
    {
        ffaudio_init_conf aconf = {};
        if (audio->init(&aconf) != 0)
            return 0;
        did_init = true;
    }

    // mode: FFAUDIO_DEV_PLAYBACK (0) or FFAUDIO_DEV_CAPTURE (1)
    ffaudio_dev *d = audio->dev_alloc(mode);
    if (d == NULL)
    {
        if (did_init)
            audio->uninit();
        return 0;
    }

    for (;;)
    {
        int r = audio->dev_next(d);
        if (r != 0)
            break;
        const char *id = audio->dev_info(d, FFAUDIO_DEV_ID);
        const char *name = audio->dev_info(d, FFAUDIO_DEV_NAME);
        if (id && count < max_count)
        {
            strncpy(ids[count], id, 63);
            ids[count][63] = '\0';
            if (name) {
                strncpy(dev_names[count], name, 63);
                dev_names[count][63] = '\0';
            } else {
                strncpy(dev_names[count], id, 63);
                dev_names[count][63] = '\0';
            }
            count++;
        }
    }

    audio->dev_free(d);
    if (did_init)
        audio->uninit();
    return count;
}

void list_soundcards(int audio_system)
{
    ffaudio_interface *audio = NULL;
    bool did_init = false;
    audio_subsystem = audio_system;

    if (audio_subsystem == AUDIO_SUBSYSTEM_SHM)
    {
        // TODO: connect to the shared memory
        printf("Shared Memory (SHM) audio subsystem selected.\n");
        audio = NULL;
        return;
    }
    if (audio_subsystem == AUDIO_SUBSYSTEM_NULL)
    {
        printf("Null audio subsystem selected (developer/test backend; no devices).\n");
        audio = NULL;
        return;
    }
    if (audio_subsystem == AUDIO_SUBSYSTEM_FIFO)
    {
        printf("FIFO audio subsystem selected (developer/test backend; no devices).\n");
        audio = NULL;
        return;
    }
    
#if defined(_WIN32)
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
    {
        printf("Listing ALSA soundcards:\n");
        audio = (ffaudio_interface *) &ffalsa;
    }
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        audio = (ffaudio_interface *) &ffpulse;
#elif defined(__FREEBSD__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_OSS)
        audio = (ffaudio_interface *) &ffoss;
#elif defined(__APPLE__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_COREAUDIO)
        audio = (ffaudio_interface *) &ffcoreaudio;
#elif defined(__ANDROID__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_AAUDIO)
        audio = (ffaudio_interface *) &ffaaudio;
#endif

    if (!audio)
        return;

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
    {
        if (pulse_shared_init(&did_init) != 0)
        {
            printf("Error in audio->init()\n");
            return;
        }
    }
    else
#endif
    {
        ffaudio_init_conf aconf = {};
        if (audio->init(&aconf) != 0)
        {
            printf("Error in audio->init()\n");
            return;
        }
        did_init = true;
    }

    ffaudio_dev *d;

    // FFAUDIO_DEV_PLAYBACK, FFAUDIO_DEV_CAPTURE
    static const char* const mode[] = { "playback", "capture" };
    for (ffuint i = 0;  i != 2;  i++)
    {
        printf("%s devices:\n", mode[i]);
        d = audio->dev_alloc(i);
        if (d == NULL)
        {
            printf("Error in audio->dev_alloc\n");
            if (did_init)
                audio->uninit();
            return;
        }

        for (;;)
        {
            int r = audio->dev_next(d);
            if (r > 0)
                break;
            else
                if (r < 0)
                {
                    printf("error: %s", audio->dev_error(d));
                    break;
                }

            printf("device: name: '%s'  id: '%s'  default: %s\n"
                   , audio->dev_info(d, FFAUDIO_DEV_NAME)
                   , audio->dev_info(d, FFAUDIO_DEV_ID)
                   , audio->dev_info(d, FFAUDIO_DEV_IS_DEFAULT)
                );
        }

        audio->dev_free(d);
    }

    if (did_init)
        audio->uninit();
}

#if 0
// size in "double" samples
int tx_transfer(double *buffer, size_t len)
{
    uint8_t *buffer_internal = (uint8_t *) buffer;
    int buffer_size_bytes = len * sizeof(double);

    write_buffer(playback_buffer, buffer_internal, buffer_size_bytes);

    // printf("size %llu free %llu\n", size_buffer(playback_buffer), circular_buf_free_size(playback_buffer));

    return 0;
}

// size in "double" samples
int rx_transfer(double *buffer, size_t len)
{
    uint8_t *buffer_internal = (uint8_t *) buffer;
    int buffer_size_bytes = len * sizeof(double);

    read_buffer(capture_buffer, buffer_internal, buffer_size_bytes);

    return 0;
}
#endif

static int audioio_init_local_buffers(void)
{
    uint8_t *buffer_cap = (uint8_t *) malloc(SIGNAL_BUFFER_SIZE);
    uint8_t *buffer_play = (uint8_t *) malloc(SIGNAL_BUFFER_SIZE);
    if (!buffer_cap || !buffer_play)
    {
        free(buffer_cap);
        free(buffer_play);
        return -1;
    }

    capture_buffer = circular_buf_init(buffer_cap, SIGNAL_BUFFER_SIZE);
    playback_buffer = circular_buf_init(buffer_play, SIGNAL_BUFFER_SIZE);
    if (!capture_buffer || !playback_buffer)
    {
        if (capture_buffer)
            circular_buf_free(capture_buffer);
        if (playback_buffer)
            circular_buf_free(playback_buffer);
        free(buffer_cap);
        free(buffer_play);
        capture_buffer = NULL;
        playback_buffer = NULL;
        return -1;
    }
    return 0;
}

static void audioio_deinit_local_buffers(void)
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
    if (s_buffers_initialized)
        return 0;  // already created

    if (audio_subsystem == AUDIO_SUBSYSTEM_NULL ||
        audio_subsystem == AUDIO_SUBSYSTEM_FIFO)
    {
        if (audioio_init_local_buffers() != 0)
            return -1;
    }
#if defined(_WIN32)
    else if (audioio_init_local_buffers() != 0)
        return -1;
#else
    else
    {
    capture_buffer = circular_buf_init_shm(SIGNAL_BUFFER_SIZE, (char *) SIGNAL_INPUT);
    playback_buffer = circular_buf_init_shm(SIGNAL_BUFFER_SIZE, (char *) SIGNAL_OUTPUT);
    }
#endif

    clear_buffer(capture_buffer);
    clear_buffer(playback_buffer);
    s_buffers_initialized = 1;
    return 0;
}

void audioio_deinit_buffers(void)
{
    if (!s_buffers_initialized)
        return;

    if (audio_subsystem == AUDIO_SUBSYSTEM_NULL ||
        audio_subsystem == AUDIO_SUBSYSTEM_FIFO)
    {
        audioio_deinit_local_buffers();
    }
#if defined(_WIN32)
    else
    {
        audioio_deinit_local_buffers();
    }
#else
    else
    {
    circular_buf_destroy_shm(capture_buffer, SIGNAL_BUFFER_SIZE, (char *) SIGNAL_INPUT);
    circular_buf_free_shm(capture_buffer);

    circular_buf_destroy_shm(playback_buffer, SIGNAL_BUFFER_SIZE, (char *) SIGNAL_OUTPUT);
    circular_buf_free_shm(playback_buffer);
    capture_buffer = NULL;
    playback_buffer = NULL;
    }
#endif
    s_buffers_initialized = 0;
}

int audioio_init_internal(char *capture_dev, char *playback_dev, int audio_subsys, int capture_channel_layout, pthread_t *radio_capture,
                          pthread_t *radio_playback)
{
    audio_subsystem = audio_subsys;
    if (capture_channel_layout == LEFT ||
        capture_channel_layout == RIGHT ||
        capture_channel_layout == STEREO)
        capture_input_channel_layout = capture_channel_layout;
    else
        capture_input_channel_layout = LEFT;

    // Store device names for restart support
    if (capture_dev)
    {
        strncpy(s_capture_dev, capture_dev, sizeof(s_capture_dev) - 1);
        s_capture_dev[sizeof(s_capture_dev) - 1] = '\0';
    }
    else
        s_capture_dev[0] = '\0';
    if (playback_dev)
    {
        strncpy(s_playback_dev, playback_dev, sizeof(s_playback_dev) - 1);
        s_playback_dev[sizeof(s_playback_dev) - 1] = '\0';
    }
    else
        s_playback_dev[0] = '\0';

    // Create buffers if not already created
    if (audioio_init_buffers() != 0)
        return -1;

    if (audio_subsystem == AUDIO_SUBSYSTEM_FIFO)
    {
        fifo_ignore_sigpipe();
        pthread_create(radio_capture, NULL, fifo_capture_thread, (void *) s_capture_dev);
        pthread_create(radio_playback, NULL, fifo_playback_thread, (void *) s_playback_dev);
        s_radio_capture = *radio_capture;
        s_radio_playback = *radio_playback;
        return 0;
    }

    if (audio_subsystem == AUDIO_SUBSYSTEM_NULL)
    {
        pthread_create(radio_capture, NULL, null_capture_thread, NULL);
        pthread_create(radio_playback, NULL, null_playback_thread, NULL);
        s_radio_capture = *radio_capture;
        s_radio_playback = *radio_playback;
        return 0;
    }

    /* Pre-initialize PulseAudio once here in the main thread before spawning
     * capture/playback threads. ffpulse_init() uses a single global context
     * (gconn) and returns an error if called more than once. By initializing
     * here, both threads will see "already initialized" and proceed normally
     * rather than one of them failing and exiting early.
     */
#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
    {
        (void) pulse_shared_init(NULL);
    }
#endif

    pthread_create(radio_capture, NULL, radio_capture_thread, (void *) s_capture_dev);
    pthread_create(radio_playback, NULL, radio_playback_thread, (void *) s_playback_dev);

    // Keep internal copies of thread handles
    s_radio_capture = *radio_capture;
    s_radio_playback = *radio_playback;

    return 0;
}

static void audioio_stop_threads(void)
{
    // Signal audio threads to exit their loops
    audio_shutdown_ = true;
    pthread_join(s_radio_capture, NULL);
    pthread_join(s_radio_playback, NULL);
    audio_shutdown_ = false;

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        pulse_shared_uninit();
#endif

    HLOGI("audio-stop", "audioio threads stopped");
}

int audioio_restart(const char *capture_dev, const char *playback_dev,
                    int audio_subsys, int capture_channel_layout)
{
    HLOGI("audio-restart", "stopping audio threads...");
    audioio_stop_threads();

    // Update stored parameters
    audio_subsystem = audio_subsys;
    if (capture_channel_layout == LEFT ||
        capture_channel_layout == RIGHT ||
        capture_channel_layout == STEREO)
        capture_input_channel_layout = capture_channel_layout;
    else
        capture_input_channel_layout = LEFT;

    if (capture_dev && capture_dev[0] != '\0')
    {
        strncpy(s_capture_dev, capture_dev, sizeof(s_capture_dev) - 1);
        s_capture_dev[sizeof(s_capture_dev) - 1] = '\0';
    }

    if (playback_dev && playback_dev[0] != '\0')
    {
        strncpy(s_playback_dev, playback_dev, sizeof(s_playback_dev) - 1);
        s_playback_dev[sizeof(s_playback_dev) - 1] = '\0';
    }

    // Clear buffers (NEVER destroy/recreate them)
    clear_buffer(capture_buffer);
    clear_buffer(playback_buffer);

    HLOGI("audio-restart", "starting audio threads (capture=%s playback=%s channel=%d)...",
           s_capture_dev[0] ? s_capture_dev : "default",
           s_playback_dev[0] ? s_playback_dev : "default",
           capture_input_channel_layout);

    if (audio_subsystem == AUDIO_SUBSYSTEM_NULL)
    {
        pthread_create(&s_radio_capture, NULL, null_capture_thread, NULL);
        pthread_create(&s_radio_playback, NULL, null_playback_thread, NULL);
        HLOGI("audio-restart", "null audio threads restarted");
        return 0;
    }

    if (audio_subsystem == AUDIO_SUBSYSTEM_FIFO)
    {
        fifo_ignore_sigpipe();
        pthread_create(&s_radio_capture, NULL, fifo_capture_thread, (void *) s_capture_dev);
        pthread_create(&s_radio_playback, NULL, fifo_playback_thread, (void *) s_playback_dev);
        HLOGI("audio-restart", "FIFO audio threads restarted");
        return 0;
    }

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
    {
        (void) pulse_shared_init(NULL);
    }
#endif

    pthread_create(&s_radio_capture, NULL, radio_capture_thread, (void *) s_capture_dev);
    pthread_create(&s_radio_playback, NULL, radio_playback_thread, (void *) s_playback_dev);

    HLOGI("audio-restart", "audio threads restarted");
    return 0;
}

int audioio_deinit(pthread_t *radio_capture, pthread_t *radio_playback)
{
    // The external thread handles may be stale after a restart; use internal statics instead.
    (void) radio_capture;
    (void) radio_playback;
    pthread_join(s_radio_capture, NULL);
    pthread_join(s_radio_playback, NULL);

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
        pulse_shared_uninit();
#endif

    audioio_deinit_buffers();
    return 0;
}
