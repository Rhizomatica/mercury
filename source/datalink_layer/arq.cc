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
#include "datalink_layer/fsm.h"
#include "audioio/audioio.h"
#include "datalink_layer/net.h"

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

cbuf_handle_t data_tx_buffer;
cbuf_handle_t data_rx_buffer;

extern bool shutdown_;

// the physical layer class
cl_telecom_system *arq_telecom_system;

// ARQ definitions
arq_info arq_conn;

#define DEBUG

static pthread_t tid[8];


/* ---- FSM Definitions ---- */

// our simple fsm struct
fsm_handle arq_fsm;

// FSM Events
#define EV_START_LISTEN 0
#define EV_STOP_LISTEN 1

#define EV_START_CONNECTION 2
#define EV_STOP_CONNECTION 3

#define EV_CLIENT_CONNECT 4
#define EV_CLIENT_DISCONNECT 5

#define EV_LINK_INCOMING_CALL 6

#define EV_LINK_ESTABLISHED 7


// FSM States
void state_disconnected(int event)
{
    switch(event)
    {
    case EV_CLIENT_CONNECT:
        clear_connection_data();
        arq_fsm.current = state_idle;
        break;
    default:
        printf("Event: %d ignored in state_disconnected().\n");
    }
    return;
}

// FSM States
void state_connected(int event)
{
    switch(event)
    {
    default:
        printf("Event: %d ignored in state_disconnected().\n");
    }
    return;
}


void state_listen(int event)
{
    switch(event)
    {
    case EV_START_LISTEN:
        printf("EV_START_LISTEN ignored in state_listen() - already listening.\n");   
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        arq_fsm.current = state_idle;
        break;
    case EV_START_CONNECTION:
        call_remote();
        arq_fsm.current = state_connecting_caller;
        break;
    case EV_STOP_CONNECTION:
        printf("EV_STOP_CONNECTION ignored in state_listen() - not connected.\n");   
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_disconnected;
        break;
    default:
        printf("Event: %d ignored in state_listen().\n");   
    }

    return;
}

void state_idle(int event)
{
    switch(event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        arq_fsm.current = state_listen;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        printf("EV_STOP_LISTEN ignored in state_idle() - already stopped.\n");   
        break;
    case EV_START_CONNECTION:
        call_remote();
        arq_fsm.current = state_connecting_caller;
        break;
    case EV_STOP_CONNECTION:
        printf("EV_STOP_CONNECTION ignored in state_idle() - not connected.\n");   
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_disconnected;
        break;
    default:
        printf("Event: %d ignored from state_idle\n");   
    }
    
    return;
}

void state_connecting_caller(int event)
{
    switch(event)
    {
    case EV_START_LISTEN:
        arq_conn.listen = true;
        break;
    case EV_STOP_LISTEN:
        arq_conn.listen = false;
        break;
    case EV_START_CONNECTION:
        printf("EV_START_CONNECTION ignored in state_connecting_caller() - already connecting.\n");           
        break;
    case EV_STOP_CONNECTION:
        if (arq_conn.listen)
            arq_fsm.current = state_listen;
        else
            arq_fsm.current = state_idle;
        printf("EV_STOP_CONNECTION ignored in state_idle() - not connected.\n");   
        break;
    case EV_CLIENT_DISCONNECT:
        clear_connection_data();
        arq_fsm.current = state_disconnected;
        break;
    case EV_LINK_ESTABLISHED:
        // TODO: do some house cleeping here?
        arq_fsm.current = state_connected;
        break;
    default:
        printf("Event: %d ignored from state_idle\n");   
    }


    
// prepare connection package
// "CONNECTED "+this->my_call_sign+" "+this->destination_call_sign+" "+ std::to_string(telecom_system->bandwidth)+"\r";
    return;
}

void state_connecting_callee(int event)
{
// "CONNECTED "+this->my_call_sign+" "+this->destination_call_sign+" "+ std::to_string(telecom_system->bandwidth)+"\r";

    return;
}

bool check_crc(uint8_t *data)
{
    int nReal_data = arq_telecom_system->data_container.nBits - arq_telecom_system->ldpc.P;
    int frame_size = (nReal_data - arq_telecom_system->outer_code_reserved_bits) / 8;    

    uint16_t crc = (uint16_t) (data[0] & 0x3f);
    uint16_t calculated_crc = crc6_0X6F(1, data + HEADER_SIZE, frame_size - HEADER_SIZE);

    if (crc == calculated_crc)
        return true;

    return false;
    
}

