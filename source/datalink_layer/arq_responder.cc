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

#ifdef MERCURY_GUI_ENABLED
#include "gui/gui_state.h"
#endif

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
	if(loc>=nMessages || loc<0)
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
#ifdef MERCURY_GUI_ENABLED
		gui_add_throughput_bytes_rx(messages_rx[loc].length);
#endif
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

		// Emergency BREAK: commander signals "drop to ROBUST_0"
		if(break_detected == YES)
		{
			printf("[BREAK] Responding with ACK, dropping to ROBUST_0\n");
			fflush(stdout);
			break_detected = NO;

			// Send ACK to confirm BREAK received
			send_ack_pattern();

			// Drop to ROBUST_0 (commander will send SET_CONFIG at ROBUST_0)
			int target = robust_enabled ? ROBUST_0 : CONFIG_0;
			data_configuration = target;
			load_configuration(target, PHYSICAL_LAYER_ONLY, YES);

			// Wait for SET_CONFIG from commander
			calculate_receiving_timeout();
			receiving_timer.start();
			connection_status = RECEIVING;
			link_timer.start();
			return;
		}

		if(messages_rx_buffer.status==RECEIVED)
		{
			if(messages_rx_buffer.type==CONTROL)
			{
				printf("[RX] CONTROL message received on CONFIG_%d, code=%d seq=%d/%d\n",
					current_configuration, (int)messages_rx_buffer.data[0],
					messages_rx_buffer.sequence_number, control_batch_size);
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
				// BUG FIX: Process control message immediately when batch is complete
				// instead of waiting for timer (which kept getting reset by retransmissions)
				if(messages_rx_buffer.sequence_number >= control_batch_size - 1)
				{
					// Last frame in batch received - process immediately
					printf("[RX] Batch complete, processing control message immediately\n");
					receiving_timer.stop();
					receiving_timer.reset();
					if(messages_control.status==RECEIVED)
					{
						process_control_responder();
					}
				}
				else
				{
					// More frames expected in this batch - wait for them
					set_receiving_timeout((control_batch_size-messages_rx_buffer.sequence_number-1)*message_transmission_time_ms+time_left_to_send_last_frame+ptt_on_delay_ms);
					receiving_timer.start();
				}
			}
			else if(messages_rx_buffer.type==DATA_LONG || messages_rx_buffer.type==DATA_SHORT)
			{
				printf("[RX-DATA] type=%d id=%d seq=%d/%d len=%d\n",
					messages_rx_buffer.type, (int)(unsigned char)messages_rx_buffer.id,
					messages_rx_buffer.sequence_number, data_batch_size,
					messages_rx_buffer.length);
				fflush(stdout);
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
	printf("[ACK-CTRL] status=%d (need %d=RECEIVED), ack_cfg=%d\n",
		messages_control.status, RECEIVED, ack_configuration);
	fflush(stdout);
	if(messages_control.status==RECEIVED)
	{
		messages_control.type=ACK_CONTROL;
		messages_control.status=ACKED;
		stats.nAcks_sent_control++;

		if(ack_pattern_time_ms > 0)
		{
			// ACK pattern uses dedicated ack_mfsk — no config switch needed
			printf("[ACK-CTRL] Sending ACK pattern (no config switch)\n");
			fflush(stdout);
			send_ack_pattern();
			// If config changed (e.g., SET_CONFIG), load the new data config now.
			// ACK was sent on old config (correct — commander is still on old config),
			// but we need to switch to new config before receiving data.
			if(data_configuration != current_configuration)
			{
				printf("[ACK-CTRL] Loading new data config %d (was %d)\n",
					data_configuration, current_configuration);
				fflush(stdout);
				load_configuration(data_configuration, PHYSICAL_LAYER_ONLY, YES);
			}
		}
		else
		{
			// Fallback: LDPC ACK needs ack_configuration for correct modulation
			printf("[ACK-CTRL] Sending LDPC ACK, loading config %d...\n", ack_configuration);
			fflush(stdout);
			load_configuration(ack_configuration, PHYSICAL_LAYER_ONLY,NO);
			messages_batch_tx[message_batch_counter_tx]=messages_control;
			message_batch_counter_tx++;
			telecom_system->set_mfsk_ctrl_mode(true);
			pad_messages_batch_tx(ack_batch_size);
			send_batch();
			load_configuration(data_configuration, PHYSICAL_LAYER_ONLY,YES);
		}
		// Capture frame + turnaround gap: ~1200ms for CMD ACK detection (~900ms
		// polling at 4-sym intervals) + guard (200ms) + config load + encode/TX.
		// Must match buffer allocation (data_container.cc) to avoid preamble
		// position exceeding upper_bound when arrival is late.
		telecom_system->set_mfsk_ctrl_mode(false);
		{
			// During load_configuration(), the capture thread keeps shifting
			// the buffer (frames_to_read=0, data_ready=1 → nUnder accumulates).
			// These shifts count toward the turnaround — the commander's ACK
			// detection + encode + TX happens concurrently with our config load.
			// Subtract the elapsed symbols so the total countdown (load_time +
			// ftr) matches the intended turnaround, keeping the preamble near
			// the right edge of the buffer instead of buried in silence.
			int nUnder_during_load = telecom_system->data_container.nUnder_processing_events.load();
			telecom_system->data_container.nUnder_processing_events = 0;

			double sym_time_ms = telecom_system->data_container.Nofdm
				* telecom_system->data_container.interpolation_rate / 48.0;
			int turnaround_symb = (int)ceil(1200.0 / sym_time_ms) + 4;
			turnaround_symb -= nUnder_during_load;
			if(turnaround_symb < 0) turnaround_symb = 0;
			int ftr = telecom_system->data_container.preamble_nSymb
				+ telecom_system->data_container.Nsymb
				+ turnaround_symb;
			int buf_nsymb = telecom_system->data_container.buffer_Nsymb.load();
			if(ftr > buf_nsymb) ftr = buf_nsymb;
			telecom_system->data_container.frames_to_read = ftr;

			printf("[ACK-CTRL] ftr=%d (turnaround=%d - load_shift=%d)\n",
				ftr, (int)ceil(1200.0 / sym_time_ms) + 4, nUnder_during_load);
			fflush(stdout);
		}

		messages_control.status=FREE;
		connection_status=RECEIVING;
		connection_id=assigned_connection_id;

		if (messages_control.data[0]==SWITCH_ROLE)
		{
			set_role(COMMANDER);
			this->link_status=CONNECTED;
			cl_timer ptt_off_wait;
			ptt_off_wait.reset();
			ptt_off_wait.start();
			while(ptt_off_wait.get_elapsed_time_ms()<ptt_off_delay_ms);

			bool has_asymmetric = (forward_configuration != CONFIG_NONE &&
				reverse_configuration != CONFIG_NONE);

			if(has_asymmetric)
			{
				// Asymmetric gearshift: swap forward/reverse for the return path
				char tmp = forward_configuration;
				forward_configuration = reverse_configuration;
				reverse_configuration = tmp;

				if(forward_configuration != current_configuration)
				{
					data_configuration = forward_configuration;
					load_configuration(data_configuration, PHYSICAL_LAYER_ONLY, YES);
				}

				printf("[GEARSHIFT] SWITCH_ROLE: transmitting at config %d\n",
					forward_configuration);
				fflush(stdout);
			}

			// Turboshift: start probing reverse direction as new commander
			if(has_asymmetric && turboshift_phase == TURBO_FORWARD && gear_shift_on == YES)
			{
				turboshift_phase = TURBO_REVERSE;
				turboshift_active = true;
				turboshift_last_good = current_configuration;

				if(!config_is_at_top(current_configuration, robust_enabled))
				{
					negotiated_configuration = config_ladder_up(current_configuration, robust_enabled);
					printf("[TURBO] Phase: REVERSE — probing responder->commander\n");
					printf("[TURBO] UP: config %d -> %d\n",
						current_configuration, negotiated_configuration);
					fflush(stdout);
					add_message_control(SET_CONFIG);
					this->connection_status = TRANSMITTING_CONTROL;
				}
				else
				{
					printf("[TURBO] REVERSE: already at top (%d), done\n",
						current_configuration);
					fflush(stdout);
					turboshift_active = false;
					turboshift_phase = TURBO_DONE;
					cleanup();
					add_message_control(SWITCH_ROLE);
					this->connection_status = TRANSMITTING_CONTROL;
				}
			}
			else if(has_asymmetric &&
				(turboshift_phase == TURBO_REVERSE || turboshift_phase == TURBO_DONE))
			{
				// Returning to original roles after reverse probe
				turboshift_phase = TURBO_DONE;
				turboshift_active = false;
				printf("[TURBO] DONE — starting data exchange\n");
				fflush(stdout);
				this->connection_status = TRANSMITTING_DATA;
			}
			else if(!has_asymmetric)
			{
				// No asymmetric negotiation (old firmware): fall back to TEST_CONNECTION
				add_message_control(TEST_CONNECTION);
				this->connection_status = TRANSMITTING_CONTROL;
			}
			else
			{
				this->connection_status = TRANSMITTING_DATA;
			}

			// Don't start the switch_role_test_timer during turboshift —
			// turboshift probes with SET_CONFIG which doesn't reset this timer,
			// causing it to force role=RESPONDER mid-probe or after TURBO_DONE.
			if(!turboshift_active && turboshift_phase != TURBO_DONE)
			{
				switch_role_test_timer.reset();
				switch_role_test_timer.start();
			}
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
			// Reset RX state machine - wait for fresh data (prevents decode of self-received TX audio)
			telecom_system->data_container.frames_to_read =
				telecom_system->data_container.preamble_nSymb + telecom_system->data_container.Nsymb;
			telecom_system->data_container.nUnder_processing_events = 0;
		}
	}
}


void cl_arq_controller::process_messages_acknowledging_data()
{
	int nAck_messages=0;
	receiving_timer.stop();
	receiving_timer.reset();

	if(ack_pattern_time_ms > 0)
	{
		// Send ACK tone pattern (universal, all modes)
		if(repeating_last_ack==NO)
		{
			// Mark all received messages as ACKED and count for stats
			for(int i=0; i<this->nMessages; i++)
			{
				if(messages_rx[i].status==RECEIVED)
				{
					messages_rx[i].status=ACKED;
					nAck_messages++;
				}
			}
			stats.nAcks_sent_data += nAck_messages;
		}
		repeating_last_ack=NO;
		messages_control.status=FREE;

		// ACK pattern uses dedicated ack_mfsk (M=16, nStreams=1) — no config switch needed
		send_ack_pattern();

		// Capture frame + turnaround gap: ~1200ms for CMD ACK detection (~900ms
		// polling at 4-sym intervals) + guard (200ms) + config load + encode/TX.
		// Must match buffer allocation (data_container.cc) to avoid preamble
		// position exceeding upper_bound when arrival is late.
		telecom_system->set_mfsk_ctrl_mode(false);
		{
			int nUnder_during_load = telecom_system->data_container.nUnder_processing_events.load();
			telecom_system->data_container.nUnder_processing_events = 0;

			double sym_time_ms = telecom_system->data_container.Nofdm
				* telecom_system->data_container.interpolation_rate / 48.0;
			int turnaround_symb = (int)ceil(1200.0 / sym_time_ms) + 4;
			turnaround_symb -= nUnder_during_load;
			if(turnaround_symb < 0) turnaround_symb = 0;
			int ftr = telecom_system->data_container.preamble_nSymb
				+ telecom_system->data_container.Nsymb
				+ turnaround_symb;
			int buf_nsymb = telecom_system->data_container.buffer_Nsymb.load();
			if(ftr > buf_nsymb) ftr = buf_nsymb;
			telecom_system->data_container.frames_to_read = ftr;

			printf("[ACK-DATA] ftr=%d (turnaround=%d - load_shift=%d)\n",
				ftr, (int)ceil(1200.0 / sym_time_ms) + 4, nUnder_during_load);
			fflush(stdout);
		}

		connection_status=RECEIVING;
	}
	else
	{
		// Fallback: send LDPC-encoded ACK_MULTI frame (not currently reachable)
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
			// Clamp to buffer size — init_messages_buffers allocated N_MAX/8 bytes.
			// Don't reallocate: reuse existing buffer to avoid memory leak (Bug #17).
			if(nAck_messages + 1 > N_MAX / 8)
				nAck_messages = N_MAX / 8 - 1;
			messages_batch_ack[message_batch_counter_tx].length=nAck_messages+1;
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

		telecom_system->set_mfsk_ctrl_mode(true);  // data ACK TX (short ctrl frame)
		pad_messages_batch_tx(ack_batch_size);
		send_batch();

		load_configuration(data_configuration, PHYSICAL_LAYER_ONLY,YES);
		// Expect data frames next: use full Nsymb for capture.
		// Frame completeness gating handles late arrivals adaptively.
		telecom_system->set_mfsk_ctrl_mode(false);
		telecom_system->data_container.frames_to_read =
			telecom_system->data_container.preamble_nSymb + telecom_system->data_container.Nsymb;

		connection_status=RECEIVING;
	}

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
	printf("[RX-CTRL] Processing control message: code=%d (0=START, 1=TEST, 2=SET_CFG, 3=BLOCK_END, 4=FILE_END, 5=SWITCH, 6=CLOSE, 7=REPEAT)\n", (int)code);
	if((link_status==LISTENING || link_status==CONNECTION_RECEIVED) && code==START_CONNECTION)
	{
		unsigned char received_crc = (unsigned char)messages_control.data[1];
		unsigned char my_crc = CRC8_calc((char*)my_call_sign.c_str(), my_call_sign.length());
		printf("[RX-CTRL] START_CONNECTION received. CRC check: received=0x%02X, my_call='%s' (len=%d), my_crc=0x%02X\n",
			received_crc, my_call_sign.c_str(), (int)my_call_sign.length(), my_crc);

		if(received_crc == my_crc)
		{
			destination_call_sign="";
			// Clamp callsign length to buffer bounds — garbage LDPC decodes
			// under noise can have data[2]=127 (signed char max), reading
			// past valid data into the 200-byte buffer (Bug #19).
			int cs_len = (int)(unsigned char)messages_control.data[2];
			int max_cs = max_data_length + max_header_length - CONTROL_ACK_CONTROL_HEADER_LENGTH - 3;
			if(max_cs < 0) max_cs = 0;
			if(cs_len > max_cs) cs_len = max_cs;
			for(int i=0;i<cs_len;i++)
			{
				destination_call_sign+=messages_control.data[3+i];
			}

			// Send PENDING to Winlink to notify incoming connection
			// This allows Winlink to stop scanning and prepare PTT
			std::string pending_str="PENDING "+destination_call_sign+"\r";
			tcp_socket_control.message->length=pending_str.length();
			for(int i=0;i<(int)pending_str.length();i++)
			{
				tcp_socket_control.message->buffer[i]=pending_str[i];
			}
			tcp_socket_control.transmit();

			link_status=CONNECTION_RECEIVED;
			connection_status=ACKNOWLEDGING_CONTROL;
			if(ack_pattern_time_ms > 0)
			{
				// ACK pattern carries no data, both sides use BROADCAST_ID
				messages_control.data[1]=BROADCAST_ID;
			}
			else
			{
				// Fallback: assign random connection_id sent back in ACK frame
				messages_control.data[1]=1+rand()%0xfe;
			}
			messages_control.length=2;
			assigned_connection_id=messages_control.data[1];
			watchdog_timer.start();
		}
		else
		{
			printf("[RX-CTRL] START_CONNECTION REJECTED - callsign CRC mismatch! Is MYCALL set correctly?\n");
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
			// Asymmetric gearshift: extract forward and reverse configs
			// data[0]=SET_CONFIG, data[1]=forward, data[2]=reverse
			// Always 3-byte payload from our fork; data[2] is always present
			// in messages_rx_buffer (full buffer copied at arq_common.cc:2437)
			forward_configuration = messages_control.data[1];
			reverse_configuration = messages_control.data[2];

			printf("[GEARSHIFT] Received SET_CONFIG: forward=%d reverse=%d\n",
				forward_configuration, reverse_configuration);

			if(forward_configuration != current_configuration &&
				(is_ofdm_config(forward_configuration) || is_robust_config(forward_configuration)))
			{
				// Don't load_configuration here — ack_configuration must stay on
				// the OLD config so the ACK reaches the commander (still on old config).
				// Just save data_configuration; acknowledging_control will call
				// load_configuration(data_configuration, ...) after the ACK is sent.
				data_configuration = forward_configuration;
			}

			connection_status=ACKNOWLEDGING_CONTROL;
			link_timer.start();
			watchdog_timer.start();
			gear_shift_timer.start();
		}
		else if(code==BLOCK_END)
		{
			connection_status=ACKNOWLEDGING_CONTROL;
			printf("end of block\n");
			copy_data_to_buffer();
			messages_last_ack_bu.type=NONE;
			link_timer.start();
			watchdog_timer.start();
		}
		else if(code==FILE_END_)
		{
			connection_status=ACKNOWLEDGING_CONTROL;
			printf("end of file\n");
			copy_data_to_buffer();
			messages_last_ack_bu.type=NONE;
			link_timer.start();
			watchdog_timer.start();
		}
		else if(code==SWITCH_ROLE)
		{
			connection_status=ACKNOWLEDGING_CONTROL;
			printf("switch role\n");
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
		int fifo_used = fifo_buffer_rx.get_size() - fifo_buffer_rx.get_free_size();
		if(fifo_used > 0)
		{
			printf("[DBG-RSP-TX] fifo_rx has %d bytes, tcp_status=%d\n",
				fifo_used, tcp_socket_data.get_status());
			fflush(stdout);
		}
		if (tcp_socket_data.get_status()==TCP_STATUS_ACCEPTED)
		{
			while(fifo_buffer_rx.get_size()!=fifo_buffer_rx.get_free_size())
			{
				tcp_socket_data.message->length=fifo_buffer_rx.pop(tcp_socket_data.message->buffer,max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH);
				printf("[DBG-RSP-TX] Sending %d bytes via TCP\n", tcp_socket_data.message->length);
				fflush(stdout);
				tcp_socket_data.transmit();
			}
		}

	}
}


