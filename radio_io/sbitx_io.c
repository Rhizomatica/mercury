/* sBitx controller shm interface
 *
 * Copyright (C) 2023-2024 Rhizomatica
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
 */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>

#include "sbitx_io.h"
#include "radio_cmds.h"

bool radio_cmd(controller_conn *connector, uint8_t *srv_cmd, uint8_t *response)
{
    bool ret_value = false;

    pthread_mutex_lock(&connector->response_mutex);
    pthread_mutex_lock(&connector->cmd_mutex);

    memcpy(connector->service_command, srv_cmd, 5);

    connector->response_available = false; // just in case something bad happened before...

    pthread_cond_signal(&connector->cmd_condition);
    pthread_mutex_unlock(&connector->cmd_mutex);

    if (srv_cmd[4] == CMD_RADIO_RESET)
        goto get_out;

    // ~3 ms max wait
    uint32_t tries = 0;
    uint32_t sleep_time = 100;
    while (connector->response_available == false && tries < 30)
    {
        usleep(sleep_time); // 0.1 ms
        tries++;
        if (!(tries % 4))
            sleep_time <<= 1;
    }

    if (connector->response_available == true)
    {
        memcpy(response, connector->response_service, 5);
        connector->response_available = false;
        ret_value = true;
    }
    // else timeout? (create a message for this)


 get_out:
    pthread_mutex_unlock(&connector->response_mutex);

    return ret_value;
}
