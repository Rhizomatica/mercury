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
#include "audioio/audioio.h"

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

cl_arq_controller::cl_arq_controller()
{
	connection_status=IDLE;
	link_status=IDLE;
	nMessages=0;
	max_message_length=0;
	max_data_length=0;
	max_header_length=0;

	messages_last_ack_bu.data=NULL;
	messages_control.data=NULL;
	messages_rx_buffer.data=NULL;
	messages_tx=NULL;
	messages_rx=NULL;
	messages_batch_tx=NULL;
	messages_batch_ack=NULL;
	message_TxRx_byte_buffer=NULL;

	message_batch_counter_tx=0;
	ack_timeout_data=1000;
	ack_timeout_control=1000;
	link_timeout=10000;
	watchdog_timeout=1000;
	receiving_timeout=10000;
	switch_role_timeout=1000;
	switch_role_test_timeout=1000;
	gearshift_timeout=1000;
	nResends=3;
	stats.nSent_data=0;
	stats.nAcked_data=0;
	stats.nReceived_data=0;
	stats.nLost_data=0;
	stats.nReSent_data=0;
	stats.nAcks_sent_data=0;
	stats.nNAcked_data=0;

	stats.nSent_control=0;
	stats.nAcked_control=0;
	stats.nReceived_control=0;
	stats.nLost_control=0;
	stats.nReSent_control=0;
	stats.nAcks_sent_control=0;
	stats.nNAcked_control=0;
	stats.success_rate_data=0;


	last_transmission_block_stats.nSent_data=0;
	last_transmission_block_stats.nAcked_data=0;
	last_transmission_block_stats.nReceived_data=0;
	last_transmission_block_stats.nLost_data=0;
	last_transmission_block_stats.nReSent_data=0;
	last_transmission_block_stats.nAcks_sent_data=0;
	last_transmission_block_stats.nNAcked_data=0;

	last_transmission_block_stats.nSent_control=0;
	last_transmission_block_stats.nAcked_control=0;
	last_transmission_block_stats.nReceived_control=0;
	last_transmission_block_stats.nLost_control=0;
	last_transmission_block_stats.nReSent_control=0;
	last_transmission_block_stats.nAcks_sent_control=0;
	last_transmission_block_stats.nNAcked_control=0;
	last_transmission_block_stats.success_rate_data=0;

	measurements.SNR_uplink=-99.9;
	measurements.SNR_downlink=-99.9;
	measurements.signal_stregth_dbm=-99.9;
	measurements.frequency_offset=-99.9;

	data_batch_size=1;
	control_batch_size=1;
	ack_batch_size=1;
	message_transmission_time_ms=500;
	role=RESPONDER;
	original_role=RESPONDER;
	connection_id=0;
	assigned_connection_id=0;
	block_ready=0;
	block_under_tx=NO;
	ack_batch_size=1;
	my_call_sign="";
	destination_call_sign="";
	user_command_buffer="";
	telecom_system=NULL;

	print_stats_frequency_hz=2;

	init_configuration=CONFIG_0;
	current_configuration=CONFIG_0;
	negotiated_configuration=CONFIG_0;
	last_data_configuration=CONFIG_0;
	ack_configuration=CONFIG_0;
	data_configuration=CONFIG_0;

	gear_shift_on=NO;
	gear_shift_algorithm=SUCCESS_BASED_LADDER;

	gear_shift_up_success_rate_precentage=70;
	gear_shift_down_success_rate_precentage=40;
	gear_shift_block_for_nBlocks_total=0;
	gear_shift_blocked_for_nBlocks=0;

	ptt_on_delay_ms=0;
	ptt_off_delay_ms=0;
	time_left_to_send_last_frame=0;

	last_message_sent_type=NONE;
	last_message_sent_code=NONE;

	last_message_received_type=NONE;
	last_message_received_code=NONE;

	last_received_message_sequence=255;
	data_ack_received=NO;
	repeating_last_ack=NO;
	disconnect_requested=NO;

	this->messages_control_bu.status=FREE;
	this->messages_control_bu.data=NULL;
	this->messages_control_bu.data=new char[255];
	if(this->messages_control_bu.data==NULL)
	{
		exit(MEMORY_ERROR);
	}

}



cl_arq_controller::~cl_arq_controller()
{
	if(messages_control_bu.data!=NULL)
	{
		delete[] messages_control_bu.data;
	}
	this->deinit_messages_buffers();
}


void cl_arq_controller::set_nResends(int nResends)
{
	if(nResends>0)
	{
		this->nResends=nResends;
	}
}


void cl_arq_controller::set_ack_timeout_control(int ack_timeout_control)
{
	if(ack_timeout_control>0)
	{
		this->ack_timeout_control=ack_timeout_control;
	}

	if(messages_control.status==PENDING_ACK)
	{
		messages_control.ack_timeout=this->ack_timeout_control;
	}
	if(messages_control_bu.status==PENDING_ACK)
	{
		messages_control_bu.ack_timeout=this->ack_timeout_control;
	}
}

void cl_arq_controller::set_ack_timeout_data(int ack_timeout_data)
{
	if(ack_timeout_data>0)
	{
		this->ack_timeout_data=ack_timeout_data;
	}

	if(messages_tx!=NULL)
	{
		for(int i=0;i<nMessages;i++)
		{
			if(messages_tx[i].status==PENDING_ACK)
			{
				messages_tx[i].ack_timeout=this->ack_timeout_data;
			}
		}
	}
}

void cl_arq_controller::set_receiving_timeout(int receiving_timeout)
{
	if(receiving_timeout>0)
	{
		this->receiving_timeout=receiving_timeout;
	}
}

void cl_arq_controller::set_link_timeout(int link_timeout)
{
	if(link_timeout>0)
	{
		this->link_timeout=link_timeout;
	}
}


void cl_arq_controller::set_nMessages(int nMessages)
{
	if(nMessages>0 && nMessages<256)
	{
		this->nMessages=nMessages;
	}
	else
	{
		this->nMessages=255;
	}
}

void cl_arq_controller::set_max_buffer_length(int max_data_length, int max_message_length, int max_header_length)
{
	if(max_data_length>0 && max_data_length<256 && max_data_length<max_message_length)
	{
		this->max_data_length=max_data_length;
	}

	if(max_message_length>0 && max_message_length<256)
	{
		this->max_message_length=max_message_length;
	}

	if(max_header_length>0)
	{
		this->max_header_length=max_header_length;
	}
}

void cl_arq_controller::set_ack_batch_size(int ack_batch_size)
{
	if (ack_batch_size>0)
	{
		this->ack_batch_size=ack_batch_size;
	}
}

void cl_arq_controller::set_data_batch_size(int data_batch_size)
{
	if (data_batch_size>0)
	{
		if(data_batch_size<(max_data_length+max_header_length-ACK_MULTI_ACK_RANGE_HEADER_LENGTH-1))
		{
			this->data_batch_size=data_batch_size;
		}
		else
		{
			this->data_batch_size=(max_data_length+max_header_length-ACK_MULTI_ACK_RANGE_HEADER_LENGTH-1);
		}
	}
}

void cl_arq_controller::set_control_batch_size(int control_batch_size)
{
	if (control_batch_size>0)
	{
		this->control_batch_size=control_batch_size;
	}
}

void cl_arq_controller::set_role(int role)
{
	if(role==COMMANDER)
	{
		this->role=COMMANDER;
	}
	else
	{
		this->role=RESPONDER;
	}
	calculate_receiving_timeout();
}

void cl_arq_controller::calculate_receiving_timeout()
{
	if(this->role==COMMANDER)
	{
		set_receiving_timeout((ack_batch_size+1)*message_transmission_time_ms+time_left_to_send_last_frame+ptt_on_delay_ms);
	}
	else
	{
		set_receiving_timeout((data_batch_size)*message_transmission_time_ms+time_left_to_send_last_frame+ptt_on_delay_ms);
	}
}

void cl_arq_controller::set_call_sign(std::string call_sign)
{
	if(call_sign!= "")
	{
		this->my_call_sign=call_sign;
	}
}

int cl_arq_controller::get_nOccupied_messages()
{
	int nOccupied_messages=0;
	for(int i=0;i<this->nMessages;i++)
	{
		if(this->messages_tx[i].status!=FREE)
		{
			nOccupied_messages++;
		}
	}
	return nOccupied_messages;
}

int cl_arq_controller::get_nFree_messages()
{
	int nFree_messages=0;
	for(int i=0;i<this->nMessages;i++)
	{
		if(this->messages_tx[i].status==FREE)
		{
			nFree_messages++;
		}
	}
	return nFree_messages;
}

int cl_arq_controller::get_nTotal_messages()
{
	return this->nMessages;
}

