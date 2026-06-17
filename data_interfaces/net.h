/* HERMES Modem
 *
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Network related functions
 *
 */

/**
 * @file net.h
 * @author Rafael Diniz
 * @date 01 Apr 2025
 * @brief File containing network related functions
 *
 * For the sake of reusable and clean code, some network related functions.
 *
 */


#ifndef NET_H__
#define NET_H__

#include <stdint.h>
#include <sys/types.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#define CTL_TCP_PORT 0
#define DATA_TCP_PORT 1

#define NET_NONE 0
#define NET_LISTENING 1
#define NET_RESTART 2
#define NET_CONNECTED 3

#include <stdatomic.h>

extern int cli_ctl_sockfd, cli_data_sockfd;

extern atomic_int status_ctl, status_data;

void net_set_status(int port_type, int status);
int net_get_status(int port_type);
int net_wait_for_status(int port_type, int status, int timeout_ms);
int net_wait_while_status(int port_type, int status, int timeout_ms);

int listen4connection(int port_type);

int tcp_open(int portno, int port_type);

ssize_t tcp_read(int port_type, uint8_t *buffer, size_t rx_size);

ssize_t tcp_write(int port_type, uint8_t *buffer, size_t tx_size);
ssize_t tcp_write_all(int port_type, uint8_t *buffer, size_t tx_size);

int tcp_close(int port_type);

#endif // NET_H__