int check_for_incoming_connection(uint8_t *data)
{
    char callsigns[CALLSIGN_MAX_SIZE * 2];
    char dst_callsign[CALLSIGN_MAX_SIZE] = { 0 };
    char src_callsign[CALLSIGN_MAX_SIZE] = { 0 };

    int nReal_data = arq_telecom_system->data_container.nBits - arq_telecom_system->ldpc.P;
    int frame_size = (nReal_data - arq_telecom_system->outer_code_reserved_bits) / 8;    

    uint8_t pack_type = (data[0] >> 6) & 0xff;

    if (check_crc(data) == false)
    {
        printf("Bad crc for packet type %u.\n", pack_type);
        return -1;
    }

    if (pack_type != PACKET_ARQ_CONTROL)
    {
        return 1;
    }

    if (arithmetic_decode(data + HEADER_SIZE, frame_size - HEADER_SIZE, callsigns) < 0)
    {
        printf("Truncated callsigns.\n");
    }
    printf("Decoded callsigns: %s\n", callsigns);

    char *needle;
    if ( (needle = strstr(callsigns,"|")) )
    {
        int i = 0;
        while (callsigns[i] != '|')
        {
            dst_callsign[i] = callsigns[i];
            i++;
        }
        i++;
        dst_callsign[i] = 0;

        i = 0;
        needle++;
        while (callsigns[i] != 0)
        {
            src_callsign[i] = needle[i];
            i++;
        }
        src_callsign[i] = 0;
    }
    else
    {
        strcpy(dst_callsign, callsigns);
    }    

    if (!strcmp(dst_callsign, arq_conn.my_call_sign))
    {
        fsm_dispatch(&arq_fsm, EV_LINK_INCOMING_CALL);
        // Now we need to create the packets to transmit back...
    }
    else
    {
        printf("Caller call sign does not match my callsign.\n");
    }
    
    return 0;
}


void call_remote()
{
    uint8_t data[N_MAX];
    char joint_callsigns[CALLSIGN_MAX_SIZE * 2];
    uint8_t encoded_callsigns[CALLSIGN_MAX_SIZE * 2];
    
    static bool first_call = true;

    if (first_call == true)
    {
        init_model(); // the arithmetic encoder init function
        first_call = false;
    }

    int nReal_data = arq_telecom_system->data_container.nBits - arq_telecom_system->ldpc.P;
    int frame_size = (nReal_data - arq_telecom_system->outer_code_reserved_bits) / 8;    

    memset(data, 0, frame_size);
    // 1 byte header, 4 bits packet type, 6 bits crc
    data[0] = (PACKET_ARQ_CONTROL << 6) & 0xff; // set packet type

    // encode the destination callsign first, then the source, separated by "|"
    sprintf(joint_callsigns,"%s|%s", arq_conn.dst_addr, arq_conn.src_addr);
    int enc_len = arithmetic_encode(joint_callsigns, encoded_callsigns);

    if (enc_len > frame_size - HEADER_SIZE)
    {
        printf("Trucating joint destination + source callsigns. This is ok (%d bytes out of %d transmitted)\n", frame_size - HEADER_SIZE, enc_len);
        enc_len = frame_size - HEADER_SIZE;
    }
    memcpy(data + HEADER_SIZE, encoded_callsigns, enc_len);

    data[0] |= (uint8_t) crc6_0X6F(1, data + HEADER_SIZE, frame_size - HEADER_SIZE);

    write_buffer(data_tx_buffer, data, frame_size);
    
    return;
}

/* ---- END OF FSM Definitions ---- */


void clear_connection_data()
{
    clear_buffer(data_tx_buffer);
    clear_buffer(data_rx_buffer);
    reset_arq_info(&arq_conn);
}

void reset_arq_info(arq_info *arq_conn_i)
{
    arq_conn_i->TRX = RX;
    arq_conn_i->bw = 0; // 0 = auto
    arq_conn_i->encryption = false;
    arq_conn_i->listen = false;
    arq_conn_i->my_call_sign[0] = 0;
    arq_conn_i->src_addr[0] = 0;
    arq_conn_i->dst_addr[0] = 0;
}


