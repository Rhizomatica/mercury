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

void cl_arq_controller::register_ack(int message_id)
{
	if(message_id>=0 && message_id<this->nMessages && messages_tx[message_id].status==PENDING_ACK)
	{
		messages_tx[message_id].status=ACKED;
		stats.nAcked_data++;
	}
}


void cl_arq_controller::process_messages_commander()
{
	if(this->link_status==CONNECTING)
	{
		add_message_control(START_CONNECTION);
	}
	else if(this->link_status==CONNECTION_ACCEPTED)
	{
		add_message_control(TEST_CONNECTION);
	}
	else if (this->link_status==NEGOTIATING)
	{
		add_message_control(SET_CONFIG);
	}
	else if(this->link_status==DISCONNECTING)
	{
		add_message_control(CLOSE_CONNECTION);
	}


	if(this->connection_status==TRANSMITTING_CONTROL)
	{
		print_stats();
		process_messages_tx_control();
	}
	else if(this->connection_status==RECEIVING_ACKS_CONTROL)
	{
		process_messages_rx_acks_control();
	}
	else if(this->connection_status==TRANSMITTING_DATA)
	{
		print_stats();
		process_messages_tx_data();
	}
	else if(this->connection_status==RECEIVING_ACKS_DATA)
	{
		process_messages_rx_acks_data();
	}
}

int cl_arq_controller::add_message_control(char code)
{
	int success=ERROR;
	if (messages_control.status==FREE)
	{
		messages_control.type=CONTROL;
		messages_control.nResends=this->nResends;
		messages_control.ack_timeout=this->ack_timeout_control;
		messages_control.status=ADDED_TO_LIST;

		if(code==START_CONNECTION)
		{
			messages_control.length=my_call_sign.length()+destination_call_sign.length()+3;

			messages_control.data[0]=code;
			messages_control.data[1]=my_call_sign.length();
			messages_control.data[2]=destination_call_sign.length();

			for(int j=0;j<(int)my_call_sign.length();j++)
			{
				messages_control.data[j+3]=my_call_sign[j];
			}
			for(int j=0;j<(int)destination_call_sign.length();j++)
			{
				messages_control.data[j+my_call_sign.length()+3]=destination_call_sign[j];
			}
			messages_control.id=0;
			connection_timer.start();
		}
		else if(code==TEST_CONNECTION)
		{
			u_SNR tmp_SNR;
			tmp_SNR.f_SNR=(float)measurements.SNR_uplink;

			messages_control.data[0]=code;
			for(int i=0;i<4;i++)
			{
				messages_control.data[i+1]=tmp_SNR.char4_SNR[i];;
			}
			messages_control.length=5;
			messages_control.id=0;
		}
		else if(code==SET_CONFIG)
		{
			messages_control.length=2;
			messages_control.data[0]=code;
			messages_control.id=0;

			negotiated_configuration= get_configuration(measurements.SNR_downlink);
			messages_control.data[1]=negotiated_configuration;
		}
		else if(code==REPEAT_LAST_ACK)
		{
			messages_control.length=1;
			messages_control.data[0]=code;
			messages_control.id=0;
			messages_control.nResends=1;
		}
		else
		{
			messages_control.length=1;
			for(int j=0;j<messages_control.length;j++)
			{
				messages_control.data[j]=code;
			}
			messages_control.id=0;
		}

		success=SUCCESSFUL;
		this->connection_status=TRANSMITTING_CONTROL;
	}
	return success;
}

