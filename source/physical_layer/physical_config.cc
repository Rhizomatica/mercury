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

#include "physical_layer/physical_config.h"

extern double carrier_frequency_offset;
extern int radio_type;
extern char *input_dev;
extern char *output_dev;

cl_configuration_telecom_system::cl_configuration_telecom_system()
{
    // TODO: parametrize most important parameters here
	init_configuration=CONFIG_0;

	ofdm_Nc=AUTO_SELLECT;
	ofdm_Nfft=256;
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
	ofdm_pilot_density=HIGH_DENSITY;

	ofdm_preamble_configurator_Nsymb=4;
	ofdm_preamble_configurator_nIdentical_sections=2;
	ofdm_preamble_configurator_modulation=MOD_QPSK;
	ofdm_preamble_configurator_boost=sqrt(2);
	ofdm_preamble_configurator_seed=1;

	ofdm_time_sync_Nsymb=AUTO_SELLECT;


	ofdm_freq_offset_ignore_limit=0.1;
	ofdm_start_shift=1;

	ofdm_channel_estimator=LEAST_SQUARE;
	ofdm_channel_estimator_amplitude_restoration=NO;
	ofdm_LS_window_width=20;
	ofdm_LS_window_hight=20;

	bit_energy_dispersal_seed=0;

	ldpc_standard=MERCURY;
	ldpc_framesize=MERCURY_NORMAL;

	ldpc_decoding_algorithm=SPA;
	ldpc_GBF_eta=0.5;
	ldpc_nIteration_max=50;
	ldpc_print_nIteration=NO;

	outer_code=CRC16_MODBUS_RTU;

	frequency_interpolation_rate=4; // should we change to 8 when samplerate is 96 kHz?

	bandwidth=48000.0*50.0/ofdm_Nfft/frequency_interpolation_rate;

	printf("Bandwidth: %f Hz\n", bandwidth);

	time_sync_trials_max=2;
	use_last_good_time_sync=YES;
	use_last_good_freq_offset=YES;
	carrier_frequency = carrier_frequency_offset + (bandwidth / 2 + 300);
	output_power_Watt=0.1;

	printf("Center frequency: %f Hz low: %f Hz high: %f Hz\n", carrier_frequency, carrier_frequency - bandwidth/2, carrier_frequency + bandwidth/2);

	ofdm_FIR_rx_time_sync_filter_window=HAMMING;
	ofdm_FIR_rx_time_sync_filter_transition_bandwidth=3000;
	ofdm_FIR_rx_time_sync_lpf_filter_cut_frequency=0.9*bandwidth/2;
	ofdm_FIR_rx_time_sync_filter_type=LPF;

	ofdm_FIR_rx_data_filter_window=HAMMING;
	ofdm_FIR_rx_data_filter_transition_bandwidth=3000;
	ofdm_FIR_rx_data_lpf_filter_cut_frequency=1.0*bandwidth/2;
	ofdm_FIR_rx_data_filter_type=LPF;

	ofdm_FIR_tx1_filter_window=HAMMING;
	ofdm_FIR_tx1_filter_transition_bandwidth=1000;
	ofdm_FIR_tx1_lpf_filter_cut_frequency=carrier_frequency+bandwidth/2;
	ofdm_FIR_tx1_hpf_filter_cut_frequency=carrier_frequency-bandwidth/2;
	ofdm_FIR_tx1_filter_type=HPF;

	ofdm_FIR_tx2_filter_window=BLACKMAN;
	ofdm_FIR_tx2_filter_transition_bandwidth=1000;
	ofdm_FIR_tx2_lpf_filter_cut_frequency=carrier_frequency+bandwidth/2;
	ofdm_FIR_tx2_hpf_filter_cut_frequency=carrier_frequency-bandwidth/2;
	ofdm_FIR_tx2_filter_type=LPF;

	ofdm_preamble_papr_cut=7;
	ofdm_data_papr_cut=10;

	// This folder can be in a ramdisk (eg: mount -t tmpfs -o size=128M tmpfs /mnt/ramDisk)
	plot_folder="./";
	plot_plot_active=NO;

}

cl_configuration_telecom_system::~cl_configuration_telecom_system()
{

}







