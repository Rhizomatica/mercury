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

}

cl_data_container::~cl_data_container()
{
	delete data;
	delete encoded_data;
	delete modulated_data;
	delete ofdm_framed_data;
	delete ofdm_symbol_modulated_data;
	delete channaled_data;
	delete ofdm_symbol_demodulated_data;
	delete ofdm_deframed_data;
	delete equalized_data;
	delete demodulated_data;
	delete hd_decoded_data;
	delete baseband_data_interpolated;
	delete passband_data;
	delete passband_delayed_data;
	delete baseband_data;
	delete data_pb_rec;
	delete interleaved_data;
	delete deinterleaved_data;
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
	data=new int[N_MAX];
	encoded_data=new int[N_MAX];
	interleaved_data=new int[N_MAX];
	modulated_data=new std::complex <double>[nData];
	ofdm_framed_data=new std::complex <double>[Nsymb*Nc];
	ofdm_symbol_modulated_data=new std::complex <double>[Nofdm*Nsymb];
	channaled_data=new std::complex <double>[Nofdm*Nsymb*frequency_interpolation_rate];
	ofdm_symbol_demodulated_data=new std::complex <double>[Nsymb*Nc];
	ofdm_deframed_data=new std::complex <double>[Nsymb*Nc];
	equalized_data= new std::complex <double>[Nsymb*Nc];
	demodulated_data=new float[N_MAX];
	deinterleaved_data=new float[N_MAX];
	hd_decoded_data=new int[N_MAX];

	buffer_Nsymb=Nsymb+3;

	passband_data=new double[Nofdm*Nsymb*frequency_interpolation_rate];
	passband_delayed_data=new double[Nofdm*buffer_Nsymb*frequency_interpolation_rate];
	baseband_data=new std::complex <double>[Nofdm*buffer_Nsymb];
	baseband_data_interpolated=new std::complex <double>[Nofdm*buffer_Nsymb*frequency_interpolation_rate];

	frames_to_read=buffer_Nsymb;
	data_ready=0;
	interpolation_rate=frequency_interpolation_rate;
	data_pb_rec=new double[Nofdm*frequency_interpolation_rate*buffer_Nsymb];
}
