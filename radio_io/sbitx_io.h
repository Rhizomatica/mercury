/* sBitx controller shm interface
 *
 * Copyright (C) 2023-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
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
 */


#ifndef SBITX_IO_H_
#define SBITX_IO_H_

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>


#ifdef __cplusplus
extern "C" {
#endif

#define MAX_MODEM_PATH 4096
#define MAX_BUF_SIZE 4096

#define SYSV_SHM_CONTROLLER_KEY_STR 66650 // key for the controller_conn struct

#define MAX_MESSAGE_SIZE 128

typedef struct
{

    uint8_t service_command[5];
    pthread_mutex_t cmd_mutex;
    pthread_cond_t cmd_condition;

    pthread_mutex_t response_mutex;

    uint8_t response_service[5];
    atomic_bool response_available;

    int radio_fd;

    // for messaging system
    char message[MAX_MESSAGE_SIZE];
    atomic_bool message_available;
} controller_conn;


// returns true is response was received
// response is copied to response... so pass a valid 5 bytes pointer
bool radio_cmd(controller_conn *connector, uint8_t *srv_cmd, uint8_t *response);


#ifdef __cplusplus
}
#endif

#endif // SBITX_IO_H_
