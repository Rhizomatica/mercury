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

	if(disconnect_requested==YES)
	{
		if(this->link_status==CONNECTED)
		{
			disconnect_requested=NO;
			this->link_status=DISCONNECTING;
			messages_control.status=FREE;
			add_message_control(CLOSE_CONNECTION);
		}
		else
		{
			disconnect_requested=NO;
			load_configuration(init_configuration,FULL,YES);
			this->link_status=LISTENING;
			this->connection_status=RECEIVING;
			watchdog_timer.stop();
			watchdog_timer.reset();
			link_timer.stop();
			link_timer.reset();
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
	int success=ERROR_;
	if (messages_control.status==FREE)
	{
		messages_control.type=CONTROL;
		messages_control.nResends=this->nResends;
		messages_control.ack_timeout=this->ack_timeout_control;
		messages_control.status=ADDED_TO_LIST;

		if(code==START_CONNECTION)
		{
			messages_control.data[0]=code;
			messages_control.data[1]=CRC8_calc((char*)destination_call_sign.c_str(), destination_call_sign.length());
			int my_call_sign_sent_chars=0;

			for(int j=0;j<(int)my_call_sign.length();j++)
			{
				messages_control.data[j+3]=my_call_sign[j];
				my_call_sign_sent_chars++;
				if(my_call_sign_sent_chars+3>=(max_data_length+max_header_length-CONTROL_ACK_CONTROL_HEADER_LENGTH))
				{
					break;
				}
			}
			messages_control.data[2]=my_call_sign_sent_chars;
			messages_control.length=my_call_sign_sent_chars+3;
			messages_control.id=0;
			connection_id=BROADCAST_ID;
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

			if(gear_shift_algorithm==SNR_BASED)
			{
				negotiated_configuration= get_configuration(measurements.SNR_downlink);
			}
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
			// Increment connection attempts counter if trying to connect
			if((link_status==CONNECTING || link_status==NEGOTIATING || link_status==CONNECTION_ACCEPTED) &&
			   messages_control.data[0]==START_CONNECTION)
			{
				connection_attempts++;
				std::cout<<"Connection attempt "<<connection_attempts<<" of "<<max_connection_attempts<<std::endl;
			}

			messages_batch_tx[message_batch_counter_tx]=messages_control;
			message_batch_counter_tx++;
			messages_control.status=ADDED_TO_BATCH_BUFFER;
			stats.nReSent_control++;
		}
		else
		{
			stats.nLost_control++;
			messages_control.status=FAILED_;
		}
	}

	if(messages_control.status==ADDED_TO_BATCH_BUFFER)
	{

		pad_messages_batch_tx(control_batch_size);
		send_batch();
		connection_status=RECEIVING_ACKS_CONTROL;

		load_configuration(ack_configuration, PHYSICAL_LAYER_ONLY,NO);

		receiving_timer.start();

		if(messages_control.data[0]==SET_CONFIG)
		{
			if(negotiated_configuration!= current_configuration)
			{
				data_configuration=negotiated_configuration;
				gear_shift_timer.start();
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
		}
	}
}

int cl_arq_controller::add_message_tx_data(char type, int length, char* data)
{
	int success=ERROR_;
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
				last_transmission_block_stats.nSent_data++;
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
					last_transmission_block_stats.nReSent_data++;
				}
			}
			else
			{
				stats.nLost_data++;
				messages_tx[i].status=FAILED_;
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
		load_configuration(ack_configuration, PHYSICAL_LAYER_ONLY,NO);
		receiving_timer.start();
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
				for(int j=0;j<(max_data_length+max_header_length-CONTROL_ACK_CONTROL_HEADER_LENGTH);j++)
				{
					messages_control.data[j]=messages_rx_buffer.data[j];
				}
				link_timer.start();
				watchdog_timer.start();
				gear_shift_timer.stop();
				gear_shift_timer.reset();
				messages_control.status=ACKED;
				stats.nAcked_control++;
			}
		}
		messages_rx_buffer.status=FREE;
	}
	else
	{
		load_configuration(data_configuration, PHYSICAL_LAYER_ONLY,YES);
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
			watchdog_timer.start();
			gear_shift_timer.stop();
			gear_shift_timer.reset();
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
		load_configuration(data_configuration, PHYSICAL_LAYER_ONLY,YES);
		add_message_control(REPEAT_LAST_ACK);
	}
	else
	{
		load_configuration(data_configuration, PHYSICAL_LAYER_ONLY,YES);
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
			watchdog_timer.start();
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

			switch_role_test_timer.stop();
			switch_role_test_timer.reset();

			if(gear_shift_on==YES)
			{
				if(gear_shift_algorithm==SNR_BASED)
				{
					this->link_status=NEGOTIATING;
					connection_status=TRANSMITTING_CONTROL;
					link_timer.start();
					watchdog_timer.start();
					gear_shift_timer.start();
				}
			}
			else
			{
				if(this->link_status==CONNECTION_ACCEPTED)
				{
					std::string str="CONNECTED "+this->my_call_sign+" "+this->destination_call_sign+" "+ std::to_string(telecom_system->bandwidth)+"\r";
					tcp_socket_control.message->length=str.length();

					for(int i=0;i<tcp_socket_control.message->length;i++)
					{
						tcp_socket_control.message->buffer[i]=str[i];
					}
					tcp_socket_control.transmit();
				}

				this->link_status=CONNECTED;
				this->connection_status=TRANSMITTING_DATA;
				watchdog_timer.start();
				link_timer.start();
			}

		}
		else if(this->link_status==NEGOTIATING && messages_control.data[0]==SET_CONFIG)
		{
			this->link_status=CONNECTED;
			this->connection_status=TRANSMITTING_DATA;
			link_timer.start();
			watchdog_timer.start();
			gear_shift_timer.stop();
			gear_shift_timer.reset();

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
			if (messages_control.data[0]==FILE_END_)
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

				last_transmission_block_stats.success_rate_data=100*(1-((float)last_transmission_block_stats.nReSent_data/(float)last_transmission_block_stats.nSent_data));
				last_transmission_block_stats.nReSent_data=0;
				last_transmission_block_stats.nSent_data=0;
				std::string str="BUFFER ";
				str+=std::to_string(fifo_buffer_tx.get_size()-fifo_buffer_tx.get_free_size());
				str+='\r';
				for(long unsigned int i=0;i<str.length();i++)
				{
					tcp_socket_control.message->buffer[i]=str[i];
				}
				tcp_socket_control.message->length=str.length();
				tcp_socket_control.transmit();

				if(gear_shift_on==YES)
				{
					if(gear_shift_algorithm==SNR_BASED)
					{
						cleanup();
						add_message_control(TEST_CONNECTION);
					}
					else if(gear_shift_algorithm==SUCCESS_BASED_LADDER)
					{
						gear_shift_blocked_for_nBlocks++;
						if(last_transmission_block_stats.success_rate_data>gear_shift_up_success_rate_precentage && gear_shift_blocked_for_nBlocks>= gear_shift_block_for_nBlocks_total)
						{
							if(current_configuration!=(NUMBER_OF_CONFIGS-1))
							{
								negotiated_configuration=current_configuration+1;
								cleanup();
								add_message_control(SET_CONFIG);
							}
						}
						else if(last_transmission_block_stats.success_rate_data<gear_shift_down_success_rate_precentage)
						{
							if(current_configuration!=0)
							{
								negotiated_configuration=current_configuration-1;
								cleanup();
								add_message_control(SET_CONFIG);
							}
							gear_shift_blocked_for_nBlocks=0;
						}
						else
						{
							this->connection_status=TRANSMITTING_DATA;
						}
					}
				}
				else
				{
					this->connection_status=TRANSMITTING_DATA;
				}

			}
			else if (messages_control.data[0]==SWITCH_ROLE)
			{
				set_role(RESPONDER);
				this->link_status=CONNECTED;
				this->connection_status=RECEIVING;
				watchdog_timer.start();
				link_timer.start();
				last_message_received_type=NONE;
				last_message_sent_type=NONE;
				last_received_message_sequence=-1;
			}
			else if (messages_control.data[0]==SET_CONFIG)
			{
				this->connection_status=TRANSMITTING_DATA;
				watchdog_timer.start();
				link_timer.start();
			}
		}
		else if(this->link_status==DISCONNECTING && messages_control.data[0]==CLOSE_CONNECTION)
		{
			load_configuration(init_configuration,FULL,YES);
			this->link_status=LISTENING;
			this->connection_status=RECEIVING;
			watchdog_timer.stop();
			watchdog_timer.reset();
			link_timer.stop();
			link_timer.reset();
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

void cl_arq_controller::process_buffer_data_commander()
{
	int data_read_size;
	if(role==COMMANDER && link_status==CONNECTED && connection_status==TRANSMITTING_DATA)
	{
		if( fifo_buffer_tx.get_size()!=fifo_buffer_tx.get_free_size() && block_under_tx==NO)
		{
			for(int i=0;i<get_nTotal_messages();i++)
			{
				data_read_size=fifo_buffer_tx.pop(message_TxRx_byte_buffer,max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH);
				if(data_read_size==0)
				{
					last_transmission_block_stats.nSent_data=0;
					last_transmission_block_stats.nReSent_data=0;
					break;
				}
				else if(data_read_size==max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH)
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
