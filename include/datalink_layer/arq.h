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

#define DEFAULT_TCP_PORT 8300

#define TCP_BLOCK_SIZE 128
#define CALLSIGN_MAX_SIZE 16 

#define DATA_TX_BUFFER_SIZE 8192
#define DATA_RX_BUFFER_SIZE 8192

#define RX 0
#define TX 1

#define HEADER_SIZE 1

#define PACKET_ARQ_CONTROL 0x00
#define PACKET_ARQ_DATA 0x01
#define PACKET_BROADCAST_CONTROL 0x02
#define PACKET_BROADCAST_PAYLOAD 0x03

#define CALL_BURST_SIZE 3 // 3 frames

typedef struct
{
    int TRX; // RX (0) or TX (1)
    char my_call_sign[CALLSIGN_MAX_SIZE];
    char src_addr[CALLSIGN_MAX_SIZE], dst_addr[CALLSIGN_MAX_SIZE];
    bool encryption;
    bool call_burst_size;
    bool listen;
    int bw; // in Hz
} arq_info;


// frame sizes, no CRC enabled, modes 0 to 16.
// extern uint32_t mercury_frame_size[];

// FSM states
void state_listen(int event);
void state_idle(int event);
void state_connecting_caller(int event);
void state_connecting_callee(int event);

// ARQ core functions
int arq_init(int tcp_base_port, int gear_shift_on, int initial_mode);
void arq_shutdown();

void print_arq_stats();
extern cl_telecom_system *arq_telecom_system;

// TCP/IP server threads
void *server_worker_thread_ctl(void *port);
void *server_worker_thread_data(void *port);
void *data_worker_thread_tx(void *conn);
void *data_worker_thread_rx(void *conn);
void *control_worker_thread_tx(void *conn);
void *control_worker_thread_rx(void *conn);

// DSP threads
void *dsp_thread_tx(void *conn);
void *dsp_thread_rx(void *conn);

// auxiliary functions
void clear_connection_data();
void reset_arq_info(arq_info *arq_conn);
void call_remote();
void callee_accept_connection();
int check_for_incoming_connection(uint8_t *data);
int check_for_connection_acceptance_caller(uint8_t *data);
char *get_timestamp();

// TNC / radio functions
void ptt_on();
void ptt_off();
void tnc_send_connected();
void tnc_send_disconnected();

// file crc6.cc
uint16_t crc6_0X6F(uint16_t crc, const uint8_t *data, int data_len);

// from arith.cc
void init_model();
int arithmetic_encode(const char* msg, uint8_t* output);
int arithmetic_decode(uint8_t* input, int max_len, char* output);

#endif
