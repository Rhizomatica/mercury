/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
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

#include "physical_layer/physical_config.h"


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
	ofdm_pilot_configurator_seed=0;

	ofdm_preamble_configurator_Nsymb=4;
	ofdm_preamble_configurator_nIdentical_sections=2;
	ofdm_preamble_configurator_modulation=MOD_QPSK;
	ofdm_preamble_configurator_boost=sqrt(2);
	ofdm_preamble_configurator_seed=1;

	ofdm_time_sync_Nsymb=AUTO_SELLECT;


	ofdm_freq_offset_ignore_limit=0.1;
	ofdm_start_shift=1;

	ofdm_channel_estimator=ZERO_FORCE;

	ldpc_standard=HERMES;
	ldpc_framesize=HERMES_NORMAL;

	ldpc_decoding_algorithm=SPA;
	ldpc_GBF_eta=0.5;
	ldpc_nIteration_max=50;
	ldpc_print_nIteration=NO;

	frequency_interpolation_rate=2;

	bandwidth=48000.0*50.0/ofdm_Nfft/frequency_interpolation_rate;
	double max_bandwidth=2500*1.2;
	time_sync_trials_max=2;
	use_last_good_time_sync=YES;
	use_last_good_freq_offset=YES;
	carrier_frequency=6000;
	output_power_Watt=0.1;
	ofdm_FIR_rx_filter_window=HAMMING;
	ofdm_FIR_rx_filter_transition_bandwidth=3000;
	ofdm_FIR_rx_lpf_filter_cut_frequency=4*bandwidth/2;
	ofdm_FIR_rx_filter_type=LPF;

	ofdm_FIR_tx1_filter_window=HAMMING;
	ofdm_FIR_tx1_filter_transition_bandwidth=300;
	ofdm_FIR_tx1_lpf_filter_cut_frequency=carrier_frequency+max_bandwidth/2;
	ofdm_FIR_tx1_hpf_filter_cut_frequency=carrier_frequency-max_bandwidth/2;
	ofdm_FIR_tx1_filter_type=HPF;

	ofdm_FIR_tx2_filter_window=BLACKMAN;
	ofdm_FIR_tx2_filter_transition_bandwidth=300;
	ofdm_FIR_tx2_lpf_filter_cut_frequency=carrier_frequency+max_bandwidth/2;
	ofdm_FIR_tx2_hpf_filter_cut_frequency=carrier_frequency-max_bandwidth/2;
	ofdm_FIR_tx2_filter_type=LPF;

	ofdm_preamble_papr_cut=7;
	ofdm_data_papr_cut=10;

	//create the folder as RAM "sudo mount -t tmpfs -o size=128M tmpfs /mnt/ramDisk"
	plot_folder="/mnt/ramDisk/";
	plot_plot_active=YES;

	microphone_dev_name="plughw:1,0";
	speaker_dev_name="plughw:1,0";


	microphone_type=CAPTURE;
	microphone_channels=LEFT;


	speaker_type=PLAY;
	speaker_channels=MONO;
	speaker_frames_to_leave_transmit_fct=20000;
}

cl_configuration_telecom_system::~cl_configuration_telecom_system()
{

}