int cl_arq_controller::get_nToSend_messages()
{
	int nMessages_to_send=0;
	for(int i=0;i<this->nMessages;i++)
	{
		if(messages_tx[i].status==ADDED_TO_LIST)
		{
			nMessages_to_send++;
		}
		else if (messages_tx[i].status==ACK_TIMED_OUT && messages_tx[i].nResends>0)
		{
			nMessages_to_send++;
		}
	}
	return nMessages_to_send;
}

int cl_arq_controller::get_nPending_Ack_messages()
{
	int nPending_Ack_messages=0;
	for(int i=0;i<this->nMessages;i++)
	{
		if(this->messages_tx[i].status==PENDING_ACK)
		{
			nPending_Ack_messages++;
		}
	}
	return nPending_Ack_messages;
}

int cl_arq_controller::get_nReceived_messages()
{
	int nReceived_messages=0;
	for(int i=0;i<this->nMessages;i++)
	{
		if(this->messages_rx[i].status==RECEIVED)
		{
			nReceived_messages++;
		}
	}
	return nReceived_messages;
}

int cl_arq_controller::get_nAcked_messages()
{
	int nAcked_messages=0;
	for(int i=0;i<this->nMessages;i++)
	{
		if(this->messages_rx[i].status==ACKED)
		{
			nAcked_messages++;
		}
	}
	return nAcked_messages;
}


void cl_arq_controller::messages_control_backup()
{
	messages_control_bu.ack_timeout=messages_control.ack_timeout;
	messages_control_bu.id=messages_control.id;
	messages_control_bu.length=messages_control.length;
	messages_control_bu.nResends=messages_control.nResends;
	messages_control_bu.status=messages_control.status;
	messages_control_bu.type=messages_control.type;
	for(int i=0;i<max_data_length+max_header_length-CONTROL_ACK_CONTROL_HEADER_LENGTH;i++)
	{
		messages_control_bu.data[i]=messages_control.data[i];
	}
}
void cl_arq_controller::messages_control_restore()
{

	for(int i=0;i<max_data_length+max_header_length-CONTROL_ACK_CONTROL_HEADER_LENGTH;i++)
	{
		messages_control.data[i]=messages_control_bu.data[i];
	}
	messages_control.ack_timeout=messages_control_bu.ack_timeout;
	messages_control.id=messages_control_bu.id;
	messages_control.length=messages_control_bu.length;
	messages_control.nResends=messages_control_bu.nResends;
	messages_control.status=messages_control_bu.status;
	messages_control.type=messages_control_bu.type;
	messages_control.ack_timer.start();
}


int cl_arq_controller::init()
{
	int success=SUCCESSFUL;

	fifo_buffer_tx.set_size(default_configuration_ARQ.fifo_buffer_tx_size);
	fifo_buffer_rx.set_size(default_configuration_ARQ.fifo_buffer_rx_size);
	fifo_buffer_backup.set_size(default_configuration_ARQ.fifo_buffer_backup_size);

	set_link_timeout(default_configuration_ARQ.link_timeout);

	tcp_socket_control.port=default_configuration_ARQ.tcp_socket_control_port;
	tcp_socket_control.timeout_ms=default_configuration_ARQ.tcp_socket_control_timeout_ms;

	tcp_socket_data.port=default_configuration_ARQ.tcp_socket_data_port;
	tcp_socket_data.timeout_ms=default_configuration_ARQ.tcp_socket_data_timeout_ms;

	gear_shift_on=default_configuration_ARQ.gear_shift_on;
	gear_shift_algorithm=default_configuration_ARQ.gear_shift_algorithm;
	current_configuration=CONFIG_NONE;
	init_configuration=default_configuration_ARQ.init_configuration;
	data_configuration=default_configuration_ARQ.init_configuration;
	ack_configuration=default_configuration_ARQ.ack_configuration;

	if(tcp_socket_data.init()!=SUCCESS || tcp_socket_control.init()!=SUCCESS )
	{
		std::cout<<"Erro initializing the TCP sockets. Exiting.."<<std::endl;
		exit(-1);
	}

	load_configuration(ack_configuration,FULL,NO);
	load_configuration(data_configuration,PHYSICAL_LAYER_ONLY,YES);


	print_stats_timer.start();

//	TEST TX data
//		process_user_command("MYCALL rx001");
//		process_user_command("LISTEN ON");
//
//		process_user_command("MYCALL tx001");
//		process_user_command("CONNECT tx001 rx001");


//		std::string str="sent_quest1234";
//		char data;
//
//		for(int i=0;i<str.length();i++)
//		{
//			data=(char)str[i];
//			fifo_buffer_tx.push(&data,1);
//		}

	return success;
}

char cl_arq_controller::get_configuration(double SNR)
{
	char configuration;
	configuration =telecom_system->get_configuration(SNR);
	return configuration;
}

void cl_arq_controller::load_configuration(int configuration, int level, int backup_configuration)
{
	if(configuration==this->current_configuration)
	{
		return;
	}
	if(current_configuration!=CONFIG_NONE)
	{
		if(level==FULL)
		{
			this->restore_backup_buffer_data();
			this->deinit_messages_buffers();
		}
		if(backup_configuration==YES)
		{
			this->last_data_configuration= this->current_configuration;
		}
	}
	else
	{
		if(level==FULL)
		{
			this->deinit_messages_buffers();
		}
		if(backup_configuration==YES)
		{
			this->last_data_configuration=configuration;
		}
	}

	this->current_configuration=configuration;

	telecom_system->load_configuration(configuration);
	int nBytes_header=0;
	if (ACK_MULTI_ACK_RANGE_HEADER_LENGTH>nBytes_header) nBytes_header=ACK_MULTI_ACK_RANGE_HEADER_LENGTH;
	if (CONTROL_ACK_CONTROL_HEADER_LENGTH>nBytes_header) nBytes_header=CONTROL_ACK_CONTROL_HEADER_LENGTH;
	if (DATA_LONG_HEADER_LENGTH>nBytes_header) nBytes_header=DATA_LONG_HEADER_LENGTH;
	if (DATA_SHORT_HEADER_LENGTH>nBytes_header) nBytes_header=DATA_SHORT_HEADER_LENGTH;

	int nBytes_data=(telecom_system->data_container.nBits-telecom_system->ldpc.P-telecom_system->outer_code_reserved_bits)/8 - nBytes_header;
	int nBytes_message=(telecom_system->data_container.nBits)/8 ;


	set_max_buffer_length(nBytes_data, nBytes_message, nBytes_header);
	set_nMessages(default_configuration_ARQ.nMessages);
	set_nResends(default_configuration_ARQ.nResends);

	if(level==FULL)
	{
		set_data_batch_size(default_configuration_ARQ.batch_size);
	}
	set_ack_batch_size(default_configuration_ARQ.ack_batch_size);
	set_control_batch_size(default_configuration_ARQ.control_batch_size);
	
	gear_shift_up_success_rate_precentage=default_configuration_ARQ.gear_shift_up_success_rate_limit_precentage;
	gear_shift_down_success_rate_precentage=default_configuration_ARQ.gear_shift_down_success_rate_limit_precentage;

	gear_shift_block_for_nBlocks_total=default_configuration_ARQ.gear_shift_block_for_nBlocks_total;
	gear_shift_blocked_for_nBlocks=default_configuration_ARQ.gear_shift_block_for_nBlocks_total;

	message_transmission_time_ms=ceil((1000.0*(telecom_system->data_container.Nsymb+telecom_system->data_container.preamble_nSymb)*telecom_system->data_container.Nofdm*telecom_system->frequency_interpolation_rate)/(float)(telecom_system->frequency_interpolation_rate*(telecom_system->bandwidth/telecom_system->ofdm.Nc)*telecom_system->ofdm.Nfft));
    // TODO: audio migration
	// time_left_to_send_last_frame=(float)telecom_system->speaker.frames_to_leave_transmit_fct/(float)(telecom_system->frequency_interpolation_rate*(telecom_system->bandwidth/telecom_system->ofdm.Nc)*telecom_system->ofdm.Nfft);
    time_left_to_send_last_frame=0;

	set_ack_timeout_data((data_batch_size+1+control_batch_size+2*ack_batch_size)*message_transmission_time_ms+time_left_to_send_last_frame+4*ptt_on_delay_ms+4*ptt_off_delay_ms);
	set_ack_timeout_control((control_batch_size+ack_batch_size)*message_transmission_time_ms+time_left_to_send_last_frame+2*ptt_on_delay_ms+2*ptt_off_delay_ms);

	ptt_on_delay_ms=default_configuration_ARQ.ptt_on_delay_ms;
	ptt_off_delay_ms=default_configuration_ARQ.ptt_off_delay_ms;
	switch_role_timeout=default_configuration_ARQ.switch_role_timeout_ms;

	switch_role_test_timeout=(nResends/3)*ack_timeout_control;
	watchdog_timeout=(nResends/3)*ack_timeout_data;
	gearshift_timeout=(nResends/3)*ack_timeout_data;


	calculate_receiving_timeout();
	if(level==FULL)
	{
		this->init_messages_buffers();
	}
}

