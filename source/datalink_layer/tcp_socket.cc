/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 * Author: Fadi Jerji
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

#include "datalink_layer/tcp_socket.h"

cl_tcp_socket::cl_tcp_socket()
{
    socket_fd = 0;
    status = TCP_STATUS_CLOSED;
    address = NULL;
    port = 0;
    memset(&server, 0, sizeof(struct sockaddr_in));
    memset(&client, 0, sizeof(struct sockaddr_in));
    server_sent_packets=0;
    server_received_packets=0;
    message = (struct st_tcp_message *) malloc(sizeof(st_tcp_message));

}

cl_tcp_socket::~cl_tcp_socket()
{
    if(socket_fd>0)
    {
#if defined(_WIN32)
        closesocket(socket_fd);
        WSACleanup();
#else
        close(socket_fd);
#endif
    }
    free(message);
}


int cl_tcp_socket::init()
{
#if defined(_WIN32)
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2 ,2), &wsaData);
    if (iResult != 0)
    {
        printf("error at WSASturtup\n");
        return ERROR_;
    }
#endif

    if(status != TCP_STATUS_CLOSED)
        return ERROR_;

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {    
        status = TCP_STATUS_SOCKET_CREATION_ERROR;
        printf("Error opening socket()\n");
        return ERROR_;
    }
    
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons((uint16_t)port);
    status = TCP_STATUS_SOCKET_CREATED;
    printf("Server socket created\n");

    const int enable = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) != 0)
    {
        status = TCP_STATUS_REUSEADDR_ERROR;
        printf("Setsockopt() SO_REUSEADDR error\n");
        return ERROR_;
    }
#if !defined(_WIN32)
    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) != 0)
    {
        status = TCP_STATUS_REUSEPORT_ERROR;
        printf("Setsockopt() SO_REUSEPORT error\n");
        return ERROR_;
    }
#endif
    if ((bind(socket_fd, (struct sockaddr*)&server, sizeof(server))) != 0)
    {
        status = TCP_STATUS_BINDING_ERROR;
        printf("Setsockopt() bind error\n");
        return ERROR_;
    }

    status = TCP_STATUS_BINDED;
    printf("Bind() success.\n");

    if ((listen(socket_fd, 5)) < 0)
    {
        status = TCP_STATUS_LISTENING_ERROR;
        printf("listen() error\n");
        return ERROR_;
    }

    status = TCP_STATUS_LISTENING;
    printf("Listen() success\n");

    
#if defined(_WIN32)
    u_long mode = 1;  // 1 to enable non-blocking socket
    ioctlsocket(socket_fd, FIONBIO, &mode);
#else
    fcntl(socket_fd, F_SETFL, O_NONBLOCK);
#endif

    return SUCCESS;
}

int cl_tcp_socket::check_incomming_connection()
{
#if defined(_WIN32)
    int len = sizeof(client);
#else
    unsigned int len = sizeof(client);
#endif
    connection_fd = accept(socket_fd, (struct sockaddr*)&client, &len);

    if(connection_fd < 0)
    {
        status = TCP_STATUS_LISTENING;
        printf("No client connection\n");
        return ERROR_;
    }
    
#if defined(_WIN32)
    u_long mode = 1;  // 1 to enable non-blocking socket
    ioctlsocket(connection_fd, FIONBIO, &mode);
#else
    fcntl(connection_fd, F_SETFL, O_NONBLOCK);
#endif

    status = TCP_STATUS_ACCEPTED;
    printf("Server accepted a connection from %s\n", inet_ntoa(client.sin_addr));

    return SUCCESS;
}


int cl_tcp_socket::transmit()
{
	int n = 0;
        
        n = send(connection_fd,message->buffer, message->length,0);
        server_sent_packets++;

	return n;
}

int cl_tcp_socket::receive()
{
	int n = 0;
        n = recv (connection_fd,message->buffer, MAX_BUFFER_SIZE,0);
        if(n > 0)
        {
            message->length = n;
            message->status = MESSAGE_STATUS_CAPTURED;
            server_received_packets++;
        }
	return n;
}

int cl_tcp_socket::get_status()
{
	return status;
}

void cl_tcp_socket::print_packet_status()
{
    printf("Server: Packets Received = %lu Packets sent = %lu\n", server_received_packets, server_sent_packets);
}


