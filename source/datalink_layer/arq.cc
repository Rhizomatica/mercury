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

cbuf_handle_t data_tx_buffer;
cbuf_handle_t data_rx_buffer;

extern bool shutdown_;

cl_telecom_system *arq_telecom_system;

#define DEBUG

static pthread_t tid[8];

int arq_init(int tcp_base_port, int gear_shift_on, int initial_mode)
{
    status_ctl = NET_NONE;
    status_data = NET_NONE;

    uint8_t *buffer_tx = (uint8_t *) malloc(DATA_TX_BUFFER_SIZE);
    uint8_t *buffer_rx = (uint8_t *) malloc(DATA_RX_BUFFER_SIZE);
    data_tx_buffer = circular_buf_init(buffer_tx, DATA_TX_BUFFER_SIZE);
    data_rx_buffer = circular_buf_init(buffer_rx, DATA_RX_BUFFER_SIZE);

    arq_telecom_system->load_configuration(initial_mode);

    
    // here is the thread that runs the accept(), each per port, and mantains the
    // state of the connection
    pthread_create(&tid[0], NULL, server_worker_thread_ctl, (void *) &tcp_base_port);
    pthread_create(&tid[1], NULL, server_worker_thread_data, (void *) &tcp_base_port);

    // control channel threads
    pthread_create(&tid[2], NULL, control_worker_thread_rx, (void *) NULL);
    pthread_create(&tid[3], NULL, control_worker_thread_tx, (void *) NULL);

    // data channel threads
    pthread_create(&tid[4], NULL, data_worker_thread_tx, (void *) NULL);
    pthread_create(&tid[5], NULL, data_worker_thread_rx, (void *) NULL);

    // dsp threads
    pthread_create(&tid[6], NULL, dsp_thread_tx, (void *) NULL);
    pthread_create(&tid[7], NULL, dsp_thread_rx, (void *) NULL);
    
    
    return EXIT_SUCCESS;
}


void arq_shutdown()
{
    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
    pthread_join(tid[2], NULL);
    pthread_join(tid[3], NULL);
    pthread_join(tid[4], NULL);
    pthread_join(tid[5], NULL);
    pthread_join(tid[6], NULL);
    pthread_join(tid[7], NULL);

    free(data_tx_buffer->buffer);
    free(data_rx_buffer->buffer);
}


void ptt_on()
{
    char buffer[] = "PTT ON\r";

//    tcp_write();
}
void ptt_off()
{
    char buffer[] = "PTT OFF\r";
//    tcp_write();

}

// tx to tcp socket the received data from the modem
void *data_worker_thread_tx(void *conn)
{
    uint8_t *buffer = (uint8_t *) malloc(DATA_TX_BUFFER_SIZE);
    
    while(!shutdown_)
    {
        if (status_data != NET_CONNECTED)
        {
            sleep(1);
            continue;
        }

        size_t n = read_buffer_all(data_rx_buffer, buffer);

        ssize_t i = tcp_write(DATA_TCP_PORT, buffer, n);

        if (i < (ssize_t) n)
            fprintf(stderr, "Problems in data_worker_thread_tx!\n");
        
    }

    free(buffer);
    
    return NULL;
}

// rx from tcp socket and send to trasmit by the modem
void *data_worker_thread_rx(void *conn)
{
    uint8_t *buffer = (uint8_t *) malloc(TCP_BLOCK_SIZE);

    while(!shutdown_)
    {
        if (status_data != NET_CONNECTED)
        {
            sleep(1);
            continue;
        }

        int n = tcp_read(DATA_TCP_PORT, buffer, TCP_BLOCK_SIZE);

        write_buffer(data_tx_buffer, buffer, n);

    }

    free(buffer);
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

        // send IMALIVE's
        // send OK's
        // and ERROR

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
            status_ctl = NET_RESTART;
            tcp_close(CTL_TCP_PORT);
            continue;
        }

        // pthread wait here?
        while (status_ctl == NET_CONNECTED)
            sleep(1);

        // inform the data thread
        if (status_data == NET_CONNECTED)
            status_data = NET_RESTART;
        
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
            status_data = NET_RESTART;
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

void *dsp_thread_tx(void *conn)
{
    static uint32_t spinner_anim = 0; char spinner[] = ".oOo";
    uint8_t data[N_MAX];
    
    while(!shutdown_)
    {
        // TODO: cope with mode changes...      
        int nReal_data = arq_telecom_system->data_container.nBits - arq_telecom_system->ldpc.P;
        int frame_size = (nReal_data - arq_telecom_system->outer_code_reserved_bits) / 8;

        // uint8_t data[frame_size];

        // check the data in the buffer, if smaller than frame size, transmits 0
        if ((int) size_buffer(data_tx_buffer) >= frame_size)
        {
            read_buffer(data_tx_buffer, data, frame_size);

            for (int i = 0; i < frame_size; i++)
            {
                arq_telecom_system->data_container.data_byte[i] = data[i];
            }
        }
        // if there is no data in the buffer, just do nothing
        else
        {
            msleep(50);
            continue;
        }

        arq_telecom_system->transmit_byte(arq_telecom_system->data_container.data_byte,
                                      frame_size,
                                      arq_telecom_system->data_container.ready_to_transmit_passband_data_tx,
                                      SINGLE_MESSAGE);

	tx_transfer(arq_telecom_system->data_container.ready_to_transmit_passband_data_tx,
                    arq_telecom_system->data_container.Nofdm * arq_telecom_system->data_container.interpolation_rate *
                    (arq_telecom_system->data_container.Nsymb + arq_telecom_system->data_container.preamble_nSymb));


        // wait if we have already enogh data...
	while (size_buffer(playback_buffer) > 768000) // (48000 * 2 * 8)
            msleep(50);
        
        printf("%c\033[1D", spinner[spinner_anim % 4]); spinner_anim++;
        fflush(stdout);
        
    }
    
    return NULL;
}