void cl_arq_controller::return_to_last_configuration()
{
	if(last_data_configuration==this->current_configuration)
	{
		return;
	}
	int tmp;
	this->load_configuration(last_data_configuration,FULL,YES);
	tmp= last_data_configuration;
	last_data_configuration=current_configuration;
	current_configuration=tmp;
}

int cl_arq_controller::init_messages_buffers()
{
	int success=SUCCESSFUL;
	this->messages_tx=new st_message[nMessages];

	if(this->messages_tx==NULL)
	{
		success=MEMORY_ERROR;
	}
	else
	{
		for(int i=0;i<this->nMessages;i++)
		{
			this->messages_tx[i].ack_timeout=0;
			this->messages_tx[i].id=0;
			this->messages_tx[i].length=0;
			this->messages_tx[i].nResends=0;
			this->messages_tx[i].status=FREE;
			this->messages_tx[i].type=NONE;
			this->messages_tx[i].data=NULL;

			this->messages_tx[i].data=new char[max_message_length];
			if(this->messages_tx[i].data==NULL)
			{
				success=MEMORY_ERROR;
			}
		}
	}

	this->messages_rx=new st_message[nMessages];

	if(this->messages_rx==NULL)
	{
		success=MEMORY_ERROR;
	}
	else
	{
		for(int i=0;i<this->nMessages;i++)
		{
			this->messages_rx[i].ack_timeout=0;
			this->messages_rx[i].id=0;
			this->messages_rx[i].length=0;
			this->messages_rx[i].nResends=0;
			this->messages_rx[i].status=FREE;
			this->messages_rx[i].type=NONE;
			this->messages_rx[i].data=NULL;

			this->messages_rx[i].data=new char[max_message_length];
			if(this->messages_rx[i].data==NULL)
			{
				success=MEMORY_ERROR;
			}
		}
	}

	this->messages_batch_tx=new st_message[data_batch_size];

	if(this->messages_batch_tx==NULL)
	{
		success=MEMORY_ERROR;
	}
	else
	{
		for(int i=0;i<this->data_batch_size;i++)
		{
			this->messages_batch_tx[i].ack_timeout=0;
			this->messages_batch_tx[i].id=0;
			this->messages_batch_tx[i].length=0;
			this->messages_batch_tx[i].nResends=0;
			this->messages_batch_tx[i].status=FREE;
			this->messages_batch_tx[i].type=NONE;
			this->messages_batch_tx[i].data=NULL;
		}
	}

	this->messages_batch_ack=new st_message[ack_batch_size];

	if(this->messages_batch_ack==NULL)
	{
		success=MEMORY_ERROR;
	}
	else
	{
		for(int i=0;i<this->ack_batch_size;i++)
		{
			this->messages_batch_ack[i].ack_timeout=0;
			this->messages_batch_ack[i].id=0;
			this->messages_batch_ack[i].length=0;
			this->messages_batch_ack[i].nResends=0;
			this->messages_batch_ack[i].status=FREE;
			this->messages_batch_ack[i].type=NONE;
			this->messages_batch_ack[i].data=NULL;

			this->messages_batch_ack[i].data=new char[max_message_length];
			if(this->messages_batch_ack[i].data==NULL)
			{
				success=MEMORY_ERROR;
			}
		}
	}

	this->messages_last_ack_bu.status=FREE;
	this->messages_last_ack_bu.data=NULL;
	this->messages_last_ack_bu.data=new char[max_message_length];
	if(this->messages_last_ack_bu.data==NULL)
	{
		success=MEMORY_ERROR;
	}

	this->messages_control.status=FREE;
	this->messages_control.data=NULL;
	this->messages_control.data=new char[max_message_length];
	if(this->messages_control.data==NULL)
	{
		success=MEMORY_ERROR;
	}

	this->messages_rx_buffer.status=FREE;
	this->messages_rx_buffer.data=NULL;
	this->messages_rx_buffer.data=new char[max_message_length];
	if(this->messages_rx_buffer.data==NULL)
	{
		success=MEMORY_ERROR;
	}

	this->messages_control.status=FREE;
	this->message_TxRx_byte_buffer=new char[max_message_length];
	if(this->message_TxRx_byte_buffer==NULL)
	{
		success=MEMORY_ERROR;
	}
	return success;
}
int cl_arq_controller::deinit_messages_buffers()
{
	int success=SUCCESSFUL;

	if(messages_tx!=NULL)
	{
		for(int i=0;i<nMessages;i++)
		{
			if(messages_tx[i].data!=NULL)
			{
				delete[] messages_tx[i].data;
			}
		}
		delete[] messages_tx;
	}

	if(messages_rx!=NULL)
	{
		for(int i=0;i<nMessages;i++)
		{
			if(messages_rx[i].data!=NULL)
			{
				delete[] messages_rx[i].data;
			}
		}
		delete[] messages_rx;
	}

	if(messages_batch_ack!=NULL)
	{

		for(int i=0;i<ack_batch_size;i++)
		{
			if(messages_batch_ack[i].data!=NULL)
			{
				delete[] messages_batch_ack[i].data;
			}
		}
		delete[] messages_batch_ack;
	}

	if(messages_last_ack_bu.data!=NULL)
	{
		delete[] messages_last_ack_bu.data;
	}
	if(messages_control.data!=NULL)
	{
		delete[] messages_control.data;
	}
	if(messages_rx_buffer.data!=NULL)
	{
		delete[] messages_rx_buffer.data;
	}
	if(messages_batch_tx!=NULL)
	{
		delete[] messages_batch_tx;
	}
	if(message_TxRx_byte_buffer!=NULL)
	{
		delete[] message_TxRx_byte_buffer;
	}

	return success;
}

