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
#include <cmath>
#include <cstring>
#include <chrono>

#ifdef MERCURY_GUI_ENABLED
#include "gui/gui_state.h"
#endif

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
	connection_timeout=30000;
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
	ctrl_transmission_time_ms=500;
	ack_pattern_time_ms=0;
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
	forward_configuration=CONFIG_NONE;
	reverse_configuration=CONFIG_NONE;

	gear_shift_on=NO;
	robust_enabled=NO;
	gear_shift_algorithm=SUCCESS_BASED_LADDER;

	gear_shift_up_success_rate_precentage=70;
	gear_shift_down_success_rate_precentage=40;
	gear_shift_block_for_nBlocks_total=0;
	gear_shift_blocked_for_nBlocks=0;
	consecutive_data_acks=0;
	frame_shift_threshold=3;

	turboshift_phase=TURBO_FORWARD;
	turboshift_active=true;
	turboshift_last_good=-1;
	turboshift_initiator=false;
	turboshift_retries=1;

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
	connection_attempts=0;
	max_connection_attempts=15;
	exit_on_disconnect=NO;
	had_control_connection=NO;

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
		if(ack_pattern_time_ms > 0)
		{
			// ACK pattern. Allow responder decode + pattern TX + margins.
			int timeout = message_transmission_time_ms + ack_pattern_time_ms + ptt_on_delay_ms + ptt_off_delay_ms + 3000;
			// During turboshift, RSP calls load_configuration() on every probe,
			// adding ~200-500ms overhead. Extend receive window to prevent
			// premature timeout before ACK arrives.
			if(gear_shift_on && turboshift_phase != TURBO_DONE)
				timeout += 2000;
			set_receiving_timeout(timeout);
		}
		else
		{
			set_receiving_timeout((ack_batch_size+1)*ctrl_transmission_time_ms+time_left_to_send_last_frame+ptt_on_delay_ms);
		}
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