void cl_arq_controller::process_messages_tx_control()
{
	if(messages_control.status==ADDED_TO_LIST&&message_batch_counter_tx<control_batch_size)
	{
		messages_batch_tx[message_batch_counter_tx]=messages_control;
		message_batch_counter_tx++;
		messages_control.status=ADDED_TO_BATCH_BUFFER;
		stats.nSent_control++;
	}
	else if(messages_control.status==ACK_TIMED_OUT)
	{
		if(--messages_control.nResends>0&&message_batch_counter_tx<control_batch_size)
		{
			messages_batch_tx[message_batch_counter_tx]=messages_control;
			message_batch_counter_tx++;
			messages_control.status=ADDED_TO_BATCH_BUFFER;
			stats.nReSent_control++;
		}
		else
		{
			stats.nLost_control++;
			messages_control.status=FAILED;
		}
	}

	if(messages_control.status==ADDED_TO_BATCH_BUFFER)
	{
		pad_messages_batch_tx(ack_batch_size);
		send_batch();
		connection_status=RECEIVING_ACKS_CONTROL;
		receiving_timer.start();
		link_timer.start();


		if(messages_control.data[0]==SET_CONFIG)
		{
			if(negotiated_configuration!= current_configuration)
			{
				messages_control_backup();
				load_configuration(negotiated_configuration);
				messages_control_restore();
			}
			else
			{
				connection_status=TRANSMITTING_DATA;
			}
		}

		if(messages_control.data[0]==REPEAT_LAST_ACK)
		{
			messages_control.ack_timeout=0;
			messages_control.id=0;
			messages_control.length=0;
			messages_control.nResends=0;
			messages_control.status=FREE;
			messages_control.type=NONE;

			connection_status=RECEIVING_ACKS_DATA;
			receiving_timer.start();
			link_timer.start();
		}
	}
}

int cl_arq_controller::add_message_tx_data(char type, int length, char* data)
{
	int success=ERROR;
	if(length<0 || length>max_data_length)
	{
		success=MESSAGE_LENGTH_ERROR;
	}
	else
	{
		for(int i=0;i<nMessages;i++)
		{
			if (messages_tx[i].status==FREE)
			{
				messages_tx[i].type=type;
				messages_tx[i].length=length;
				for(int j=0;j<messages_tx[i].length;j++)
				{
					messages_tx[i].data[j]=data[j];
				}
				messages_tx[i].id=i;
				messages_tx[i].nResends=this->nResends;
				messages_tx[i].ack_timeout=this->ack_timeout_data;
				messages_tx[i].status=ADDED_TO_LIST;
				success=SUCCESSFUL;
				break;
			}
		}
	}
	return success;
}

void cl_arq_controller::process_messages_tx_data()
{
	for(int i=0;i<this->nMessages;i++)
	{
		if(messages_tx[i].status==ADDED_TO_LIST)
		{
			if(message_batch_counter_tx<data_batch_size)
			{
				messages_batch_tx[message_batch_counter_tx]=messages_tx[i];
				message_batch_counter_tx++;
				messages_tx[i].status=ADDED_TO_BATCH_BUFFER;
				stats.nSent_data++;
			}
		}
		else if(messages_tx[i].status==ACK_TIMED_OUT)
		{
			if(--messages_tx[i].nResends>0)
			{
				if(message_batch_counter_tx<data_batch_size)
				{
					messages_batch_tx[message_batch_counter_tx]=messages_tx[i];
					message_batch_counter_tx++;
					messages_tx[i].status=ADDED_TO_BATCH_BUFFER;
					stats.nReSent_data++;
				}
			}
			else
			{
				stats.nLost_data++;
				messages_tx[i].status=FAILED;
			}

		}

		if(message_batch_counter_tx==data_batch_size)
		{
			break;
		}
	}
	if(message_batch_counter_tx<=data_batch_size && message_batch_counter_tx!=0)
	{
		pad_messages_batch_tx(data_batch_size);
		send_batch();
		data_ack_received=NO;
		connection_status=RECEIVING_ACKS_DATA;
		receiving_timer.start();
		link_timer.start();
	}
}

void cl_arq_controller::process_messages_rx_acks_control()
{
	if (receiving_timer.get_elapsed_time_ms()<receiving_timeout)
	{
		this->receive();
		if(messages_rx_buffer.status==RECEIVED && messages_rx_buffer.type==ACK_CONTROL)
		{
			if(messages_rx_buffer.data[0]==messages_control.data[0] && messages_control.status==PENDING_ACK)
			{
				for(int j=0;j<max_data_length;j++)
				{
					messages_control.data[j]=messages_rx_buffer.data[j];
				}
				link_timer.start();
				gear_shift_timer.start();
				messages_control.status=ACKED;
				stats.nAcked_control++;
			}
		}
		messages_rx_buffer.status=FREE;
	}
	else
	{
		if(messages_control.status==ACKED)
		{
			process_control_commander();
		}
		else
		{
			connection_status=TRANSMITTING_CONTROL;
		}

		receiving_timer.stop();
		receiving_timer.reset();
		this->cleanup();
	}
}

