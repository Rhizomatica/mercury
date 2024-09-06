/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 * Author: Fadi Jerji
 * Email: fadi.jerji@  <gmail.com, caisresearch.com, ieee.org>
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

#include "datalink_layer/arq.h"

void cl_arq_controller::process_messages_responder()
{

	if(this->connection_status==ACKNOWLEDGING_CONTROL)
	{
		print_stats();
		process_messages_acknowledging_control();
	}
	else if(this->connection_status==ACKNOWLEDGING_DATA)
	{
		print_stats();
		process_messages_acknowledging_data();
	}
	else if(this->connection_status==RECEIVING)
	{
		process_messages_rx_data_control();
	}

}

int cl_arq_controller::add_message_rx_data(char type, char id, int length, char* data)
{
	int success=ERROR_;
	int loc=(int)((unsigned char)id);
	if(loc>nMessages || loc<0)
	{
		success=MESSAGE_ID_ERROR;
		return success;
	}

	if(length<0)
	{
		success=MESSAGE_LENGTH_ERROR;
		return success;
	}

	if(type==DATA_LONG && length>(max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH))
	{
		success=MESSAGE_LENGTH_ERROR;
		return success;
	}

	if(type==DATA_SHORT && length>(max_data_length+max_header_length-DATA_SHORT_HEADER_LENGTH))
	{
		success=MESSAGE_LENGTH_ERROR;
		return success;
	}

	messages_rx[loc].type=type;
	messages_rx[loc].length=length;
	for(int j=0;j<messages_rx[loc].length;j++)
	{
		messages_rx[loc].data[j]=data[j];
	}
	for(int j=messages_rx[loc].length;j<(max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH);j++)
	{
		messages_rx[loc].data[j]=0;
	}
	if(messages_rx[loc].status==FREE || messages_rx[loc].status==ACKED)
	{
		stats.nReceived_data++;
	}
	messages_rx[loc].status=RECEIVED;
	success=SUCCESSFUL;
	return success;
}


void cl_arq_controller::process_messages_rx_data_control()
{
	if (receiving_timer.get_elapsed_time_ms()<receiving_timeout)
	{
		this->receive();
		if(messages_rx_buffer.status==RECEIVED)
		{
			if(messages_rx_buffer.type==CONTROL)
			{
				if(messages_control.status==FREE)
				{
					messages_control.type=messages_rx_buffer.type;
					messages_control.id=0;
					messages_control.status=RECEIVED;
					messages_control.length=1;
					messages_control.sequence_number=messages_rx_buffer.sequence_number;
					for(int j=0;j<max_data_length+max_header_length-CONTROL_ACK_CONTROL_HEADER_LENGTH;j++)
					{
						messages_control.data[j]=messages_rx_buffer.data[j];
					}
					stats.nReceived_control++;
				}
				set_receiving_timeout((control_batch_size-messages_rx_buffer.sequence_number-1)*message_transmission_time_ms+time_left_to_send_last_frame+ptt_on_delay_ms);
				receiving_timer.start();
			}
			else if(messages_rx_buffer.type==DATA_LONG || messages_rx_buffer.type==DATA_SHORT)
			{
				add_message_rx_data(messages_rx_buffer.type, messages_rx_buffer.id, messages_rx_buffer.length, messages_rx_buffer.data);
				set_receiving_timeout((data_batch_size-messages_rx_buffer.sequence_number-1)*message_transmission_time_ms+time_left_to_send_last_frame+ptt_on_delay_ms);
				receiving_timer.start();
			}
			messages_rx_buffer.status=FREE;
			link_timer.start();
			watchdog_timer.start();
			gear_shift_timer.stop();
			gear_shift_timer.reset();
		}
	}
	else
	{
		if (messages_control.status==RECEIVED)
		{
			process_control_responder();
		}
		if ( get_nReceived_messages()!=0)
		{
			connection_status=ACKNOWLEDGING_DATA;
		}

		receiving_timer.stop();
		receiving_timer.reset();
	}
}