void cl_arq_controller::update_status()
{
	for(int i=0;i<nMessages;i++)
	{
		if(messages_tx[i].status==PENDING_ACK && messages_tx[i].ack_timer.get_elapsed_time_ms()>=messages_tx[i].ack_timeout)
		{
			messages_tx[i].status=ACK_TIMED_OUT;
			stats.nNAcked_data++;
		}
	}

	if(messages_control.status==PENDING_ACK && messages_control.ack_timer.get_elapsed_time_ms()>=messages_control.ack_timeout)
	{
		messages_control.status=ACK_TIMED_OUT;
		stats.nNAcked_control++;
	}

	if(link_timer.get_elapsed_time_ms()>=link_timeout)
	{
		this->link_status=DROPPED;
		connection_id=0;
		assigned_connection_id=0;
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

		if(this->role==COMMANDER)
		{
			set_role(RESPONDER);
			link_status=LISTENING;
			connection_status=RECEIVING;
			// SEND TO USER
		}
		else if(this->role==RESPONDER)
		{
			link_status=LISTENING;
			connection_status=RECEIVING;
			// SEND TO USER
		}
	}

	if(watchdog_timer.get_elapsed_time_ms()>= watchdog_timeout)
	{
		if(original_role==COMMANDER)
		{
			set_role(COMMANDER);
			link_status=CONNECTED;
			connection_status=TRANSMITTING_DATA;
			for(int i=0;i<nMessages;i++)
			{
				messages_tx[i].status=FREE;
			}

			int data_read_size;
			for(int i=0;i<get_nTotal_messages();i++)
			{
				data_read_size=fifo_buffer_backup.pop(message_TxRx_byte_buffer,max_data_length+max_header_length);
				if(data_read_size!=0)
				{
					fifo_buffer_tx.push(message_TxRx_byte_buffer,data_read_size);
				}
				else
				{
					break;
				}
			}
			fifo_buffer_backup.flush();

		}
		else if(original_role==RESPONDER)
		{
			set_role(RESPONDER);
			link_status=CONNECTED;
			connection_status=RECEIVING;

			for(int i=0;i<nMessages;i++)
			{
				messages_rx[i].status=FREE;
			}
		}

		last_data_configuration=init_configuration;
		data_configuration=init_configuration;
		load_configuration(data_configuration,PHYSICAL_LAYER_ONLY,YES);

		gear_shift_blocked_for_nBlocks=gear_shift_block_for_nBlocks_total;

		watchdog_timer.stop();
		watchdog_timer.reset();
		watchdog_timer.start();
		gear_shift_timer.stop();
		gear_shift_timer.reset();
		receiving_timer.stop();
		receiving_timer.reset();

	}

	if(gear_shift_on==YES && gear_shift_timer.get_elapsed_time_ms()>=gearshift_timeout)
	{
		gear_shift_timer.stop();
		gear_shift_timer.reset();

		if(gear_shift_algorithm==SNR_BASED)
		{
			messages_control_backup();
			load_configuration(init_configuration,PHYSICAL_LAYER_ONLY,YES);
			messages_control_restore();

			if(this->role==COMMANDER)
			{
				for(int i=0;i<nMessages;i++)
				{
					messages_tx[i].status=FREE;
				}

				int data_read_size;
				for(int i=0;i<get_nTotal_messages();i++)
				{
					data_read_size=fifo_buffer_backup.pop(message_TxRx_byte_buffer,max_data_length+max_header_length);
					if(data_read_size!=0)
					{
						fifo_buffer_tx.push(message_TxRx_byte_buffer,data_read_size);
					}
					else
					{
						break;
					}
				}
				fifo_buffer_backup.flush();

				if (current_configuration!= last_data_configuration)
				{
					add_message_control(TEST_CONNECTION);
					gear_shift_timer.start();
					connection_status=TRANSMITTING_CONTROL;
				}
				else
				{
					connection_status=TRANSMITTING_DATA;
				}
			}
			else if(this->role==RESPONDER)
			{
				for(int i=0;i<nMessages;i++)
				{
					messages_rx[i].status=FREE;
				}

				connection_status=RECEIVING;
			}

		}
		else if(gear_shift_algorithm==SUCCESS_BASED_LADDER)
		{

			messages_control_backup();
			data_configuration=current_configuration-1;
			load_configuration(data_configuration,PHYSICAL_LAYER_ONLY,YES);
			messages_control_restore();

			if(this->role==COMMANDER)
			{
				gear_shift_blocked_for_nBlocks=0;

				for(int i=0;i<nMessages;i++)
				{
					messages_tx[i].status=FREE;
				}

				int data_read_size;
				for(int i=0;i<get_nTotal_messages();i++)
				{
					data_read_size=fifo_buffer_backup.pop(message_TxRx_byte_buffer,max_data_length+max_header_length);
					if(data_read_size!=0)
					{
						fifo_buffer_tx.push(message_TxRx_byte_buffer,data_read_size);
					}
					else
					{
						break;
					}
				}
				fifo_buffer_backup.flush();

				connection_status=TRANSMITTING_DATA;
			}
			else if(this->role==RESPONDER)
			{
				for(int i=0;i<nMessages;i++)
				{
					messages_rx[i].status=FREE;
				}

				connection_status=RECEIVING;
			}
		}
	}

	if(switch_role_test_timer.get_elapsed_time_ms()>switch_role_test_timeout)
	{
		switch_role_test_timer.stop();
		switch_role_test_timer.reset();

		set_role(RESPONDER);
		this->link_status=CONNECTED;
		this->connection_status=RECEIVING;

		this->messages_control.ack_timeout=0;
		this->messages_control.id=0;
		this->messages_control.length=0;
		this->messages_control.nResends=0;
		this->messages_control.status=FREE;
		this->messages_control.type=NONE;

	}


	if(print_stats_timer.get_elapsed_time_ms()>(int)(1000.0/print_stats_frequency_hz))
	{
		print_stats_timer.start();
		print_stats();
	}

}

void cl_arq_controller::cleanup()
{

	if(messages_control.status==ACKED)
	{
		// SEND ACK TO USER
		this->messages_control.ack_timeout=0;
		this->messages_control.id=0;
		this->messages_control.length=0;
		this->messages_control.nResends=0;
		this->messages_control.status=FREE;
		this->messages_control.type=NONE;
	}
	else if(messages_control.status==FAILED_)
	{
		// SEND FAILED TO USER
		this->messages_control.ack_timeout=0;
		this->messages_control.id=0;
		this->messages_control.length=0;
		this->messages_control.nResends=0;
		this->messages_control.status=FREE;
		this->messages_control.type=NONE;
	}


	for(int i=0;i<this->nMessages;i++)
	{
		if(messages_tx[i].status==ACKED)
		{
			// SEND ACK TO USER
			this->messages_tx[i].ack_timeout=0;
			this->messages_tx[i].id=0;
			this->messages_tx[i].length=0;
			this->messages_tx[i].nResends=0;
			this->messages_tx[i].status=FREE;
			this->messages_tx[i].type=NONE;
		}
		else if(messages_tx[i].status==FAILED_)
		{
			// SEND FAILED TO USER
			this->messages_tx[i].ack_timeout=0;
			this->messages_tx[i].id=0;
			this->messages_tx[i].length=0;
			this->messages_tx[i].nResends=0;
			this->messages_tx[i].status=FREE;
			this->messages_tx[i].type=NONE;
		}
	}


}


void cl_arq_controller::pad_messages_batch_tx(int size)
{
	int counter=0;
	if(message_batch_counter_tx!=0 && message_batch_counter_tx<size)
	{
		for(int i=0;i<size-message_batch_counter_tx;i++)
		{
			messages_batch_tx[i+message_batch_counter_tx]=messages_batch_tx[counter];
			counter++;
			if(counter>=message_batch_counter_tx)
			{
				counter=0;
			}
		}
		message_batch_counter_tx=size;
	}
}

void cl_arq_controller::process_main()
{
	std::string command="";

	if (tcp_socket_control.get_status()==TCP_STATUS_ACCEPTED)
	{
		if(tcp_socket_control.timer.counting==0)
		{
			tcp_socket_control.timer.start();
		}
		int nBytes_received=tcp_socket_control.receive();
		if(nBytes_received>0)
		{
			tcp_socket_control.timer.start();

			for(int i=0;i<tcp_socket_control.message->length;i++)
			{
				user_command_buffer+=tcp_socket_control.message->buffer[i];
			}
		}
		else if(nBytes_received==0 || (tcp_socket_control.timer.get_elapsed_time_ms()>=tcp_socket_control.timeout_ms && tcp_socket_control.timeout_ms!=INFINITE_))
		{

			fifo_buffer_tx.flush();
			fifo_buffer_backup.flush();
			fifo_buffer_rx.flush();

			tcp_socket_control.check_incomming_connection();
			if (tcp_socket_control.get_status()==TCP_STATUS_ACCEPTED)
			{
				tcp_socket_control.timer.start();
			}

		}
		size_t pos=std::string::npos;
		do
		{
			size_t pos=user_command_buffer.find('\r');
			if(pos!=std::string::npos)
			{
				command=user_command_buffer.substr(0, pos);
				process_user_command(command);
				user_command_buffer=user_command_buffer.substr(pos+1,std::string::npos);
			}
		}while(pos!=std::string::npos);

	}
	else
	{
		tcp_socket_control.check_incomming_connection();
		if (tcp_socket_control.get_status()==TCP_STATUS_ACCEPTED)
		{
			tcp_socket_control.timer.start();
		}
	}


	if (tcp_socket_data.get_status()==TCP_STATUS_ACCEPTED)
	{
		if(tcp_socket_data.timer.counting==0)
		{
			tcp_socket_data.timer.start();
		}
		int nBytes_received=tcp_socket_data.receive();
		if(nBytes_received>0)
		{
			tcp_socket_data.timer.start();
			fifo_buffer_tx.push(tcp_socket_data.message->buffer, tcp_socket_data.message->length);

			std::string str="BUFFER ";
			str+=std::to_string(fifo_buffer_tx.get_size()-fifo_buffer_tx.get_free_size());
			str+='\r';
			for(long unsigned int i=0;i<str.length();i++)
			{
				tcp_socket_control.message->buffer[i]=str[i];
			}
			tcp_socket_control.message->length=str.length();
			tcp_socket_control.transmit();
		}
		else if(nBytes_received==0 || (tcp_socket_data.timer.get_elapsed_time_ms()>=tcp_socket_data.timeout_ms && tcp_socket_data.timeout_ms!=INFINITE_))
		{

			fifo_buffer_tx.flush();
			fifo_buffer_backup.flush();
			fifo_buffer_rx.flush();

			tcp_socket_data.check_incomming_connection();

			if (tcp_socket_data.get_status()==TCP_STATUS_ACCEPTED)
			{
				tcp_socket_data.timer.start();
			}
		}

	}
	else
	{
		tcp_socket_data.check_incomming_connection();
		if (tcp_socket_data.get_status()==TCP_STATUS_ACCEPTED)
		{
			tcp_socket_data.timer.start();
		}
	}


	process_messages();
	usleep(2000);
}