void cl_arq_controller::process_messages_rx_acks_data()
{
	if (receiving_timer.get_elapsed_time_ms()<receiving_timeout)
	{
		this->receive();
		if(messages_rx_buffer.status==RECEIVED)
		{
			link_timer.start();
			gear_shift_timer.start();
			if(messages_rx_buffer.type==ACK_RANGE)
			{
				data_ack_received=YES;
				unsigned char start=(unsigned char)messages_rx_buffer.data[0];
				unsigned char end=(unsigned char)messages_rx_buffer.data[1];
				for(unsigned char i=start;i<=end;i++)
				{
					register_ack(i);
				}
			}
			else if(messages_rx_buffer.type==ACK_MULTI)
			{
				data_ack_received=YES;
				for(unsigned char i=0;i<(unsigned char)messages_rx_buffer.data[0];i++)
				{
					register_ack((unsigned char)messages_rx_buffer.data[i+1]);
				}
			}
			messages_rx_buffer.status=FREE;


			if(messages_control.data[0]==REPEAT_LAST_ACK && messages_control.status==PENDING_ACK)
			{
				this->messages_control.ack_timeout=0;
				this->messages_control.id=0;
				this->messages_control.length=0;
				this->messages_control.nResends=0;
				this->messages_control.status=FREE;
				this->messages_control.type=NONE;
				stats.nAcked_control++;
			}
		}
	}
	else if (data_ack_received==NO && !(last_message_sent_type==CONTROL && last_message_sent_code==REPEAT_LAST_ACK))
	{
		add_message_control(REPEAT_LAST_ACK);
	}
	else
	{
		if (last_message_sent_type==CONTROL && last_message_sent_code==REPEAT_LAST_ACK)
		{
			stats.nNAcked_control++;
		}
		this->cleanup();
		connection_status=TRANSMITTING_DATA;
	}
}

