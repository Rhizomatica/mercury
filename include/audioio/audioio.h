/* Audio subsystem
 *
 * Copyright (C) 2024 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#pragma once

#include <ffbase/string.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "physical_layer/telecom_system.h"
#include "common/ring_buffer_posix.h"

#define AUDIO_SUBSYSTEM_ALSA 0
#define AUDIO_SUBSYSTEM_PULSE 1
#define AUDIO_SUBSYSTEM_WASAPI 2
#define AUDIO_SUBSYSTEM_DSOUND 3
#define AUDIO_SUBSYSTEM_COREAUDIO 4
#define AUDIO_SUBSYSTEM_OSS 5
#define AUDIO_SUBSYSTEM_AAUDIO 6

#define AUDIO_CAPT_PAYLOAD_NAME "/audio-capt"
#define AUDIO_PLAY_PAYLOAD_NAME "/audio-play"

#define AUDIO_PAYLOAD_BUFFER_SIZE 1536000

#define LEFT 0
#define RIGHT 1
#define STEREO 2

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

int audioio_init_internal(char *capture_dev, char *playback_dev, int audio_subsys, pthread_t *radio_capture,
						  pthread_t *radio_playback, pthread_t *radio_capture_prep, cl_telecom_system *telecom_system);

int audioio_deinit(pthread_t *radio_capture, pthread_t *radio_playback, pthread_t *radio_capture_prep);

int tx_transfer(double *buffer, size_t len);
int rx_transfer(double *buffer, size_t len);


void list_soundcards(int audio_system);