void cl_arq_controller::process_user_command(std::string command)
{

	if(command.substr(0,7)=="MYCALL ")
	{
		this->my_call_sign=command.substr(7);

		tcp_socket_control.message->buffer[0]='O';
		tcp_socket_control.message->buffer[1]='K';
		tcp_socket_control.message->buffer[2]='\r';
		tcp_socket_control.message->length=3;
	}
	else if(command.substr(0,8)=="CONNECT ")
	{
		command=command.substr(8,std::string::npos);
		this->my_call_sign=command.substr(0,command.find(" "));
		this->destination_call_sign=command.substr(my_call_sign.length()+1);
		original_role=COMMANDER;
		set_role(COMMANDER);
		link_status=CONNECTING;
		reset_all_timers();

		tcp_socket_control.message->buffer[0]='O';
		tcp_socket_control.message->buffer[1]='K';
		tcp_socket_control.message->buffer[2]='\r';
		tcp_socket_control.message->length=3;
	}
	else if(command=="DISCONNECT")
	{
		disconnect_requested=YES;

		tcp_socket_control.message->buffer[0]='O';
		tcp_socket_control.message->buffer[1]='K';
		tcp_socket_control.message->buffer[2]='\r';
		tcp_socket_control.message->length=3;
	}
	else if(command=="LISTEN ON")
	{
		original_role=RESPONDER;
		set_role(RESPONDER);
		link_status=LISTENING;
		connection_status=RECEIVING;
		reset_all_timers();

		tcp_socket_control.message->buffer[0]='O';
		tcp_socket_control.message->buffer[1]='K';
		tcp_socket_control.message->buffer[2]='\r';
		tcp_socket_control.message->length=3;
	}
	else if(command=="LISTEN OFF")
	{
		original_role=RESPONDER;
		set_role(RESPONDER);
		link_status=IDLE;
		connection_status=IDLE;
		reset_all_timers();

		tcp_socket_control.message->buffer[0]='O';
		tcp_socket_control.message->buffer[1]='K';
		tcp_socket_control.message->buffer[2]='\r';
		tcp_socket_control.message->length=3;
	}
	else if(command=="BW2300")
	{
		telecom_system->bandwidth=2300;
		load_configuration(data_configuration,FULL,YES);

		tcp_socket_control.message->buffer[0]='O';
		tcp_socket_control.message->buffer[1]='K';
		tcp_socket_control.message->buffer[2]='\r';
		tcp_socket_control.message->length=3;
	}
	else if(command=="BW2500")
	{
		telecom_system->bandwidth=2500;
		load_configuration(data_configuration,FULL,YES);

		tcp_socket_control.message->buffer[0]='O';
		tcp_socket_control.message->buffer[1]='K';
		tcp_socket_control.message->buffer[2]='\r';
		tcp_socket_control.message->length=3;
	}
	else if(command=="BUFFER TX")
	{
		std::string reply="BUFFER ";
		reply+=std::to_string(fifo_buffer_tx.get_size()-fifo_buffer_tx.get_free_size());
		reply+='\r';
		for(long unsigned int i=0;i<reply.length();i++)
		{
			tcp_socket_control.message->buffer[i]=reply[i];
		}
		tcp_socket_control.message->length=reply.length();
	}
	else
	{
		tcp_socket_control.message->buffer[0]='O';
		tcp_socket_control.message->buffer[1]='K';
		tcp_socket_control.message->buffer[2]='\r';
		tcp_socket_control.message->length=3;
	}

	if (tcp_socket_control.get_status()==TCP_STATUS_ACCEPTED)
	{
		tcp_socket_control.transmit();
	}
}

void cl_arq_controller::ptt_on()
{
	std::string str="PTT ON\r";
	tcp_socket_control.message->length=str.length();

	for(int i=0;i<tcp_socket_control.message->length;i++)
	{
		tcp_socket_control.message->buffer[i]=str[i];
	}
	tcp_socket_control.transmit();
}
void cl_arq_controller::ptt_off()
{
	std::string str="PTT OFF\r";
	tcp_socket_control.message->length=str.length();

	for(int i=0;i<tcp_socket_control.message->length;i++)
	{
		tcp_socket_control.message->buffer[i]=str[i];
	}
	tcp_socket_control.transmit();
}



void cl_arq_controller::process_messages()
{
	this->update_status();
	if(this->role==COMMANDER)
	{
		process_messages_commander();
		process_buffer_data_commander();
	}
	else if(this->role==RESPONDER)
	{
		process_messages_responder();
		process_buffer_data_responder();
	}
}

void cl_arq_controller::reset_all_timers()
{
	link_timer.stop();
	link_timer.reset();
	watchdog_timer.stop();
	watchdog_timer.reset();
	gear_shift_timer.stop();
	gear_shift_timer.reset();
	receiving_timer.stop();
	receiving_timer.reset();
	switch_role_timer.stop();
	switch_role_timer.reset();
}


void cl_arq_controller::send(st_message* message, int message_location)
{
	int header_length=0;
	if(message->type==DATA_LONG)
	{
		message_TxRx_byte_buffer[0]=message->type;
		message_TxRx_byte_buffer[1]=connection_id;
		message_TxRx_byte_buffer[2]=message->sequence_number;
		message_TxRx_byte_buffer[3]=message->id;
		header_length=DATA_LONG_HEADER_LENGTH;
	}
	else if (message->type==DATA_SHORT)
	{
		message_TxRx_byte_buffer[0]=message->type;
		message_TxRx_byte_buffer[1]=connection_id;
		message_TxRx_byte_buffer[2]=message->sequence_number;
		message_TxRx_byte_buffer[3]=message->id;
		message_TxRx_byte_buffer[4]=message->length;
		header_length=DATA_SHORT_HEADER_LENGTH;
	}
	else if (message->type==ACK_RANGE || message->type==ACK_MULTI)
	{
		message_TxRx_byte_buffer[0]=message->type;
		message_TxRx_byte_buffer[1]=connection_id;
		message_TxRx_byte_buffer[2]=message->sequence_number;
		header_length=ACK_MULTI_ACK_RANGE_HEADER_LENGTH;
	}
	else if (message->type==CONTROL || message->type==ACK_CONTROL)
	{
		message_TxRx_byte_buffer[0]=message->type;
		message_TxRx_byte_buffer[1]=connection_id;
		message_TxRx_byte_buffer[2]=message->sequence_number;
		header_length=CONTROL_ACK_CONTROL_HEADER_LENGTH;
	}

	for(int i=0;i<message->length;i++)
	{
		message_TxRx_byte_buffer[i+header_length]=message->data[i];
	}

	if(header_length>max_header_length)
	{
		std::cout<<"header size is too big, adjust the configuration parameters"<<std::endl;
		exit(0);
	}

	for(int i=0;i<(header_length+message->length);i++)
	{
		telecom_system->data_container.data_byte[i]=(int)message_TxRx_byte_buffer[i];
	}

	telecom_system->transmit_byte(telecom_system->data_container.data_byte,header_length+message->length,telecom_system->data_container.ready_to_transmit_passband_data_tx,message_location);

	clear_buffer(playback_buffer);

	tx_transfer(telecom_system->data_container.ready_to_transmit_passband_data_tx,
                telecom_system->data_container.Nofdm * telecom_system->data_container.interpolation_rate *
                (telecom_system->data_container.Nsymb + telecom_system->data_container.preamble_nSymb));

	while (size_buffer(playback_buffer) > 0)
		msleep(1);

	clear_buffer(capture_buffer);

	last_message_sent_type=message->type;
	if(message->type==CONTROL || message->type==ACK_CONTROL)
	{
		last_message_sent_code=message->data[0];
	}
	last_received_message_sequence=-1;

	clear_buffer(capture_buffer);
}

