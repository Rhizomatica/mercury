/* Audio subsystem
 *
 * Copyright (C) 2024 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */


#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <string.h>
#ifdef _WIN32
#include <wchar.h>
#endif
#include <ffaudio/audio.h>
#include "std.h"
#include "../../include/audioio/audioio.h"
#ifdef FF_LINUX
#include <time.h>
#endif

#include "common/ring_buffer_posix.h"
#include "common/shm_posix.h"
#include "common/common_defines.h"
#include "common/os_interop.h"

#ifdef MERCURY_GUI_ENABLED
#ifdef __cplusplus
extern "C++" {
#include "gui/gui_state.h"
}
#endif
#endif

// bool shutdown_;
extern bool shutdown_;
extern int radio_type;

// Audio channel configuration (set from main.cc / GUI settings)
// 0=LEFT, 1=RIGHT, 2=STEREO (L+R)
int configured_input_channel = 0;   // Default: LEFT (matches pre-GUI CLI default)
int configured_output_channel = 2;  // Default: STEREO

// Tune tone state (for GUI tune button)
static long tune_sample_index = 0;

cbuf_handle_t capture_buffer;
cbuf_handle_t playback_buffer;

int audio_subsystem;

#if defined(_WIN32)
    HANDLE            capture_prep_mutex;
#else
    pthread_mutex_t   capture_prep_mutex;
#endif


// tap to file FOR DEBUGGING PURPOSES //
#define ENABLE_FLOAT64_TAP 0
#define ENABLE_FLOAT64_TAP_BEFORE 0
#if ENABLE_FLOAT64_TAP_BEFORE == 1
	FILE *tap_play;
#endif
// FOR DEBUGGING PURPOSES //


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

#if defined(_WIN32)
/**
 * Convert device name string to GUID for DirectSound
 * Returns allocated GUID pointer on success, NULL if device not found or on error
 * Caller must free the returned pointer
 */
static void* dsound_device_name_to_guid(const char *device_name, ffuint mode)
{
	if (device_name == NULL)
		return NULL;

	ffaudio_interface *audio = (ffaudio_interface *) &ffdsound;
	ffaudio_init_conf aconf = {};
	if (audio->init(&aconf) != 0)
		return NULL;

	ffaudio_dev *d = audio->dev_alloc(mode);
	if (d == NULL) {
		audio->uninit();
		return NULL;
	}

	void *result_guid = NULL;
	ffsize device_name_len = strlen(device_name);

	for (;;) {
		int r = audio->dev_next(d);
		if (r > 0)
			break;  // No more devices
		if (r < 0) {
			// Error
			break;
		}

		const char *name = audio->dev_info(d, FFAUDIO_DEV_NAME);
		if (name != NULL) {
			ffsize name_len = strlen(name);
			ffsize min_len = (name_len < device_name_len) ? name_len : device_name_len;

			// Match using prefix comparison to handle truncated device names
			// This allows "Microphone (2- USB Audio CODEC )" to match "Microphone (2- USB Audio CODEC " (truncated)
			if (strncmp(name, device_name, min_len) == 0) {
				// Found matching device
				const void *guid_ptr = audio->dev_info(d, FFAUDIO_DEV_ID);
				if (guid_ptr != NULL) {
					// Allocate and copy GUID
					result_guid = malloc(sizeof(GUID));
					if (result_guid != NULL) {
						memcpy(result_guid, guid_ptr, sizeof(GUID));
					}
				}
				break;
			}
		}
	}

	audio->dev_free(d);
	audio->uninit();
	return result_guid;
}

/**
 * Convert device name string to wide-string device ID for WASAPI
 * Returns allocated wchar_t* on success, NULL if device not found or on error
 * Caller must free the returned pointer
 */