int arq_init(int tcp_base_port, int gear_shift_on, int initial_mode)
{
    status_ctl = NET_NONE;
    status_data = NET_NONE;

    arq_conn.call_burst_size = CALL_BURST_SIZE;

    uint8_t *buffer_tx = (uint8_t *) malloc(DATA_TX_BUFFER_SIZE);
    uint8_t *buffer_rx = (uint8_t *) malloc(DATA_RX_BUFFER_SIZE);
    data_tx_buffer = circular_buf_init(buffer_tx, DATA_TX_BUFFER_SIZE);
    data_rx_buffer = circular_buf_init(buffer_rx, DATA_RX_BUFFER_SIZE);

    arq_telecom_system->load_configuration(initial_mode);
    reset_arq_info(&arq_conn);

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

    
    fsm_init(&arq_fsm, state_disconnected);
    
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
    arq_conn.TRX = TX;
    ssize_t i = tcp_write(CTL_TCP_PORT, (uint8_t *)buffer, strlen(buffer));
}
void ptt_off()
{
    char buffer[] = "PTT OFF\r";
    arq_conn.TRX = RX;
    ssize_t i = tcp_write(CTL_TCP_PORT, (uint8_t *)buffer, strlen(buffer));
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

        if(arq_fsm.current == state_connected)
        {
            size_t n = read_buffer_all(data_rx_buffer, buffer);

            ssize_t i = tcp_write(DATA_TCP_PORT, buffer, n);

            if (i < (ssize_t) n)
                fprintf(stderr, "Problems in data_worker_thread_tx!\n");
        }
        else
        {
            msleep(50);
        }
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


        if(arq_fsm.current == state_connected)
        {
            int n = tcp_read(DATA_TCP_PORT, buffer, TCP_BLOCK_SIZE);

            write_buffer(data_tx_buffer, buffer, n);
        }
        else
        {
            msleep(50);
        }

    }

    free(buffer);
    return NULL;
}

void *control_worker_thread_tx(void *conn)
{
    int counter = 0;
    char imalive[] = "IAMALIVE\r";

    while(!shutdown_)
    {
        if (status_ctl != NET_CONNECTED)
        {
            sleep(1);
            continue;
        }

        if (counter == 60)
        {
            counter = 0;
            ssize_t i = tcp_write(CTL_TCP_PORT, (uint8_t *)imalive, strlen(imalive));

        }

        sleep(1);
        counter++;
    }
    
    return NULL;
}