void cl_arq_controller::send_batch()
{
	ptt_on();

	cl_timer ptt_on_delay, ptt_off_delay;
	ptt_on_delay.start();

	int frame_output_size = telecom_system->data_container.Nofdm*telecom_system->data_container.interpolation_rate*(telecom_system->data_container.Nsymb+telecom_system->data_container.preamble_nSymb);

	double *batch_frames_output_data=NULL;
	double *batch_frames_output_data_filtered1=NULL;
	double *batch_frames_output_data_filtered2=NULL;

	batch_frames_output_data=new double[(message_batch_counter_tx+2)*frame_output_size];
	batch_frames_output_data_filtered1=new double[(message_batch_counter_tx+2)*frame_output_size];
	batch_frames_output_data_filtered2=new double[(message_batch_counter_tx+2)*frame_output_size];

	if (batch_frames_output_data==NULL)
	{
		exit(-31);
	}
	if (batch_frames_output_data_filtered1==NULL)
	{
		exit(-32);
	}
	if (batch_frames_output_data_filtered2==NULL)
	{
		exit(-33);
	}

	int header_length=0;
	for(int i=0;i<message_batch_counter_tx;i++)
	{
		messages_batch_tx[i].sequence_number=i;

		header_length=0;

		if(messages_batch_tx[i].type==DATA_LONG)
		{
			message_TxRx_byte_buffer[0]=messages_batch_tx[i].type;
			message_TxRx_byte_buffer[1]=connection_id;
			message_TxRx_byte_buffer[2]=messages_batch_tx[i].sequence_number;
			message_TxRx_byte_buffer[3]=messages_batch_tx[i].id;
			header_length=DATA_LONG_HEADER_LENGTH;
		}
		else if (messages_batch_tx[i].type==DATA_SHORT)
		{
			message_TxRx_byte_buffer[0]=messages_batch_tx[i].type;
			message_TxRx_byte_buffer[1]=connection_id;
			message_TxRx_byte_buffer[2]=messages_batch_tx[i].sequence_number;
			message_TxRx_byte_buffer[3]=messages_batch_tx[i].id;
			message_TxRx_byte_buffer[4]=messages_batch_tx[i].length;
			header_length=DATA_SHORT_HEADER_LENGTH;
		}
		else if (messages_batch_tx[i].type==ACK_RANGE || messages_batch_tx[i].type==ACK_MULTI)
		{
			message_TxRx_byte_buffer[0]=messages_batch_tx[i].type;
			message_TxRx_byte_buffer[1]=connection_id;
			message_TxRx_byte_buffer[2]=messages_batch_tx[i].sequence_number;
			header_length=ACK_MULTI_ACK_RANGE_HEADER_LENGTH;
		}
		else if (messages_batch_tx[i].type==CONTROL || messages_batch_tx[i].type==ACK_CONTROL)
		{
			message_TxRx_byte_buffer[0]=messages_batch_tx[i].type;
			message_TxRx_byte_buffer[1]=connection_id;
			message_TxRx_byte_buffer[2]=messages_batch_tx[i].sequence_number;
			header_length=CONTROL_ACK_CONTROL_HEADER_LENGTH;
		}

		for(int j=0;j<messages_batch_tx[i].length;j++)
		{
			message_TxRx_byte_buffer[j+header_length]=messages_batch_tx[i].data[j];
		}

		if(header_length>max_header_length)
		{
			std::cout<<"header size is too big, adjust the configuration parameters"<<std::endl;
			exit(0);
		}

		for(int j=0;j<(header_length+messages_batch_tx[i].length);j++)
		{
			telecom_system->data_container.data_byte[j]=(int)message_TxRx_byte_buffer[j];
		}

		telecom_system->transmit_byte(telecom_system->data_container.data_byte,header_length+messages_batch_tx[i].length,&batch_frames_output_data[(i+1)*frame_output_size],NO_FILTER_MESSAGE);


		last_message_sent_type=messages_batch_tx[i].type;
		if(messages_batch_tx[i].type==CONTROL || messages_batch_tx[i].type==ACK_CONTROL)
		{
			last_message_sent_code=messages_batch_tx[i].data[0];
		}
		last_received_message_sequence=-1;

	}

	for(int i=0;i<frame_output_size;i++) //padding start and end to prepare for filtering
	{
		batch_frames_output_data[(0)*frame_output_size+i]=batch_frames_output_data[(0+1)*frame_output_size+i];
		batch_frames_output_data[(message_batch_counter_tx+1)*frame_output_size+i]=batch_frames_output_data[(message_batch_counter_tx)*frame_output_size+i];
	}

	telecom_system->ofdm.FIR_tx1.apply(batch_frames_output_data,batch_frames_output_data_filtered1,(message_batch_counter_tx+2)*frame_output_size);
	telecom_system->ofdm.FIR_tx2.apply(batch_frames_output_data_filtered1,batch_frames_output_data_filtered2,(message_batch_counter_tx+2)*frame_output_size);

	while(ptt_on_delay.get_elapsed_time_ms()<ptt_on_delay_ms)
		msleep(1);

	clear_buffer(playback_buffer);

	if(messages_batch_tx[0].type==DATA_LONG || messages_batch_tx[0].type==DATA_SHORT)
	{
		tx_transfer(&batch_frames_output_data_filtered2[(0+1)*frame_output_size], frame_output_size); // TODO: aren't we transmitting this message twice?
	}

	for(int i=0;i<message_batch_counter_tx;i++)
	{
		tx_transfer(&batch_frames_output_data_filtered2[(i+1)*frame_output_size], frame_output_size);
	}

	// wait buffer to be played
	while (size_buffer(playback_buffer) > 0)
		msleep(1);

	clear_buffer(capture_buffer);

	ptt_off_delay.start();
	while(ptt_off_delay.get_elapsed_time_ms()<ptt_off_delay_ms)
		msleep(1);

	if (batch_frames_output_data!=NULL)
	{
		delete[] batch_frames_output_data;
		batch_frames_output_data=NULL;
	}
	if (batch_frames_output_data_filtered1!=NULL)
	{
		delete[] batch_frames_output_data_filtered1;
		batch_frames_output_data_filtered1=NULL;
	}
	if (batch_frames_output_data_filtered2!=NULL)
	{
		delete[] batch_frames_output_data_filtered2;
		batch_frames_output_data_filtered2=NULL;
	}

	for(int i=0;i<message_batch_counter_tx;i++)
	{
		if(messages_batch_tx[i].type==DATA_LONG || messages_batch_tx[i].type==DATA_SHORT)
		{
			messages_tx[(int)(unsigned char)messages_batch_tx[i].id].ack_timer.start();
			messages_tx[(int)(unsigned char)messages_batch_tx[i].id].status=PENDING_ACK;
		}
		if(messages_batch_tx[i].type==CONTROL)
		{
			messages_control.ack_timer.start();
			messages_control.status=PENDING_ACK;
		}

		this->messages_batch_tx[i].ack_timeout=0;
		this->messages_batch_tx[i].id=0;
		this->messages_batch_tx[i].length=0;
		this->messages_batch_tx[i].nResends=0;
		this->messages_batch_tx[i].status=FREE;
		this->messages_batch_tx[i].type=NONE;

	}
	message_batch_counter_tx=0;
	ptt_off();
}

