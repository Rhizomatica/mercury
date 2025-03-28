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

#include "datalink_layer/arq.h"
#include "audioio/audioio.h"

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

cl_arq_controller::cl_arq_controller()
{
}



cl_arq_controller::~cl_arq_controller()
{
}


int cl_arq_controller::init(int tcp_base_port, int gear_shift_on, int initial_mode)
{

    return EXIT_SUCCESS;
}


void cl_arq_controller::process_main()
{

}


void cl_arq_controller::print_stats()
{

}

void cl_arq_controller::ptt_on()
{
#if 0
    std::string str="PTT ON\r";
    tcp_socket_control.message->length=str.length();

    for(int i=0;i<tcp_socket_control.message->length;i++)
    {
        tcp_socket_control.message->buffer[i]=str[i];
    }
    tcp_socket_control.transmit();
#endif
}
void cl_arq_controller::ptt_off()
{
#if 0
    std::string str="PTT OFF\r";
    tcp_socket_control.message->length=str.length();

    for(int i=0;i<tcp_socket_control.message->length;i++)
    {
        tcp_socket_control.message->buffer[i]=str[i];
    }
    tcp_socket_control.transmit();
#endif
}