void cl_arq_controller::process_messages_acknowledging_control()
{
	message_batch_counter_tx=0;
	if(messages_control.status==RECEIVED)
	{
		messages_control.type=ACK_CONTROL;
		messages_control.status=ACKED;
		messages_batch_tx[message_batch_counter_tx]=messages_control;
		message_batch_counter_tx++;
		stats.nAcks_sent_control++;

		load_configuration(ack_configuration, PHYSICAL_LAYER_ONLY,NO);

		pad_messages_batch_tx(ack_batch_size);
		send_batch();

		load_configuration(data_configuration, PHYSICAL_LAYER_ONLY,YES);

		messages_control.status=FREE;
		connection_status=RECEIVING;
		connection_id=assigned_connection_id;

		if (messages_control.data[0]==SWITCH_ROLE)
		{
			set_role(COMMANDER);
			this->link_status=CONNECTED;
			cl_timer ptt_off_delay;
			ptt_off_delay.reset();
			ptt_off_delay.start();
			while(ptt_off_delay.get_elapsed_time_ms()<ptt_off_delay_ms);
			add_message_control(TEST_CONNECTION);
			this->connection_status=TRANSMITTING_CONTROL;
			switch_role_test_timer.reset();
			switch_role_test_timer.start();
			last_message_received_type=NONE;
			last_message_sent_type=NONE;
			last_received_message_sequence=-1;
		}
		else if(messages_control.data[0]==CLOSE_CONNECTION)
		{
			disconnect_requested=NO;
			load_configuration(init_configuration,FULL,YES);
			this->link_status=LISTENING;
			this->connection_status=RECEIVING;
		}
	}
}


void cl_arq_controller::process_messages_acknowledging_data()
{
	// ACK_MULTI
	int nAck_messages=0;
	receiving_timer.stop();
	receiving_timer.reset();

	if(repeating_last_ack==YES)
	{
		messages_control.status=FREE;
		message_batch_counter_tx=0;
		if(messages_last_ack_bu.type==ACK_MULTI ||messages_last_ack_bu.type==ACK_RANGE)
		{
			messages_batch_ack[message_batch_counter_tx].type=messages_last_ack_bu.type;
			messages_batch_ack[message_batch_counter_tx].id=messages_last_ack_bu.id;
			messages_batch_ack[message_batch_counter_tx].length=messages_last_ack_bu.length;
			for(int i=0;i<messages_batch_ack[message_batch_counter_tx].length;i++)
			{
				messages_batch_ack[message_batch_counter_tx].data[i]=messages_last_ack_bu.data[i];
			}
		}
		else
		{
			messages_batch_ack[message_batch_counter_tx].type=NONE;
			messages_batch_ack[message_batch_counter_tx].id=0;
			messages_batch_ack[message_batch_counter_tx].length=0;
		}
		repeating_last_ack=NO;
	}
	else
	{
		nAck_messages=0;
		for(int i=0;i<this->nMessages;i++)
		{
			if(messages_rx[i].status==RECEIVED)
			{
				nAck_messages++;
			}
		}
		message_batch_counter_tx=0;
		messages_batch_ack[message_batch_counter_tx].type=ACK_MULTI;
		messages_batch_ack[message_batch_counter_tx].id=0;
		messages_batch_ack[message_batch_counter_tx].length=nAck_messages+1;
		messages_batch_ack[message_batch_counter_tx].data=new char[nAck_messages+1];
		messages_batch_ack[message_batch_counter_tx].data[0]=nAck_messages;

		int counter=1;
		for(int i=0;i<this->nMessages;i++)
		{
			if(messages_rx[i].status==RECEIVED)
			{
				messages_rx[i].status=ACKED;
				messages_batch_ack[message_batch_counter_tx].data[counter]=i;
				counter++;
			}
		}

		messages_last_ack_bu.type=messages_batch_ack[message_batch_counter_tx].type;
		messages_last_ack_bu.id=messages_batch_ack[message_batch_counter_tx].id;
		messages_last_ack_bu.length=messages_batch_ack[message_batch_counter_tx].length;
		for(int i=0;i<messages_last_ack_bu.length;i++)
		{
			messages_last_ack_bu.data[i]=messages_batch_ack[message_batch_counter_tx].data[i];
		}
		stats.nAcks_sent_data+=nAck_messages;
	}
	messages_batch_tx[message_batch_counter_tx]=messages_batch_ack[message_batch_counter_tx];
	message_batch_counter_tx++;

	load_configuration(ack_configuration, PHYSICAL_LAYER_ONLY,NO);

	pad_messages_batch_tx(ack_batch_size);
	send_batch();

	load_configuration(data_configuration, PHYSICAL_LAYER_ONLY,YES);

	connection_status=RECEIVING;

	// ACK_RANGE
	//	int nAcks_sent=0;
	//	int nAck_messages=0;
	//	receiving_timer.stop();
	//	receiving_timer.reset();
	//	for(int j=0;j<ack_batch_size;j++)
	//	{
	//		int start=-1;
	//		int end=-1;
	//		for(int i=0;i<this->nMessages;i++)
	//		{
	//			if(messages_rx[i].status==RECEIVED)
	//			{
	//				start=i;
	//				end=i;
	//				break;
	//			}
	//		}
	//		for(int i=start+1;i<this->nMessages;i++)
	//		{
	//			if(messages_rx[i].status==RECEIVED)
	//			{
	//				end=i;
	//			}
	//			else
	//			{
	//				break;
	//			}
	//		}
	//
	//		if(start!=-1)
	//		{
	//			messages_batch_ack[message_batch_counter_tx].type=ACK_RANGE;
	//			messages_batch_ack[message_batch_counter_tx].id=(char)start;
	//			messages_batch_ack[message_batch_counter_tx].length=2;
	//			messages_batch_ack[message_batch_counter_tx].data=new char[2];
	//			messages_batch_ack[message_batch_counter_tx].data[0]=(char)start;
	//			messages_batch_ack[message_batch_counter_tx].data[1]=(char)end;
	//			nAcks_sent=end-start+1;
	//
	//			for(int i=start;i<=end;i++)
	//			{
	//				messages_rx[i].status=ACKED;
	//			}
	//
	//			messages_batch_tx[message_batch_counter_tx]=messages_batch_ack[message_batch_counter_tx];
	//			message_batch_counter_tx++;
	//			nAck_messages++;
	//			stats.nAcks_sent_data+=nAcks_sent;
	//		}
	//
	//		if(nAcks_sent>=ack_batch_size || get_nReceived_messages()==0)
	//		{
	//			pad_messages_batch_tx(ack_batch_size);
	//
	//			send_batch();
	//			connection_status=RECEIVING;
	//			break;
	//		}
	//	}
}

