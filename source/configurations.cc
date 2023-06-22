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

#include "configurations.h"


cl_configuration_arq::cl_configuration_arq()
{

	fifo_buffer_tx_size=128000;
	fifo_buffer_rx_size=128000;
	fifo_buffer_backup_size=128000;

	link_timeout=100000;

	tcp_socket_control_port=7002;
	tcp_socket_control_timeout_ms=5000;

	tcp_socket_data_port=7003;
	tcp_socket_data_timeout_ms=5000;

	gear_shift_on=YES;
	current_configuration=CONFIG_0;

	batch_size=50; //MAX Max_data_length-5 bytes
	nMessages=250; //MAX 255
	nBytes_header=5;

	nResends=10;
	ack_batch_size=2;
	control_batch_size=2;
}

cl_configuration_arq::~cl_configuration_arq()
{
}

cl_configuration_telecom_system::cl_configuration_telecom_system()
{
	test_tx_AWGN_EsN0_calibration=1;
	test_tx_AWGN_EsN0=1000;

	tcp_socket_test_port=7001;
	tcp_socket_test_timeout_ms=10000;

	current_configuration=CONFIG_0;

	ofdm_Nc=AUTO_SELLECT;
	ofdm_Nfft=512;
	ofdm_gi=1.0/16.0;
	ofdm_Nsymb=AUTO_SELLECT;
	ofdm_pilot_configurator_Dx=AUTO_SELLECT;
	ofdm_pilot_configurator_Dy=AUTO_SELLECT;
	ofdm_pilot_configurator_first_row=DATA;
	ofdm_pilot_configurator_last_row=DATA;
	ofdm_pilot_configurator_first_col=DATA;
	ofdm_pilot_configurator_second_col=DATA;
	ofdm_pilot_configurator_last_col=AUTO_SELLECT;
	ofdm_pilot_configurator_pilot_boost=1.33;
	ofdm_pilot_configurator_first_row_zeros=YES;
	ofdm_print_time_sync_status=NO;
	ofdm_freq_offset_ignore_limit=0.1;

	ldpc_standard=HERMES;
	ldpc_framesize=HERMES_NORMAL;

	ldpc_decoding_algorithm=GBF;
	ldpc_GBF_eta=0.5;
	ldpc_nIteration_max=50;
	ldpc_print_nIteration=NO;

	bandwidth=2500;
	time_sync_trials_max=1;
	lock_time_sync=NO;
	frequency_interpolation_rate=1;
	carrier_frequency=1450;
	output_power_Watt=1.6;
	ofdm_FIR_filter_window=HANNING;
	ofdm_FIR_filter_transition_bandwidth=1000;
	ofdm_FIR_filter_cut_frequency=6000;

	plot_folder="/mnt/ramDisk/";
	plot_plot_active=NO;


	microphone_dev_name="plughw:0,0";
	speaker_dev_name="plughw:0,0";


	microphone_type=CAPTURE;
	microphone_channels=LEFT;


	speaker_type=PLAY;
	speaker_channels=MONO;
	speaker_frames_to_leave_transmit_fct=800;
}

cl_configuration_telecom_system::~cl_configuration_telecom_system()
{

}