void *control_worker_thread_rx(void *conn)
{
    char *buffer = (char *) malloc(TCP_BLOCK_SIZE+1);
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
            sscanf(buffer,"MYCALL %s", arq_conn.my_call_sign);
            goto send_ok;
        }
        
        if (!memcmp(buffer, "LISTEN", strlen("LISTEN")))
        {
            sscanf(buffer,"LISTEN %s", temp);
            if (temp[1] == 'N') // ON
            {
                fsm_dispatch(&arq_fsm, EV_START_LISTEN);
            }
            if (temp[1] == 'F') // OFF
            {
                fsm_dispatch(&arq_fsm, EV_STOP_LISTEN);
            }

            goto send_ok;
        }

        if (!memcmp(buffer, "PUBLIC", strlen("PUBLIC")))
        {
            sscanf(buffer,"PUBLIC %s", temp);
            if (temp[1] == 'N') // ON
                arq_conn.encryption = false;
            if (temp[1] == 'F') // OFF
               arq_conn.encryption = true;
            
            goto send_ok;
        }

        if (!memcmp(buffer, "BW", strlen("BW")))
        {
            sscanf(buffer,"BW%d", &arq_conn.bw);
            goto send_ok;
        }

        if (!memcmp(buffer, "CONNECT", strlen("CONNECT")))
        {
            sscanf(buffer,"CONNECT %s %s", arq_conn.src_addr, arq_conn.dst_addr);
            fsm_dispatch(&arq_fsm, EV_START_CONNECTION);
            goto send_ok;
        }

        if (!memcmp(buffer, "DISCONNECT", strlen("DISCONNECT")))
        {   
            fsm_dispatch(&arq_fsm, EV_STOP_CONNECTION);
            goto send_ok;
        }

        fprintf(stderr, "Unknown command\n");
        tcp_write(CTL_TCP_PORT, (uint8_t *) "WRONG\r", 6);
        continue;
        
    send_ok:
        tcp_write(CTL_TCP_PORT, (uint8_t *) "OK\r", 3);

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

        fsm_dispatch(&arq_fsm, EV_CLIENT_CONNECT);
        
        // TODO: pthread wait here?
        while (status_ctl == NET_CONNECTED)
            sleep(1);

        // inform the data thread
        if (status_data == NET_CONNECTED)
            status_data = NET_RESTART;

        fsm_dispatch(&arq_fsm, EV_CLIENT_DISCONNECT);
        
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


    
    // TODO: may be we need another function to queue the already prepared packets?
    while(!shutdown_)
    {
        // TODO: cope with mode changes...      
        int nReal_data = arq_telecom_system->data_container.nBits - arq_telecom_system->ldpc.P;
        int frame_size = (nReal_data - arq_telecom_system->outer_code_reserved_bits) / 8;

        // uint8_t data[frame_size];

        // check if there is data in the buffer
        // should we add a header already here, to know the size of each package? (size need to match frame_size
        if ((int) size_buffer(data_tx_buffer) < frame_size ||
            arq_fsm.current == state_idle ||
            arq_fsm.current == state_disconnected)
        {
            msleep(50);
            continue;
        }

            
        if(arq_fsm.current != state_idle && arq_fsm.current != state_listen)
        {
            read_buffer(data_tx_buffer, data, frame_size);
        }
        else
        {
            msleep(50);
            continue;
        }
        
        for (int i = 0; i < frame_size; i++)
        {
            arq_telecom_system->data_container.data_byte[i] = data[i];
        }


        arq_telecom_system->transmit_byte(arq_telecom_system->data_container.data_byte,
                                          frame_size,
                                          arq_telecom_system->data_container.ready_to_transmit_passband_data_tx,
                                          SINGLE_MESSAGE);


        ptt_on();
        msleep(10); // TODO: tune me!

        // our connection request
        if (arq_fsm.current == state_connecting_caller)
        {
            for(int i = 0; i < arq_conn.call_burst_size; i++)
            {
                tx_transfer(arq_telecom_system->data_container.ready_to_transmit_passband_data_tx,
                            arq_telecom_system->data_container.Nofdm * arq_telecom_system->data_container.interpolation_rate *
                            (arq_telecom_system->data_container.Nsymb + arq_telecom_system->data_container.preamble_nSymb));
            }
        }

        if (arq_fsm.current == state_connected)
        {
            tx_transfer(arq_telecom_system->data_container.ready_to_transmit_passband_data_tx,
                        arq_telecom_system->data_container.Nofdm * arq_telecom_system->data_container.interpolation_rate *
                        (arq_telecom_system->data_container.Nsymb + arq_telecom_system->data_container.preamble_nSymb));
        }
        
        // TODO: signal when stream is finished playing via pthread_cond_wait() here
        while (size_buffer(playback_buffer) != 0)
            msleep(10);

        msleep(40); // TODO: tune me!
        ptt_off();
        
#if 0
        // wait if we have already enogh data in playback buffer?...
	while (size_buffer(playback_buffer) > 768000) // (48000 * 2 * 8)
            msleep(50);
#endif
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
            msleep(3);
            continue;
        }

        MUTEX_LOCK(&capture_prep_mutex);
        if (arq_telecom_system->data_container.frames_to_read == 0)
        {
            st_receive_stats received_message_stats;
            if (arq_conn.TRX == RX &&
                arq_fsm.current != state_idle &&
                arq_fsm.current != state_disconnected)
            {
                memcpy(arq_telecom_system->data_container.ready_to_process_passband_delayed_data, arq_telecom_system->data_container.passband_delayed_data, signal_period * sizeof(double));

                received_message_stats = arq_telecom_system->receive_byte(arq_telecom_system->data_container.ready_to_process_passband_delayed_data, out_data);
            }
            else
            {
                received_message_stats.message_decoded = NO;
            }
            arq_telecom_system->data_container.data_ready = 0;
            MUTEX_UNLOCK(&capture_prep_mutex);


            if(received_message_stats.message_decoded == YES)
            {
                // printf("Frame decoded in %d iterations. Data: \n", received_message_stats.iterations_done);
                for(int i = 0; i < frame_size; i++)
                {
                    data[i] = (uint8_t) out_data[i];
                }

                if(arq_fsm.current == state_listen)
                {
                    check_for_incoming_connection(data);
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
