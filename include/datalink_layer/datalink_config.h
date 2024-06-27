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

#ifndef INC_DATALINK_CONFIG_H_
#define INC_DATALINK_CONFIG_H_

#include "common/common_defines.h"
#include "datalink_defines.h"

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


	char init_configuration;
	char ack_configuration;

	int gear_shift_on;
	int gear_shift_algorithm;
	double gear_shift_up_success_rate_limit_precentage;
	double gear_shift_down_success_rate_limit_precentage;
	int gear_shift_block_for_nBlocks_total;

	int batch_size;
	int nMessages;

	int nResends;
	int ack_batch_size;
	int control_batch_size;
	int ptt_on_delay_ms;
	int ptt_off_delay_ms;
	int switch_role_timeout_ms;

};


#endif
