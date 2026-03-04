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

#include "physical_layer/data_container.h"
#include "debug/canary_guard.h"
#include <cstring>


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
	this->data_bit=NULL;
	this->data_bit_energy_dispersal=NULL;
	this->data_byte=NULL;
	this->encoded_data=NULL;
	this->bit_interleaved_data=NULL;
	this->ofdm_time_freq_interleaved_data=NULL;
	this->ofdm_time_freq_deinterleaved_data=NULL;
	this->modulated_data=NULL;
	this->ofdm_framed_data=NULL;
	this->ofdm_symbol_modulated_data=NULL;
	this->preamble_symbol_modulated_data=NULL;
	this->preamble_data=NULL;
	this->ofdm_symbol_demodulated_data=NULL;
	this->ofdm_deframed_data=NULL;
	this->ofdm_deframed_data_without_amplitude_restoration=NULL;
	this->equalized_data= NULL;
	this->equalized_data_without_amplitude_restoration= NULL;
	this->demodulated_data=NULL;
	this->deinterleaved_data=NULL;
	this->hd_decoded_data_bit=NULL;
	this->hd_decoded_data_byte=NULL;

	this->buffer_Nsymb=0;
	this->preamble_nSymb=0;

	this->passband_data=NULL;
	this->passband_delayed_data=NULL;
	this->ready_to_process_passband_delayed_data=NULL;
	this->baseband_data=NULL;
	this->baseband_data_interpolated=NULL;

	this->frames_to_read=0;
	this->data_ready=0;
	this->nUnder_processing_events=0;
	this->rx_mute=0;
	this->ring_write_index=0;
	this->interpolation_rate=0;

	this->total_frame_size=0;

	this->passband_data_tx=NULL;
	this->passband_data_tx_buffer=NULL;
	this->passband_data_tx_filtered_fir_1=NULL;
	this->passband_data_tx_filtered_fir_2=NULL;
	this->ready_to_transmit_passband_data_tx=NULL;

	this->bit_energy_dispersal_sequence=NULL;
}

cl_data_container::~cl_data_container()
{
	this->deinit();
}