void cl_arq_controller::process_control_commander()
{
	if(this->connection_status==RECEIVING_ACKS_CONTROL)
	{
		if(this->link_status==CONNECTING && messages_control.data[0]==START_CONNECTION)
		{
			connection_timer.start();
			this->link_status=CONNECTION_ACCEPTED;
			connection_status=TRANSMITTING_CONTROL;
			this->connection_id=messages_control.data[1];
			this->assigned_connection_id=messages_control.data[1];
		}
		else if((this->link_status==CONNECTION_ACCEPTED || this->link_status==CONNECTED) && messages_control.data[0]==TEST_CONNECTION)
		{
			u_SNR tmp_SNR;
			for(int i=0;i<4;i++)
			{
				tmp_SNR.char4_SNR[i]=messages_control.data[i+1];

			}
			measurements.SNR_downlink=tmp_SNR.f_SNR;


			if(gear_shift_on==YES)
			{
				this->link_status=NEGOTIATING;
				connection_status=TRANSMITTING_CONTROL;
				connection_timer.stop();
				connection_timer.reset();
				link_timer.start();
				gear_shift_timer.start();
			}
			else
			{
				this->link_status=CONNECTED;
				this->connection_status=TRANSMITTING_DATA;
				connection_timer.stop();
				connection_timer.reset();
				link_timer.start();

				std::string str="CONNECTED "+this->my_call_sign+" "+this->destination_call_sign+" "+ std::to_string(telecom_system->bandwidth)+"\r";
				tcp_socket_control.message->length=str.length();

				for(int i=0;i<tcp_socket_control.message->length;i++)
				{
					tcp_socket_control.message->buffer[i]=str[i];
				}
				tcp_socket_control.transmit();
			}

		}
		else if(this->link_status==NEGOTIATING && messages_control.data[0]==SET_CONFIG)
		{
			this->link_status=CONNECTED;
			this->connection_status=TRANSMITTING_DATA;
			link_timer.start();
			gear_shift_timer.start();

			std::string str="CONNECTED "+this->my_call_sign+" "+this->destination_call_sign+" "+ std::to_string(telecom_system->bandwidth)+"\r";
			tcp_socket_control.message->length=str.length();

			for(int i=0;i<tcp_socket_control.message->length;i++)
			{
				tcp_socket_control.message->buffer[i]=str[i];
			}
			tcp_socket_control.transmit();
		}
		else if(this->link_status==CONNECTED)
		{
			if (messages_control.data[0]==FILE_END)
			{
				this->connection_status=TRANSMITTING_DATA;
				std::cout<<"end of file acked"<<std::endl;
			}
			else if (messages_control.data[0]==BLOCK_END)
			{
				for(int i=0;i<this->nMessages;i++)
				{
					this->messages_tx[i].ack_timeout=0;
					this->messages_tx[i].id=0;
					this->messages_tx[i].length=0;
					this->messages_tx[i].nResends=0;
					this->messages_tx[i].status=FREE;
					this->messages_tx[i].type=NONE;
				}
				block_under_tx=NO;
				fifo_buffer_backup.flush();
				this->connection_status=TRANSMITTING_DATA;
				add_message_control(TEST_CONNECTION);
				std::cout<<"end of block acked"<<std::endl;
			}
			else if (messages_control.data[0]==SWITCH_ROLE)
			{
				set_role(RESPONDER);
				this->link_status=CONNECTED;
				this->connection_status=RECEIVING;
				set_receiving_timeout((data_batch_size+1.5)*message_transmission_time_ms);
				connection_timer.stop();
				connection_timer.reset();
				link_timer.start();
				last_message_received_type=NONE;
				last_message_sent_type=NONE;
				last_received_message_sequence=-1;
			}
		}
		else if(this->link_status==DISCONNECTING && messages_control.data[0]==CLOSE_CONNECTION)
		{
			this->link_status=IDLE;
			connection_timer.stop();
			connection_timer.reset();
			link_timer.stop();
			link_timer.reset();
			gear_shift_timer.stop();
			gear_shift_timer.reset();

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

void cl_arq_controller::process_buffer_data_commander()
{
	int data_read_size;
	if(role==COMMANDER && link_status==CONNECTED )
	{
		if( fifo_buffer_tx.get_size()!=fifo_buffer_tx.get_free_size() && block_under_tx==NO)
		{
			for(int i=0;i<get_nTotal_messages();i++)
			{
				data_read_size=fifo_buffer_tx.pop(message_TxRx_byte_buffer,max_data_length);
				if(data_read_size==0)
				{
					break;
				}
				else if(data_read_size==max_data_length)
				{
					block_under_tx=YES;
					add_message_tx_data(DATA_LONG, data_read_size, message_TxRx_byte_buffer);
					fifo_buffer_backup.push(message_TxRx_byte_buffer,data_read_size);
				}
				else
				{
					block_under_tx=YES;
					add_message_tx_data(DATA_SHORT, data_read_size, message_TxRx_byte_buffer);
					fifo_buffer_backup.push(message_TxRx_byte_buffer,data_read_size);
				}
			}
		}
		else if(block_under_tx==YES && message_batch_counter_tx==0 && get_nOccupied_messages()==0 && messages_control.status==FREE)
		{
			add_message_control(BLOCK_END);
		}
		else if(block_under_tx==NO && message_batch_counter_tx==0 && get_nOccupied_messages()==0 && messages_control.status==FREE)
		{
			if(switch_role_timer.counting==NO)
			{
				switch_role_timer.reset();
				switch_role_timer.start();
			}
			else if(switch_role_timer.get_elapsed_time_ms()>switch_role_timeout)
			{
				switch_role_timer.stop();
				switch_role_timer.reset();
				add_message_control(SWITCH_ROLE);
			}
		}
	}
}
