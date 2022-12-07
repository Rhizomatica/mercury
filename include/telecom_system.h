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
#ifndef INC_TELECOM_SYSTEM_H_
#define INC_TELECOM_SYSTEM_H_

#include "data_container.h"
#include "psk.h"
#include "awgn.h"
#include "error_rate.h"
#include "plot.h"
#include "ofdm.h"
#include "ldpc.h"
#include "alsa_sound_dev.h"
#include "interleaver.h"
#include "tcp_socket.h"

#define RECTANGULAR 0
#define HANNING 1
#define HAMMING 2
#define BLACKMAN 3

struct st_receive_stats{
	int iterations_done;
	int delay;
	int sync_trials;
	int time_sync_locked;
	double phase_error_avg;
	double freq_offset;
	int freq_offset_locked;
	int first_symbol_location;
	int message_decoded;
	double SNR;
	double signal_stregth_dbm;
};


class cl_telecom_system
{
private:


public:
	cl_telecom_system();
	~cl_telecom_system();
	cl_data_container data_container;
	cl_psk psk;
	cl_awgn awgn_channel;
	cl_error_rate error_rate;
	cl_ofdm ofdm;
	cl_error_rate passband_test_EsN0(float EsN0,int max_frame_no);
	cl_error_rate baseband_test_EsN0(float EsN0,int max_frame_no);
	cl_ldpc ldpc;
	cl_alsa_sound_device microphone;
	cl_alsa_sound_device speaker;
	double sampling_frequency;
	double carrier_frequency;
	double carrier_amplitude;
	int frequency_interpolation_rate;
	int time_sync_trials_max;
	int lock_time_sync;
	st_receive_stats receive_stats;
	cl_tcp_socket tcp_socket;

	double output_power_Watt;

	int filter_window;
	int filter_nTaps;
	double filter_transition_bandwidth;
	double filter_cut_frequency;
	void FIR_designer();
	void transmit(const int* data, double* out);
	st_receive_stats receive(const double* data, int* out);
	int operation_mode;
	float test_tx_AWGN_EsN0,test_tx_AWGN_EsN0_calibration;

	double M;
	double bandwidth;
	double LDPC_real_CR;
	double Tu;
	double Ts;
	double Tf;
	double rb;
	double rbc;
	double Shannon_limit;

	double* filter_coefficients;

	int bit_interleaver_block_size;

	void calculate_parameters();

	void init();
	cl_plot plot;
};



#endif