int cl_arq_controller::init(int tcp_base_port, int gear_shift_on, int initial_mode)
{
	int success=SUCCESSFUL;

	fifo_buffer_tx.set_size(default_configuration_ARQ.fifo_buffer_tx_size);
	fifo_buffer_rx.set_size(default_configuration_ARQ.fifo_buffer_rx_size);
	fifo_buffer_backup.set_size(default_configuration_ARQ.fifo_buffer_backup_size);

	set_link_timeout(default_configuration_ARQ.link_timeout);

	if (tcp_base_port)
	{
		tcp_socket_control.port = tcp_base_port;
		tcp_socket_data.port = tcp_base_port + 1;
	}
	else
	{
		tcp_socket_control.port = default_configuration_ARQ.tcp_socket_control_port;
		tcp_socket_data.port = default_configuration_ARQ.tcp_socket_data_port;
	}

	tcp_socket_control.timeout_ms=default_configuration_ARQ.tcp_socket_control_timeout_ms;
	tcp_socket_data.timeout_ms=default_configuration_ARQ.tcp_socket_data_timeout_ms;

	this->gear_shift_on = gear_shift_on;
	gear_shift_algorithm=default_configuration_ARQ.gear_shift_algorithm;
	current_configuration=CONFIG_NONE;

	if(robust_enabled)
	{
		// ROBUST mode: use requested ROBUST config for all roles
		// Gearshift between ROBUST levels handled separately
		init_configuration = initial_mode;
		data_configuration = initial_mode;
		ack_configuration = initial_mode;
	}
	else
	{
		init_configuration = initial_mode;
		data_configuration = initial_mode;
		ack_configuration=default_configuration_ARQ.ack_configuration;
	}

	if(tcp_socket_data.init()!=SUCCESS || tcp_socket_control.init()!=SUCCESS )
	{
		printf("Error initializing the TCP sockets. Exiting..\n");
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
	printf("[CFG] load_configuration(%d) current=%d level=%s backup=%s\n",
		configuration, this->current_configuration,
		level == FULL ? "FULL" : "PHYS_ONLY",
		backup_configuration == YES ? "YES" : "NO");
	if(configuration==this->current_configuration)
	{
		printf("[CFG] Already on config %d, skipping\n", configuration);
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

	// Pause audio capture during PHY reconfig to prevent race condition:
	// telecom_system->load_configuration may deinit/reinit buffers that
	// the audio callback accesses (passband_delayed_data, etc.)
	telecom_system->data_container.frames_to_read = 0;

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

	// MFSK modes: single ACK/control frame per batch (no redundant copies).
	// LDPC provides cliff-effect protection; if a frame decodes, it's correct.
	// Saves 1 frame per ACK cycle + 1 per control cycle = major time savings
	// at 4.6-7.3s per frame.
	if(is_robust_config(configuration))
	{
		set_data_batch_size(1);
		set_ack_batch_size(1);
		set_control_batch_size(1);
	}
	
	gear_shift_up_success_rate_precentage=default_configuration_ARQ.gear_shift_up_success_rate_limit_precentage;
	gear_shift_down_success_rate_precentage=default_configuration_ARQ.gear_shift_down_success_rate_limit_precentage;

	gear_shift_block_for_nBlocks_total=default_configuration_ARQ.gear_shift_block_for_nBlocks_total;
	gear_shift_blocked_for_nBlocks=default_configuration_ARQ.gear_shift_block_for_nBlocks_total;
	consecutive_data_acks=0;
	// NOTE: turboshift state is NOT reset here — it persists across config changes.
	// Only reset at connection init (see init code above).

	message_transmission_time_ms=ceil((1000.0*(telecom_system->data_container.Nsymb+telecom_system->data_container.preamble_nSymb)*telecom_system->data_container.Nofdm*telecom_system->frequency_interpolation_rate)/(float)(telecom_system->frequency_interpolation_rate*(telecom_system->bandwidth/telecom_system->ofdm.Nc)*telecom_system->ofdm.Nfft));
	if(telecom_system->ctrl_nsymb > 0)
	{
		ctrl_transmission_time_ms=ceil((1000.0*(telecom_system->ctrl_nsymb+telecom_system->data_container.preamble_nSymb)*telecom_system->data_container.Nofdm*telecom_system->frequency_interpolation_rate)/(float)(telecom_system->frequency_interpolation_rate*(telecom_system->bandwidth/telecom_system->ofdm.Nc)*telecom_system->ofdm.Nfft));
	}
	else
	{
		ctrl_transmission_time_ms=message_transmission_time_ms;
	}
    // TODO: After audio I/O rewrite we don't use this anymore. Was:
	// time_left_to_send_last_frame=(float)telecom_system->speaker.frames_to_leave_transmit_fct/(float)(telecom_system->frequency_interpolation_rate*(telecom_system->bandwidth/telecom_system->ofdm.Nc)*telecom_system->ofdm.Nfft);
    time_left_to_send_last_frame=0;

	// Scale data_batch_size for ~10s block duration (OFDM modes only).
	// MFSK modes keep batch_size=1 for pattern ACK optimization.
	// Adapts each gearshift: fast configs send more frames per block.
	if(!is_robust_config(configuration) && message_transmission_time_ms > 0)
	{
		int target_batch = (int)(10000.0 / message_transmission_time_ms + 0.5);
		if(target_batch < 5) target_batch = 5;
		if(target_batch > nMessages) target_batch = nMessages;
		set_data_batch_size(target_batch);
		printf("[CFG] Batch scaling: msg_time=%dms target=%d actual=%d nMessages=%d\n",
			message_transmission_time_ms, target_batch, data_batch_size, nMessages);
		fflush(stdout);
	}

	// ACK pattern transmission time (universal: all modes)
	if(telecom_system->ack_pattern_passband_samples > 0)
	{
		ack_pattern_time_ms = (int)ceil(1000.0 * telecom_system->ack_pattern_passband_samples / telecom_system->sampling_frequency);
	}
	else
	{
		ack_pattern_time_ms = 0;
	}

	// With pattern-based ACK, control_batch_size=1: LDPC cliff effect makes
	// redundant copies wasteful, and halving TX means faster ACK round-trip.
	// ack_batch_size=1: irrelevant for pattern ACK but keeps consistency.
	if(ack_pattern_time_ms > 0)
	{
		set_control_batch_size(1);
		set_ack_batch_size(1);
	}

	if(ack_pattern_time_ms > 0)
	{
		// ACK is a short tone pattern, not a full LDPC frame
		set_ack_timeout_data((data_batch_size+1)*message_transmission_time_ms + ack_pattern_time_ms + 4*ptt_on_delay_ms + 4*ptt_off_delay_ms + 1500);
		set_ack_timeout_control(control_batch_size*message_transmission_time_ms + ack_pattern_time_ms + 2*ptt_on_delay_ms + 2*ptt_off_delay_ms + 1500);
	}
	else
	{
		set_ack_timeout_data((data_batch_size+1)*message_transmission_time_ms+control_batch_size*message_transmission_time_ms+2*ack_batch_size*ctrl_transmission_time_ms+time_left_to_send_last_frame+4*ptt_on_delay_ms+4*ptt_off_delay_ms);
		set_ack_timeout_control(control_batch_size*message_transmission_time_ms+ack_batch_size*ctrl_transmission_time_ms+time_left_to_send_last_frame+2*ptt_on_delay_ms+2*ptt_off_delay_ms);
	}

	// During turboshift, extend ack_timeout_control (the overall NAck deadline).
	// receiving_timeout margin is handled inside calculate_receiving_timeout().
	if(gear_shift_on && turboshift_phase != TURBO_DONE)
	{
		set_ack_timeout_control(ack_timeout_control + 2000);
	}

	ptt_on_delay_ms=default_configuration_ARQ.ptt_on_delay_ms;
	ptt_off_delay_ms=default_configuration_ARQ.ptt_off_delay_ms;
	pilot_tone_ms=default_configuration_ARQ.pilot_tone_ms;
	pilot_tone_hz=default_configuration_ARQ.pilot_tone_hz;
	switch_role_timeout=default_configuration_ARQ.switch_role_timeout_ms;
	// MFSK modes: frame durations are 4-7s, so both sides have ample prep time
	// during the frame itself. Reduce role-switch wait from 1500ms to 200ms.
	if(is_robust_config(configuration))
		switch_role_timeout = 200;

	switch_role_test_timeout=(nResends/3)*ack_timeout_control;
	watchdog_timeout=(nResends/3)*ack_timeout_data;
	gearshift_timeout=(nResends/3)*ack_timeout_data;

	// Ensure connection_timeout and link_timeout are adequate for MFSK frame durations.
	{
		int ack_time = (ack_pattern_time_ms > 0) ? ack_pattern_time_ms : (ack_batch_size * ctrl_transmission_time_ms);

		// Connection handshake: 2 round-trips of control+ack batches
		int min_ct = 2 * (control_batch_size * message_transmission_time_ms + ack_time)
			+ 4 * ptt_on_delay_ms + 4 * ptt_off_delay_ms + 5000;
		if (connection_timeout < min_ct)
			connection_timeout = min_ct;

		// Link timeout: must survive a full data+ack round-trip
		int min_lt = (data_batch_size + 2) * message_transmission_time_ms + ack_time
			+ 2 * ptt_on_delay_ms + 2 * ptt_off_delay_ms + 5000;
		if (link_timeout < min_lt)
			link_timeout = min_lt;
	}

	calculate_receiving_timeout();

	// Guard: ack_timeout_control must cover the full TX + receive window.
	// ack_timer starts at frame send (T=0); receiving_timer starts after TX + PTT.
	// Without this, ack_timeout can expire while CMD is still polling for ACK.
	if(gear_shift_on && turboshift_phase != TURBO_DONE && this->role == COMMANDER)
	{
		int min_ack = message_transmission_time_ms + ptt_off_delay_ms + receiving_timeout + 500;
		if(ack_timeout_control < min_ack)
			set_ack_timeout_control(min_ack);
	}

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

	// Check connection attempt timeout - separate from link_timer which gets restarted on every message
	if((link_status==CONNECTING || link_status==NEGOTIATING || link_status==CONNECTION_ACCEPTED) &&
	   connection_attempt_timer.counting==1 &&
	   connection_attempt_timer.get_elapsed_time_ms()>=connection_timeout)
	{
		std::cout<<"Connection attempt timeout after "<<connection_timeout<<" ms"<<std::endl;

		// Send CANCELPENDING and DISCONNECTED to Winlink (connection attempt timed out)
		if(role==COMMANDER && tcp_socket_control.get_status()==TCP_STATUS_ACCEPTED)
		{
			std::string str="CANCELPENDING\r";
			tcp_socket_control.message->length=str.length();
			for(int i=0;i<tcp_socket_control.message->length;i++)
			{
				tcp_socket_control.message->buffer[i]=str[i];
			}
			tcp_socket_control.transmit();

			str="DISCONNECTED\r";
			tcp_socket_control.message->length=str.length();
			for(int i=0;i<tcp_socket_control.message->length;i++)
			{
				tcp_socket_control.message->buffer[i]=str[i];
			}
			tcp_socket_control.transmit();
		}

		this->link_status=DROPPED;
		connection_id=0;
		assigned_connection_id=0;
		connection_attempt_timer.stop();
		connection_attempt_timer.reset();
		connection_attempts=0;

		fifo_buffer_tx.flush();
		fifo_buffer_backup.flush();
		fifo_buffer_rx.flush();

		// Reset messages_control so new CONNECT commands can work
		messages_control.status=FREE;

		// After failed connection attempt, always switch to RESPONDER/LISTENING
		// so we can receive incoming connections from the other side
		set_role(RESPONDER);
		link_status=LISTENING;
		connection_status=RECEIVING;
		load_configuration(init_configuration, FULL, YES);
		printf("Switching to RESPONDER mode after connection timeout\n");
	}

	// Check for max connection attempts
	if((link_status==CONNECTING || link_status==NEGOTIATING || link_status==CONNECTION_ACCEPTED) &&
	   connection_attempts >= max_connection_attempts)
	{
		std::cout<<"Maximum connection attempts ("<<max_connection_attempts<<") reached"<<std::endl;

		// Send CANCELPENDING and DISCONNECTED to Winlink (max connection attempts reached)
		if(role==COMMANDER && tcp_socket_control.get_status()==TCP_STATUS_ACCEPTED)
		{
			std::string str="CANCELPENDING\r";
			tcp_socket_control.message->length=str.length();
			for(int i=0;i<tcp_socket_control.message->length;i++)
			{
				tcp_socket_control.message->buffer[i]=str[i];
			}
			tcp_socket_control.transmit();

			str="DISCONNECTED\r";
			tcp_socket_control.message->length=str.length();
			for(int i=0;i<tcp_socket_control.message->length;i++)
			{
				tcp_socket_control.message->buffer[i]=str[i];
			}
			tcp_socket_control.transmit();
		}

		this->link_status=DROPPED;
		connection_id=0;
		assigned_connection_id=0;
		connection_attempt_timer.stop();
		connection_attempt_timer.reset();
		connection_attempts=0;

		fifo_buffer_tx.flush();
		fifo_buffer_backup.flush();
		fifo_buffer_rx.flush();

		// Reset messages_control so new CONNECT commands can work
		messages_control.status=FREE;

		// After failed connection attempts, always switch to RESPONDER/LISTENING
		// so we can receive incoming connections from the other side
		set_role(RESPONDER);
		link_status=LISTENING;
		connection_status=RECEIVING;
		load_configuration(init_configuration, FULL, YES);
		printf("Switching to RESPONDER mode after max connection attempts\n");
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
			load_configuration(init_configuration, FULL, YES);
		}
		else if(this->role==RESPONDER)
		{
			link_status=LISTENING;
			connection_status=RECEIVING;
			load_configuration(init_configuration, FULL, YES);
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

		last_data_configuration=data_configuration;
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

	// Fallback: if we're in COMMANDER mode and haven't received anything for 60+ seconds
	// while supposedly connected, force switch to RESPONDER mode to break infinite loops
	const int FORCED_ROLE_SWITCH_TIMEOUT = 60000;  // 60 seconds
	if(role==COMMANDER && link_status==CONNECTED &&
	   receiving_timer.get_elapsed_time_ms() >= FORCED_ROLE_SWITCH_TIMEOUT)
	{
		printf("Forced role switch: no RX for %d seconds, switching to RESPONDER\n", FORCED_ROLE_SWITCH_TIMEOUT/1000);

		set_role(RESPONDER);
		link_status=LISTENING;
		connection_status=RECEIVING;
		load_configuration(init_configuration, FULL, YES);

		// Reset timers
		watchdog_timer.stop();
		watchdog_timer.reset();
		link_timer.stop();
		link_timer.reset();
		receiving_timer.stop();
		receiving_timer.reset();

		// Flush buffers
		fifo_buffer_tx.flush();
		fifo_buffer_backup.flush();
		fifo_buffer_rx.flush();
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
			// Turboshift: retry once before declaring ceiling
			if(turboshift_active && this->role==COMMANDER)
			{
				if(turboshift_retries > 0)
				{
					turboshift_retries--;
					printf("[TURBO] RETRY config %d (retries left: %d)\n",
						current_configuration, turboshift_retries);
					fflush(stdout);
					// Re-send the same SET_CONFIG
					cleanup();
					add_message_control(SET_CONFIG);
					connection_status=TRANSMITTING_CONTROL;
					return;
				}

				int failed_config = current_configuration;
				int settle_config = (turboshift_last_good >= 0) ?
					turboshift_last_good : init_configuration;

				printf("[TURBO] CEILING at config %d, settling at %d\n",
					failed_config, settle_config);
				fflush(stdout);

				messages_control_backup();
				data_configuration = settle_config;
				load_configuration(data_configuration, PHYSICAL_LAYER_ONLY, YES);
				messages_control_restore();

				for(int i=0;i<nMessages;i++)
					messages_tx[i].status=FREE;
				fifo_buffer_backup.flush();

				finish_turbo_direction();
				return;
			}

			messages_control_backup();
			data_configuration=config_ladder_down(current_configuration, robust_enabled);
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

	if(!turboshift_active && switch_role_test_timer.get_elapsed_time_ms()>switch_role_test_timeout)
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
		// Mark that we had a control connection
		had_control_connection=YES;

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
			// Check if client disconnected cleanly (nBytes_received==0)
			if(nBytes_received==0 && exit_on_disconnect==YES && had_control_connection==YES)
			{
				std::cout<<std::endl;
				std::cout<<"Control connection closed by client - exiting as requested"<<std::endl;
				exit(0);
			}

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
			// Strip any leading \n characters (from Windows \r\n line endings)
			while(!user_command_buffer.empty() && user_command_buffer[0]=='\n')
			{
				user_command_buffer=user_command_buffer.substr(1);
			}

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

	// Signal measurement when idle: measure_signal_only() uses FIR_rx_time_sync,
	// the same filter that receive_byte() uses for preamble detection. Running both
	// on the same iteration corrupts the FIR delay line state. During active
	// connections, receive_byte() already provides signal strength, so we only
	// need measure_signal_only() when idle/listening (no receive() calls happening).
	if(link_status == LISTENING || link_status == IDLE || link_status == DROPPED)
	{
		MUTEX_LOCK(&capture_prep_mutex);
		if(telecom_system->data_container.frames_to_read == 0)
		{
			int signal_period = telecom_system->data_container.Nofdm *
				telecom_system->data_container.buffer_Nsymb *
				telecom_system->data_container.interpolation_rate;

			memcpy(telecom_system->data_container.ready_to_process_passband_delayed_data,
				telecom_system->data_container.passband_delayed_data,
				signal_period * sizeof(double));

			MUTEX_UNLOCK(&capture_prep_mutex);

			measurements.signal_stregth_dbm = telecom_system->measure_signal_only(
				telecom_system->data_container.ready_to_process_passband_delayed_data);
		}
		else
		{
			MUTEX_UNLOCK(&capture_prep_mutex);
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

		// Reset messages_control so new connection can add START_CONNECTION
		messages_control.status=FREE;

		// Start connection attempt timer and reset counter
		connection_attempts=0;
		connection_attempt_timer.reset();
		connection_attempt_timer.start();

		// Send OK acknowledgement
		tcp_socket_control.message->buffer[0]='O';
		tcp_socket_control.message->buffer[1]='K';
		tcp_socket_control.message->buffer[2]='\r';
		tcp_socket_control.message->length=3;
		tcp_socket_control.transmit();

		// Send PENDING status to indicate connection attempt is starting
		std::string str="PENDING\r";
		tcp_socket_control.message->length=str.length();
		for(int i=0;i<tcp_socket_control.message->length;i++)
		{
			tcp_socket_control.message->buffer[i]=str[i];
		}
		// Note: transmit() will be called in process_main after all commands are processed
	}
	else if(command=="DISCONNECT")
	{
		disconnect_requested=YES;

		tcp_socket_control.message->buffer[0]='O';
		tcp_socket_control.message->buffer[1]='K';
		tcp_socket_control.message->buffer[2]='\r';
		tcp_socket_control.message->length=3;
	}
	else if(command=="ABORT")
	{
		// Abort connection attempt - stop trying to connect
		if(link_status==CONNECTING || link_status==NEGOTIATING || link_status==CONNECTION_ACCEPTED)
		{
			// Always switch to RESPONDER/LISTENING after abort so we can receive incoming connections
			set_role(RESPONDER);
			link_status=LISTENING;
			connection_status=RECEIVING;
			load_configuration(init_configuration, FULL, YES);

			// Clear buffers and reset timers
			fifo_buffer_tx.flush();
			fifo_buffer_backup.flush();
			reset_all_timers();

			// Reset connection attempts counter
			connection_attempts=0;

			// Reset messages_control so new CONNECT commands can work
			messages_control.status=FREE;

			// Send CANCELPENDING to cancel the connection attempt
			std::string str="CANCELPENDING\r";
			tcp_socket_control.message->length=str.length();
			for(int i=0;i<tcp_socket_control.message->length;i++)
			{
				tcp_socket_control.message->buffer[i]=str[i];
			}
			tcp_socket_control.transmit();

			// Send DISCONNECTED to fully clear Winlink's state and show we're free
			str="DISCONNECTED\r";
			tcp_socket_control.message->length=str.length();
			for(int i=0;i<tcp_socket_control.message->length;i++)
			{
				tcp_socket_control.message->buffer[i]=str[i];
			}
			tcp_socket_control.transmit();
		}

		// Send OK acknowledgement
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

		// Load init_configuration so we can hear incoming START_CONNECTION messages
		load_configuration(init_configuration, FULL, YES);

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
	printf("send()\n");

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

	{
		int active_nsymb = telecom_system->get_active_nsymb();
		tx_transfer(telecom_system->data_container.ready_to_transmit_passband_data_tx,
					telecom_system->data_container.Nofdm * telecom_system->data_container.interpolation_rate *
					(active_nsymb + telecom_system->data_container.preamble_nSymb));
	}

	while (size_buffer(playback_buffer) > 0)
		msleep(1);

	last_message_sent_type=message->type;
	if(message->type==CONTROL || message->type==ACK_CONTROL)
	{
		last_message_sent_code=message->data[0];
	}
	last_received_message_sequence=-1;

}

void cl_arq_controller::send_batch()
{
	printf("[TX] send_batch() on CONFIG_%d, %d messages, first type=%d\n",
		current_configuration, message_batch_counter_tx,
		message_batch_counter_tx > 0 ? messages_batch_tx[0].type : -1);
	fflush(stdout);

	// Flush capture buffer at the START of send_batch(), before TX begins.
	// On VB-Cable (and real radios), the responder decodes the frame and sends
	// its ACK pattern while the commander may still be draining playback or in
	// PTT-off delay. By flushing here, the buffer is clean BEFORE self-echo
	// starts, and the ACK pattern that arrives after the frame is preserved.
	// The order-aware ACK detector distinguishes ACK tones from OFDM self-echo.
	circular_buf_reset(capture_buffer);
	{
		int buf_samples = telecom_system->data_container.Nofdm * telecom_system->data_container.buffer_Nsymb * telecom_system->data_container.interpolation_rate;
		MUTEX_LOCK(&capture_prep_mutex);
		memset(telecom_system->data_container.passband_delayed_data, 0, buf_samples * sizeof(double));
		MUTEX_UNLOCK(&capture_prep_mutex);
	}
	telecom_system->data_container.nUnder_processing_events = 0;
	telecom_system->receive_stats.delay_of_last_decoded_message = -1;
	telecom_system->receive_stats.mfsk_search_raw = 0;

	ptt_on();

	cl_timer ptt_on_delay, ptt_off_delay;
	ptt_on_delay.start();

	int active_nsymb = telecom_system->get_active_nsymb();
	int frame_output_size = telecom_system->data_container.Nofdm*telecom_system->data_container.interpolation_rate*(active_nsymb+telecom_system->data_container.preamble_nSymb);

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

		// Debug: show serialized bytes before transmit
		{
			int total = header_length + messages_batch_tx[i].length;
			printf("[TX-BYTES] frame=%d type=%d connid=%d hdr=%d len=%d bytes:",
				i, messages_batch_tx[i].type, (int)(unsigned char)connection_id,
				header_length, messages_batch_tx[i].length);
			for(int j=0; j<total && j<12; j++)
				printf(" %02x", (unsigned char)message_TxRx_byte_buffer[j]);
			printf("\n");
			fflush(stdout);
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

	{
		int total_fir_size = (message_batch_counter_tx+2)*frame_output_size;
		memset(batch_frames_output_data_filtered1, 0, total_fir_size * sizeof(double));
		memset(batch_frames_output_data_filtered2, 0, total_fir_size * sizeof(double));
		telecom_system->ofdm.FIR_tx1.apply(batch_frames_output_data,batch_frames_output_data_filtered1,total_fir_size);
		telecom_system->ofdm.FIR_tx2.apply(batch_frames_output_data_filtered1,batch_frames_output_data_filtered2,total_fir_size);
	}

	while(ptt_on_delay.get_elapsed_time_ms() < ptt_on_delay_ms)
		msleep(1);

	// Generate pilot tone if enabled (configurable frequency to warm up TX/amp)
	if(pilot_tone_ms > 0 && pilot_tone_hz > 0)
	{
		const double SAMPLE_RATE = 48000.0;
		const double PILOT_FREQ = (double)pilot_tone_hz;
		const double PI = 3.14159265358979323846;
		int pilot_samples = (int)(pilot_tone_ms * SAMPLE_RATE / 1000.0);
		double* pilot_buffer = new double[pilot_samples];

		for(int i = 0; i < pilot_samples; i++)
		{
			// Generate sine wave with soft ramp up/down to avoid clicks
			double t = (double)i / SAMPLE_RATE;
			double envelope = 1.0;
			int ramp_samples = (int)(SAMPLE_RATE * 0.005); // 5ms ramp
			if(i < ramp_samples)
				envelope = (double)i / ramp_samples;
			else if(i > pilot_samples - ramp_samples)
				envelope = (double)(pilot_samples - i) / ramp_samples;

			pilot_buffer[i] = envelope * 0.5 * sin(2.0 * PI * PILOT_FREQ * t);
		}

		tx_transfer(pilot_buffer, pilot_samples);
		delete[] pilot_buffer;
	}

	for(int i=0;i<message_batch_counter_tx;i++)
	{
		printf("[TX] tx_transfer frame %d/%d, size=%d\n", i, message_batch_counter_tx, frame_output_size);
		fflush(stdout);
		tx_transfer(&batch_frames_output_data_filtered2[(i+1)*frame_output_size], frame_output_size);
	}

	printf("[TX] Waiting for playback buffer to drain...\n");
	fflush(stdout);
	// wait buffer to be played
	while (size_buffer(playback_buffer) > 0)
		msleep(1);

	// No flush here — buffer was flushed at start of send_batch().
	// Self-echo from TX is in the buffer, followed by the responder's ACK.
	// The order-aware ACK detector can find the ACK amid self-echo.

	ptt_off_delay.start();
	while(ptt_off_delay.get_elapsed_time_ms() < ptt_off_delay_ms)
		msleep(1);

	ptt_off();

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



	// Buffer was flushed at start of send_batch(). Self-echo + ACK are in
	// the sliding buffer. Set frames_to_read for default data frame reception
	// — callers may override for ACK pattern capture after send_batch() returns.
	telecom_system->data_container.frames_to_read =
		telecom_system->data_container.preamble_nSymb + telecom_system->get_active_nsymb();
	printf("[TX-END] frames_to_read=%d (ctrl=%d)\n",
		telecom_system->data_container.frames_to_read.load(),
		telecom_system->mfsk_ctrl_mode ? 1 : 0);
}

// Transmit short ACK tone pattern instead of LDPC-encoded ACK frame
void cl_arq_controller::send_ack_pattern()
{
	printf("[TX-ACK-PAT] Sending ACK pattern on CONFIG_%d\n", current_configuration);
	fflush(stdout);

	ptt_on();

	cl_timer ptt_on_delay_timer, ptt_off_delay_timer;
	ptt_on_delay_timer.start();

	int pattern_samples = telecom_system->ack_pattern_passband_samples;
	int symbol_period = telecom_system->data_container.Nofdm * telecom_system->data_container.interpolation_rate;

	// Allocate buffers: pattern + 1 symbol padding at each end for FIR filtering
	int padded_size = pattern_samples + 2 * symbol_period;
	double *raw_output = new double[padded_size];
	double *filtered1 = new double[padded_size];
	double *filtered2 = new double[padded_size];

	if(!raw_output || !filtered1 || !filtered2) exit(-34);

	memset(raw_output, 0, padded_size * sizeof(double));

	// Generate ACK pattern passband into the middle section
	telecom_system->generate_ack_pattern_passband(&raw_output[symbol_period]);

	// Pad start and end with copies of first/last symbol for FIR boundary
	memcpy(&raw_output[0], &raw_output[symbol_period], symbol_period * sizeof(double));
	memcpy(&raw_output[symbol_period + pattern_samples], &raw_output[pattern_samples], symbol_period * sizeof(double));

	// FIR filter chain (same as send_batch)
	memset(filtered1, 0, padded_size * sizeof(double));
	memset(filtered2, 0, padded_size * sizeof(double));
	telecom_system->ofdm.FIR_tx1.apply(raw_output, filtered1, padded_size);
	telecom_system->ofdm.FIR_tx2.apply(filtered1, filtered2, padded_size);

	// Wait PTT on delay
	while(ptt_on_delay_timer.get_elapsed_time_ms() < ptt_on_delay_ms)
		msleep(1);

	// Pilot tone (if enabled)
	if(pilot_tone_ms > 0 && pilot_tone_hz > 0)
	{
		const double SAMPLE_RATE = 48000.0;
		const double PILOT_FREQ = (double)pilot_tone_hz;
		const double PI = 3.14159265358979323846;
		int pilot_samples = (int)(pilot_tone_ms * SAMPLE_RATE / 1000.0);
		double* pilot_buffer = new double[pilot_samples];

		for(int i = 0; i < pilot_samples; i++)
		{
			double t = (double)i / SAMPLE_RATE;
			double envelope = 1.0;
			int ramp_samples = (int)(SAMPLE_RATE * 0.005);
			if(i < ramp_samples)
				envelope = (double)i / ramp_samples;
			else if(i > pilot_samples - ramp_samples)
				envelope = (double)(pilot_samples - i) / ramp_samples;
			pilot_buffer[i] = envelope * 0.5 * sin(2.0 * PI * PILOT_FREQ * t);
		}

		tx_transfer(pilot_buffer, pilot_samples);
		delete[] pilot_buffer;
	}

	// Transmit the filtered ACK pattern (skip padding at start)
	tx_transfer(&filtered2[symbol_period], pattern_samples);

	// Wait for playback to drain
	while(size_buffer(playback_buffer) > 0)
		msleep(1);

	// PTT off delay
	ptt_off_delay_timer.start();
	while(ptt_off_delay_timer.get_elapsed_time_ms() < ptt_off_delay_ms)
		msleep(1);

	ptt_off();

	delete[] raw_output;
	delete[] filtered1;
	delete[] filtered2;

	// Flush capture buffer and passband_delayed_data (discard self-echo + stale patterns)
	circular_buf_reset(capture_buffer);
	{
		int buf_samples = telecom_system->data_container.Nofdm * telecom_system->data_container.buffer_Nsymb * telecom_system->data_container.interpolation_rate;
		MUTEX_LOCK(&capture_prep_mutex);
		memset(telecom_system->data_container.passband_delayed_data, 0, buf_samples * sizeof(double));
		MUTEX_UNLOCK(&capture_prep_mutex);
	}
	telecom_system->data_container.nUnder_processing_events = 0;
	telecom_system->receive_stats.delay_of_last_decoded_message = -1;
	telecom_system->receive_stats.mfsk_search_raw = 0;

	printf("[TX-ACK-PAT] Done, flushed capture buffer\n");
	fflush(stdout);
}

// Receive and detect ACK tone pattern, returns true if detected.
// Scans only the TAIL (newest 24 symbols) of the capture buffer.
// Self-echo from our TX lives in the older part and is never scanned.
// Called frequently (every ~4 symbols / 91ms) to adapt to any round-trip latency.
bool cl_arq_controller::receive_ack_pattern()
{
	// Tail = ACK pattern (16 sym) + guard (8 sym) = 24 symbols
	const int tail_nsymb = cl_mfsk::ACK_PATTERN_NSYMB + 8;
	int sym_samples = telecom_system->data_container.Nofdm
	                * telecom_system->data_container.interpolation_rate;
	int signal_period = sym_samples * telecom_system->data_container.buffer_Nsymb;
	int tail_samples = tail_nsymb * sym_samples;
	if(tail_samples > signal_period)
		tail_samples = signal_period;
	int tail_offset = signal_period - tail_samples;

	MUTEX_LOCK(&capture_prep_mutex);

	if(telecom_system->data_container.frames_to_read == 0)
	{
		// Snapshot only the tail (newest audio) — smaller copy, shorter mutex hold
		memcpy(telecom_system->data_container.ready_to_process_passband_delayed_data,
			&telecom_system->data_container.passband_delayed_data[tail_offset],
			tail_samples * sizeof(double));

		telecom_system->data_container.data_ready = 0;
		MUTEX_UNLOCK(&capture_prep_mutex);

		int matched_count = 0;
		double metric = telecom_system->detect_ack_pattern_from_passband(
			telecom_system->data_container.ready_to_process_passband_delayed_data,
			tail_samples, &matched_count);

		printf("[ACK-RX] metric=%.3f threshold=%.3f matched=%d/16\n",
			metric, telecom_system->ack_pattern_detection_threshold, matched_count);
		fflush(stdout);

		if(metric >= telecom_system->ack_pattern_detection_threshold &&
		   matched_count >= cl_mfsk::ACK_PATTERN_NSYMB / 2)
		{
			// Detected — start capturing a full frame immediately so audio
			// accumulates during guard delay and next preamble isn't missed.
			MUTEX_LOCK(&capture_prep_mutex);
			telecom_system->data_container.frames_to_read =
				telecom_system->data_container.preamble_nSymb + telecom_system->data_container.Nsymb;
			telecom_system->data_container.nUnder_processing_events = 0;
			telecom_system->receive_stats.mfsk_search_raw = 0;
			MUTEX_UNLOCK(&capture_prep_mutex);
			return true;
		}

		// Not detected — poll again in 4 symbols (~91ms)
		telecom_system->data_container.frames_to_read = 4;
		telecom_system->data_container.nUnder_processing_events = 0;
		return false;
	}

	telecom_system->data_container.data_ready = 0;
	MUTEX_UNLOCK(&capture_prep_mutex);
	return false;
}

void cl_arq_controller::receive()
{
	int signal_period = telecom_system->data_container.Nofdm * telecom_system->data_container.buffer_Nsymb * telecom_system->data_container.interpolation_rate; // in samples
	int symbol_period = telecom_system->data_container.Nofdm * telecom_system->data_container.interpolation_rate;

#if 0 // TODO:  do we need this?
	if(telecom_system->data_container.data_ready == 0)
	{
		msleep(1);
		return;
	}
#endif
	MUTEX_LOCK(&capture_prep_mutex);
	st_receive_stats received_message_stats;


	if(telecom_system->data_container.frames_to_read==0)
	{


		memcpy(telecom_system->data_container.ready_to_process_passband_delayed_data, telecom_system->data_container.passband_delayed_data, signal_period * sizeof(double));

		// Clear data_ready while we have the lock, before unlocking
		telecom_system->data_container.data_ready = 0;

		MUTEX_UNLOCK(&capture_prep_mutex);

		// Buffer energy diagnostic: peak |amplitude| per ~11-symbol chunk (10 chunks for 109-symb buffer)
		// Shows energy distribution to detect self-hearing (H5c) or silence-preceded frames
		if(telecom_system->M != MOD_MFSK)
		{
			int sym_samples = telecom_system->data_container.Nofdm * telecom_system->data_container.interpolation_rate;
			int buf_nsymb = telecom_system->data_container.buffer_Nsymb;
			int chunk_symb = (buf_nsymb + 9) / 10;  // ~11 symbols per chunk
			int chunk_samples = chunk_symb * sym_samples;
			printf("[BUF-ENERGY] nUnder=%d |", telecom_system->data_container.nUnder_processing_events.load());
			for(int c = 0; c < signal_period; c += chunk_samples)
			{
				double peak = 0.0;
				int end = (c + chunk_samples < signal_period) ? c + chunk_samples : signal_period;
				for(int s = c; s < end; s++)
				{
					double v = fabs(telecom_system->data_container.ready_to_process_passband_delayed_data[s]);
					if(v > peak) peak = v;
				}
				printf(" %.3f", peak);
			}
			printf("\n");
			fflush(stdout);
		}

#ifdef MERCURY_GUI_ENABLED
		// Apply live LDPC iteration limit from GUI
		int gui_ldpc_max = g_gui_state.ldpc_iterations_max.load();
		if (gui_ldpc_max >= 5 && gui_ldpc_max <= 50)
			telecom_system->ldpc.nIteration_max = gui_ldpc_max;
#endif

		auto proc_start = std::chrono::steady_clock::now();
		received_message_stats = telecom_system->receive_byte(telecom_system->data_container.ready_to_process_passband_delayed_data,telecom_system->data_container.data_byte);
		auto proc_end = std::chrono::steady_clock::now();
		double proc_ms = std::chrono::duration<double, std::milli>(proc_end - proc_start).count();

		// Frame period = (preamble + data symbols) in wall clock time
		double frame_samples = (double)(telecom_system->data_container.Nofdm *
			(telecom_system->data_container.Nsymb + telecom_system->data_container.preamble_nSymb) *
			telecom_system->data_container.interpolation_rate);
		double frame_ms = (frame_samples / 48000.0) * 1000.0;
		float load = (frame_ms > 0) ? (float)(proc_ms / frame_ms) : 0.0f;

#ifdef MERCURY_GUI_ENABLED
		g_gui_state.processing_load.store(load);
		{
			size_t buf_used = size_buffer(capture_buffer);
			size_t buf_cap = circular_buf_capacity(capture_buffer);
			g_gui_state.buffer_fill_pct.store(buf_cap > 0 ? 100.0f * (float)buf_used / (float)buf_cap : 0.0f);
		}
#endif

		measurements.signal_stregth_dbm = received_message_stats.signal_stregth_dbm;

		if (received_message_stats.message_decoded==YES)
		{
			int rx_nsymb = telecom_system->get_active_nsymb();
			int rx_frame = rx_nsymb + telecom_system->data_container.preamble_nSymb;
			int end_of_current_message = received_message_stats.delay / symbol_period  + rx_frame;
			int frames_left_in_buffer = telecom_system->data_container.buffer_Nsymb - end_of_current_message;
			if(frames_left_in_buffer<0)
				frames_left_in_buffer=0;

			int nUnder_snapshot = telecom_system->data_container.nUnder_processing_events.load();
			telecom_system->data_container.frames_to_read=rx_frame-frames_left_in_buffer-nUnder_snapshot;

			int ftr_clamped = 0;
			if(telecom_system->data_container.frames_to_read > rx_frame || telecom_system->data_container.frames_to_read<0)
			{
				telecom_system->data_container.frames_to_read = rx_frame-frames_left_in_buffer;
				ftr_clamped = 1;
			}

			printf("[RX-TIMING] OK: delay=%d delay_symb=%d rx_frame=%d end=%d left=%d nUnder=%d ftr=%d clamped=%d proc=%.0fms\n",
				received_message_stats.delay, received_message_stats.delay / symbol_period,
				rx_frame, end_of_current_message, frames_left_in_buffer, nUnder_snapshot,
				telecom_system->data_container.frames_to_read.load(), ftr_clamped, proc_ms);
			fflush(stdout);

			// MFSK anti-re-decode: after successful decode, record where the old
			// frame ends so the next time_sync_mfsk skips past it entirely.
			// Must skip the full frame (preamble + data), not just the preamble,
			// because MFSK data tones can create false preamble correlations.
			// mfsk_search_raw = frame_end_symb - frames_to_read (base value).
			// telecom_system subtracts nUnder at search time for the effective start.
			if(telecom_system->M == MOD_MFSK)
			{
				int frame_end_symb = received_message_stats.delay / symbol_period + rx_frame;
				telecom_system->receive_stats.mfsk_search_raw = frame_end_symb - telecom_system->data_container.frames_to_read;
			}

			telecom_system->receive_stats.delay_of_last_decoded_message += (rx_frame - (telecom_system->data_container.frames_to_read + telecom_system->data_container.nUnder_processing_events)) * symbol_period;

			telecom_system->data_container.nUnder_processing_events = 0;

			measurements.frequency_offset = received_message_stats.freq_offset;
			if(this->role == COMMANDER)
			{
				measurements.SNR_uplink = received_message_stats.SNR;
			}
			else
			{
				measurements.SNR_downlink = received_message_stats.SNR;
			}

			for(int i=0; i < this->max_data_length + this->max_header_length; i++)
			{
				message_TxRx_byte_buffer[i] = (char)telecom_system->data_container.data_byte[i];
			}

			printf("[RX-DECODE] type=%d connid_rx=%d my_connid=%d broadcast=%d bytes:",
				(int)(unsigned char)message_TxRx_byte_buffer[0],
				(int)(unsigned char)message_TxRx_byte_buffer[1],
				(int)(unsigned char)this->connection_id,
				(int)BROADCAST_ID);
			for(int db=0; db<12; db++)
				printf(" %02x", (unsigned char)message_TxRx_byte_buffer[db]);
			printf("\n");
			fflush(stdout);
			if(message_TxRx_byte_buffer[1] == this->connection_id || message_TxRx_byte_buffer[1] == BROADCAST_ID)
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
			// MFSK frame completeness: if the frame extends beyond captured audio,
			// capture the remaining symbols instead of wasting a full recapture cycle.
			if(received_message_stats.frame_overflow_symbols > 0)
			{
				int shift_symbols = received_message_stats.frame_overflow_symbols + 4;
				telecom_system->data_container.frames_to_read = shift_symbols;
				telecom_system->data_container.nUnder_processing_events = 0;
				telecom_system->receive_stats.mfsk_search_raw = 0;

				// Save the preamble position adjusted for the upcoming buffer shift.
				// On the recapture attempt, receive_byte() uses this directly instead
				// of re-searching — avoids false peaks from FIR transients at the
				// zero/audio boundary in the shifted buffer.
				telecom_system->mfsk_fixed_delay =
					received_message_stats.delay - shift_symbols * symbol_period;
				if(telecom_system->mfsk_fixed_delay < 0)
					telecom_system->mfsk_fixed_delay = 0;

				printf("[RX-TIMING] INCOMPLETE: overflow=%d symbols, capturing %d more, saved_delay=%d\n",
					received_message_stats.frame_overflow_symbols,
					telecom_system->data_container.frames_to_read.load(),
					telecom_system->mfsk_fixed_delay);
				fflush(stdout);
				return;
			}

			printf("[RX-TIMING] FAIL: nUnder=%d proc=%.0fms search_raw=%d delay_last=%d mod=%d\n",
				telecom_system->data_container.nUnder_processing_events.load(), proc_ms,
				telecom_system->receive_stats.mfsk_search_raw,
				telecom_system->receive_stats.delay_of_last_decoded_message,
				telecom_system->M);
			fflush(stdout);

			// Prevent OFDM FAIL spin loop: pause 8 callbacks (~181ms) to let the
			// buffer accumulate fresh audio instead of burning CPU on doomed LDPC
			// decodes of the same shifting frame. MFSK has anti-re-decode so it
			// doesn't spin on the same frame.
			if(telecom_system->M != MOD_MFSK && telecom_system->data_container.frames_to_read == 0)
			{
				telecom_system->data_container.frames_to_read = 8;
				telecom_system->data_container.nUnder_processing_events = 0;
			}

			if(telecom_system->data_container.frames_to_read==0 && telecom_system->receive_stats.delay_of_last_decoded_message!=-1)
			{
				telecom_system->receive_stats.delay_of_last_decoded_message -= telecom_system->data_container.Nofdm*telecom_system->data_container.interpolation_rate;
				if(telecom_system->receive_stats.delay_of_last_decoded_message < 0)
				{
					telecom_system->receive_stats.delay_of_last_decoded_message = -1;
				}
			}
		}
		// Return here - we already unlocked the mutex at line 1929 and cleared data_ready
		return;
	}

	// frames_to_read != 0: just clear data_ready and unlock
	telecom_system->data_container.data_ready = 0;
	MUTEX_UNLOCK(&capture_prep_mutex);
}


void cl_arq_controller::copy_data_to_buffer()
{
	int copied = 0;
	int total_bytes = 0;
	for(int i=0;i<this->nMessages;i++)
	{
		if(messages_rx[i].status==ACKED)
		{
			fifo_buffer_rx.push(messages_rx[i].data,messages_rx[i].length);
			total_bytes += messages_rx[i].length;
			messages_rx[i].status=FREE;
			copied++;
		}
		else if(messages_rx[i].status!=FREE)
		{
			printf("[DBG-COPY] msg[%d] has status=%d (not ACKED=%d, not FREE=%d)\n",
				i, messages_rx[i].status, ACKED, FREE);
			fflush(stdout);
		}
	}
	printf("[DBG-COPY] copy_data_to_buffer: copied %d/%d messages, %d bytes to fifo_rx\n",
		copied, this->nMessages, total_bytes);
	fflush(stdout);
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
	printf("\033[2J");  // clean screen
	printf("\033[H");   // go to upper left corner

	if(this->current_configuration!=CONFIG_NONE)
	{
		printf("configuration:CONFIG_%d (%.1f bps)\n", (int)this->current_configuration, telecom_system->rbc);
	}
	else
	{
		printf("configuration: ERROR..( 0 bps)\n");
	}

	// Display audio devices
	extern char *input_dev;
	extern char *output_dev;
	printf("Audio_IN: %s\n", (input_dev ? input_dev : "default"));
	printf("Audio_OUT: %s\n", (output_dev ? output_dev : "default"));

	printf("\n");

	if(this->role==COMMANDER)
	{
		printf("Role:COM call sign= %s\n", this->my_call_sign.c_str());
	}
	else if (this->role==RESPONDER)
	{
		printf("Role:Res call sign= %s\n", this->my_call_sign.c_str());
	}

	if(this->link_status==DROPPED)
	{
		printf("link_status:Dropped\n");
	}
	else if(this->link_status==IDLE)
	{
		printf("link_status:Idle\n");
	}
	else if (this->link_status==CONNECTING)
	{
		printf("link_status:Connecting to %s\n", this->destination_call_sign.c_str());
	}
	else if (this->link_status==CONNECTED)
	{
		printf("link_status:Connected to %s ID= %d\n", this->destination_call_sign.c_str(), (int)this->connection_id);
	}
	else if (this->link_status==DISCONNECTING)
	{
		printf("link_status:Disconnecting\n");
	}
	else if (this->link_status==LISTENING)
	{
		printf("link_status:Listening\n");
	}
	else if (this->link_status==CONNECTION_RECEIVED)
	{
		printf("link_status:Connection Received from %s\n", this->destination_call_sign.c_str());
	}
	else if (this->link_status==CONNECTION_ACCEPTED)
	{
		printf("link_status:Connection Accepted by %s\n", this->destination_call_sign.c_str());
	}
	else if (link_status==NEGOTIATING)
	{
		printf("link_status:Negotiating with %s\n", this->destination_call_sign.c_str());
	}

	if (this->connection_status==TRANSMITTING_DATA)
	{
		printf("connection_status:Transmitting data\n");
	}
	else if (this->connection_status==RECEIVING)
	{
		printf("connection_status:Receiving\n");
	}
	else if (this->connection_status==RECEIVING_ACKS_DATA)
	{
		printf("connection_status:Receiving data Ack\n");
	}
	else if(this->connection_status==ACKNOWLEDGING_DATA)
	{
		printf("connection_status:Acknowledging data\n");
	}
	else if (this->connection_status==TRANSMITTING_CONTROL)
	{
		printf("connection_status:Transmitting control\n");
	}
	else if (this->connection_status==RECEIVING_ACKS_CONTROL)
	{
		printf("connection_status:Receiving control Ack\n");
	}
	else if (this->connection_status==ACKNOWLEDGING_CONTROL)
	{
		printf("connection_status:Acknowledging control\n");
	}
	else if(this->connection_status==IDLE)
	{
		printf("connection_status:Idle\n");
	}

	printf("measurements.SNR_uplink= %.2f\n", measurements.SNR_uplink);
	printf("measurements.SNR_downlink= %.2f\n", measurements.SNR_downlink);
	printf("measurements.signal_stregth_dbm= %.2f\n", measurements.signal_stregth_dbm);
	printf("measurements.frequency_offset= %.2f\n", measurements.frequency_offset);

	printf("\n");

	printf("stats.nSent_data= %d\n", stats.nSent_data);
	printf("stats.nAcked_data= %d\n", stats.nAcked_data);
	printf("stats.nReceived_data= %d\n", stats.nReceived_data);
	printf("stats.nLost_data= %d\n", stats.nLost_data);
	printf("stats.nReSent_data= %d\n", stats.nReSent_data);
	printf("stats.nAcks_sent_data= %d\n", stats.nAcks_sent_data);
	printf("stats.nNAcked_data= %d\n", stats.nNAcked_data);
	printf("stats.ToSend_data:%d\n", this->get_nToSend_messages());

	printf("\n");

	printf("stats.nSent_control= %d\n", stats.nSent_control);
	printf("stats.nAcked_control= %d\n", stats.nAcked_control);
	printf("stats.nReceived_control= %d\n", stats.nReceived_control);
	printf("stats.nLost_control= %d\n", stats.nLost_control);
	printf("stats.nReSent_control= %d\n", stats.nReSent_control);
	printf("stats.nAcks_sent_control= %d\n", stats.nAcks_sent_control);
	printf("stats.nNAcked_control= %d\n", stats.nNAcked_control);

	printf("\n");
	printf("link_timer= %d\n", link_timer.get_elapsed_time_ms());
	printf("watchdog_timer= %d\n", watchdog_timer.get_elapsed_time_ms());
	printf("gear_shift_timer= %d\n", gear_shift_timer.get_elapsed_time_ms());
	printf("receiving_timer= %d\n", receiving_timer.get_elapsed_time_ms());

	printf("\n");
	printf("last_received_message_sequence= %d\n", (int)last_received_message_sequence);

	printf("last_transmission_block_success_rate= %d %%\n", (int)last_transmission_block_stats.success_rate_data);
	if(gear_shift_blocked_for_nBlocks<gear_shift_block_for_nBlocks_total)
	{
		printf("gear_shift_blocked_for_nBlocks= %d\n", (int)gear_shift_blocked_for_nBlocks);
	}
	else
	{
		printf("gear_shift_blocked_for_nBlocks=\n");
	}

	printf("\n");

	const char* msg_sent_str = "";
	if (this->last_message_sent_type==NONE)
	{
		msg_sent_str = "last_message_sent:";
	}
	else if (this->last_message_sent_type==DATA_LONG)
	{
		msg_sent_str = "last_message_sent:DATA:DATA_LONG";
	}
	else if (this->last_message_sent_type==DATA_SHORT)
	{
		msg_sent_str = "last_message_sent:DATA:DATA_SHORT";
	}
	else if (this->last_message_sent_type==ACK_MULTI)
	{
		msg_sent_str = "last_message_sent:DATA:ACK_MULTI";
	}
	else if (this->last_message_sent_type==ACK_RANGE)
	{
		msg_sent_str = "last_message_sent:DATA:ACK_RANGE";
	}
	else if (this->last_message_sent_type==CONTROL)
	{
		msg_sent_str = "last_message_sent:CONTROL:";
	}
	else if (this->last_message_sent_type==ACK_CONTROL)
	{
		msg_sent_str = "last_message_sent:ACK_CONTROL:";
	}

	const char* msg_sent_code_str = "";
	if(this->last_message_sent_type==CONTROL || this->last_message_sent_type==ACK_CONTROL)
	{
		if (this->last_message_sent_code==START_CONNECTION) msg_sent_code_str = "START_CONNECTION";
		else if (this->last_message_sent_code==TEST_CONNECTION) msg_sent_code_str = "TEST_CONNECTION";
		else if (this->last_message_sent_code==CLOSE_CONNECTION) msg_sent_code_str = "CLOSE_CONNECTION";
		else if (this->last_message_sent_code==KEEP_ALIVE) msg_sent_code_str = "KEEP_ALIVE";
		else if (this->last_message_sent_code==FILE_START) msg_sent_code_str = "FILE_START";
		else if (this->last_message_sent_code==FILE_END_) msg_sent_code_str = "FILE_END";
		else if (this->last_message_sent_code==PIPE_OPEN) msg_sent_code_str = "PIPE_OPEN";
		else if (this->last_message_sent_code==PIPE_CLOSE) msg_sent_code_str = "PIPE_CLOSE";
		else if (this->last_message_sent_code==SWITCH_ROLE) msg_sent_code_str = "SWITCH_ROLE";
		else if (this->last_message_sent_code==BLOCK_END) msg_sent_code_str = "BLOCK_END";
		else if (this->last_message_sent_code==SET_CONFIG) msg_sent_code_str = "SET_CONFIG";
		else if (this->last_message_sent_code==REPEAT_LAST_ACK) msg_sent_code_str = "REPEAT_LAST_ACK";
	}
	printf("%s%s\n", msg_sent_str, msg_sent_code_str);

	const char* msg_recv_str = "";
	if (this->last_message_received_type==NONE)
	{
		msg_recv_str = "last_message_received:";
	}
	else if (this->last_message_received_type==DATA_LONG)
	{
		msg_recv_str = "last_message_received:DATA:DATA_LONG";
	}
	else if (this->last_message_received_type==DATA_SHORT)
	{
		msg_recv_str = "last_message_received:DATA:DATA_SHORT";
	}
	else if (this->last_message_received_type==ACK_MULTI)
	{
		msg_recv_str = "last_message_received:DATA:ACK_MULTI";
	}
	else if (this->last_message_received_type==ACK_RANGE)
	{
		msg_recv_str = "last_message_received:DATA:ACK_RANGE";
	}
	else if (this->last_message_received_type==CONTROL)
	{
		msg_recv_str = "last_message_received:CONTROL:";
	}
	else if (this->last_message_received_type==ACK_CONTROL)
	{
		msg_recv_str = "last_message_received:ACK_CONTROL:";
	}

	const char* msg_recv_code_str = "";
	if(this->last_message_received_type==CONTROL || this->last_message_received_type==ACK_CONTROL)
	{
		if (this->last_message_received_code==START_CONNECTION) msg_recv_code_str = "START_CONNECTION";
		else if (this->last_message_received_code==TEST_CONNECTION) msg_recv_code_str = "TEST_CONNECTION";
		else if (this->last_message_received_code==CLOSE_CONNECTION) msg_recv_code_str = "CLOSE_CONNECTION";
		else if (this->last_message_received_code==KEEP_ALIVE) msg_recv_code_str = "KEEP_ALIVE";
		else if (this->last_message_received_code==FILE_START) msg_recv_code_str = "FILE_START";
		else if (this->last_message_received_code==FILE_END_) msg_recv_code_str = "FILE_END";
		else if (this->last_message_received_code==PIPE_OPEN) msg_recv_code_str = "PIPE_OPEN";
		else if (this->last_message_received_code==PIPE_CLOSE) msg_recv_code_str = "PIPE_CLOSE";
		else if (this->last_message_received_code==SWITCH_ROLE) msg_recv_code_str = "SWITCH_ROLE";
		else if (this->last_message_received_code==BLOCK_END) msg_recv_code_str = "BLOCK_END";
		else if (this->last_message_received_code==SET_CONFIG) msg_recv_code_str = "SET_CONFIG";
		else if (this->last_message_received_code==REPEAT_LAST_ACK) msg_recv_code_str = "REPEAT_LAST_ACK";
	}
	printf("%s%s\n", msg_recv_str, msg_recv_code_str);

	printf("\n");
	printf("TX buffer occupancy= %.2f %%\n", (float)(fifo_buffer_tx.get_size()-fifo_buffer_tx.get_free_size())*100.0f/(float)fifo_buffer_tx.get_size());
	printf("RX buffer occupancy= %.2f %%\n", (float)(fifo_buffer_rx.get_size()-fifo_buffer_rx.get_free_size())*100.0f/(float)fifo_buffer_rx.get_size());
	printf("Backup buffer occupancy= %.2f %%\n", (float)(fifo_buffer_backup.get_size()-fifo_buffer_backup.get_free_size())*100.0f/(float)fifo_buffer_backup.get_size());
	fflush(stdout);
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

