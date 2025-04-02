/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz
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

#ifndef ARQ_H_
#define ARQ_H_

#include "physical_layer/telecom_system.h"

#define TCP_BLOCK_SIZE 128
#define CALLSIGN_MAX_SIZE 16 

int arq_init(int tcp_base_port, int gear_shift_on, int initial_mode);
void arq_shutdown();

void ptt_on();
void ptt_off();

void print_arq_stats();
extern cl_telecom_system *arq_telecom_system;

void *data_worker_thread_tx(void *conn);
void *data_worker_thread_rx(void *conn);
void *control_worker_thread_tx(void *conn);
void *control_worker_thread_rx(void *conn);
void *server_worker_thread_ctl(void *port);
void *server_worker_thread_data(void *port);


#endif