void cl_arq_controller::process_control_responder()
{
	char code=messages_control.data[0];
	if((link_status==LISTENING || link_status==CONNECTION_RECEIVED) && code==START_CONNECTION)
	{
		if(messages_control.data[1]==CRC8_calc((char*)my_call_sign.c_str(), my_call_sign.length()))
		{
			destination_call_sign="";
			for(int i=0;i<messages_control.data[2];i++)
			{
				destination_call_sign+=messages_control.data[3+i];
			}

			link_status=CONNECTION_RECEIVED;
			connection_status=ACKNOWLEDGING_CONTROL;
			messages_control.data[1]=1+rand()%0xfe;
			messages_control.length=2;
			assigned_connection_id=messages_control.data[1];
			watchdog_timer.start();
		}
		else
		{
			messages_control.status=FREE;
		}
	}
	else if((link_status==CONNECTION_RECEIVED || link_status==CONNECTED) && code==TEST_CONNECTION)
	{
		u_SNR tmp_SNR;
		for(int i=0;i<4;i++)
		{
			tmp_SNR.char4_SNR[i]=messages_control.data[i+1];
		}
		measurements.SNR_uplink=(double)tmp_SNR.f_SNR;

		tmp_SNR.f_SNR=(float)measurements.SNR_downlink;
		for(int i=0;i<4;i++)
		{
			messages_control.data[i+1]=tmp_SNR.char4_SNR[i];;
		}
		messages_control.length=5;

		if(this->link_status==CONNECTION_RECEIVED)
		{
			std::string str="CONNECTED "+this->destination_call_sign+" "+this->my_call_sign+" "+ std::to_string(telecom_system->bandwidth)+"\r";
			tcp_socket_control.message->length=str.length();

			for(int i=0;i<tcp_socket_control.message->length;i++)
			{
				tcp_socket_control.message->buffer[i]=str[i];
			}
			tcp_socket_control.transmit();
		}

		link_status=CONNECTED;
		connection_status=ACKNOWLEDGING_CONTROL;
		watchdog_timer.start();
		link_timer.start();


	}
	else if(link_status==CONNECTED && (code==SET_CONFIG || code==BLOCK_END || code==FILE_END_ || code==SWITCH_ROLE || code==REPEAT_LAST_ACK))
	{
		if(code==SET_CONFIG)
		{
			if(messages_control.data[1]!= current_configuration && messages_control.data[1]>=0 && messages_control.data[1]<NUMBER_OF_CONFIGS)
			{
				messages_control_backup();
				data_configuration=messages_control.data[1];
				load_configuration(data_configuration,PHYSICAL_LAYER_ONLY,YES);
				messages_control_restore();
			}

			connection_status=ACKNOWLEDGING_CONTROL;
			link_timer.start();
			watchdog_timer.start();
			gear_shift_timer.start();
		}
		else if(code==BLOCK_END)
		{
			connection_status=ACKNOWLEDGING_CONTROL;
			std::cout<<"end of block"<<std::endl;
			copy_data_to_buffer();
			messages_last_ack_bu.type=NONE;
			link_timer.start();
			watchdog_timer.start();
		}
		else if(code==FILE_END_)
		{
			connection_status=ACKNOWLEDGING_CONTROL;
			std::cout<<"end of file"<<std::endl;
			copy_data_to_buffer();
			messages_last_ack_bu.type=NONE;
			link_timer.start();
			watchdog_timer.start();
		}
		else if(code==SWITCH_ROLE)
		{
			connection_status=ACKNOWLEDGING_CONTROL;
			std::cout<<"switch role"<<std::endl;
			copy_data_to_buffer();
			link_timer.start();
			watchdog_timer.start();
			// Received data test code
//			char data,data2;
//			int error=NO;
//			srand(5);
//			int nRec= fifo_buffer_rx.get_size()-fifo_buffer_rx.get_free_size();
//			std::cout<<"nRec= "<<nRec<<std::endl;
//			for(int i=0;i<nRec;i++)
//			{
//				fifo_buffer_rx.pop(&data, 1);
//				data2=(char)(rand()%0xff);
//				if(data!=data2)
//				{
//					std::cout<<"error @" <<i<<" data="<<(int)data<<" data2="<<(int)data2<<std::endl;
//					error=YES;
//				}
//			}
//			if(error==YES)
//			{
//				exit(0);
//			}
//			else
//			{
//				std::cout<<"all is good"<<std::endl;
//				exit(0);
//			}
		}
		else if(code==REPEAT_LAST_ACK)
		{
			repeating_last_ack=YES;
			connection_status=ACKNOWLEDGING_DATA;
		}
	}
	else
	{
		if(code==CLOSE_CONNECTION)
		{
			assigned_connection_id=0;
			link_status=DISCONNECTING;
			connection_status=ACKNOWLEDGING_CONTROL;
			link_timer.stop();
			link_timer.reset();
			watchdog_timer.stop();
			watchdog_timer.reset();
			gear_shift_timer.stop();
			gear_shift_timer.reset();
			receiving_timer.stop();
			receiving_timer.reset();

			fifo_buffer_tx.flush();
			fifo_buffer_backup.flush();
			fifo_buffer_rx.flush();

			std::string str="DISCONNECTED\r";
			tcp_socket_control.message->length=str.length();

			for(int i=0;i<tcp_socket_control.message->length;i++)
			{
				tcp_socket_control.message->buffer[i]=str[i];
			}
			tcp_socket_control.transmit();
		}
	}

}

void cl_arq_controller::process_buffer_data_responder()
{
	if(link_status==CONNECTED)
	{
		if (tcp_socket_data.get_status()==TCP_STATUS_ACCEPTED)
		{
			while(fifo_buffer_rx.get_size()!=fifo_buffer_rx.get_free_size())
			{
				tcp_socket_data.message->length=fifo_buffer_rx.pop(tcp_socket_data.message->buffer,max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH);
				tcp_socket_data.transmit();
			}
		}

	}
}


