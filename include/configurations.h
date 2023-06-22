/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2023 Fadi Jerji
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

#ifndef INC_CONFIGURATIONS_H_
#define INC_CONFIGURATIONS_H_

#include "defines.h"
#include "ofdm.h"
#include "ldpc.h"
#include "fir_filter.h"
#include "alsa_sound_dev.h"


class cl_configuration_arq
{
private:
public:
	cl_configuration_arq();
	~cl_configuration_arq();

	int fifo_buffer_tx_size;
	int fifo_buffer_rx_size;
	int fifo_buffer_backup_size;

	int link_timeout;

	int tcp_socket_control_port;
	long int tcp_socket_control_timeout_ms;

	int tcp_socket_data_port;
	long int tcp_socket_data_timeout_ms;

	int gear_shift_on;
	char current_configuration;

	int batch_size;
	int nMessages;
	int nBytes_header;

	int nResends;
	int ack_batch_size;
	int control_batch_size;

};

struct cl_configuration_telecom_system
{
private:
public:
	cl_configuration_telecom_system();
	~cl_configuration_telecom_system();

	float test_tx_AWGN_EsN0_calibration;
	float test_tx_AWGN_EsN0;

	int tcp_socket_test_port;
	long int tcp_socket_test_timeout_ms;

	char current_configuration;

	int ofdm_Nc;
	int ofdm_Nfft;
	float ofdm_gi;
	int ofdm_Nsymb;
	int ofdm_pilot_configurator_Dx;
	int ofdm_pilot_configurator_Dy;
	int ofdm_pilot_configurator_first_row;
	int ofdm_pilot_configurator_last_row;
	int ofdm_pilot_configurator_first_col;
	int ofdm_pilot_configurator_second_col;
	int ofdm_pilot_configurator_last_col;
	float ofdm_pilot_configurator_pilot_boost;
	int ofdm_pilot_configurator_first_row_zeros;
	int ofdm_print_time_sync_status;
	float ofdm_freq_offset_ignore_limit;

	int ldpc_standard;
	int ldpc_framesize;

	int ldpc_decoding_algorithm;
	float ldpc_GBF_eta;
	int ldpc_nIteration_max;
	int ldpc_print_nIteration;

	double bandwidth;
	int time_sync_trials_max;
	int lock_time_sync;
	int frequency_interpolation_rate;
	double carrier_frequency;
	double output_power_Watt;
	int ofdm_FIR_filter_window;
	double ofdm_FIR_filter_transition_bandwidth;
	double ofdm_FIR_filter_cut_frequency;




	std::string plot_folder;
	int plot_plot_active;


	std::string microphone_dev_name;
	std::string speaker_dev_name;


	int microphone_type;
	unsigned int microphone_channels;


	int speaker_type;
	unsigned int speaker_channels;
	snd_pcm_uframes_t speaker_frames_to_leave_transmit_fct;

};




#endif