static void* wasapi_device_name_to_id(const char *device_name, ffuint mode)
{
	if (device_name == NULL)
		return NULL;

	ffaudio_interface *audio = (ffaudio_interface *) &ffwasapi;
	ffaudio_init_conf aconf = {};
	if (audio->init(&aconf) != 0)
		return NULL;

	ffaudio_dev *d = audio->dev_alloc(mode);
	if (d == NULL) {
		audio->uninit();
		return NULL;
	}

	wchar_t *result_id = NULL;
	ffsize device_name_len = strlen(device_name);

	for (;;) {
		int r = audio->dev_next(d);
		if (r > 0)
			break;  // No more devices
		if (r < 0) {
			// Error
			break;
		}

		const char *name = audio->dev_info(d, FFAUDIO_DEV_NAME);
		if (name != NULL) {
			ffsize name_len = strlen(name);
			ffsize min_len = (name_len < device_name_len) ? name_len : device_name_len;

			// Match using prefix comparison to handle truncated device names
			if (strncmp(name, device_name, min_len) == 0) {
				// Found matching device - get the wide-string ID
				const wchar_t *id_ptr = (const wchar_t *)audio->dev_info(d, FFAUDIO_DEV_ID);
				if (id_ptr != NULL) {
					// Allocate and copy wide string
					ffsize id_len = wcslen(id_ptr) + 1;
					result_id = (wchar_t *)malloc(id_len * sizeof(wchar_t));
					if (result_id != NULL) {
						wcscpy(result_id, id_ptr);
					}
				}
				break;
			}
		}
	}

	audio->dev_free(d);
	audio->uninit();
	return result_id;
}


/**
 * Validate audio device configuration for Mercury
 * Checks that devices exist and are configured correctly (stereo, 48kHz)
 *
 * Returns bitmask of errors:
 *   0 = OK
 *   1 = Capture device not found
 *   2 = Playback device not found
 *   4 = Capture device not stereo (must be 2 channels)
 *   8 = Playback device not stereo (must be 2 channels)
 */