void cl_arq_controller::receive()
{
	int signal_period = telecom_system->data_container.Nofdm * telecom_system->data_container.buffer_Nsymb * telecom_system->data_container.interpolation_rate; // in samples
	int symbol_period = telecom_system->data_container.Nofdm * telecom_system->data_container.interpolation_rate;

	int location_of_last_frame = signal_period - symbol_period - 1; // TODO: do we need this "-1"?

	if (size_buffer(capture_buffer) >= symbol_period * sizeof(double))
	{

		if(telecom_system->data_container.data_ready == 1)
			telecom_system->data_container.nUnder_processing_events++;

		shift_left(telecom_system->data_container.passband_delayed_data, signal_period, symbol_period);

		rx_transfer(&telecom_system->data_container.passband_delayed_data[location_of_last_frame], symbol_period);

		telecom_system->data_container.frames_to_read--;
		if(telecom_system->data_container.frames_to_read < 0)
			telecom_system->data_container.frames_to_read = 0;

		telecom_system->data_container.data_ready = 1;
	}

#if 1
	if(telecom_system->data_container.data_ready == 0)
	{
		msleep(1);
		return;
	}
#endif

	st_receive_stats received_message_stats;
	if(telecom_system->data_container.data_ready==1)
	{
		if(telecom_system->data_container.frames_to_read==0)
		{
			for(int i=0;i<telecom_system->data_container.Nofdm*telecom_system->data_container.buffer_Nsymb*telecom_system->data_container.interpolation_rate;i++)
			{
				telecom_system->data_container.ready_to_process_passband_delayed_data[i]=telecom_system->data_container.passband_delayed_data[i];
			}
			received_message_stats=telecom_system->receive_byte(telecom_system->data_container.ready_to_process_passband_delayed_data,telecom_system->data_container.data_byte);

			measurements.signal_stregth_dbm=received_message_stats.signal_stregth_dbm;

			if (received_message_stats.message_decoded==YES)
			{
				int end_of_current_message=received_message_stats.delay/(telecom_system->data_container.Nofdm*telecom_system->data_container.interpolation_rate)+telecom_system->data_container.Nsymb+telecom_system->data_container.preamble_nSymb;
				int frames_left_in_buffer=telecom_system->data_container.buffer_Nsymb-end_of_current_message;
				if(frames_left_in_buffer<0) {frames_left_in_buffer=0;}

				telecom_system->data_container.frames_to_read=telecom_system->data_container.Nsymb+telecom_system->data_container.preamble_nSymb-frames_left_in_buffer-telecom_system->data_container.nUnder_processing_events;

				if(telecom_system->data_container.frames_to_read>(telecom_system->data_container.Nsymb+telecom_system->data_container.preamble_nSymb) || telecom_system->data_container.frames_to_read<0)
				{
					telecom_system->data_container.frames_to_read=telecom_system->data_container.Nsymb+telecom_system->data_container.preamble_nSymb-frames_left_in_buffer;
				}

				telecom_system->receive_stats.delay_of_last_decoded_message+=(telecom_system->data_container.Nsymb+telecom_system->data_container.preamble_nSymb-(telecom_system->data_container.frames_to_read+telecom_system->data_container.nUnder_processing_events))*telecom_system->data_container.Nofdm*telecom_system->data_container.interpolation_rate;

				telecom_system->data_container.nUnder_processing_events=0;

				measurements.frequency_offset=received_message_stats.freq_offset;
				if(this->role==COMMANDER)
				{
					measurements.SNR_uplink=received_message_stats.SNR;
				}
				else
				{
					measurements.SNR_downlink=received_message_stats.SNR;
				}

				for(int i=0;i<this->max_data_length+this->max_header_length;i++)
				{
					message_TxRx_byte_buffer[i]=(char)telecom_system->data_container.data_byte[i];
				}

				if(message_TxRx_byte_buffer[1]==this->connection_id || message_TxRx_byte_buffer[1]==BROADCAST_ID)
				{
					messages_rx_buffer.status=RECEIVED;
					messages_rx_buffer.type=message_TxRx_byte_buffer[0];
					messages_rx_buffer.sequence_number=message_TxRx_byte_buffer[2];
					last_received_message_sequence=messages_rx_buffer.sequence_number;
					if(messages_rx_buffer.type==ACK_CONTROL  ||  messages_rx_buffer.type==CONTROL)
					{
						for(int j=0;j<max_data_length+max_header_length-CONTROL_ACK_CONTROL_HEADER_LENGTH;j++)
						{
							messages_rx_buffer.data[j]=message_TxRx_byte_buffer[j+CONTROL_ACK_CONTROL_HEADER_LENGTH];
						}
					}
					if( messages_rx_buffer.type==ACK_MULTI || messages_rx_buffer.type==ACK_RANGE)
					{
						for(int j=0;j<max_data_length+max_header_length-ACK_MULTI_ACK_RANGE_HEADER_LENGTH;j++)
						{
							messages_rx_buffer.data[j]=message_TxRx_byte_buffer[j+ACK_MULTI_ACK_RANGE_HEADER_LENGTH];
						}
					}
					else if(messages_rx_buffer.type==DATA_LONG)
					{
						messages_rx_buffer.id=message_TxRx_byte_buffer[3];
						messages_rx_buffer.length=max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH;
						for(int j=0;j<max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH;j++)
						{
							messages_rx_buffer.data[j]=message_TxRx_byte_buffer[j+DATA_LONG_HEADER_LENGTH];
						}
					}
					else if(messages_rx_buffer.type==DATA_SHORT)
					{
						messages_rx_buffer.id=message_TxRx_byte_buffer[3];
						messages_rx_buffer.length=(unsigned char)message_TxRx_byte_buffer[4];
						for(int j=0;j<messages_rx_buffer.length;j++)
						{
							messages_rx_buffer.data[j]=message_TxRx_byte_buffer[j+DATA_SHORT_HEADER_LENGTH];
						}
					}

					last_message_received_type=messages_rx_buffer.type;
					if(messages_rx_buffer.type==CONTROL || messages_rx_buffer.type==ACK_CONTROL)
					{
						last_message_received_code=messages_rx_buffer.data[0];
					}
				}
			}
			else
			{
				if(telecom_system->data_container.data_ready==1 && telecom_system->data_container.frames_to_read==0 && telecom_system->receive_stats.delay_of_last_decoded_message!=-1)
				{
					telecom_system->receive_stats.delay_of_last_decoded_message-=telecom_system->data_container.Nofdm*telecom_system->data_container.interpolation_rate;
					if(telecom_system->receive_stats.delay_of_last_decoded_message<0)
					{
						telecom_system->receive_stats.delay_of_last_decoded_message=-1;
					}
				}
			}
		}
		telecom_system->data_container.data_ready=0;
	}
}


void cl_arq_controller::copy_data_to_buffer()
{
	for(int i=0;i<this->nMessages;i++)
	{
		if(messages_rx[i].status==ACKED)
		{
			fifo_buffer_rx.push(messages_rx[i].data,messages_rx[i].length);
			messages_rx[i].status=FREE;
		}
	}
	block_ready=1;
}

void cl_arq_controller::restore_backup_buffer_data()
{
	int nBackedup_bytes, data_read_size, nMessages;
	nBackedup_bytes=fifo_buffer_backup.get_size()-fifo_buffer_backup.get_free_size();
	if(nBackedup_bytes!=0 && (max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH)!=0)
	{
		nMessages=nBackedup_bytes/(max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH);

		for(int i=0;i<nMessages+1;i++)
		{
			data_read_size=fifo_buffer_backup.pop(message_TxRx_byte_buffer,max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH);
			fifo_buffer_tx.push(message_TxRx_byte_buffer,data_read_size);
		}
	}
}

