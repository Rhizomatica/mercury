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

#include "datalink_layer/tcp_socket.h"

cl_tcp_socket::cl_tcp_socket()
{
	type=TYPE_SERVER;
	socket_fd=0;
	connection_fd=0;
	status=TCP_STATUS_CLOSED;
	address="";
	port=0;
	message_counter=0;
	server_sent_packets=0;
	server_received_packets=0;
	client_sent_packets=0;
	client_received_packets=0;
	bzero(&server, sizeof(server));
	bzero(&client, sizeof(client));
	allow_out_of_order_release=0;
	message= new st_tcp_message;
	timeout_ms=1000;
	link_buffer=NULL;
	buffer_occupancy=0;


}
cl_tcp_socket::~cl_tcp_socket()
{
	if(socket_fd>0)
	{
		close(socket_fd);
	}
}


int cl_tcp_socket::init()
{
	int return_val=SUCCESS;
	if(status==TCP_STATUS_CLOSED)
	{
		if(type==TYPE_SERVER)
		{
			socket_fd = socket(AF_INET, SOCK_STREAM, 0);
			if (socket_fd == ERROR)
			{
				status=TCP_STATUS_SOCKET_CREATION_ERROR;
				std::cout<<"Error-Server socket can't be created"<<std::endl;
				return_val=ERROR;
			}
			else
			{
				server.sin_family = AF_INET;
				server.sin_addr.s_addr = htonl(INADDR_ANY);
				server.sin_port = htons((uint16_t)port);
				status=TCP_STATUS_SOCKET_CREATED;
				std::cout<<"Server socket is created"<<std::endl;
			}
			const int enable = 1;
			if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) != 0)
			{
				status=TCP_STATUS_REUSEADDR_ERROR;
				std::cout<<"Error-Server address reuse can't be set."<<std::endl;
				return_val=ERROR;
			}
			if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) != 0)
			{
				status=TCP_STATUS_REUSEPORT_ERROR;
				std::cout<<"Error-Server port reuse can't be set."<<std::endl;
				return_val=ERROR;
			}
			if ((bind(socket_fd, (struct sockaddr*)&server, sizeof(server))) != 0)
			{
				status=TCP_STATUS_BINDING_ERROR;
				std::cout<<"Error-Server socket can't be binded"<<std::endl;
				return_val=ERROR;
			}
			else
			{
				status=TCP_STATUS_BINDED;
				std::cout<<"Server socket is binded"<<std::endl;
			}
			if ((listen(socket_fd, 5)) != 0)
			{
				status=TCP_STATUS_LISTENING_ERROR;
				std::cout<<"Error-Server socket can't listen"<<std::endl;
				return_val=ERROR;
			}
			else
			{
				status=TCP_STATUS_LISTENING;
				std::cout<<"Server socket is listening"<<std::endl;
			}
			fcntl(socket_fd, F_SETFL, O_NONBLOCK);


		}
		if(type==TYPE_CLIENT)
		{
			socket_fd = socket(AF_INET, SOCK_STREAM, 0);
			if (socket_fd == ERROR)
			{
				status=TCP_STATUS_SOCKET_CREATION_ERROR;
				std::cout<<"Error-Client socket can't be created"<<std::endl;
				return_val=ERROR;
			}
			else
			{
				server.sin_family = AF_INET;
				server.sin_addr.s_addr = inet_addr(address);
				server.sin_port = htons(port);
				status=TCP_STATUS_SOCKET_CREATED;
				std::cout<<"Client socket is created"<<std::endl;

			}
			if (connect(socket_fd, (struct sockaddr*)&server, sizeof(server)) != 0) {
				status=TCP_STATUS_CONNECTING_ERROR;
				std::cout<<"Error-Client can't initial the connection"<<std::endl;
				return_val=ERROR;
			}
			else
			{
				status=TCP_STATUS_CONNECTED;
				std::cout<<"Client is connected to "<<inet_ntoa(server.sin_addr)<<std::endl;
			}

		}
	}
	return return_val;
}

int cl_tcp_socket::check_incomming_connection()
{
	int return_val=SUCCESS;
	unsigned int len = sizeof(client);
	fcntl(socket_fd, F_SETFL, O_NONBLOCK);
	connection_fd = accept(socket_fd, (struct sockaddr*)&client, &len);

	if(connection_fd < 0 || (client.sin_addr.s_addr==htonl(INADDR_ANY)))
	{
		if(connection_fd < 0 && get_status()!=TCP_STATUS_LISTENING)
		{
			return_val=ERROR;
			status=TCP_STATUS_LISTENING;
			std::cout<<"Client connection dropped"<<std::endl;
			std::cout<<"Server socket is listening"<<std::endl;
		}
		return_val=ERROR;
	}
	else
	{
		if (connection_fd < 0) {
			status=TCP_STATUS_ACCEPTING_ERROR;
			std::cout<<"Error-Server can't accept the connection"<<std::endl;
			return_val=ERROR;
		}
		else
		{
			status=TCP_STATUS_ACCEPTED;
			std::cout<<"Server accepted a connection from "<<inet_ntoa(client.sin_addr)<<std::endl;
		}
	}
	return return_val;
}


int cl_tcp_socket::transmit()
{
	int n=0;
	if (type==TYPE_SERVER)
	{
		fcntl(connection_fd, F_SETFL, O_NONBLOCK);
		n= send(connection_fd,message->buffer, message->length,0);
		server_sent_packets++;

	}
	else if(type==TYPE_CLIENT)
	{
		fcntl(socket_fd, F_SETFL, O_NONBLOCK);
		n= send(socket_fd,message->buffer, message->length,0);
		client_sent_packets++;
	}

	return n;
}

int cl_tcp_socket::receive()
{
	int n=0;
	if (type==TYPE_SERVER)
	{
		fcntl(connection_fd, F_SETFL, O_NONBLOCK);
		n= recv (connection_fd,message->buffer, MAX_BUFFER_SIZE,0);
		if(n>0)
		{
			message->length=n;
			message->status=MESSAGE_STATUS_CAPTURED;
			server_received_packets++;
		}
	}
	else if(type==TYPE_CLIENT)
	{
		fcntl(socket_fd, F_SETFL, O_NONBLOCK);
		n= recv (socket_fd,message->buffer, MAX_BUFFER_SIZE,0);
		if(n>0)
		{
			message->length=n;
			message->status=MESSAGE_STATUS_CAPTURED;
			client_received_packets++;
		}
	}

	return n;
}

int cl_tcp_socket::get_status()
{
	return status;
}

void cl_tcp_socket::set_type(int _type)
{
	if (_type==TYPE_SERVER)
	{
		type=TYPE_SERVER;
	}
	else if(_type==TYPE_CLIENT)
	{
		type=TYPE_CLIENT;
	}
	else
	{
		std::cout<< "wrong type"<<std::endl;
		exit(ERROR);
	}
}

void cl_tcp_socket::print_packet_status()
{
	if (type==TYPE_SERVER)
	{
		std::cout<<"Server: Packets Received= "<<server_received_packets<<" Packets sent= "<<server_sent_packets<<std::endl;

	}
	else if(type==TYPE_CLIENT)
	{
		std::cout<<"Client: Packets Received= "<<client_received_packets<<" Packets sent= "<<client_sent_packets<<std::endl;

	}
}


