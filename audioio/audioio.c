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
#include "os_interop.h"
#include <ffaudio/audio.h>
#include "std.h"
#ifdef FF_LINUX
#include <time.h>
#endif

#include "ring_buffer_posix.h"
#include "shm_posix.h"
#include "defines_modem.h"

#include "audioio.h"
#include "hermes_log.h"

extern volatile bool shutdown_;

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

int audioio_pick_default_subsystem(void)
{
#if defined(__linux__)
    return AUDIO_SUBSYSTEM_ALSA;
#elif defined(_WIN32)
    return AUDIO_SUBSYSTEM_DSOUND;
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


void *radio_playback_thread(void *device_ptr)
{
    ffaudio_interface *audio;
    struct conf conf = {};
    conf.buf.app_name = "mercury_playback";
    conf.buf.format = FFAUDIO_F_INT32;
    conf.buf.sample_rate = 48000;
    conf.buf.channels = 2;
    conf.buf.device_id = (const char *) device_ptr;
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
    conf.buf.buffer_length_msec = 30;
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

    HLOGI("audio-play", "I/O playback (%s) %d bits per sample / %dHz / %dch / %dms buffer", conf.buf.device_id ? conf.buf.device_id : "default", cfg->format, cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);


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
    conf.buf.device_id = (const char *) device_ptr;

#if defined(_WIN32)
    conf.buf.buffer_length_msec = 40;
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    conf.buf.buffer_length_msec = 30;
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

    HLOGI("audio-cap", "I/O capture (%s) %d bits per sample / %dHz / %dch / %dms buffer", conf.buf.device_id ? conf.buf.device_id : "default", cfg->format, cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);

    frame_size = cfg->channels * (cfg->format & 0xff) / 8;
    msec_bytes = cfg->sample_rate * frame_size / 1000;

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
    
    while (!shutdown_ && !audio_shutdown_)
    {
        r = audio->read(b, (const void **)&buffer);
        if (r < 0)
        {
            HLOGE("audio-cap", "ffaudio.read: %s", audio->error(b));
            continue;
        }
#if 0
        else
        {
            printf(" %dms\n", r / msec_bytes);
        }
#endif

        int frames_read = r / frame_size;
        int frames_to_write = frames_read;
        
        // Downsample from 48kHz to 8kHz with decimation
        // resample_remainder tracks position in decimation cycle (0 to resample_ratio-1)
        // When remainder is 0, we take a sample; otherwise skip
        int downsampled_frames = 0;
        for (int i = 0; i < frames_to_write; i++)
        {
            int32_t sample;
            if (ch_layout == LEFT)
            {
                sample = buffer[i*2];
            }
            else if (ch_layout == RIGHT)
            {
                sample = buffer[i*2 + 1];
            }
            else // STEREO
            {
                sample = (buffer[i*2] + buffer[i*2 + 1]) / 2;
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
                HLOGW("audio-cap", "Buffer full in capture buffer!");
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

    if (audio_system == AUDIO_SUBSYSTEM_SHM)
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

    ffaudio_init_conf aconf = {};
    if (audio->init(&aconf) != 0)
        return 0;

    // mode: FFAUDIO_DEV_PLAYBACK (0) or FFAUDIO_DEV_CAPTURE (1)
    ffaudio_dev *d = audio->dev_alloc(mode);
    if (d == NULL)
    {
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
    audio->uninit();
    return count;
}

void list_soundcards(int audio_system)
{
    ffaudio_interface *audio;
    audio_subsystem = audio_system;

    if (audio_subsystem == AUDIO_SUBSYSTEM_SHM)
    {
        // TODO: connect to the shared memory
        printf("Shared Memory (SHM) audio subsystem selected.\n");
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

    ffaudio_init_conf aconf = {};
    if ( audio->init(&aconf) != 0)
    {
        printf("Error in audio->init()\n");
        return;
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

int audioio_init_buffers(void)
{
    if (s_buffers_initialized)
        return 0;  // already created

#if defined(_WIN32)
    uint8_t *buffer_cap = (uint8_t *)malloc(SIGNAL_BUFFER_SIZE);
    uint8_t *buffer_play = (uint8_t *)malloc(SIGNAL_BUFFER_SIZE);
    capture_buffer = circular_buf_init(buffer_cap, SIGNAL_BUFFER_SIZE);
    playback_buffer = circular_buf_init(buffer_play, SIGNAL_BUFFER_SIZE);
#else
    capture_buffer = circular_buf_init_shm(SIGNAL_BUFFER_SIZE, (char *) SIGNAL_INPUT);
    playback_buffer = circular_buf_init_shm(SIGNAL_BUFFER_SIZE, (char *) SIGNAL_OUTPUT);
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

#if defined(_WIN32)
    free(capture_buffer->buffer);
    circular_buf_free(capture_buffer);
    free(playback_buffer->buffer);
    circular_buf_free(playback_buffer);
#else
    circular_buf_destroy_shm(capture_buffer, SIGNAL_BUFFER_SIZE, (char *) SIGNAL_INPUT);
    circular_buf_free_shm(capture_buffer);

    circular_buf_destroy_shm(playback_buffer, SIGNAL_BUFFER_SIZE, (char *) SIGNAL_OUTPUT);
    circular_buf_free_shm(playback_buffer);
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
        strncpy(s_capture_dev, capture_dev, sizeof(s_capture_dev) - 1);
    else
        s_capture_dev[0] = '\0';
    if (playback_dev)
        strncpy(s_playback_dev, playback_dev, sizeof(s_playback_dev) - 1);
    else
        s_playback_dev[0] = '\0';

    // Create buffers if not already created
    audioio_init_buffers();

    /* Pre-initialize PulseAudio once here in the main thread before spawning
     * capture/playback threads. ffpulse_init() uses a single global context
     * (gconn) and returns an error if called more than once. By initializing
     * here, both threads will see "already initialized" and proceed normally
     * rather than one of them failing and exiting early.
     */
#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
    {
        ffaudio_interface *audio = (ffaudio_interface *) &ffpulse;
        ffaudio_init_conf aconf = {};
        aconf.app_name = "mercury";
        if (audio->init(&aconf) != 0)
        {
            printf("Error pre-initializing PulseAudio: %s\n", aconf.error ? aconf.error : "unknown");
        }
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
        strncpy(s_capture_dev, capture_dev, sizeof(s_capture_dev) - 1);
    if (playback_dev && playback_dev[0] != '\0')
        strncpy(s_playback_dev, playback_dev, sizeof(s_playback_dev) - 1);

    // Clear buffers (NEVER destroy/recreate them)
    clear_buffer(capture_buffer);
    clear_buffer(playback_buffer);

    HLOGI("audio-restart", "starting audio threads (capture=%s playback=%s channel=%d)...",
           s_capture_dev[0] ? s_capture_dev : "default",
           s_playback_dev[0] ? s_playback_dev : "default",
           capture_input_channel_layout);

#if defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_PULSE)
    {
        ffaudio_interface *audio = (ffaudio_interface *) &ffpulse;
        ffaudio_init_conf aconf = {};
        aconf.app_name = "mercury";
        if (audio->init(&aconf) != 0)
        {
            // "already initialized" is expected and fine
        }
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

    audioio_deinit_buffers();
    return 0;
}