void cl_arq_controller::print_stats()
{
	std::cout << std::fixed;

	printf ("\e[2J");// clean screen
	printf ("\e[H"); // go to upper left corner
	std::cout << std::setprecision(1);
	if(this->current_configuration!=CONFIG_NONE)
	{
		std::cout<<"configuration:CONFIG_"<<(int)this->current_configuration<<" ("<<telecom_system->rbc<<" bps)";
	}
	else
	{
		std::cout<<"configuration: ERROR..( 0 bps)";
	}
	std::cout << std::setprecision(2);

	std::cout<<std::endl;

	if(this->role==COMMANDER)
	{
		std::cout<<"Role:COM call sign= ";
		std::cout<<this->my_call_sign;
	}
	else if (this->role==RESPONDER)
	{
		std::cout<<"Role:Res call sign= ";
		std::cout<<this->my_call_sign;
	}
	std::cout<<std::endl;

	if(this->link_status==DROPPED)
	{
		std::cout<<"link_status:Dropped";
	}
	else if(this->link_status==IDLE)
	{
		std::cout<<"link_status:Idle";
	}
	else if (this->link_status==CONNECTING)
	{
		std::cout<<"link_status:Connecting to ";
		std::cout<<this->destination_call_sign;
	}
	else if (this->link_status==CONNECTED)
	{
		std::cout<<"link_status:Connected to ";
		std::cout<<this->destination_call_sign;
		std::cout<<" ID= ";
		std::cout<<(int)this->connection_id;
	}
	else if (this->link_status==DISCONNECTING)
	{
		std::cout<<"link_status:Disconnecting";
	}
	else if (this->link_status==LISTENING)
	{
		std::cout<<"link_status:Listening";
	}
	else if (this->link_status==CONNECTION_RECEIVED)
	{
		std::cout<<"link_status:Connection Received from ";
		std::cout<<this->destination_call_sign;
	}
	else if (this->link_status==CONNECTION_ACCEPTED)
	{
		std::cout<<"link_status:Connection Accepted by ";
		std::cout<<this->destination_call_sign;
	}
	else if (link_status==NEGOTIATING)
	{
		std::cout<<"link_status:Negotiating with ";
		std::cout<<this->destination_call_sign;
	}
	std::cout<<std::endl;

	if (this->connection_status==TRANSMITTING_DATA)
	{
		std::cout<<"connection_status:Transmitting data";
	}
	else if (this->connection_status==RECEIVING)
	{
		std::cout<<"connection_status:Receiving";
	}
	else if (this->connection_status==RECEIVING_ACKS_DATA)
	{
		std::cout<<"connection_status:Receiving data Ack";
	}
	if(this->connection_status==ACKNOWLEDGING_DATA)
	{
		std::cout<<"connection_status:Acknowledging data";
	}
	else if (this->connection_status==TRANSMITTING_CONTROL)
	{
		std::cout<<"connection_status:Transmitting control";
	}
	else if (this->connection_status==RECEIVING_ACKS_CONTROL)
	{
		std::cout<<"connection_status:Receiving control Ack";
	}
	else if (this->connection_status==ACKNOWLEDGING_CONTROL)
	{
		std::cout<<"connection_status:Acknowledging control";
	}
	else if(this->connection_status==IDLE)
	{
		std::cout<<"connection_status:Idle";
	}
	std::cout<<std::endl;

	std::cout<<"measurements.SNR_uplink= "<<measurements.SNR_uplink<<std::endl;
	std::cout<<"measurements.SNR_downlink= "<<measurements.SNR_downlink<<std::endl;
	std::cout<<"measurements.signal_stregth_dbm= "<<measurements.signal_stregth_dbm<<std::endl;
	std::cout<<"measurements.frequency_offset= "<<measurements.frequency_offset<<std::endl;

	std::cout<<std::endl;

	std::cout<<"stats.nSent_data= "<<stats.nSent_data<<std::endl;
	std::cout<<"stats.nAcked_data= "<<stats.nAcked_data<<std::endl;
	std::cout<<"stats.nReceived_data= "<<stats.nReceived_data<<std::endl;
	std::cout<<"stats.nLost_data= "<<stats.nLost_data<<std::endl;
	std::cout<<"stats.nReSent_data= "<<stats.nReSent_data<<std::endl;
	std::cout<<"stats.nAcks_sent_data= "<<stats.nAcks_sent_data<<std::endl;
	std::cout<<"stats.nNAcked_data= "<<stats.nNAcked_data<<std::endl;
	std::cout<<"stats.ToSend_data:"<<this->get_nToSend_messages()<<std::endl;

	std::cout<<std::endl;

	std::cout<<"stats.nSent_control= "<<stats.nSent_control<<std::endl;
	std::cout<<"stats.nAcked_control= "<<stats.nAcked_control<<std::endl;
	std::cout<<"stats.nReceived_control= "<<stats.nReceived_control<<std::endl;
	std::cout<<"stats.nLost_control= "<<stats.nLost_control<<std::endl;
	std::cout<<"stats.nReSent_control= "<<stats.nReSent_control<<std::endl;
	std::cout<<"stats.nAcks_sent_control= "<<stats.nAcks_sent_control<<std::endl;
	std::cout<<"stats.nNAcked_control= "<<stats.nNAcked_control<<std::endl;

	std::cout<<std::endl;
	std::cout<<"link_timer= "<<link_timer.get_elapsed_time_ms()<<std::endl;
	std::cout<<"watchdog_timer= "<<watchdog_timer.get_elapsed_time_ms()<<std::endl;
	std::cout<<"gear_shift_timer= "<<gear_shift_timer.get_elapsed_time_ms()<<std::endl;
	std::cout<<"receiving_timer= "<<receiving_timer.get_elapsed_time_ms()<<std::endl;

	std::cout<<std::endl;
	std::cout<<"last_received_message_sequence= "<<(int)last_received_message_sequence<<std::endl;

	std::cout<<"last_transmission_block_success_rate= "<<(int)last_transmission_block_stats.success_rate_data <<" %"<<std::endl;
	if(gear_shift_blocked_for_nBlocks<gear_shift_block_for_nBlocks_total)
	{
		std::cout<<"gear_shift_blocked_for_nBlocks= "<<(int)gear_shift_blocked_for_nBlocks <<std::endl;
	}
	else
	{
		std::cout<<"gear_shift_blocked_for_nBlocks= "<<std::endl;
	}


	std::cout<<std::endl;
	if (this->last_message_sent_type==NONE)
	{
		std::cout<<"last_message_sent:";
	}
	else if (this->last_message_sent_type==DATA_LONG)
	{
		std::cout<<"last_message_sent:DATA:DATA_LONG";
	}
	else if (this->last_message_sent_type==DATA_SHORT)
	{
		std::cout<<"last_message_sent:DATA:DATA_SHORT";
	}
	else if (this->last_message_sent_type==ACK_MULTI)
	{
		std::cout<<"last_message_sent:DATA:ACK_MULTI";
	}
	else if (this->last_message_sent_type==ACK_RANGE)
	{
		std::cout<<"last_message_sent:DATA:ACK_RANGE";
	}
	else if (this->last_message_sent_type==CONTROL)
	{
		std::cout<<"last_message_sent:CONTROL:";
	}
	else if (this->last_message_sent_type==ACK_CONTROL)
	{
		std::cout<<"last_message_sent:ACK_CONTROL:";
	}

	if(this->last_message_sent_type==CONTROL || this->last_message_sent_type==ACK_CONTROL)
	{
		if (this->last_message_sent_code==START_CONNECTION)
		{
			std::cout<<"START_CONNECTION";
		}
		else if (this->last_message_sent_code==TEST_CONNECTION)
		{
			std::cout<<"TEST_CONNECTION";
		}
		else if (this->last_message_sent_code==CLOSE_CONNECTION)
		{
			std::cout<<"CLOSE_CONNECTION";
		}
		else if (this->last_message_sent_code==KEEP_ALIVE)
		{
			std::cout<<"KEEP_ALIVE";
		}
		else if (this->last_message_sent_code==FILE_START)
		{
			std::cout<<"FILE_START";
		}
		else if (this->last_message_sent_code==FILE_END_)
		{
			std::cout<<"FILE_END";
		}
		else if (this->last_message_sent_code==PIPE_OPEN)
		{
			std::cout<<"PIPE_OPEN";
		}
		else if (this->last_message_sent_code==PIPE_CLOSE)
		{
			std::cout<<"PIPE_CLOSE";
		}
		else if (this->last_message_sent_code==SWITCH_ROLE)
		{
			std::cout<<"SWITCH_ROLE";
		}
		else if (this->last_message_sent_code==BLOCK_END)
		{
			std::cout<<"BLOCK_END";
		}
		else if (this->last_message_sent_code==SET_CONFIG)
		{
			std::cout<<"SET_CONFIG";
		}
		else if (this->last_message_sent_code==REPEAT_LAST_ACK)
		{
			std::cout<<"REPEAT_LAST_ACK";
		}
	}
	std::cout<<std::endl;

	if (this->last_message_received_type==NONE)
	{
		std::cout<<"last_message_received:";
	}
	else if (this->last_message_received_type==DATA_LONG)
	{
		std::cout<<"last_message_received:DATA:DATA_LONG";
	}
	else if (this->last_message_received_type==DATA_SHORT)
	{
		std::cout<<"last_message_received:DATA:DATA_SHORT";
	}
	else if (this->last_message_received_type==ACK_MULTI)
	{
		std::cout<<"last_message_received:DATA:ACK_MULTI";
	}
	else if (this->last_message_received_type==ACK_RANGE)
	{
		std::cout<<"last_message_received:DATA:ACK_RANGE";
	}
	else if (this->last_message_received_type==CONTROL)
	{
		std::cout<<"last_message_received:CONTROL:";
	}
	else if (this->last_message_received_type==ACK_CONTROL)
	{
		std::cout<<"last_message_received:ACK_CONTROL:";
	}

	if(this->last_message_received_type==CONTROL || this->last_message_received_type==ACK_CONTROL)
	{
		if (this->last_message_received_code==START_CONNECTION)
		{
			std::cout<<"START_CONNECTION";
		}
		else if (this->last_message_received_code==TEST_CONNECTION)
		{
			std::cout<<"TEST_CONNECTION";
		}
		else if (this->last_message_received_code==CLOSE_CONNECTION)
		{
			std::cout<<"CLOSE_CONNECTION";
		}
		else if (this->last_message_received_code==KEEP_ALIVE)
		{
			std::cout<<"KEEP_ALIVE";
		}
		else if (this->last_message_received_code==FILE_START)
		{
			std::cout<<"FILE_START";
		}
		else if (this->last_message_received_code==FILE_END_)
		{
			std::cout<<"FILE_END";
		}
		else if (this->last_message_received_code==PIPE_OPEN)
		{
			std::cout<<"PIPE_OPEN";
		}
		else if (this->last_message_received_code==PIPE_CLOSE)
		{
			std::cout<<"PIPE_CLOSE";
		}
		else if (this->last_message_received_code==SWITCH_ROLE)
		{
			std::cout<<"SWITCH_ROLE";
		}
		else if (this->last_message_received_code==BLOCK_END)
		{
			std::cout<<"BLOCK_END";
		}
		else if (this->last_message_received_code==SET_CONFIG)
		{
			std::cout<<"SET_CONFIG";
		}
		else if (this->last_message_received_code==REPEAT_LAST_ACK)
		{
			std::cout<<"REPEAT_LAST_ACK";
		}
	}
	std::cout<<std::endl;

	std::cout<<std::endl;
	std::cout<<"TX buffer occupancy= "<<(float)(fifo_buffer_tx.get_size()-fifo_buffer_tx.get_free_size())*100.0/(float)fifo_buffer_tx.get_size()<<" %"<<std::endl;
	std::cout<<"RX buffer occupancy= "<<(float)(fifo_buffer_rx.get_size()-fifo_buffer_rx.get_free_size())*100.0/(float)fifo_buffer_rx.get_size()<<" %"<<std::endl;
	std::cout<<"Backup buffer occupancy= "<<(float)(fifo_buffer_backup.get_size()-fifo_buffer_backup.get_free_size())*100.0/(float)fifo_buffer_backup.get_size()<<" %"<<std::endl;
}

uint8_t cl_arq_controller::CRC8_calc(char* data_byte, int nItems)
{
	uint8_t crc = 0xff;
	for(int j=0; j < nItems; j++)
	{
		crc ^= data_byte[j];
		for (int i = 0; i < 8; i++)
		{
			if ((crc & 0x01) == 0x01)
			{
				crc = crc >> 1;
				crc ^= POLY_CRC8;
			}
			else
			{
				crc = crc >> 1;
			}
		}
	}
	return crc;
	//ref: MODBUS over serial line specification and implementation guide V1.02, Dec 20,2006, available at https://modbus.org/docs/Modbus_over_serial_line_V1_02.pdf
}