int validate_audio_config(const char *capture_dev, const char *playback_dev, int audio_system)
{
	int errors = 0;

	// Only WASAPI provides mix format info
	if (audio_system != AUDIO_SUBSYSTEM_WASAPI) {
		printf("[AUDIO CHECK] DirectSound selected - format validation not available\n");
		printf("[AUDIO CHECK] Recommendation: Use WASAPI (-x wasapi) for virtual audio cables\n");
		return 0;
	}

	ffaudio_interface *audio = (ffaudio_interface *) &ffwasapi;
	ffaudio_init_conf aconf = {};
	if (audio->init(&aconf) != 0) {
		printf("[AUDIO CHECK] ERROR: Failed to initialize WASAPI\n");
		return 1 | 2;
	}

	printf("\n");
	printf("========================================================================\n");
	printf("  MERCURY AUDIO CONFIGURATION CHECK\n");
	printf("========================================================================\n\n");

	// Check capture device
	printf("Checking CAPTURE device...\n");
	{
		ffaudio_dev *d = audio->dev_alloc(FFAUDIO_DEV_CAPTURE);
		if (d == NULL) {
			printf("  ERROR: Failed to enumerate capture devices\n");
			errors |= 1;
		} else {
			int found = 0;
			const char *target_name = capture_dev;
			ffsize target_len = target_name ? strlen(target_name) : 0;

			for (;;) {
				int r = audio->dev_next(d);
				if (r > 0) break;
				if (r < 0) break;

				const char *name = audio->dev_info(d, FFAUDIO_DEV_NAME);
				const char *is_default = audio->dev_info(d, FFAUDIO_DEV_IS_DEFAULT);

				int is_match = 0;
				if (target_name == NULL && is_default != NULL) {
					is_match = 1;
				} else if (target_name != NULL && name != NULL) {
					ffsize name_len = strlen(name);
					ffsize min_len = (name_len < target_len) ? name_len : target_len;
					if (strncmp(name, target_name, min_len) == 0) {
						is_match = 1;
					}
				}

				if (is_match) {
					found = 1;
					printf("  Device: %s%s\n", name, is_default ? " (DEFAULT)" : "");

					const ffuint *fmt = (const ffuint *)audio->dev_info(d, FFAUDIO_DEV_MIX_FORMAT);
					if (fmt != NULL) {
						ffuint format = fmt[0];
						ffuint sample_rate = fmt[1];
						ffuint channels = fmt[2];

						const char *fmt_name = "UNKNOWN";
						if (format == FFAUDIO_F_INT16) fmt_name = "INT16";
						else if (format == FFAUDIO_F_INT32) fmt_name = "INT32";
						else if (format == FFAUDIO_F_FLOAT32) fmt_name = "FLOAT32";

						printf("  Format: %s / %u Hz / %u channels\n", fmt_name, sample_rate, channels);

						if (channels != 2) {
							printf("  *** ERROR: Must be 2 channels (stereo), found %u ***\n", channels);
							printf("  FIX: Windows Sound Settings -> Recording -> %s\n", name);
							printf("       -> Properties -> Advanced -> Set to 2 channel, 48000 Hz\n");
							errors |= 4;
						} else {
							printf("  Channels: OK (stereo)\n");
						}

						if (sample_rate != 48000) {
							printf("  *** WARNING: Sample rate %u Hz, recommended 48000 Hz ***\n", sample_rate);
						} else {
							printf("  Sample rate: OK (48000 Hz)\n");
						}
					}
					break;
				}
			}

			if (!found) {
				printf("  ERROR: Device '%s' not found\n", target_name ? target_name : "(default)");
				errors |= 1;
			}
			audio->dev_free(d);
		}
	}

	printf("\n");

	// Check playback device
	printf("Checking PLAYBACK device...\n");
	{
		ffaudio_dev *d = audio->dev_alloc(FFAUDIO_DEV_PLAYBACK);
		if (d == NULL) {
			printf("  ERROR: Failed to enumerate playback devices\n");
			errors |= 2;
		} else {
			int found = 0;
			const char *target_name = playback_dev;
			ffsize target_len = target_name ? strlen(target_name) : 0;

			for (;;) {
				int r = audio->dev_next(d);
				if (r > 0) break;
				if (r < 0) break;

				const char *name = audio->dev_info(d, FFAUDIO_DEV_NAME);
				const char *is_default = audio->dev_info(d, FFAUDIO_DEV_IS_DEFAULT);

				int is_match = 0;
				if (target_name == NULL && is_default != NULL) {
					is_match = 1;
				} else if (target_name != NULL && name != NULL) {
					ffsize name_len = strlen(name);
					ffsize min_len = (name_len < target_len) ? name_len : target_len;
					if (strncmp(name, target_name, min_len) == 0) {
						is_match = 1;
					}
				}

				if (is_match) {
					found = 1;
					printf("  Device: %s%s\n", name, is_default ? " (DEFAULT)" : "");

					const ffuint *fmt = (const ffuint *)audio->dev_info(d, FFAUDIO_DEV_MIX_FORMAT);
					if (fmt != NULL) {
						ffuint format = fmt[0];
						ffuint sample_rate = fmt[1];
						ffuint channels = fmt[2];

						const char *fmt_name = "UNKNOWN";
						if (format == FFAUDIO_F_INT16) fmt_name = "INT16";
						else if (format == FFAUDIO_F_INT32) fmt_name = "INT32";
						else if (format == FFAUDIO_F_FLOAT32) fmt_name = "FLOAT32";

						printf("  Format: %s / %u Hz / %u channels\n", fmt_name, sample_rate, channels);

						if (channels != 2) {
							printf("  *** ERROR: Must be 2 channels (stereo), found %u ***\n", channels);
							printf("  FIX: Windows Sound Settings -> Playback -> %s\n", name);
							printf("       -> Properties -> Advanced -> Set to 2 channel, 48000 Hz\n");
							errors |= 8;
						} else {
							printf("  Channels: OK (stereo)\n");
						}

						if (sample_rate != 48000) {
							printf("  *** WARNING: Sample rate %u Hz, recommended 48000 Hz ***\n", sample_rate);
						} else {
							printf("  Sample rate: OK (48000 Hz)\n");
						}
					}
					break;
				}
			}

			if (!found) {
				printf("  ERROR: Device '%s' not found\n", target_name ? target_name : "(default)");
				errors |= 2;
			}
			audio->dev_free(d);
		}
	}

	printf("\n========================================================================\n");
	if (errors == 0) {
		printf("  AUDIO CONFIGURATION: OK\n");
	} else {
		printf("  AUDIO CONFIGURATION: ERRORS FOUND (code %d)\n", errors);
		printf("\n");
		printf("  Common issues with VB-Cable or virtual audio:\n");
		printf("  1. Device must be set to STEREO (2 channels) in Windows Sound settings\n");
		printf("  2. Both Input and Output should use same sample rate (48000 Hz)\n");
		printf("  3. Use WASAPI audio system (-x wasapi) for virtual cables\n");
	}
	printf("========================================================================\n\n");

	audio->uninit();
	return errors;
}
#endif


