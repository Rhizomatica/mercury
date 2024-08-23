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

#ifndef INC_TCP_SOCKET_H_
#define INC_TCP_SOCKET_H_
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <stdlib.h>

#include <fcntl.h>
#include <string>
#include <cstring>
#include <cstdint>
#include <sys/types.h>

#if defined (_WIN32)
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include "timer.h"

#define SUCCESS 0
#define ERROR_ -1




#define MAX_BUFFER_SIZE 8192
#define MAX_BUFFER_DEPTH 800
#define TYPE_SERVER 0
#define TYPE_CLIENT 1


#define TCP_STATUS_CLOSED 0
#define TCP_STATUS_SOCKET_CREATED 1
#define TCP_STATUS_BINDED 2
#define TCP_STATUS_LISTENING 3
#define TCP_STATUS_CONNECTED 4
#define TCP_STATUS_ACCEPTED 5

#define TCP_STATUS_SOCKET_CREATION_ERROR -1
#define TCP_STATUS_BINDING_ERROR -2
#define TCP_STATUS_LISTENING_ERROR -3
#define TCP_STATUS_CONNECTING_ERROR -4
#define TCP_STATUS_ACCEPTING_ERROR -5
#define TCP_STATUS_REUSEADDR_ERROR -6
#define TCP_STATUS_REUSEPORT_ERROR -7
#define TCP_STATUS_OTHER_ERROR -8


#define MESSAGE_STATUS_FREE 0
#define MESSAGE_STATUS_CAPTURED 1
#define MESSAGE_STATUS_READY_TO_RELEASE 2

struct st_tcp_message
{
	int status;
	char buffer[MAX_BUFFER_SIZE];
	int length;
};


class cl_tcp_socket
{
private:
public:
	struct sockaddr_in server,client;
	int allow_out_of_order_release;
	int socket_fd, connection_fd;
	int status;
	int type;
	st_tcp_message* message;
	int message_counter;
	char* link_buffer;
	float buffer_occupancy;
	long unsigned int server_sent_packets;
	long unsigned int server_received_packets;
	long unsigned int client_sent_packets;
	long unsigned int client_received_packets;

	cl_timer timer;
	long int timeout_ms;

	cl_tcp_socket();
	~cl_tcp_socket();

	const char* address;
	int port;
	void set_type(int _type);
	void set_buffer(st_tcp_message* buffer);
	int init();
	int check_incomming_connection();
	int transmit();
	int receive();
	void print_packet_status();
	int get_status();

};




#endif
