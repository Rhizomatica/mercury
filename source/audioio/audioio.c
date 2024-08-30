/* Audio subsystem
 *
 * Copyright (C) 2024 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */


#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <limits.h>
#include <ffaudio/audio.h>
#include <ffbase/args.h>
#include <ffbase/stringz.h>
#include "std.h"
#include "../../include/audioio/audioio.h"
#ifdef FF_LINUX
#include <time.h>
#endif

#include "common/ring_buffer_posix.h"
#include "common/shm_posix.h"
#include "common/common_defines.h"

// bool shutdown_;
extern bool shutdown_;
extern int radio_type;

cbuf_handle_t capture_buffer;
cbuf_handle_t playback_buffer;

int audio_subsystem;

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


void *radio_playback_thread(void *device_ptr)
{
    ffaudio_interface *audio;
	struct conf conf = {};
	conf.buf.app_name = "mercury_playback";
	conf.buf.format = FFAUDIO_F_INT32;
	conf.buf.sample_rate = 48000;
	conf.buf.channels = 2;
	conf.buf.device_id = device_ptr;

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

    conf.flags = FFAUDIO_PLAYBACK;
	ffaudio_init_conf aconf = {};
	aconf.app_name = "mercury_playback";
	if ( audio->init(&aconf) != 0)
    {
        printf("Error in audio->init()\n");
        goto finish_play;
    }

    // playback code...
	int r;
	ffaudio_buf *b;

	b = audio->alloc();
	if (b == NULL)
	{
		printf("Error in audio->alloc()\n");
		goto finish_play;
	}

	ffaudio_conf *cfg = &conf.buf;
	r = audio->open(b, cfg, conf.flags);
	if (r == FFAUDIO_EFORMAT)
		r = audio->open(b, cfg, conf.flags);
	if (r != 0)
	{
		printf("error in audio->open(): %d: %s\n", r, audio->error(b));
		goto cleanup_play;
	}

	printf("(%s) %d bits per sample / %d / %d / %dms buffer\n", conf.buf.device_id ? conf.buf.device_id : "default", cfg->format, cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);


	ffuint frame_size = cfg->channels * (cfg->format & 0xff) / 8;
	ffuint msec_bytes = cfg->sample_rate * frame_size / 1000;

	uint8_t *buffer =  malloc(AUDIO_PAYLOAD_BUFFER_SIZE);
	double *buffer_double =  (double *) buffer;

	ffuint total_written = 0;

	int ch_layout = STEREO;

    if (radio_type == RADIO_SBITX)
        ch_layout = RIGHT;
    if (radio_type == RADIO_STOCKHF)
        ch_layout = STEREO;

    while (!shutdown_)
    {
        ffssize n = read_buffer_all(playback_buffer, buffer);
        total_written = 0;

		int samples_read = n / sizeof(double);

		int32_t buffer_internal_stereo[samples_read * cfg->channels];

		// convert from double to int32
		for (int i = 0; i < samples_read; i++)
		{
			int idx = i * cfg->channels;
			if (ch_layout == LEFT)
			{
				buffer_internal_stereo[idx] = buffer_double[i] * INT_MAX;
				buffer_internal_stereo[idx + 1] = 0;
			}

			if (ch_layout == RIGHT)
			{
				buffer_internal_stereo[idx] = 0;
				buffer_internal_stereo[idx + 1] = buffer_double[i] * INT_MAX;
			}


			if (ch_layout == STEREO)
			{
				buffer_internal_stereo[idx] = buffer_double[i] * INT_MAX;
				buffer_internal_stereo[idx + 1] = buffer_internal_stereo[idx];
			}
		}

		n = samples_read * frame_size;

        while (n >= frame_size)
        {
            r = audio->write(b, ((uint8_t *)buffer_internal_stereo) + total_written, n);

            if (r == -FFAUDIO_ESYNC) {
                printf("detected underrun");
                continue;
            }
            if (r < 0)
            {
                printf("ffaudio.write: %s", audio->error(b));
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


    r = audio->drain(b);
    if (r < 0)
        printf("ffaudio.drain: %s", audio->error(b));

    r = audio->stop(b);
    if (r != 0)
        printf("ffaudio.stop: %s", audio->error(b));

    r = audio->clear(b);
    if (r != 0)
        printf("ffaudio.clear: %s", audio->error(b));

    free(buffer);

cleanup_play:

    audio->free(b);

	audio->uninit();

finish_play:

	printf("radio_playback_thread exit\n");

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
	conf.buf.device_id = device_ptr;

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
	if ( audio->init(&aconf) != 0)
    {
        printf("Error in audio->init()\n");
        goto finish_cap;
    }

    // capture code
	int r;
	ffaudio_buf *b;

	b = audio->alloc();
	if (b == NULL)
    {
        printf("Error in audio->alloc()\n");
        goto finish_cap;
    }

    ffaudio_conf *cfg = &conf.buf;
	r = audio->open(b, cfg, conf.flags);
	if (r == FFAUDIO_EFORMAT)
		r = audio->open(b, cfg, conf.flags);
	if (r != 0)
    {
        printf("error in audio->open(): %d: %s\n", r, audio->error(b));
        goto cleanup_cap;
    }

	printf("(%s) %d bits per sample / %d / %d / %dms buffer\n", conf.buf.device_id ? conf.buf.device_id : "default", cfg->format, cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);

    ffuint frame_size = cfg->channels * (cfg->format & 0xff) / 8;
    ffuint msec_bytes = cfg->sample_rate * frame_size / 1000;

	int32_t *buffer = NULL;

	int ch_layout = STEREO;

	if (radio_type == RADIO_SBITX)
		ch_layout = LEFT;
	if (radio_type == RADIO_STOCKHF)
		ch_layout = STEREO;

	while (!shutdown_)
    {
		r = audio->read(b, (const void **)&buffer);
		if (r < 0)
        {
			printf("ffaudio.read: %s", audio->error(b));
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

		double buffer_internal[frames_to_write];

		for (int i = 0; i < frames_to_write; i++)
		{
			if (ch_layout == LEFT)
			{
				buffer_internal[i] = (double) buffer[i*2] / (double) INT_MAX;
			}

			if (ch_layout == RIGHT)
			{
				buffer_internal[i] = (double) buffer[i*2 + 1] / (double) INT_MAX;
			}

			if (ch_layout == STEREO)
			{
				buffer_internal[i] = (double) ((buffer[i*2] + buffer[i*2 + 1]) / 2.0) / (double) INT_MAX;
			}

		}

        write_buffer(capture_buffer, (uint8_t *)buffer_internal, frames_to_write * sizeof(double));
		// ffstderr_write(data.ptr, data.len);

	}

	r = audio->stop(b);
	if (r != 0)
		printf("ffaudio.stop: %s", audio->error(b));

	r = audio->clear(b);
	if (r != 0)
		printf("ffaudio.clear: %s", audio->error(b));

cleanup_cap:

	audio->free(b);

    audio->uninit();

finish_cap:
	printf("radio_capture_thread exit\n");

    shutdown_ = true;

    return NULL;
}

void list_soundcards(int audio_system)
{
    ffaudio_interface *audio;
    audio_subsystem = audio_system;

#if defined(_WIN32)
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI)
        audio = (ffaudio_interface *) &ffwasapi;
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND)
        audio = (ffaudio_interface *) &ffdsound;
#elif defined(__linux__)
    if (audio_subsystem == AUDIO_SUBSYSTEM_ALSA)
        audio = (ffaudio_interface *) &ffalsa;
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

// size in "double" samples
int tx_transfer(double *buffer, size_t len)
{
	uint8_t *buffer_internal = (uint8_t *) buffer;
	int buffer_size_bytes = len * sizeof(double);

	write_buffer(playback_buffer, buffer_internal, buffer_size_bytes);

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


int audioio_init(char *capture_dev, char *playback_dev, int audio_subsys, pthread_t *radio_capture, pthread_t *radio_playback)
{
    audio_subsystem = audio_subsys;

    capture_buffer = circular_buf_init_shm(AUDIO_PAYLOAD_BUFFER_SIZE, (char *) AUDIO_CAPT_PAYLOAD_NAME);
    playback_buffer = circular_buf_init_shm(AUDIO_PAYLOAD_BUFFER_SIZE, (char *) AUDIO_PLAY_PAYLOAD_NAME);

    pthread_create(radio_capture, NULL, radio_capture_thread, (void*)capture_dev);
    pthread_create(radio_playback, NULL, radio_playback_thread, (void*)playback_dev);

	return 0;
}

int audioio_deinit(pthread_t *radio_capture, pthread_t *radio_playback)
{
    pthread_join(*radio_capture, NULL);
    pthread_join(*radio_playback, NULL);

    circular_buf_destroy_shm(capture_buffer, AUDIO_PAYLOAD_BUFFER_SIZE, (char *) AUDIO_CAPT_PAYLOAD_NAME);
    circular_buf_free_shm(capture_buffer);

    circular_buf_destroy_shm(playback_buffer, AUDIO_PAYLOAD_BUFFER_SIZE, (char *) AUDIO_PLAY_PAYLOAD_NAME);
    circular_buf_free_shm(playback_buffer);

    return 0;
}
