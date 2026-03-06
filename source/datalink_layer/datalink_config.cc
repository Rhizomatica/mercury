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

#include "datalink_layer/datalink_config.h"

cl_configuration_arq::cl_configuration_arq()
{

	fifo_buffer_tx_size=128000;
	fifo_buffer_rx_size=128000;
	fifo_buffer_backup_size=128000;

	link_timeout=30000;

	tcp_socket_control_port=7002;
	tcp_socket_control_timeout_ms=INFINITE_;

	tcp_socket_data_port=7003;
	tcp_socket_data_timeout_ms=INFINITE_;


	init_configuration=CONFIG_1;
	ack_configuration=CONFIG_0;


	gear_shift_on=NO;
	gear_shift_algorithm=SUCCESS_BASED_LADDER;

	gear_shift_up_success_rate_limit_precentage=70;
	gear_shift_down_success_rate_limit_precentage=45;

	gear_shift_block_for_nBlocks_total=2;

	batch_size=5; //MAX Max_data_length-3 (ack header) bytes
	nMessages=75; //MAX 255

	nResends=20;
	ack_batch_size=2;
	control_batch_size=2;
	
	ptt_on_delay_ms=100;
	ptt_off_delay_ms=200;
	pilot_tone_ms=0;    // Disabled by default — enable only for radios that need TX warmup
	pilot_tone_hz=250;  // 250Hz = out of OFDM band, won't interfere with decoder
	switch_role_timeout_ms=1500;
}

cl_configuration_arq::~cl_configuration_arq()
{
}

