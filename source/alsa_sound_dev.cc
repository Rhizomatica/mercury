/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022 Fadi Jerji
 * Author: Fadi Jerji
 * Email: fadi.jerji@  <gmail.com, rhizomatica.org, caisresearch.com, ieee.org>
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
#include "alsa_sound_dev.h"


cl_alsa_sound_device::cl_alsa_sound_device()
{
	dev_name="default";
	baudrate=44100;
	channels=STEREO;
	_dev_ptr=NULL;
	_buffer_ptr=NULL;
	data_container_ptr=NULL;
	_error=0;
	nbuffer_Samples=baudrate*2;
	_buffer_size=NOT_DEFINED;
	frames_per_period=NOT_DEFINED;
	frames_to_leave_transmit_fct=600;
	callback_ptr=NULL;
	type=PLAY;
}


cl_alsa_sound_device::~cl_alsa_sound_device()
{
	if(_dev_ptr!=NULL)
	{
		if(type==PLAY)
		{
			snd_pcm_drain(_dev_ptr);
		}
		snd_pcm_close (_dev_ptr);
	}

	if(_buffer_ptr!=NULL)
	{
		delete _buffer_ptr;
	}
}


int cl_alsa_sound_device::init()
{
	int _success=SUCCESS;

	if(frames_per_period==NOT_DEFINED)
	{
		frames_per_period=baudrate;
	}
	if(this->_init()<0)
	{
		_success=NOT_SUCCESS;
	}

	_success=NOT_SUCCESS;
	while(nbuffer_Samples>=1)
	{
		if(nbuffer_Samples<_buffer_size)
		{
			_buffer_ptr= new double[nbuffer_Samples];
			_success=SUCCESS;
			break;
		}
		else
		{
			nbuffer_Samples/=2;
		}
	}
	if(_buffer_ptr==NULL)
	{
		_success=NOT_SUCCESS;
	}
	if(type==CAPTURE)
	{
		if(snd_pcm_start(_dev_ptr)<0)
		{
			_success=NOT_SUCCESS;
		}
	}

	return _success;
}

snd_pcm_uframes_t cl_alsa_sound_device::get_buffer_size()
{
	return _buffer_size;
}


std::string cl_alsa_sound_device::get_erro_message()
{
	return snd_strerror(_error);
}