void cl_data_container::set_size(int nData, int Nc, int M, int Nfft , int Nofdm, int Nsymb, int preamble_nSymb, int frequency_interpolation_rate)
{
	this->nData=nData;
	this->nBits=nData*(int)log2(M);
	this->Nc=Nc;
	this->M=M;
	this->Nofdm=Nofdm;
	this->Nfft=Nfft;
	this->Ngi=Nofdm-Nfft;
	this->Nsymb=Nsymb;
	this->preamble_nSymb=preamble_nSymb;
	this->data_bit=CNEW(int, N_MAX, "dc.data_bit");
	this->data_bit_energy_dispersal=CNEW(int, N_MAX, "dc.data_bit_energy_dispersal");
	this->data_byte=CNEW(int, N_MAX, "dc.data_byte");
	this->encoded_data=CNEW(int, N_MAX, "dc.encoded_data");
	this->bit_interleaved_data=CNEW(int, N_MAX, "dc.bit_interleaved_data");
	this->modulated_data=CNEW(std::complex<double>, nData, "dc.modulated_data");
	// ACK pattern generation reuses ofdm_framed_data and ofdm_symbol_modulated_data.
	// NB Sidelnikov ACK uses up to 48 symbols (M=4), WB Welch-Costas uses 16.
	// For high-order modulations (16QAM+), Nsymb < 48, so allocate for the max.
	int alloc_Nsymb = (Nsymb > 48) ? Nsymb : 48;
	this->ofdm_framed_data=CNEW(std::complex<double>, alloc_Nsymb*Nc, "dc.ofdm_framed_data");
	this->ofdm_time_freq_interleaved_data=CNEW(std::complex<double>, Nsymb*Nc, "dc.ofdm_time_freq_interleaved_data");
	this->ofdm_time_freq_deinterleaved_data=CNEW(std::complex<double>, Nsymb*Nc, "dc.ofdm_time_freq_deinterleaved_data");
	this->ofdm_symbol_modulated_data=CNEW(std::complex<double>, Nofdm*alloc_Nsymb, "dc.ofdm_symbol_modulated_data");
	this->ofdm_symbol_demodulated_data=CNEW(std::complex<double>, Nsymb*Nc, "dc.ofdm_symbol_demodulated_data");
	this->ofdm_deframed_data=CNEW(std::complex<double>, Nsymb*Nc, "dc.ofdm_deframed_data");
	this->ofdm_deframed_data_without_amplitude_restoration=CNEW(std::complex<double>, Nsymb*Nc, "dc.ofdm_deframed_data_noamp");
	this->equalized_data=CNEW(std::complex<double>, Nsymb*Nc, "dc.equalized_data");
	this->equalized_data_without_amplitude_restoration=CNEW(std::complex<double>, Nsymb*Nc, "dc.equalized_data_noamp");
	this->preamble_symbol_modulated_data=CNEW(std::complex<double>, preamble_nSymb*Nofdm, "dc.preamble_symbol_mod");
	this->preamble_data=CNEW(std::complex<double>, preamble_nSymb*Nc, "dc.preamble_data");
	this->demodulated_data=CNEW(float, N_MAX, "dc.demodulated_data");
	this->deinterleaved_data=CNEW(float, N_MAX, "dc.deinterleaved_data");
	this->hd_decoded_data_bit=CNEW(int, N_MAX, "dc.hd_decoded_data_bit");
	this->hd_decoded_data_byte=CNEW(int, N_MAX, "dc.hd_decoded_data_byte");

	this->bit_energy_dispersal_sequence=CNEW(int, N_MAX, "dc.bit_energy_dispersal_seq");

	// Buffer: frame + turnaround + frame + tail margin.
	// Tail margin gives headroom for preambles that land close to buffer end.
	double sym_time_ms = 1000.0 * Nofdm * frequency_interpolation_rate / 48000.0;
	int frame_symb = preamble_nSymb + Nsymb;
	// NB OFDM (Nc=10) has longer inter-batch turnaround than WB (Nc=50):
	// NB ACK pattern TX (~1.5s) + Commander processing/compression (~1.5s) = ~3s.
	// 4000ms gives ~1s headroom for radio propagation delay.
	double turnaround_ms = (Nc <= 10) ? 4000.0 : 2000.0;
	int turnaround_symb = (int)ceil(turnaround_ms / sym_time_ms) + 4;
	int margin = frame_symb / 2;
	if(margin < 20) margin = 20;
	if(margin > 50) margin = 50;
	int min_buf = frame_symb + turnaround_symb + frame_symb + margin;
	if(min_buf < 32) min_buf = 32;
	if(this->buffer_Nsymb_min > 0 && min_buf < this->buffer_Nsymb_min)
		min_buf = this->buffer_Nsymb_min;
	this->buffer_Nsymb = min_buf;

	// ACK pattern uses 16*Nofdm*freq_interp passband samples, which can exceed
	// the normal frame size at high modulations (16QAM+). Allocate for whichever is larger.
	int passband_frame = (Nsymb + preamble_nSymb) * Nofdm * frequency_interpolation_rate;
	int passband_ack = 16 * Nofdm * frequency_interpolation_rate;
	this->passband_data=CNEW(double, (passband_frame > passband_ack) ? passband_frame : passband_ack, "dc.passband_data");
	this->passband_delayed_data=CNEW(double, 2*Nofdm*buffer_Nsymb*frequency_interpolation_rate, "dc.passband_delayed_data");
	memset(this->passband_delayed_data, 0, 2*Nofdm*buffer_Nsymb*frequency_interpolation_rate * sizeof(double));
	this->ready_to_process_passband_delayed_data=CNEW(double, Nofdm*buffer_Nsymb*frequency_interpolation_rate, "dc.ready_to_process_pdd");
	memset(this->ready_to_process_passband_delayed_data, 0, Nofdm*buffer_Nsymb*frequency_interpolation_rate * sizeof(double));
	this->baseband_data=CNEW(std::complex<double>, Nofdm*buffer_Nsymb, "dc.baseband_data");
	this->baseband_data_interpolated=CNEW(std::complex<double>, Nofdm*buffer_Nsymb*frequency_interpolation_rate, "dc.baseband_data_interp");

	this->frames_to_read=preamble_nSymb+Nsymb;
	this->data_ready=0;
	this->nUnder_processing_events=0;
	this->ring_write_index=0;
	this->interpolation_rate=frequency_interpolation_rate;
	this->total_frame_size=Nofdm*(Nsymb+preamble_nSymb)*frequency_interpolation_rate;


	this->passband_data_tx=CNEW(double, total_frame_size, "dc.passband_data_tx");
	this->passband_data_tx_buffer=CNEW(double, 3*total_frame_size, "dc.passband_data_tx_buffer");
	this->passband_data_tx_filtered_fir_1=CNEW(double, 2*total_frame_size, "dc.passband_data_tx_filt1");
	this->passband_data_tx_filtered_fir_2=CNEW(double, 2*total_frame_size, "dc.passband_data_tx_filt2");
	this->ready_to_transmit_passband_data_tx=CNEW(double, total_frame_size, "dc.ready_to_tx_passband");

	// Buffer is zero-initialized (memset above). Do NOT fill with random noise:
	// after a config switch (e.g. MFSK→OFDM gearshift), random data in the buffer
	// passes the energy gate and gets decoded as noise, producing garbage channel
	// estimates (mean_H ≈ 0.12). Zero-fill lets the energy gate skip empty frames
	// until real audio arrives from the capture thread.
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
	this->total_frame_size=0;

	CDELETE(this->data_bit);
	CDELETE(this->data_bit_energy_dispersal);
	CDELETE(this->data_byte);
	CDELETE(this->encoded_data);
	CDELETE(this->bit_interleaved_data);
	CDELETE(this->modulated_data);
	CDELETE(this->ofdm_framed_data);
	CDELETE(this->ofdm_time_freq_interleaved_data);
	CDELETE(this->ofdm_time_freq_deinterleaved_data);
	CDELETE(this->ofdm_symbol_modulated_data);
	CDELETE(this->ofdm_symbol_demodulated_data);
	CDELETE(this->ofdm_deframed_data);
	CDELETE(this->ofdm_deframed_data_without_amplitude_restoration);
	CDELETE(this->equalized_data);
	CDELETE(this->equalized_data_without_amplitude_restoration);
	CDELETE(this->preamble_symbol_modulated_data);
	CDELETE(this->preamble_data);
	CDELETE(this->demodulated_data);
	CDELETE(this->deinterleaved_data);
	CDELETE(this->hd_decoded_data_bit);
	CDELETE(this->hd_decoded_data_byte);
	CDELETE(this->bit_energy_dispersal_sequence);

	this->buffer_Nsymb=0;

	CDELETE(this->passband_data);
	CDELETE(this->passband_delayed_data);
	CDELETE(this->ready_to_process_passband_delayed_data);
	CDELETE(this->baseband_data);
	CDELETE(this->baseband_data_interpolated);
	CDELETE(this->passband_data_tx);
	CDELETE(this->passband_data_tx_buffer);
	CDELETE(this->passband_data_tx_filtered_fir_1);
	CDELETE(this->passband_data_tx_filtered_fir_2);
	CDELETE(this->ready_to_transmit_passband_data_tx);

	this->frames_to_read=0;
	this->data_ready=0;
	this->nUnder_processing_events=0;
	this->ring_write_index=0;
	this->interpolation_rate=0;
}
