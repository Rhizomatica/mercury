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

#ifndef ARQ_H_
#define ARQ_H_

#include "timer.h"
#include <unistd.h>
#include "tcp_socket.h"
#include "fifo_buffer.h"
#include "telecom_system.h"
#include "configurations.h"
#include <iomanip>

//Message status
#define FAILED -2
#define ACK_TIMED_OUT -1
#define FREE 0
#define ADDED_TO_LIST 1
#define ADDED_TO_BATCH_BUFFER 2
#define PENDING_ACK 3
#define ACKED 4
#define RECEIVED 5

// link status
#define DROPPED -1
#define IDLE 0
#define CONNECTING 1
#define CONNECTED 2
#define DISCONNECTING 3
#define LISTENING 4
#define CONNECTION_RECEIVED 5
#define CONNECTION_ACCEPTED 6
#define NEGOTIATING 7

//connection status
#define IDLE 0
#define TRANSMITTING_DATA 1
#define RECEIVING 2
#define RECEIVING_ACKS_DATA 3
#define ACKNOWLEDGING_DATA 4
#define TRANSMITTING_CONTROL 5
#define RECEIVING_ACKS_CONTROL 6
#define ACKNOWLEDGING_CONTROL 7


//Message type
#define NONE 0x00
#define DATA_LONG 0x10
#define DATA_SHORT 0x11
#define ACK_CONTROL 0x20
#define ACK_RANGE 0x21
#define ACK_MULTI 0x22

#define CONTROL 0x30 //multi-commands or especial commands

//Control commands
#define START_CONNECTION 0x31
#define TEST_CONNECTION 0x32
#define CLOSE_CONNECTION 0x33
#define KEEP_ALIVE 0x34
#define FILE_START 0x35
#define FILE_END 0x36
#define PIPE_OPEN 0x37
#define PIPE_CLOSE 0x38
#define SWITCH_ROLE 0x39
#define BLOCK_END 0x3A
#define SET_CONFIG 0x3B
#define REPEAT_LAST_ACK 0x3C

//Error control
#define MEMORY_ERROR -3
#define MESSAGE_LENGTH_ERROR -2
#define ERROR -1
#define SUCCESSFUL 0

//Node role
#define COMMANDER 0
#define RESPONDER 1

//Gear shift
#define GEAR_SHIFT 255

void byte_to_bit(char* data_byte, char* data_bit, int nBytes);
void bit_to_byte(char* data_bit, char* data_byte, int nBits);

union u_SNR {
  float f_SNR;
  char char4_SNR[4];
};

struct st_message
{
	int ack_timeout;
	int nResends;
	int length;
	char* data;
	char type;
	char id;
	char sequence_number;
	int status;
	cl_timer ack_timer;
};

struct st_stats
{
	  int nSent_data;
	  int nAcked_data;
	  int nReceived_data;
	  int nLost_data;
	  int nReSent_data;
	  int nAcks_sent_data;
	  int nNAcked_data;

	  int nSent_control;
	  int nAcked_control;
	  int nReceived_control;
	  int nLost_control;
	  int nReSent_control;
	  int nAcks_sent_control;
	  int nNAcked_control;
};

struct st_measurements
{
	  double SNR_uplink;
	  double SNR_downlink;
	  double signal_stregth_dbm;
	  double frequency_offset;;
};


class cl_arq_controller
{

public:
	cl_arq_controller();
  ~cl_arq_controller();


  void set_nResends(int nResends);
  void set_ack_timeout(int ack_timeout);
  void set_receiving_timeout(int receiving_timeout);
  void set_link_timeout(int link_timeout);
  void set_nMessages(int nMessages);
  void set_max_buffer_length(int max_data_length, int max_message_length, int max_header_length);
  void set_ack_batch_size(int ack_batch_size);
  void set_data_batch_size(int data_batch_size);
  void set_control_batch_size(int control_batch_size);
  void set_role(int role);
  void set_call_sign(std::string call_sign);

  int get_nOccupied_messages();
  int get_nFree_messages();
  int get_nTotal_messages();
  int get_nToSend_messages();
  int get_nPending_Ack_messages();
  int get_nReceived_messages();
  int get_nAcked_messages();

  void messages_control_backup();
  void messages_control_restore();

  int init();