int cl_alsa_sound_device::_init()
{
	int _success=SUCCESS;
	snd_pcm_hw_params_t *configuration_parameters;
	if(type==PLAY)
	{
		_error = snd_pcm_open(&_dev_ptr, dev_name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	}
	else if (type==CAPTURE)
	{
		_error = snd_pcm_open(&_dev_ptr, dev_name.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
	}
	if(_error<0)
	{
		return NOT_SUCCESS;
	}
	_error = snd_pcm_hw_params_malloc (&configuration_parameters);
	if(_error<0)
	{
		return NOT_SUCCESS;
	}
	_error = snd_pcm_hw_params_any (_dev_ptr, configuration_parameters);
	if(_error<0)
	{
		return NOT_SUCCESS;
	}
	_error = snd_pcm_hw_params_set_rate_resample(_dev_ptr, configuration_parameters, 1);
	if(_error<0)
	{
		return NOT_SUCCESS;
	}
	_error = snd_pcm_hw_params_set_access (_dev_ptr, configuration_parameters, SND_PCM_ACCESS_RW_INTERLEAVED);
	if(_error<0)
	{
		return NOT_SUCCESS;
	}
	_error = snd_pcm_hw_params_set_format (_dev_ptr, configuration_parameters, SND_PCM_FORMAT_FLOAT64_LE);
	if(_error<0)
	{
		return NOT_SUCCESS;
	}
	_error = snd_pcm_hw_params_set_channels (_dev_ptr, configuration_parameters, 2);
	if(_error<0)
	{
		return NOT_SUCCESS;
	}
	_error = snd_pcm_hw_params_set_rate_near (_dev_ptr, configuration_parameters, &baudrate, 0);
	if(_error<0)
	{
		return NOT_SUCCESS;
	}
	_error=snd_pcm_hw_params_set_period_size_near(_dev_ptr,configuration_parameters, &frames_per_period, 0);
	if(_error<0)
	{
		return NOT_SUCCESS;
	}
	_error = snd_pcm_hw_params (_dev_ptr, configuration_parameters);
	if(_error<0)
	{
		return NOT_SUCCESS;
	}
	snd_pcm_hw_params_get_buffer_size( configuration_parameters, &_buffer_size);
	snd_pcm_hw_params_get_period_size(configuration_parameters, &frames_per_period,0);

	if (type==CAPTURE)
	{
		snd_async_add_pcm_handler(&callback_ptr, _dev_ptr, interrupt_handler, data_container_ptr);
	}

	snd_pcm_hw_params_free (configuration_parameters);

	_error = snd_pcm_prepare (_dev_ptr);
	if(_error<0)
	{
		return NOT_SUCCESS;
	}
	return _success;
}

snd_pcm_sframes_t cl_alsa_sound_device::transfere(double* buffer, unsigned int size)
{
	snd_pcm_sframes_t frames_transfered=0;
	int nChunks=0;
	if(size<=nbuffer_Samples/2)
	{
		frames_transfered=_transfere(buffer,size);
	}
	else
	{
		if((size%(nbuffer_Samples/2))==0)
		{
			nChunks=size/(nbuffer_Samples/2);
			for(int i=0;i<nChunks;i++)
			{
				frames_transfered+=_transfere(&buffer[i*(nbuffer_Samples/2)],nbuffer_Samples/2);
			}
		}
		else
		{
			nChunks=size/(nbuffer_Samples/2)-1;
			for(int i=0;i<nChunks;i++)
			{
				frames_transfered+=_transfere(&buffer[i*(nbuffer_Samples/2)],nbuffer_Samples/2);
			}

			frames_transfered+=_transfere(&buffer[nChunks*(nbuffer_Samples/2)],nbuffer_Samples/2+(size%(nbuffer_Samples/2)));
		}
	}
	return frames_transfered;
}

snd_pcm_sframes_t cl_alsa_sound_device::_transfere(double* buffer, unsigned int size)
{
	snd_pcm_sframes_t frames=0;
	snd_pcm_sframes_t frames_transfered=0;
	snd_pcm_uframes_t frames_avail;
	if(type==PLAY)
	{
		if(size<=nbuffer_Samples/2)
		{
			for(unsigned int j=0;j<size*2;j++)
			{
				*(_buffer_ptr+j)= 0;
			}
			if(channels==LEFT || channels==MONO)
			{
				for(unsigned int j=0;j<size;j++)
				{
					*(_buffer_ptr+j*2)= *(buffer+j);
				}
			}
			if(channels==RIGHT || channels==MONO)
			{
				for(unsigned int j=0;j<size;j++)
				{
					*(_buffer_ptr+j*2+1)= *(buffer+j);
				}
			}

			frames=snd_pcm_writei(_dev_ptr, _buffer_ptr, size);

			if(frames<=0 || snd_pcm_avail(_dev_ptr)<0)
			{
				snd_pcm_prepare(_dev_ptr);

				frames=snd_pcm_writei(_dev_ptr, _buffer_ptr, size);

				if(frames>0 && snd_pcm_avail(_dev_ptr)<(snd_pcm_sframes_t)_buffer_size)
				{
					frames_transfered+=frames;
				}

			}
			else
			{
				frames_transfered+=frames;
			}

			do
			{
				frames_avail=snd_pcm_avail(_dev_ptr);
				usleep(1000);
			}
			while(frames_avail<_buffer_size-frames_to_leave_transmit_fct);
		}
	}
	else if (type==CAPTURE)
	{
		if(size<=nbuffer_Samples/2)
			{
				frames=snd_pcm_readi(_dev_ptr, _buffer_ptr, size);

				if(frames<=0 || snd_pcm_avail(_dev_ptr)<0)
				{
					snd_pcm_prepare(_dev_ptr);
					snd_pcm_start(_dev_ptr);
				}
				else
				{
					frames_transfered+=frames;

					if(channels==LEFT || channels==MONO)
					{
						for(unsigned int j=0;j<size;j++)
						{
							*(buffer+j)=*(_buffer_ptr+j*2);
						}
					}
					if(channels==RIGHT)
					{
						for(unsigned int j=0;j<size;j++)
						{
							*(buffer+j)=*(_buffer_ptr+j*2+1);
						}
					}
				}

			}

	}
	return frames_transfered;
}

snd_pcm_sframes_t cl_alsa_sound_device::transfere(double* buffer_left, double* buffer_right, unsigned int size)
{
	snd_pcm_sframes_t frames=0;
	snd_pcm_sframes_t frames_transfered=0;
	snd_pcm_uframes_t frames_avail;
	if(type==PLAY)
	{
		do
		{
			frames_avail=snd_pcm_avail(_dev_ptr);
		}
		while(frames_avail<_buffer_size-frames_to_leave_transmit_fct);

		if(size<=nbuffer_Samples/2)
		{
			for(unsigned int j=0;j<size;j++)
			{
				*(_buffer_ptr+j*2)= *(buffer_left+j);
			}
			for(unsigned int j=0;j<size;j++)
			{
				*(_buffer_ptr+j*2+1)= *(buffer_right+j);
			}
			frames=snd_pcm_writei(_dev_ptr, _buffer_ptr, size);

			if(frames<=0 || snd_pcm_avail(_dev_ptr)<0)
			{
				snd_pcm_prepare(_dev_ptr);

				frames=snd_pcm_writei(_dev_ptr, _buffer_ptr, size);

				if(frames>0 && snd_pcm_avail(_dev_ptr)<(snd_pcm_sframes_t)_buffer_size)
				{
					frames_transfered+=frames;
				}

			}
			else
			{
				frames_transfered+=frames;
			}
		}
	}
	else if (type==CAPTURE)
	{
		if(size<=nbuffer_Samples/2)
			{
				frames=snd_pcm_readi(_dev_ptr, _buffer_ptr, size);

				if(frames<=0 || snd_pcm_avail(_dev_ptr)<0)
				{
					snd_pcm_prepare(_dev_ptr);
					snd_pcm_start(_dev_ptr);
				}
				else
				{
					frames_transfered+=frames;
					for(unsigned int j=0;j<size;j++)
					{
						*(buffer_left+j)=*(_buffer_ptr+j*2);
					}
					for(unsigned int j=0;j<size;j++)
					{
						*(buffer_right+j)=*(_buffer_ptr+j*2+1);
					}
				}
			}
	}
	return frames_transfered;
}

snd_pcm_sframes_t cl_alsa_sound_device::get_available_frames()
{
	return snd_pcm_avail(_dev_ptr);
}


void interrupt_handler(snd_async_handler_t *callback_ptr)
{
	cl_data_container * data_container_ptr = (cl_data_container*) snd_async_handler_get_callback_private(callback_ptr);
	cl_alsa_sound_device * sound_device_ptr= (cl_alsa_sound_device*) data_container_ptr->sound_device_ptr;

	int location_of_last_frame=data_container_ptr->Nofdm*data_container_ptr->interpolation_rate*(data_container_ptr->buffer_Nsymb)-data_container_ptr->Nofdm*data_container_ptr->interpolation_rate-1;

	sound_device_ptr->transfere(&data_container_ptr->passband_delayed_data[location_of_last_frame],data_container_ptr->Nofdm*data_container_ptr->interpolation_rate);
	data_container_ptr->frames_to_read--;
	if(data_container_ptr->frames_to_read>0)
	{
		shift_left(data_container_ptr->passband_delayed_data, data_container_ptr->Nofdm*data_container_ptr->interpolation_rate*data_container_ptr->buffer_Nsymb, data_container_ptr->Nofdm*data_container_ptr->interpolation_rate);
	}
	else
	{
		data_container_ptr->frames_to_read=0;
	}
	data_container_ptr->data_ready=1;

}
