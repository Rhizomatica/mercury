/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 * Author: Fadi Jerji
 * Email: fadi.jerji@  <gmail.com, rhizomatica.org, caisresearch.com, ieee.org>
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
	int success=ERROR;
	int loc=(int)((unsigned char)id);
	if(loc<nMessages)
	{
		if(length<0 || length>max_data_length)
		{
			success=MESSAGE_LENGTH_ERROR;
		}
		else
		{
			messages_rx[loc].type=type;
			messages_rx[loc].length=length;
			for(int j=0;j<messages_rx[loc].length;j++)
			{
				messages_rx[loc].data[j]=data[j];
			}
			for(int j=messages_rx[loc].length;j<max_data_length;j++)
			{
				messages_rx[loc].data[j]=0;
			}
			if(messages_rx[loc].status==FREE || messages_rx[loc].status==ACKED)
			{
				stats.nReceived_data++;
			}
			messages_rx[loc].status=RECEIVED;
			success=SUCCESSFUL;
		}
	}
	return success;
}


void cl_arq_controller::process_messages_rx_data_control()
{
	if (receiving_timer.get_elapsed_time_ms()<receiving_timeout)
	{
		this->receive();
		if(messages_rx_buffer.status==RECEIVED)
		{
			if(messages_rx_buffer.type==CONTROL && messages_control.status==FREE)
			{
				messages_control.type=messages_rx_buffer.type;
				messages_control.id=0;
				messages_control.status=RECEIVED;
				messages_control.length=1;
				messages_control.sequence_number=messages_rx_buffer.sequence_number;
				for(int j=0;j<max_data_length;j++)
				{
					messages_control.data[j]=messages_rx_buffer.data[j];
				}
				stats.nReceived_control++;
				set_receiving_timeout((control_batch_size-messages_rx_buffer.sequence_number)*message_transmission_time_ms);
				receiving_timer.start();
			}
			else if(messages_rx_buffer.type==DATA_LONG || messages_rx_buffer.type==DATA_SHORT)
			{
				add_message_rx_data(messages_rx_buffer.type, messages_rx_buffer.id, messages_rx_buffer.length, messages_rx_buffer.data);
				set_receiving_timeout((data_batch_size-messages_rx_buffer.sequence_number)*message_transmission_time_ms);
				receiving_timer.start();
			}
			messages_rx_buffer.status=FREE;
			link_timer.start();
			gear_shift_timer.start();
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
		pad_messages_batch_tx(ack_batch_size);

		send_batch();

		messages_control.status=FREE;
		connection_status=RECEIVING;
		connection_id=assigned_connection_id;

		if (messages_control.data[0]==SWITCH_ROLE)
		{
			set_role(COMMANDER);
			this->link_status=CONNECTED;
			this->connection_status=TRANSMITTING_DATA;
			set_receiving_timeout((ack_batch_size+1.5)*message_transmission_time_ms);
			connection_timer.stop();
			connection_timer.reset();
			link_timer.start();
			last_message_received_type=NONE;
			last_message_sent_type=NONE;
			last_received_message_sequence=-1;
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
		messages_batch_ack[message_batch_counter_tx].type=messages_last_ack_bu.type;
		messages_batch_ack[message_batch_counter_tx].id=messages_last_ack_bu.id;
		messages_batch_ack[message_batch_counter_tx].length=messages_last_ack_bu.length;
		for(int i=0;i<messages_batch_ack[message_batch_counter_tx].length;i++)
		{
			messages_batch_ack[message_batch_counter_tx].data[i]=messages_last_ack_bu.data[i];
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
	pad_messages_batch_tx(ack_batch_size);
	send_batch();
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
		int call_for_me=1;
		if(messages_control.data[2]!=(char)my_call_sign.length())
		{
			call_for_me=0;
		}
		else
		{
			for(int i=0;i<messages_control.data[2];i++)
			{
				if(my_call_sign[i]!=messages_control.data[messages_control.data[1]+3+i])
				{
					call_for_me=0;
				}
			}
		}
		if(call_for_me==1)
		{
			destination_call_sign="";
			for(int i=0;i<messages_control.data[1];i++)
			{
				destination_call_sign+=messages_control.data[3+i];
			}

			link_status=CONNECTION_RECEIVED;
			connection_status=ACKNOWLEDGING_CONTROL;
			messages_control.data[1]=rand()%0xff;
			messages_control.length=2;
			assigned_connection_id=messages_control.data[1];
			connection_timer.start();
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
		link_status=CONNECTED;
		connection_status=ACKNOWLEDGING_CONTROL;
		connection_timer.stop();
		connection_timer.reset();
		connection_timer.reset();
		link_timer.start();

		std::string str="CONNECTED "+this->destination_call_sign+" "+this->my_call_sign+" "+ std::to_string(telecom_system->bandwidth)+"\r";
		tcp_socket_control.message->length=str.length();

		for(int i=0;i<tcp_socket_control.message->length;i++)
		{
			tcp_socket_control.message->buffer[i]=str[i];
		}
		tcp_socket_control.transmit();
	}
	else if(link_status==CONNECTED)
	{
		if(code==CLOSE_CONNECTION)
		{
			assigned_connection_id=0;
			link_status=LISTENING;
			connection_status=RECEIVING;
			link_timer.stop();
			link_timer.reset();
			connection_timer.stop();
			connection_timer.reset();
			load_configuration(CONFIG_0);
			gear_shift_timer.stop();
			gear_shift_timer.reset();

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
		else if(code==SET_CONFIG)
		{
			if(messages_control.data[1]!= current_configuration)
			{
				messages_control_backup();
				load_configuration(messages_control.data[1]);
				messages_control_restore();
			}

			connection_status=ACKNOWLEDGING_CONTROL;
			link_timer.start();
			gear_shift_timer.start();
		}
		else if(code==BLOCK_END)
		{
			connection_status=ACKNOWLEDGING_CONTROL;
			std::cout<<"end of block"<<std::endl;
			copy_data_to_buffer();
			link_timer.start();
		}
		else if(code==FILE_END)
		{
			connection_status=ACKNOWLEDGING_CONTROL;
			std::cout<<"end of file"<<std::endl;
			copy_data_to_buffer();
			link_timer.start();
		}
		else if(code==SWITCH_ROLE)
		{
			connection_status=ACKNOWLEDGING_CONTROL;
			std::cout<<"switch role"<<std::endl;
			copy_data_to_buffer();
			link_timer.start();
			// Received data test code
			//		char data,data2;
			//		int error=NO;
			//		srand(5);
			//		int nRec= fifo_buffer_rx.get_size()-fifo_buffer_rx.get_free_size();
			//		std::cout<<"nRec= "<<nRec<<std::endl;
			//		for(int i=0;i<nRec;i++)
			//		{
			//			fifo_buffer_rx.pop(&data, 1);
			//			data2=(char)(rand()%0xff);
			//			if(data!=data2)
			//			{
			//				std::cout<<"error @" <<i<<" data="<<(int)data<<" data2="<<(int)data2<<std::endl;
			//				error=YES;
			//			}
			//		}
			//		if(error==YES)
			//		{
			//			exit(0);
			//		}
		}
		else if(code==REPEAT_LAST_ACK)
		{
			repeating_last_ack=YES;
			connection_status=ACKNOWLEDGING_DATA;
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
				tcp_socket_data.message->length=fifo_buffer_rx.pop(tcp_socket_data.message->buffer,max_data_length);
				tcp_socket_data.transmit();
			}
		}

	}
}


