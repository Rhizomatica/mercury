/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 * Author: Fadi Jerji
 * Email: fadi.jerji@  <gmail.com, caisresearch.com, ieee.org>
 * ORCID: 0000-0002-2076-5831
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef INC_ALSA_SOUND_DEV_H_
#define INC_ALSA_SOUND_DEV_H_

#include <alsa/asoundlib.h>
#include <string>
#include "data_container.h"
#include <iostream>

#define PLAY 0
#define CAPTURE 1

#define MONO 0
#define STEREO 1
#define LEFT 2
#define RIGHT 3

#define SUCCESS 0
#define NOT_SUCCESS -1

#define NOT_DEFINED 0

void interrupt_handler(snd_async_handler_t *callback_ptr);

class cl_alsa_sound_device
{

public:
	cl_alsa_sound_device();
	~cl_alsa_sound_device();
	unsigned int baudrate;
	std::string dev_name;
	unsigned int channels;
	unsigned int nbuffer_Samples;
	snd_pcm_uframes_t frames_per_period;
	snd_pcm_uframes_t frames_to_leave_transmit_fct;
	int type;


	int init();
	void deinit();
	std::string get_erro_message();
	snd_pcm_sframes_t transfere(double* buffer, unsigned int size);
	snd_pcm_sframes_t _transfere(double* buffer, unsigned int size);
	snd_pcm_sframes_t transfere(double* buffer_left, double* buffer_right, unsigned int size);
	snd_pcm_uframes_t get_buffer_size();
	snd_pcm_sframes_t get_available_frames();
	void* data_container_ptr;

private:
	_snd_pcm * _dev_ptr;
	int _error;
	double * _buffer_ptr;
	snd_pcm_uframes_t _buffer_size;
	int _init();
	snd_async_handler_t *callback_ptr;


};

#endif
