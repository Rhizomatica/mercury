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
#include "datalink_layer/net.h"

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

// TODO add the data buffers here, one for tx, other for rx

extern bool shutdown_;

#define DEBUG

cl_arq_controller::cl_arq_controller()
{
}


cl_arq_controller::~cl_arq_controller()
{
}

int cl_arq_controller::init(int tcp_base_port, int gear_shift_on, int initial_mode)
{
    status_ctl = NET_NONE;
    status_data = NET_NONE;

    // here is the thread that runs the accept(), each per port, and mantains the
    // state of the connection
    pthread_t tid0;
    pthread_create(&tid0, NULL, server_worker_thread_ctl, (void *) &tcp_base_port);
    pthread_t tid1;
    pthread_create(&tid1, NULL, server_worker_thread_data, (void *) &tcp_base_port);
    
    // we start our control thread
    pthread_t tid2;
    pthread_create(&tid2, NULL, control_worker_thread_rx, (void *) NULL);

    // we start our control tx thread
    pthread_t tid3;
    pthread_create(&tid3, NULL, control_worker_thread_tx, (void *) NULL);

    pthread_t tid4;
    pthread_create(&tid4, NULL, data_worker_thread_tx, (void *) NULL);

    pthread_t tid5;
    pthread_create(&tid5, NULL, data_worker_thread_rx, (void *) NULL);


    pthread_join(tid0, NULL);
    pthread_join(tid1, NULL);
    pthread_join(tid2, NULL);
    pthread_join(tid3, NULL);
    pthread_join(tid4, NULL);
    pthread_join(tid5, NULL);
    
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
    char buffer[] = "PTT ON\r";

//    tcp_write();

    
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

void *data_worker_thread_tx(void *conn)
{

    while(!shutdown_)
    {
        if (status_ctl != NET_CONNECTED)
        {
            sleep(1);
            continue;
        }
        

        
        // send IMALIVE's
        // send OK's
        // and ERROR
        // TODO: implement-me        
        sleep(1);        
    }
    
    return NULL;
}

void *data_worker_thread_rx(void *conn)
{

    while(!shutdown_)
    {
        if (status_ctl != NET_CONNECTED)
        {
            sleep(1);
            continue;
        }

        // TODO: implement-me        
        sleep(1);
    }

    return NULL;
}

void *control_worker_thread_tx(void *conn)
{

    while(!shutdown_)
    {
        if (status_ctl != NET_CONNECTED)
        {
            sleep(1);
            continue;
        }


        // TODO: implement-me
        sleep(1);
        
    }
    
    return NULL;
}

void *control_worker_thread_rx(void *conn)
{
    char *buffer = (char *) malloc(TCP_BLOCK_SIZE+1);
    char my_call_sign[CALLSIGN_MAX_SIZE];
    char src_addr[CALLSIGN_MAX_SIZE], dst_addr[CALLSIGN_MAX_SIZE];
    bool listen = false;
    bool encryption = false;
    int bw = 0; // in Hz
    char temp[16];
    int count = 0;

    memset(buffer, 0, TCP_BLOCK_SIZE+1);

    while(!shutdown_)
    {
        if (status_ctl != NET_CONNECTED)
        {
            sleep(1);
            continue;
        }

#if 0 // we can do the slow way...
        // get number of bytes in the socket
        ioctl(cli_ctl_sockfd, FIONREAD, &count);           
#endif
        
        int n = tcp_read(CTL_TCP_PORT, (uint8_t *)buffer + count, 1);

        if (n <= 0)
        {
            count = 0;
            fprintf(stderr, "ERROR ctl socket reading\n");            
            status_ctl = NET_RESTART;
            continue;
        }

        if (buffer[count] != '\r')
        {
            count++;
            continue;
        }

        // we found the '\r'
        buffer[count] = 0;
        count = 0;

        if (count >= TCP_BLOCK_SIZE)
        {
            count = 0;
            fprintf(stderr, "ERROR in command parsing\n");
            continue;
        }
        
#ifdef DEBUG
        fprintf(stderr,"Command received: %s\n", buffer);  
#endif
        
        // now we parse the commands
        if (!memcmp(buffer, "MYCALL", strlen("MYCALL")))
        {
            sscanf(buffer,"MYCALL %s", my_call_sign);
            continue;
        }
        
        if (!memcmp(buffer, "LISTEN", strlen("LISTEN")))
        {
            sscanf(buffer,"LISTEN %s", temp);
            if (temp[1] == 'N') // ON
                listen = true;
            if (temp[1] == 'F') // OFF
                listen = false;
            
            continue;
        }

        if (!memcmp(buffer, "PUBLIC", strlen("PUBLIC")))
        {
            sscanf(buffer,"PUBLIC %s", temp);
            if (temp[1] == 'N') // ON
                encryption = false;
            if (temp[1] == 'F') // OFF
               encryption = true;
            
            continue;
        }

        if (!memcmp(buffer, "BW", strlen("BW")))
        {
            sscanf(buffer,"BW%d", &bw);
            continue;
        }

        if (!memcmp(buffer, "CONNECT", strlen("CONNECT")))
        {
            sscanf(buffer,"CONNECT %s %s", src_addr, dst_addr);
            continue;
        }

        if (!memcmp(buffer, "DISCONNECT", strlen("DISCONNECT")))
        {
            continue;
        }        

        fprintf(stderr, "Unknown command\n");

    }

    free(buffer);

    return NULL;
}

void *server_worker_thread_ctl(void *port)
{
    int tcp_base_port = *((int *) port);
    int socket;
    
    while(!shutdown_)
    {
        int ret = tcp_open(tcp_base_port, CTL_TCP_PORT);

        if (ret < 0)
        {
            fprintf(stderr, "Could not open TCP port %d\n", tcp_base_port);
            shutdown_ = true;
        }
        
        socket = listen4connection(CTL_TCP_PORT);

        if (socket < 0)
        {
            tcp_close(CTL_TCP_PORT);
            continue;
        }

        // pthread wait here?
        while (status_ctl == NET_CONNECTED)
            sleep(1);

        tcp_close(CTL_TCP_PORT);

    }

    return NULL;
    
}

void *server_worker_thread_data(void *port)
{
    int tcp_base_port = *((int *) port);
    int socket;

    while(!shutdown_)
    {
        int ret = tcp_open(tcp_base_port+1, DATA_TCP_PORT);

        if (ret < 0)
        {
            fprintf(stderr, "Could not open TCP port %d\n", tcp_base_port+1);
            shutdown_ = true;
        }
        
        socket = listen4connection(DATA_TCP_PORT);

        if (socket < 0)
        {
            tcp_close(DATA_TCP_PORT);
            continue;
        }

        // pthread wait here?
        while (status_data == NET_CONNECTED)
            sleep(1);

        tcp_close(DATA_TCP_PORT);
    }
    
    return NULL;
}
