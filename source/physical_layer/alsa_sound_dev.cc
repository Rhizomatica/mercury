/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 * Copyright (C) 2024 Rafael Diniz
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

#include "physical_layer/alsa_sound_dev.h"

void show_alsa(snd_pcm_t *handle, snd_pcm_hw_params_t *params)
{
    unsigned int val, val2;
    int dir = 0;
    snd_pcm_uframes_t frames;

    /* Display information about the PCM interface */

    printf("PCM handle name = '%s'\n", snd_pcm_name(handle));

    printf("PCM state = %s\n", snd_pcm_state_name(snd_pcm_state(handle)));

    snd_pcm_hw_params_get_access(params, (snd_pcm_access_t *) &val);
    printf("access type = %s\n",snd_pcm_access_name((snd_pcm_access_t)val));

    snd_pcm_hw_params_get_format(params, (snd_pcm_format_t *)&val);
    printf("format = '%s' (%s)\n",
           snd_pcm_format_name((snd_pcm_format_t)val),
           snd_pcm_format_description(
               (snd_pcm_format_t)val));

    snd_pcm_hw_params_get_subformat(params, (snd_pcm_subformat_t *)&val);
    printf("subformat = '%s' (%s)\n",
           snd_pcm_subformat_name((snd_pcm_subformat_t)val),
           snd_pcm_subformat_description(
               (snd_pcm_subformat_t)val));

    snd_pcm_hw_params_get_channels(params, &val);
    printf("channels = %d\n", val);

    snd_pcm_hw_params_get_rate(params, &val, &dir);
    printf("rate = %d Hz\n", val);

    snd_pcm_hw_params_get_periods(params, &val, &dir);
    printf("periods per buffer = %d frames\n", val);

    snd_pcm_hw_params_get_period_size(params, &frames, &dir);
    printf("period size = %d frames\n", (int)frames);

    snd_pcm_hw_params_get_buffer_size(params, (snd_pcm_uframes_t *) &val);
    printf("buffer size = %d frames\n", val);

    snd_pcm_hw_params_get_period_time(params, &val, &dir);
    printf("period time = %d us\n", val);

    snd_pcm_hw_params_get_buffer_time(params, &val, &dir);
    printf("buffer time = %d us\n", val);

    snd_pcm_hw_params_get_rate_numden(params, &val, &val2);
    printf("exact rate = %d/%d Hz\n", val, val2);

    val = snd_pcm_hw_params_get_sbits(params);
    printf("significant bits = %d\n", val);

    val = snd_pcm_hw_params_is_batch(params);
    printf("is batch double buffering = %d\n", val);

    val = snd_pcm_hw_params_is_block_transfer(params);
    printf("is block transfer = %d\n", val);

    val = snd_pcm_hw_params_is_double(params);
    printf("is double buffered = %d\n", val);

    val = snd_pcm_hw_params_is_half_duplex(params);
    printf("is half duplex = %d\n", val);

    val = snd_pcm_hw_params_is_joint_duplex(params);
    printf("is joint duplex = %d\n", val);

    val = snd_pcm_hw_params_can_overrange(params);
    printf("can overrange = %d\n", val);

    val = snd_pcm_hw_params_can_mmap_sample_resolution(params);
    printf("can mmap = %d\n", val);

    val = snd_pcm_hw_params_can_pause(params);
    printf("can pause = %d\n", val);

    val = snd_pcm_hw_params_can_resume(params);
    printf("can resume = %d\n", val);

    val = snd_pcm_hw_params_can_sync_start(params);
    printf("can sync start = %d\n", val);

    val = snd_pcm_hw_params_get_fifo_size(params);
    printf("fifo size = %d\n", val);

    val = snd_pcm_hw_params_is_monotonic(params);
    printf("is monotonic = %d\n", val);
}


cl_alsa_sound_device::cl_alsa_sound_device()
{
	dev_name="default";
	baudrate=48000;   // TODO: FIXME, should be 96000
	channels=STEREO;
	_dev_ptr=NULL;
	_buffer_ptr=NULL;
	data_container_ptr=NULL;
	_error=0;
	nbuffer_Samples=baudrate*2;
	_buffer_size=NOT_DEFINED;
	frames_per_period=NOT_DEFINED;
	frames_to_leave_transmit_fct=600; // TODO: REMOVE-ME
	callback_ptr=NULL;
	type=PLAY;
}


cl_alsa_sound_device::~cl_alsa_sound_device()
{
	this->deinit();
}


int cl_alsa_sound_device::init()
{
	int _success=SUCCESS;

	if(frames_per_period==NOT_DEFINED)
	{
		frames_per_period=baudrate; // TODO: FIXME
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


void cl_alsa_sound_device::deinit()
{
	if(_dev_ptr!=NULL)
	{
		if(type==PLAY)
		{
			snd_pcm_drain(_dev_ptr);
		}
		snd_pcm_close (_dev_ptr);

		delete[] _buffer_ptr;
		_buffer_ptr=NULL;
		_dev_ptr=NULL;
	}
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

	// show_alsa(_dev_ptr, configuration_parameters);

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
        if (nbuffer_Samples)
            nChunks=size/(nbuffer_Samples/2);
        else
            return 0;
		for(int i=0;i<nChunks;i++)
		{
			frames_transfered+=_transfere(&buffer[i*(nbuffer_Samples/2)],nbuffer_Samples/2);
		}

		if((size%(nbuffer_Samples/2))!=0)
		{
			frames_transfered+=_transfere(&buffer[nChunks*(nbuffer_Samples/2)],(size%(nbuffer_Samples/2)));

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
			while(frames_avail<_buffer_size-frames_to_leave_transmit_fct); // TODO: FIXME
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

	if(data_container_ptr->data_ready==1)
	{
		data_container_ptr->nUnder_processing_events++;
		//std::cout<<"under_processing No= "<<data_container_ptr->nUnder_processing_events<<std::endl;
	}

	shift_left(data_container_ptr->passband_delayed_data, data_container_ptr->Nofdm*data_container_ptr->interpolation_rate*data_container_ptr->buffer_Nsymb, data_container_ptr->Nofdm*data_container_ptr->interpolation_rate);
	sound_device_ptr->transfere(&data_container_ptr->passband_delayed_data[location_of_last_frame],data_container_ptr->Nofdm*data_container_ptr->interpolation_rate);

	data_container_ptr->frames_to_read--;
	if(data_container_ptr->frames_to_read<0)
	{
		data_container_ptr->frames_to_read=0;
	}
	data_container_ptr->data_ready=1;

}
