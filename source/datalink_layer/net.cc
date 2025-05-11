/* Mercury modem
 * Copyright (C) 2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 * Network related functions
 */

#include "datalink_layer/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>

#include <pthread.h>

#include <sys/ioctl.h>

static int ctl_sockfd, data_sockfd;

int cli_ctl_sockfd, cli_data_sockfd;
std::atomic_int status_ctl, status_data;

static pthread_mutex_t read_mutex[2] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };
static pthread_mutex_t write_mutex[2] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };


int listen4connection(int port_type)
{
    socklen_t clilen;
    int newsockfd = 0;
    struct sockaddr_in cli_addr;

    clilen = sizeof(cli_addr);

    if (port_type == CTL_TCP_PORT)
        newsockfd = accept(ctl_sockfd, (struct sockaddr *) &cli_addr, &clilen);

    if (port_type == DATA_TCP_PORT)
        newsockfd = accept(data_sockfd, (struct sockaddr *) &cli_addr, &clilen);        

    if (newsockfd < 0)
    {
        fprintf(stderr, "ERROR on accept");
        return -1;
    }

    if (port_type == CTL_TCP_PORT)
    {
        cli_ctl_sockfd = newsockfd;
        status_ctl = NET_CONNECTED;
    }
        
    if (port_type == DATA_TCP_PORT)
    {
        cli_data_sockfd = newsockfd;
        status_data = NET_CONNECTED;
    }
    return newsockfd;
}

int tcp_open(int portno, int port_type)
{
    int sockfd;
    struct sockaddr_in serv_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0){
        fprintf(stderr, "ERROR opening socket\n");
        return -1;
    }

    int opt = 1;  
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        fprintf(stderr, "setsockopt(SO_REUSEADDR) failed\n");
        close(sockfd);
        return -1;
    }
      
    memset((char *) &serv_addr, 0,  sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) 
    {
        fprintf(stderr, "ERROR on binding\n");
        return -1;
    }

    listen(sockfd,1); // just 1 concurrent connections

    if (port_type == CTL_TCP_PORT)
    {
        status_ctl = NET_LISTENING; 
        ctl_sockfd = sockfd;
    }
    if (port_type == DATA_TCP_PORT)
    {
        status_data = NET_LISTENING;
        data_sockfd = sockfd;
    }
    return sockfd;
}

ssize_t tcp_read(int port_type, uint8_t *buffer, size_t rx_size)
{
    ssize_t n = 0;

    pthread_mutex_lock(&read_mutex[port_type]);

    size_t count = 0;

    if (port_type == CTL_TCP_PORT && status_ctl == NET_CONNECTED)
    {
        ioctl(cli_ctl_sockfd, FIONREAD, &count);
        if (count < rx_size)
            rx_size = count;
        n = recv(cli_ctl_sockfd, buffer, rx_size, MSG_NOSIGNAL);

        if (n <= 0) status_ctl = NET_RESTART;        
    }
    if (port_type == DATA_TCP_PORT && status_data == NET_CONNECTED)
    {
        ioctl(cli_data_sockfd, FIONREAD, &count);
        if (count < rx_size)
            rx_size = count;
        n = recv(cli_data_sockfd, buffer, rx_size, MSG_NOSIGNAL);

        if (n <= 0) status_data = NET_RESTART;        
    }
    
    pthread_mutex_unlock(&read_mutex[port_type]);

    if (n < 0)
        fprintf(stderr, "ERROR reading from socket\n");
    
    return n;
}

ssize_t tcp_write(int port_type, uint8_t *buffer, size_t tx_size)
{
    ssize_t n = 0;

    pthread_mutex_lock(&write_mutex[port_type]);

    if (port_type == CTL_TCP_PORT && status_ctl == NET_CONNECTED)
    {
        n = send(cli_ctl_sockfd, buffer, tx_size, MSG_NOSIGNAL);

        if (n != (ssize_t) tx_size)
            status_ctl = NET_RESTART;
    }

    if (port_type == DATA_TCP_PORT && status_data == NET_CONNECTED)
    {
        n = send(cli_data_sockfd, buffer, tx_size, MSG_NOSIGNAL);

        if (n != (ssize_t) tx_size)
            status_data = NET_RESTART;
    }

    pthread_mutex_unlock(&write_mutex[port_type]);
    
    if (n != (ssize_t) tx_size)
        fprintf(stderr, "ERROR writing to socket\n");

    return n;
}

int tcp_close(int port_type)
{

    if(port_type == CTL_TCP_PORT)
    {
        close(cli_ctl_sockfd);
        close(ctl_sockfd);
    }
    if(port_type == DATA_TCP_PORT)
    {
        close(cli_data_sockfd);
        close(data_sockfd);
    }
    status_ctl = NET_NONE;
    status_data = NET_NONE;
    
    return 0;
}