void *radio_playback_thread(void *device_ptr)
{
    ffaudio_interface *audio;
	int device_is_mono = 0;  // Will be set after device opens
	struct conf conf = {};
	conf.buf.app_name = "mercury_playback";
	conf.buf.format = FFAUDIO_F_INT32;
	conf.buf.sample_rate = 48000;
	conf.buf.channels = 2;
	conf.buf.device_id = (const char *) device_ptr;
	uint32_t period_ms;
	uint32_t period_bytes;

#if defined(_WIN32)
	void *dsound_guid = NULL;  // For DirectSound GUID allocated memory
	void *wasapi_id = NULL;    // For WASAPI device ID allocated memory
#endif

#if defined(_WIN32)
    conf.buf.buffer_length_msec = 40;
	period_ms = conf.buf.buffer_length_msec / 4;
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI) {
        audio = (ffaudio_interface *) &ffwasapi;
		// Convert device name to wide-string ID for WASAPI
		if (device_ptr != NULL) {
			wasapi_id = wasapi_device_name_to_id((const char*)device_ptr, FFAUDIO_DEV_PLAYBACK);
			if (wasapi_id != NULL) {
				conf.buf.device_id = (const char*)wasapi_id;
			} else {
				printf("Warning: WASAPI device '%s' not found, using default\n", (const char*)device_ptr);
				conf.buf.device_id = NULL;  // Use default device
			}
		}
	}
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND) {
        audio = (ffaudio_interface *) &ffdsound;
		// DirectSound: Keep INT32 format (DirectSound handles conversion to device format)
		// conf.buf.format = FFAUDIO_F_INT16;  // Disabled - using INT32 default
		// Convert device name to GUID for DirectSound
		if (device_ptr != NULL) {
			dsound_guid = dsound_device_name_to_guid((const char*)device_ptr, FFAUDIO_DEV_PLAYBACK);
			if (dsound_guid != NULL) {
				conf.buf.device_id = (const char*)dsound_guid;
			} else {
				printf("Warning: DirectSound device '%s' not found, using default\n", (const char*)device_ptr);
				conf.buf.device_id = NULL;  // Use default device
			}
		}
	}
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

	uint8_t *buffer = (uint8_t *) malloc(AUDIO_PAYLOAD_BUFFER_SIZE * sizeof(double) * 2);
	double *buffer_double =  (double *) buffer;
	int32_t *buffer_internal_stereo = (int32_t *) malloc(AUDIO_PAYLOAD_BUFFER_SIZE * sizeof(int32_t) * 2); // a big enough buffer

	ffuint total_written = 0;
	int ch_layout = STEREO;

#if ENABLE_FLOAT64_TAP == 1
	FILE *tap_pay = fopen("tap-playback.f64", "w");