	//! Updates timers values and check for timeouts.
	    /*!
	      \return None
	   */
  void update_status();
	//! removes any acked of failed messages.
	    /*!
	      \return None
	   */
  void cleanup();

	//! registers the ack of a data message.
	    /*!
	      \param message_id is the id of the received message (its location in the buffer).
	      \return None
	   */
  void register_ack(int message_id);
  void pad_messages_batch_tx(int size);

  void process_main();

  void process_user_command(std::string command);
	//! Sends PPT on to the user.
	    /*!
	      \return None
	   */
  void ptt_on();
	//! Sends PPT off to the user.
	    /*!
	      \return None
	   */
  void ptt_off();

  void process_messages();


  void process_messages_commander();
  int add_message_control(char code);
  void process_messages_tx_control();
  int add_message_tx_data(char type, int length, char* data);
  void process_messages_tx_data();
	//! Sends a data or a control message to the other end (via ALSA driver).
	    /*!
	     * \param message the st_message structure to be sent.
	      \return None
	   */
  void send(st_message* message);
  void send_batch();
  void process_messages_rx_acks_control();
  void process_messages_rx_acks_data();
  void process_control_commander();
  void process_buffer_data_commander();


  void process_messages_responder();
	//! Adds the received data message to the buffer.
	    /*!
	     * \param type is the message type.
	     * \param id is message id.
	     * \param length is the message content length.
	     * \param data is the message content.
	     *  \return SUCESSFUL or ERROR
	   */
  int add_message_rx_data(char type, char id, int length, char* data);
	//! Prepares control ack message.
	    /*!
	      \return None
	   */
  void process_messages_rx_data_control();
	//! Prepares control ack message.
	    /*!
	      \return None
	   */
  void process_messages_acknowledging_control();
	//! Prepares data ack message.
	    /*!
	      \return None
	   */
  void process_messages_acknowledging_data();
  void process_control_responder();
  void process_buffer_data_responder();

  void copy_data_to_buffer();
  void restore_backup_buffer_data();

	//! Receives a data or a control message from the other end (via ALSA driver).
	    /*!
	      \return None
	   */
  void receive();

	//! Prints debug information.
	    /*!
	      \return None
	   */
  void print_stats();

  cl_configuration_arq default_configuration_ARQ;


  int message_transmission_time_ms;
  int data_batch_size;
  int control_batch_size;
  int ack_batch_size;
  int block_ready;
  int block_under_tx;
  int max_message_length;
  int max_data_length;
  int max_header_length;

  int connection_status;
  int link_status;
  int role;
  char connection_id;
  char assigned_connection_id;


  cl_tcp_socket tcp_socket_control;
  cl_tcp_socket tcp_socket_data;


  cl_timer connection_timer;
  cl_timer link_timer;
  cl_timer receiving_timer;
  cl_timer print_stats_timer;
  cl_timer gear_shift_timer;

  float print_stats_frequency_hz;

  int message_batch_counter_tx;

  char* message_TxRx_byte_buffer;
  struct st_message messages_rx_buffer;

  struct st_message messages_last_ack_bu;
  struct st_message messages_control_bu;
  struct st_message messages_control;
  struct st_message* messages_batch_tx;

  int ack_timeout;
  int link_timeout;
  int receiving_timeout;

  std::string destination_call_sign;

  cl_fifo_buffer fifo_buffer_tx;
  cl_fifo_buffer fifo_buffer_rx;
  cl_fifo_buffer fifo_buffer_backup;

  cl_telecom_system* telecom_system;

  char last_configuration;
  char current_configuration;
  char negotiated_configuration;

  int gear_shift_on;

private:
  int nMessages;
  struct st_message* messages_tx;
  struct st_message* messages_rx;


  struct st_message* messages_batch_ack;


  std::string my_call_sign;


  st_stats stats;
  st_measurements measurements;

  int nResends;

  char get_configuration(double SNR);
  void load_configuration(int configuration);
  void return_to_last_configuration();
  int init_messages_buffers();
  int deinit_messages_buffers();

  char last_received_message_sequence;
  char last_message_sent_type;
  char last_message_sent_code;

  char last_message_received_type;
  char last_message_received_code;

  int data_ack_received;
  int repeating_last_ack;

};


#endif
