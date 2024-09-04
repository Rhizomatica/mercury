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

#ifndef INC_DATA_CONTAINER_H_
#define INC_DATA_CONTAINER_H_

#include <complex>
#include <atomic>
#define _Atomic(X) std::atomic< X >

#include "physical_defines.h"
#include "misc.h"

class cl_data_container
{
	public:
	cl_data_container();
	~cl_data_container();
	int* data_bit;
	int* data_bit_energy_dispersal;
	int* data_byte;
	int* encoded_data;
	int* bit_interleaved_data;
	std::complex <double>* modulated_data;
	std::complex <double>* ofdm_symbol_modulated_data;
	std::complex <double>* ofdm_symbol_demodulated_data;
	std::complex <double>* ofdm_framed_data;
	std::complex <double>* ofdm_time_freq_interleaved_data;
	std::complex <double>* ofdm_time_freq_deinterleaved_data;
	std::complex <double>* ofdm_deframed_data;
	std::complex <double>* ofdm_deframed_data_without_amplitude_restoration;
	std::complex <double>* equalized_data;
	std::complex <double>* equalized_data_without_amplitude_restoration;
	std::complex <double>* preamble_symbol_modulated_data;
	std::complex <double>* preamble_data;
	double* passband_data;
	double* passband_delayed_data;
	double* ready_to_process_passband_delayed_data;
	std::complex <double>* baseband_data;
	std::complex <double>* baseband_data_interpolated;
	float* demodulated_data;
	float* deinterleaved_data;
	int* hd_decoded_data_bit;
	int* hd_decoded_data_byte;
	int nData,Nc,M,Nfft,Nofdm,Nsymb,preamble_nSymb,nBits,Ngi,interpolation_rate;
	void set_size(int nData, int Nc,int M,int Nfft, int Nofdm, int Nsymb, int preamble_nSymb, int interpolation_rate);

	_Atomic(int) frames_to_read;
	_Atomic(int) data_ready;
	_Atomic(int) nUnder_processing_events;
	_Atomic(int) buffer_Nsymb;

	int total_frame_size;

	double* passband_data_tx;
	double* passband_data_tx_buffer;
	double* passband_data_tx_filtered_fir_1;
	double* passband_data_tx_filtered_fir_2;
	double* ready_to_transmit_passband_data_tx;

	int *bit_energy_dispersal_sequence;

	void deinit();


};


#endif