#endif

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

	printf("I/O playback (%s) format=%d (%s) / %dHz / %dch / %dms buffer\n",
		conf.buf.device_id ? conf.buf.device_id : "default",
		cfg->format,
		(cfg->format == FFAUDIO_F_INT16) ? "INT16" : (cfg->format == FFAUDIO_F_INT32) ? "INT32" : (cfg->format == FFAUDIO_F_FLOAT32) ? "FLOAT32" : "UNKNOWN",
		cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);
	fflush(stdout);


	frame_size = cfg->channels * (cfg->format & 0xff) / 8;
	msec_bytes = cfg->sample_rate * frame_size / 1000;

	// Use configured output channel from settings
	if (configured_output_channel == 0)
		ch_layout = LEFT;
	else if (configured_output_channel == 1)
		ch_layout = RIGHT;
	else
		ch_layout = STEREO;
	// Set mono flag based on actual device channels
	device_is_mono = (cfg->channels == 1);

    while (!shutdown_)
    {
		ffssize n;
		size_t buffer_size = size_buffer(playback_buffer);
		if (buffer_size >= period_bytes)
		{
			read_buffer(playback_buffer, buffer, period_bytes);
			n = period_bytes;
		}
		else
		{
			// we just play zeros if there is nothing to play
			memset(buffer, 0, period_bytes);
			if (buffer_size > frame_size)
				read_buffer(playback_buffer, buffer, buffer_size);
			n = period_bytes;
		}

#if ENABLE_FLOAT64_TAP == 1
		fwrite(buffer, 1, n, tap);
#endif

        total_written = 0;

		int samples_read = n / sizeof(double);

#ifdef MERCURY_GUI_ENABLED
		// Check if tune mode is active - generate 1500 Hz sine wave
		if (g_gui_state.tune_active.load()) {
			for (int i = 0; i < samples_read; i++) {
				buffer_double[i] = gui_generate_tune_tone(48000, tune_sample_index);
			}
		}

		// Apply TX gain from GUI
		gui_apply_tx_gain(buffer_double, samples_read);
#endif

		// convert from double to format-specific output
		// Check if format is FLOAT32 (WASAPI), INT32 (DirectSound/ALSA), or INT16 (DirectSound 16-bit)
		int is_float32 = (cfg->format == FFAUDIO_F_FLOAT32);
		int is_int16 = (cfg->format == FFAUDIO_F_INT16);
		float *buffer_float_out = (float*)buffer_internal_stereo;
		int16_t *buffer_int16_out = (int16_t*)buffer_internal_stereo;

		for (int i = 0; i < samples_read; i++)
		{
			// Clamp to [-1.0, 1.0]
			double clamped = buffer_double[i];
			if (clamped > 1.0) clamped = 1.0;
			if (clamped < -1.0) clamped = -1.0;

			int idx = i * cfg->channels;

			// Handle mono device - just write single channel
			if (device_is_mono)
			{
				if (is_float32) {
					buffer_float_out[i] = (float)clamped;
				} else if (is_int16) {
					buffer_int16_out[i] = (int16_t)(clamped * 32767.0);
				} else {
					buffer_internal_stereo[i] = clamped * INT_MAX;
				}
			}
			else if (ch_layout == LEFT)
			{
				if (is_float32) {
					buffer_float_out[idx] = (float)clamped;
					buffer_float_out[idx + 1] = 0.0f;
				} else if (is_int16) {
					buffer_int16_out[idx] = (int16_t)(clamped * 32767.0);
					buffer_int16_out[idx + 1] = 0;
				} else {
					buffer_internal_stereo[idx] = clamped * INT_MAX;
					buffer_internal_stereo[idx + 1] = 0;
				}
			}
			else if (ch_layout == RIGHT)
			{
				if (is_float32) {
					buffer_float_out[idx] = 0.0f;
					buffer_float_out[idx + 1] = (float)clamped;
				} else if (is_int16) {
					buffer_int16_out[idx] = 0;
					buffer_int16_out[idx + 1] = (int16_t)(clamped * 32767.0);
				} else {
					buffer_internal_stereo[idx] = 0;
					buffer_internal_stereo[idx + 1] = clamped * INT_MAX;
				}
			}
			else // STEREO
			{
				if (is_float32) {
					buffer_float_out[idx] = (float)clamped;
					buffer_float_out[idx + 1] = (float)clamped;
				} else if (is_int16) {
					buffer_int16_out[idx] = (int16_t)(clamped * 32767.0);
					buffer_int16_out[idx + 1] = buffer_int16_out[idx];
				} else {
					buffer_internal_stereo[idx] = clamped * INT_MAX;
					buffer_internal_stereo[idx + 1] = buffer_internal_stereo[idx];
				}
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

#if ENABLE_FLOAT64_TAP == 1
	fclose(tap);
#endif

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

#if defined(_WIN32)
	// Free DirectSound GUID if allocated
	if (dsound_guid != NULL)
		free(dsound_guid);
	// Free WASAPI device ID if allocated
	if (wasapi_id != NULL)
		free(wasapi_id);
#endif

	printf("radio_playback_thread exit\n");

    shutdown_ = true;

    return NULL;
}


void *radio_capture_thread(void *device_ptr)
{
    ffaudio_interface *audio;
	int device_is_mono = 0;  // Will be set after device opens
	struct conf conf = {};
	conf.buf.app_name = "mercury_capture";
	conf.buf.format = FFAUDIO_F_INT32;
	conf.buf.sample_rate = 48000;
	conf.buf.channels = 2;
	conf.buf.device_id = (const char *) device_ptr;

#if defined(_WIN32)
	void *dsound_guid = NULL;  // For DirectSound GUID allocated memory
	void *wasapi_id = NULL;    // For WASAPI device ID allocated memory
#endif

#if defined(_WIN32)
    conf.buf.buffer_length_msec = 40;
    if (audio_subsystem == AUDIO_SUBSYSTEM_WASAPI) {
        audio = (ffaudio_interface *) &ffwasapi;
		// Convert device name to wide-string ID for WASAPI
		if (device_ptr != NULL) {
			wasapi_id = wasapi_device_name_to_id((const char*)device_ptr, FFAUDIO_DEV_CAPTURE);
			if (wasapi_id != NULL) {
				conf.buf.device_id = (const char*)wasapi_id;
			} else {
				printf("Warning: WASAPI device '%s' not found, using default\n", (const char*)device_ptr);
				conf.buf.device_id = NULL;  // Use default device
			}
		}
		fflush(stdout);
	}
    if (audio_subsystem == AUDIO_SUBSYSTEM_DSOUND) {
        audio = (ffaudio_interface *) &ffdsound;
		// DirectSound: Keep INT32 format (DirectSound handles conversion to device format)
		// conf.buf.format = FFAUDIO_F_INT16;  // Disabled - using INT32 default
		// Convert device name to GUID for DirectSound
		if (device_ptr != NULL) {
			dsound_guid = dsound_device_name_to_guid((const char*)device_ptr, FFAUDIO_DEV_CAPTURE);
			if (dsound_guid != NULL) {
				conf.buf.device_id = (const char*)dsound_guid;
				printf("[CAPTURE INIT] Found device GUID, using specific device\n");
			} else {
				printf("[CAPTURE INIT] Warning: DirectSound device '%s' not found, using default\n", (const char*)device_ptr);
				conf.buf.device_id = NULL;  // Use default device
			}
		} else {
			printf("[CAPTURE INIT] No device specified, using default\n");
		}
		fflush(stdout);
	}
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

	double *buffer_internal = NULL;

#if ENABLE_FLOAT64_TAP == 1
	FILE *tap = fopen("tap-capture.f64", "w");
#endif

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

	printf("I/O capture (%s) format=%d (%s) / %dHz / %dch / %dms buffer\n",
		conf.buf.device_id ? conf.buf.device_id : "default",
		cfg->format,
		(cfg->format == FFAUDIO_F_INT16) ? "INT16" : (cfg->format == FFAUDIO_F_INT32) ? "INT32" : (cfg->format == FFAUDIO_F_FLOAT32) ? "FLOAT32" : "UNKNOWN",
		cfg->sample_rate, cfg->channels, cfg->buffer_length_msec);
	fflush(stdout);

    frame_size = cfg->channels * (cfg->format & 0xff) / 8;
    msec_bytes = cfg->sample_rate * frame_size / 1000;

	buffer_internal = (double *) malloc(AUDIO_PAYLOAD_BUFFER_SIZE * sizeof(double) * 2);

	// Use configured input channel from settings
	if (configured_input_channel == 0)
		ch_layout = LEFT;
	else if (configured_input_channel == 1)
		ch_layout = RIGHT;
	else
		ch_layout = STEREO;

	// Detect if device is mono - override ch_layout to work with single channel
	device_is_mono = (cfg->channels == 1);

	static int read_loop_counter = 0;
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

		// Check format: FLOAT32 (WASAPI), INT32 (DirectSound/ALSA), or INT16 (DirectSound with 16-bit)
		int is_float32 = (cfg->format == FFAUDIO_F_FLOAT32);
		int is_int16 = (cfg->format == FFAUDIO_F_INT16);
		float *buffer_float = (float*)buffer;  // For FLOAT32 interpretation
		int16_t *buffer_int16 = (int16_t*)buffer;  // For INT16 interpretation

		for (int i = 0; i < frames_to_write; i++)
		{
			// Handle mono device - just read single channel directly
			if (device_is_mono)
			{
				if (is_float32)
					buffer_internal[i] = (double) buffer_float[i];
				else if (is_int16)
					buffer_internal[i] = (double) buffer_int16[i] / 32768.0;
				else
					buffer_internal[i] = (double) buffer[i] / (double) INT_MAX;
			}
			else if (ch_layout == LEFT)
			{
				if (is_float32)
					buffer_internal[i] = (double) buffer_float[i*2];
				else if (is_int16)
					buffer_internal[i] = (double) buffer_int16[i*2] / 32768.0;
				else
					buffer_internal[i] = (double) buffer[i*2] / (double) INT_MAX;
			}
			else if (ch_layout == RIGHT)
			{
				if (is_float32)
					buffer_internal[i] = (double) buffer_float[i*2 + 1];
				else if (is_int16)
					buffer_internal[i] = (double) buffer_int16[i*2 + 1] / 32768.0;
				else
					buffer_internal[i] = (double) buffer[i*2 + 1] / (double) INT_MAX;
			}
			else // STEREO - average both channels
			{
				if (is_float32)
					buffer_internal[i] = (double) ((buffer_float[i*2] + buffer_float[i*2 + 1]) / 2.0);
				else if (is_int16)
					buffer_internal[i] = (double) ((buffer_int16[i*2] + buffer_int16[i*2 + 1]) / 2.0) / 32768.0;
				else
					buffer_internal[i] = (double) ((buffer[i*2] + buffer[i*2 + 1]) / 2.0) / (double) INT_MAX;
			}

		}

#ifdef MERCURY_GUI_ENABLED
		// Apply RX gain as preprocessing step (affects Mercury's core too)
		gui_apply_rx_gain_for_display(buffer_internal, frames_to_write);

		// Push to VU meter and waterfall
		gui_push_audio_samples(buffer_internal, frames_to_write);
#endif

#if ENABLE_FLOAT64_TAP == 1
		fwrite(buffer_internal, 1, frames_to_write * sizeof(double), tap);
#endif

		// Write (possibly gained) samples to capture_buffer for Mercury's core
		if (circular_buf_free_size(capture_buffer) >= frames_to_write * sizeof(double))
			write_buffer(capture_buffer, (uint8_t *)buffer_internal, frames_to_write * sizeof(double));
		else
			printf("Buffer full in capture buffer!\n");
	}

	r = audio->stop(b);
	if (r != 0)
		printf("ffaudio.stop: %s", audio->error(b));

	r = audio->clear(b);
	if (r != 0)
		printf("ffaudio.clear: %s", audio->error(b));

	free(buffer_internal);

#if ENABLE_FLOAT64_TAP == 1
	fclose(tap);
#endif


cleanup_cap:

	audio->free(b);

    audio->uninit();

finish_cap:

#if defined(_WIN32)
	// Free DirectSound GUID if allocated
	if (dsound_guid != NULL)
		free(dsound_guid);
	// Free WASAPI device ID if allocated
	if (wasapi_id != NULL)
		free(wasapi_id);
#endif

	printf("radio_capture_thread exit\n");

    shutdown_ = true;

    return NULL;
}

void *radio_capture_prep_thread(void *telecom_ptr_void)
{
	cl_telecom_system *telecom_ptr = (cl_telecom_system *) telecom_ptr_void;

	double *buffer_temp = (double *) malloc(AUDIO_PAYLOAD_BUFFER_SIZE * sizeof(double) * 2);

	while (!shutdown_)
    {
		cl_data_container *data_container_ptr = &telecom_ptr->data_container;
		int signal_period = data_container_ptr->Nofdm * data_container_ptr->buffer_Nsymb * data_container_ptr->interpolation_rate; // in samples
		int symbol_period = data_container_ptr->Nofdm * data_container_ptr->interpolation_rate;
		int location_of_last_frame = signal_period - symbol_period - 1; // TODO: do we need this "-1"?

		if (symbol_period == 0) {
			continue;
		}

		rx_transfer(buffer_temp, symbol_period);

		MUTEX_LOCK(&capture_prep_mutex);
		if(data_container_ptr->data_ready == 1)
			data_container_ptr->nUnder_processing_events++;

		shift_left(data_container_ptr->passband_delayed_data, signal_period, symbol_period);


		memcpy(&data_container_ptr->passband_delayed_data[location_of_last_frame], buffer_temp, symbol_period * sizeof(double));

		data_container_ptr->frames_to_read--;
		if(data_container_ptr->frames_to_read < 0)
			data_container_ptr->frames_to_read = 0;

		data_container_ptr->data_ready = 1;
		MUTEX_UNLOCK(&capture_prep_mutex);
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

#if ENABLE_FLOAT64_TAP_BEFORE == 1
	fwrite(buffer_internal, 1, buffer_size_bytes, tap_play);
#endif

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


int audioio_init_internal(char *capture_dev, char *playback_dev, int audio_subsys, pthread_t *radio_capture,
						  pthread_t *radio_playback, pthread_t *radio_capture_prep, cl_telecom_system *telecom_system)
{
    audio_subsystem = audio_subsys;

#if ENABLE_FLOAT64_TAP_BEFORE == 1
	tap_play = fopen("tap-playback-b.f64", "w");
#endif

#if defined(_WIN32)
	uint8_t *buffer_cap = (uint8_t *)malloc(AUDIO_PAYLOAD_BUFFER_SIZE);
	uint8_t *buffer_play = (uint8_t *)malloc(AUDIO_PAYLOAD_BUFFER_SIZE);
    capture_buffer = circular_buf_init(buffer_cap, AUDIO_PAYLOAD_BUFFER_SIZE);
    playback_buffer = circular_buf_init(buffer_play, AUDIO_PAYLOAD_BUFFER_SIZE);
#else
    capture_buffer = circular_buf_init_shm(AUDIO_PAYLOAD_BUFFER_SIZE, (char *) AUDIO_CAPT_PAYLOAD_NAME);
    playback_buffer = circular_buf_init_shm(AUDIO_PAYLOAD_BUFFER_SIZE, (char *) AUDIO_PLAY_PAYLOAD_NAME);
#endif

	clear_buffer(capture_buffer);
	clear_buffer(playback_buffer);

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

#if ENABLE_FLOAT64_TAP_BEFORE == 1
	fclose(tap_play);
#endif

#if defined(_WIN32)
	free(capture_buffer->buffer);
	circular_buf_free(capture_buffer);
	free(playback_buffer->buffer);
	circular_buf_free(playback_buffer);
#else
    circular_buf_destroy_shm(capture_buffer, AUDIO_PAYLOAD_BUFFER_SIZE, (char *) AUDIO_CAPT_PAYLOAD_NAME);
    circular_buf_free_shm(capture_buffer);

    circular_buf_destroy_shm(playback_buffer, AUDIO_PAYLOAD_BUFFER_SIZE, (char *) AUDIO_PLAY_PAYLOAD_NAME);
    circular_buf_free_shm(playback_buffer);
#endif
    return 0;
}
