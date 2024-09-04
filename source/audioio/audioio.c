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

	period_bytes = conf.buf.sample_rate * (conf.buf.format & 0xff) / 8 * conf.buf.channels * period_ms / 1000;

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

	uint8_t *buffer = (uint8_t *) malloc(AUDIO_PAYLOAD_BUFFER_SIZE * sizeof(double));
	double *buffer_double =  (double *) buffer;
	int32_t *buffer_internal_stereo = (int32_t *) malloc(AUDIO_PAYLOAD_BUFFER_SIZE * sizeof(int32_t)); // a big enough buffer

	ffuint total_written = 0;
	int ch_layout = STEREO;

	if ( audio->init(&aconf) != 0)
    {
        printf("Error in audio->init()\n");
        goto finish_play;
    }

    // playback code...
	b = audio->alloc();
	if (b == NULL)
	{
		printf("Error in audio->alloc()\n");
		goto finish_play;
	}

	cfg = &conf.buf;
	r = audio->open(b, cfg, conf.flags);
	if (r == FFAUDIO_EFORMAT)
		r = audio->open(b, cfg, conf.flags);
	if (r != 0)
	{
		printf("error in audio->open(): %d: %s\n", r, audio->error(b));
		goto cleanup_play;
	}

	printf("playback (%s) %d bits per sample / %dHz / %dch / %dms buffer\n", conf.buf.device_id ? conf.buf.device_id : "default", cfg->format, cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);


	frame_size = cfg->channels * (cfg->format & 0xff) / 8;
	msec_bytes = cfg->sample_rate * frame_size / 1000;

	if (radio_type == RADIO_SBITX)
		ch_layout = RIGHT;
	if (radio_type == RADIO_STOCKHF)
		ch_layout = STEREO;

    while (!shutdown_)
    {
		ffssize n;
		size_t buffer_size = size_buffer(playback_buffer);
		if (buffer_size >= period_bytes || buffer_size == 0) // if buffer_size == 0, we just block here... should we transmitt zeros?
		{
			read_buffer(playback_buffer, buffer, period_bytes);
			n = period_bytes;
		}
		else
		{
			read_buffer(playback_buffer, buffer, buffer_size);
			n = buffer_size;
		}

        total_written = 0;

		int samples_read = n / sizeof(double);


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

cleanup_play:

    audio->free(b);

	audio->uninit();

	finish_play:

	free(buffer);
	free(buffer_internal_stereo);

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


	if ( audio->init(&aconf) != 0)
    {
        printf("Error in audio->init()\n");
        goto finish_cap;
    }

    // capture code
	b = audio->alloc();
	if (b == NULL)
    {
        printf("Error in audio->alloc()\n");
        goto finish_cap;
    }

    cfg = &conf.buf;
	r = audio->open(b, cfg, conf.flags);
	if (r == FFAUDIO_EFORMAT)
		r = audio->open(b, cfg, conf.flags);
	if (r != 0)
    {
        printf("error in audio->open(): %d: %s\n", r, audio->error(b));
        goto cleanup_cap;
    }

	printf("capture (%s) %d bits per sample / %dHz / %dch / %dms buffer\n", conf.buf.device_id ? conf.buf.device_id : "default", cfg->format, cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);

    frame_size = cfg->channels * (cfg->format & 0xff) / 8;
    msec_bytes = cfg->sample_rate * frame_size / 1000;

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

		if (circular_buf_free_size(capture_buffer) >= frames_to_write * sizeof(double))
			write_buffer(capture_buffer, (uint8_t *)buffer_internal, frames_to_write * sizeof(double));
		// else
		// BUFFER FULL!!
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

void *radio_capture_prep_thread(void *telecom_ptr_void)
{
	cl_telecom_system *telecom_ptr = (cl_telecom_system *) telecom_ptr_void;

	double *buffer_temp = (double *) malloc(AUDIO_PAYLOAD_BUFFER_SIZE * sizeof(double));

	while (!shutdown_)
    {
		cl_data_container *data_container_ptr = &telecom_ptr->data_container;
		int signal_period = data_container_ptr->Nofdm * data_container_ptr->buffer_Nsymb * data_container_ptr->interpolation_rate; // in samples
		int symbol_period = data_container_ptr->Nofdm * data_container_ptr->interpolation_rate;
		int location_of_last_frame = signal_period - symbol_period - 1; // TODO: do we need this "-1"?

		if (symbol_period == 0)
			continue;

		rx_transfer(buffer_temp, symbol_period);

		// TODO: lock
		if(data_container_ptr->data_ready == 1)
			data_container_ptr->nUnder_processing_events++;

		// TODO: race condition here with "passband_delayed_data" !!! WTF!!!
		shift_left(data_container_ptr->passband_delayed_data, signal_period, symbol_period);

		memcpy(&data_container_ptr->passband_delayed_data[location_of_last_frame], buffer_temp, symbol_period * sizeof(double));

		data_container_ptr->frames_to_read--;
		if(data_container_ptr->frames_to_read < 0)
			data_container_ptr->frames_to_read = 0;

		data_container_ptr->data_ready = 1;
		// TODO: unlock
	}


	shutdown_ = true;

	free(buffer_temp);

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


int audioio_init_internal(char *capture_dev, char *playback_dev, int audio_subsys, pthread_t *radio_capture,
						  pthread_t *radio_playback, pthread_t *radio_capture_prep, cl_telecom_system *telecom_system)
{
    audio_subsystem = audio_subsys;

    capture_buffer = circular_buf_init_shm(AUDIO_PAYLOAD_BUFFER_SIZE, (char *) AUDIO_CAPT_PAYLOAD_NAME);
    playback_buffer = circular_buf_init_shm(AUDIO_PAYLOAD_BUFFER_SIZE, (char *) AUDIO_PLAY_PAYLOAD_NAME);

    pthread_create(radio_capture, NULL, radio_capture_thread, (void *) capture_dev);
    pthread_create(radio_playback, NULL, radio_playback_thread, (void *) playback_dev);
	pthread_create(radio_capture_prep, NULL, radio_capture_prep_thread, (void *) telecom_system);

	return 0;
}

int audioio_deinit(pthread_t *radio_capture, pthread_t *radio_playback, pthread_t *radio_capture_prep)
{
    pthread_join(*radio_capture_prep, NULL);
    pthread_join(*radio_capture, NULL);
    pthread_join(*radio_playback, NULL);


    circular_buf_destroy_shm(capture_buffer, AUDIO_PAYLOAD_BUFFER_SIZE, (char *) AUDIO_CAPT_PAYLOAD_NAME);
    circular_buf_free_shm(capture_buffer);

    circular_buf_destroy_shm(playback_buffer, AUDIO_PAYLOAD_BUFFER_SIZE, (char *) AUDIO_PLAY_PAYLOAD_NAME);
    circular_buf_free_shm(playback_buffer);

    return 0;
}
