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

#include "datalink_layer/datalink_config.h"

cl_configuration_arq::cl_configuration_arq()
{

	fifo_buffer_tx_size=128000;
	fifo_buffer_rx_size=128000;
	fifo_buffer_backup_size=128000;

	link_timeout=100000;

	tcp_socket_control_port=7002;
	tcp_socket_control_timeout_ms=INFINIT;

	tcp_socket_data_port=7003;
	tcp_socket_data_timeout_ms=INFINIT;

	gear_shift_on=NO;
	current_configuration=CONFIG_0;

	batch_size=10; //MAX Max_data_length-5 bytes
	nMessages=50; //MAX 255
	nBytes_header=5;

	nResends=10;
	ack_batch_size=2;
	control_batch_size=2;
	
	ptt_on_delay_ms=500;
}

cl_configuration_arq::~cl_configuration_arq()
{
}