void *dsp_thread_rx(void *conn)
{
    static uint32_t spinner_anim = 0; char spinner[] = ".oOo";
    int out_data[N_MAX];
    uint8_t data[N_MAX];

    while(!shutdown_)
    {
        // TODO: cope with mode changes...      
        int nReal_data = arq_telecom_system->data_container.nBits - arq_telecom_system->ldpc.P;
        int frame_size = (nReal_data - arq_telecom_system->outer_code_reserved_bits) / 8;

        int signal_period = arq_telecom_system->data_container.Nofdm * arq_telecom_system->data_container.buffer_Nsymb * arq_telecom_system->data_container.interpolation_rate; // in samples
        int symbol_period = arq_telecom_system->data_container.Nofdm * arq_telecom_system->data_container.interpolation_rate;

        if(arq_telecom_system->data_container.data_ready == 0)
        {
            // TODO: use some locking primitive here
            msleep(2);
            continue;
        }

        MUTEX_LOCK(&capture_prep_mutex);
        if (arq_telecom_system->data_container.frames_to_read == 0)
        {

            memcpy(arq_telecom_system->data_container.ready_to_process_passband_delayed_data, arq_telecom_system->data_container.passband_delayed_data, signal_period * sizeof(double));

            st_receive_stats received_message_stats = arq_telecom_system->receive_byte(arq_telecom_system->data_container.ready_to_process_passband_delayed_data, out_data);

            arq_telecom_system->data_container.data_ready = 0;
            MUTEX_UNLOCK(&capture_prep_mutex);
            
            if(received_message_stats.message_decoded == YES)
            {
                // printf("Frame decoded in %d iterations. Data: \n", received_message_stats.iterations_done);
                for(int i = 0; i < frame_size; i++)
                {
                    data[i] = (uint8_t) out_data[i];
                }

                if ( frame_size <= (int) circular_buf_free_size(data_rx_buffer) )
                    write_buffer(data_rx_buffer, data, frame_size);
                else
                    printf("Decoded frame lost because of full buffer!\n");


                printf("\rSNR: %5.1f db  Level: %5.1f dBm  RX: %c", arq_telecom_system->receive_stats.SNR, arq_telecom_system->receive_stats.signal_stregth_dbm, spinner[spinner_anim % 4]);
                spinner_anim++;
                fflush(stdout);

                int end_of_current_message = received_message_stats.delay / symbol_period + arq_telecom_system->data_container.Nsymb + arq_telecom_system->data_container.preamble_nSymb;
                int frames_left_in_buffer = arq_telecom_system->data_container.buffer_Nsymb - end_of_current_message;
                if(frames_left_in_buffer < 0)
                    frames_left_in_buffer = 0;

                arq_telecom_system->data_container.frames_to_read = arq_telecom_system->data_container.Nsymb + arq_telecom_system->data_container.preamble_nSymb -
                    frames_left_in_buffer - arq_telecom_system->data_container.nUnder_processing_events;

                if(arq_telecom_system->data_container.frames_to_read > (arq_telecom_system->data_container.Nsymb + arq_telecom_system->data_container.preamble_nSymb) || arq_telecom_system->data_container.frames_to_read < 0)
                    arq_telecom_system->data_container.frames_to_read = arq_telecom_system->data_container.Nsymb + arq_telecom_system->data_container.preamble_nSymb - frames_left_in_buffer;

                arq_telecom_system->receive_stats.delay_of_last_decoded_message += (arq_telecom_system->data_container.Nsymb + arq_telecom_system->data_container.preamble_nSymb - arq_telecom_system->data_container.frames_to_read) * symbol_period;

                arq_telecom_system->data_container.nUnder_processing_events = 0;
            }
            else
            {
                if(arq_telecom_system->data_container.frames_to_read == 0 && arq_telecom_system->receive_stats.delay_of_last_decoded_message != -1)
                {
                    arq_telecom_system->receive_stats.delay_of_last_decoded_message -= symbol_period;
                    if(arq_telecom_system->receive_stats.delay_of_last_decoded_message < 0)
                    {
                        arq_telecom_system->receive_stats.delay_of_last_decoded_message = -1;
                    }
                }
                // std::cout<<" Signal Strength="<<receive_stats.signal_stregth_dbm<<" dBm ";
                // std::cout<<std::endl;
            }
        
        }
        else
        {
            arq_telecom_system->data_container.data_ready = 0;
            MUTEX_UNLOCK(&capture_prep_mutex);
        }
    }

    return NULL;
}
