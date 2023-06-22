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
#include "data_container.h"


cl_data_container::cl_data_container()
{
	this->nData=0;
	this->nBits=0;
	this->Nc=0;
	this->M=0;
	this->Nofdm=0;
	this->Nfft=0;
	this->Ngi=0;
	this->Nsymb=0;
	this->data=NULL;
	this->encoded_data=NULL;
	this->interleaved_data=NULL;
	this->modulated_data=NULL;
	this->ofdm_framed_data=NULL;
	this->ofdm_symbol_modulated_data=NULL;
	this->channaled_data=NULL;
	this->ofdm_symbol_demodulated_data=NULL;
	this->ofdm_deframed_data=NULL;
	this->equalized_data= NULL;
	this->demodulated_data=NULL;
	this->deinterleaved_data=NULL;
	this->hd_decoded_data=NULL;

	this->buffer_Nsymb=0;

	this->passband_data=NULL;
	this->passband_delayed_data=NULL;
	this->baseband_data=NULL;
	this->baseband_data_interpolated=NULL;

	this->frames_to_read=0;
	this->data_ready=0;
	this->interpolation_rate=0;
	this->data_pb_rec=NULL;

	this->sound_device_ptr=NULL;
}

cl_data_container::~cl_data_container()
{
	this->deinit();
}

void cl_data_container::set_size(int nData, int Nc, int M, int Nfft , int Nofdm, int Nsymb, int frequency_interpolation_rate)
{
	this->nData=nData;
	this->nBits=nData*(int)log2(M);
	this->Nc=Nc;
	this->M=M;
	this->Nofdm=Nofdm;
	this->Nfft=Nfft;
	this->Ngi=Nofdm-Nfft;
	this->Nsymb=Nsymb;
	this->data=new int[N_MAX];
	this->encoded_data=new int[N_MAX];
	this->interleaved_data=new int[N_MAX];
	this->modulated_data=new std::complex <double>[nData];
	this->ofdm_framed_data=new std::complex <double>[Nsymb*Nc];
	this->ofdm_symbol_modulated_data=new std::complex <double>[Nofdm*Nsymb];
	this->channaled_data=new std::complex <double>[Nofdm*Nsymb*frequency_interpolation_rate];
	this->ofdm_symbol_demodulated_data=new std::complex <double>[Nsymb*Nc];
	this->ofdm_deframed_data=new std::complex <double>[Nsymb*Nc];
	this->equalized_data= new std::complex <double>[Nsymb*Nc];
	this->demodulated_data=new float[N_MAX];
	this->deinterleaved_data=new float[N_MAX];
	this->hd_decoded_data=new int[N_MAX];

	this->buffer_Nsymb=Nsymb+3;

	this->passband_data=new double[Nofdm*Nsymb*frequency_interpolation_rate];
	this->passband_delayed_data=new double[Nofdm*buffer_Nsymb*frequency_interpolation_rate];
	this->baseband_data=new std::complex <double>[Nofdm*buffer_Nsymb];
	this->baseband_data_interpolated=new std::complex <double>[Nofdm*buffer_Nsymb*frequency_interpolation_rate];

	this->frames_to_read=buffer_Nsymb;
	this->data_ready=0;
	this->interpolation_rate=frequency_interpolation_rate;
	this->data_pb_rec=new double[Nofdm*frequency_interpolation_rate*buffer_Nsymb];
}

void cl_data_container::deinit()
{
	this->nData=0;
	this->nBits=0;
	this->Nc=0;
	this->M=0;
	this->Nofdm=0;
	this->Nfft=0;
	this->Ngi=0;
	this->Nsymb=0;
	if(this->data!=NULL)
	{
		delete[] this->data;
		this->data=NULL;
	}
	if(this->encoded_data!=NULL)
	{
		delete[] this->encoded_data;
		this->encoded_data=NULL;
	}
	if(this->interleaved_data!=NULL)
	{
		delete[] this->interleaved_data;
		this->interleaved_data=NULL;
	}
	if(this->modulated_data!=NULL)
	{
		delete[] this->modulated_data;
		this->modulated_data=NULL;
	}
	if(this->ofdm_framed_data!=NULL)
	{
		delete[] this->ofdm_framed_data;
		this->ofdm_framed_data=NULL;
	}
	if(this->ofdm_symbol_modulated_data!=NULL)
	{
		delete[] this->ofdm_symbol_modulated_data;
		this->ofdm_symbol_modulated_data=NULL;
	}
	if(this->channaled_data!=NULL)
	{
		delete[] this->channaled_data;
		this->channaled_data=NULL;
	}
	if(this->ofdm_symbol_demodulated_data!=NULL)
	{
		delete[] this->ofdm_symbol_demodulated_data;
		this->ofdm_symbol_demodulated_data=NULL;
	}
	if(this->ofdm_deframed_data!=NULL)
	{
		delete[] this->ofdm_deframed_data;
		this->ofdm_deframed_data=NULL;
	}
	if(this->equalized_data!=NULL)
	{
		delete[] this->equalized_data;
		this->equalized_data=NULL;
	}
	if(this->demodulated_data!=NULL)
	{
		delete[] this->demodulated_data;
		this->demodulated_data=NULL;
	}
	if(this->deinterleaved_data!=NULL)
	{
		delete[] this->deinterleaved_data;
		this->deinterleaved_data=NULL;
	}
	if(this->hd_decoded_data!=NULL)
	{
		delete[] this->hd_decoded_data;
		this->hd_decoded_data=NULL;
	}

	this->buffer_Nsymb=0;

	if(this->passband_data!=NULL)
	{
		delete[] this->passband_data;
		this->passband_data=NULL;
	}
	if(this->passband_delayed_data!=NULL)
	{
		delete[] this->passband_delayed_data;
		this->passband_delayed_data=NULL;
	}
	if(this->baseband_data!=NULL)
	{
		delete[] this->baseband_data;
		this->baseband_data=NULL;
	}
	if(this->baseband_data_interpolated!=NULL)
	{
		delete[] this->baseband_data_interpolated;
		this->baseband_data_interpolated=NULL;
	}

	this->frames_to_read=0;
	this->data_ready=0;
	this->interpolation_rate=0;
	if(this->data_pb_rec!=NULL)
	{
		delete[] this->data_pb_rec;
		this->data_pb_rec=NULL;
	}

	this->sound_device_ptr=NULL;
}
