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

void cl_arq_controller::register_ack(int message_id)
{
	if(message_id>=0 && message_id<this->nMessages && messages_tx[message_id].status==PENDING_ACK)
	{
		messages_tx[message_id].status=ACKED;
		stats.nAcked_data++;
#ifdef MERCURY_GUI_ENABLED
		gui_add_throughput_bytes_tx(messages_tx[message_id].length);
#endif
	}
}


void cl_arq_controller::process_messages_commander()
{
	// Emergency BREAK state machine: poll for ACK after sending BREAK pattern
	if(emergency_break_active)
	{
		if(disconnect_requested==YES)
		{
			emergency_break_active=0;
			emergency_nack_count=0;
			// Fall through to normal disconnect handling below
		}
		else
		{
		if(receiving_timer.get_elapsed_time_ms() < receiving_timeout)
		{
			if(receive_ack_pattern())
			{
				// Use ROBUST_0 as coordination layer, then probe target config.
				// Phase 1: send SET_CONFIG at ROBUST_0 (guaranteed delivery).
				// Phase 2: send SET_CONFIG at target to verify it works (2 tries).
				int target = config_ladder_down_n(emergency_previous_config, break_drop_step, robust_enabled);
				printf("[BREAK] ACK received! Dropping %d step(s): config %d -> %d\n",
					break_drop_step, emergency_previous_config, target);
				fflush(stdout);
				if(break_drop_step < 4) break_drop_step *= 2;

				emergency_break_active = 0;
				emergency_nack_count = 0;
				break_recovery_phase = 1;
				break_recovery_retries = 2;

				int robust_0 = robust_enabled ? ROBUST_0 : CONFIG_0;
				messages_control_backup();
				data_configuration = target;
				load_configuration(robust_0, PHYSICAL_LAYER_ONLY, YES);
				messages_control_restore();

				negotiated_configuration = target;
				forward_configuration = target;
				if(reverse_configuration == CONFIG_NONE)
					reverse_configuration = target;

				for(int i=0; i<nMessages; i++)
				{
					if(messages_tx[i].status == ADDED_TO_LIST || messages_tx[i].status == ADDED_TO_BATCH_BUFFER)
						fifo_buffer_tx.push(messages_tx[i].data, messages_tx[i].length);
					messages_tx[i].status = FREE;
				}
				fifo_buffer_backup.flush();
				block_under_tx = NO;

				// Force-clear: cleanup() skips PENDING_ACK status
				messages_control.status = FREE;
				add_message_control(SET_CONFIG);
				connection_status = TRANSMITTING_CONTROL;
				link_timer.start();
				watchdog_timer.start();
			}
		}
		else
		{
			// Timeout — retry BREAK
			emergency_break_retries--;
			if(emergency_break_retries > 0)
			{
				printf("[BREAK] Retry (%d left)\n", emergency_break_retries);
				fflush(stdout);
				send_break_pattern();
				telecom_system->data_container.frames_to_read = 4;
				calculate_receiving_timeout();
				receiving_timer.start();
			}
			else
			{
				printf("[BREAK] All retries exhausted — assuming responder already at ROBUST_0\n");
				fflush(stdout);
				emergency_break_active = 0;
				emergency_nack_count = 0;
				break_recovery_phase = 1;
				break_recovery_retries = 2;

				int target = config_ladder_down_n(emergency_previous_config, break_drop_step, robust_enabled);
				printf("[BREAK] Dropping %d step(s): config %d -> %d\n",
					break_drop_step, emergency_previous_config, target);
				fflush(stdout);
				if(break_drop_step < 4) break_drop_step *= 2;

				int robust_0 = robust_enabled ? ROBUST_0 : CONFIG_0;
				messages_control_backup();
				data_configuration = target;
				load_configuration(robust_0, PHYSICAL_LAYER_ONLY, YES);
				messages_control_restore();

				negotiated_configuration = target;
				forward_configuration = target;
				if(reverse_configuration == CONFIG_NONE)
					reverse_configuration = target;

				for(int i=0; i<nMessages; i++)
				{
					if(messages_tx[i].status == ADDED_TO_LIST || messages_tx[i].status == ADDED_TO_BATCH_BUFFER)
						fifo_buffer_tx.push(messages_tx[i].data, messages_tx[i].length);
					messages_tx[i].status = FREE;
				}
				fifo_buffer_backup.flush();
				block_under_tx = NO;

				// Force-clear: cleanup() skips PENDING_ACK status
				messages_control.status = FREE;
				add_message_control(SET_CONFIG);
				connection_status = TRANSMITTING_CONTROL;
				link_timer.start();
				watchdog_timer.start();
			}
		}
		return;
		} // else (not disconnect_requested)
	}

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

			// Switch to RESPONDER/LISTENING so we can receive incoming connections
			set_role(RESPONDER);
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
			// Reset RX state machine - wait for fresh data (prevents decode of self-received TX audio)
			telecom_system->data_container.frames_to_read =
				telecom_system->data_container.preamble_nSymb + telecom_system->data_container.Nsymb;
			telecom_system->data_container.nUnder_processing_events = 0;

			fifo_buffer_tx.flush();
			fifo_buffer_backup.flush();
			fifo_buffer_rx.flush();

			// Reset messages_control so new CONNECT commands can work
			messages_control.status=FREE;

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

			// Calculate max callsign length that fits in the message buffer
			int max_callsign_chars = max_data_length + max_header_length - CONTROL_ACK_CONTROL_HEADER_LENGTH - 3;
			if (max_callsign_chars < 0) max_callsign_chars = 0;
			int callsign_len = (int)my_call_sign.length();
			if (callsign_len > max_callsign_chars) callsign_len = max_callsign_chars;

			for(int j=0;j<callsign_len;j++)
			{
				messages_control.data[j+3]=my_call_sign[j];
				my_call_sign_sent_chars++;
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
			messages_control.data[0]=code;
			messages_control.id=0;

			if(gear_shift_algorithm==SNR_BASED)
			{
				forward_configuration = get_configuration(measurements.SNR_downlink);
				reverse_configuration = get_configuration(measurements.SNR_uplink);
			}
			else
			{
				// SUCCESS_BASED_LADDER: asymmetric — only update forward (TX direction)
				forward_configuration = negotiated_configuration;
				// reverse_configuration preserved from other direction's gearshift
				if(reverse_configuration == CONFIG_NONE)
					reverse_configuration = forward_configuration;
			}

			negotiated_configuration = forward_configuration;
			messages_control.data[1] = forward_configuration;
			messages_control.data[2] = reverse_configuration;
			messages_control.length = 3;

			printf("[GEARSHIFT] SET_CONFIG: forward=%d reverse=%d (SNR down=%.1f up=%.1f)\n",
				forward_configuration, reverse_configuration,
				measurements.SNR_downlink, measurements.SNR_uplink);
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
				// Reset timer for this new attempt (making connection_timeout a per-attempt timeout)
				connection_attempt_timer.reset();
				connection_attempt_timer.start();
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
		// Commander CONTROL TX: full-length frames (responder can't predict frame type)
		telecom_system->set_mfsk_ctrl_mode(false);
		pad_messages_batch_tx(control_batch_size);
		send_batch();
		if(ack_pattern_time_ms > 0)
		{
			// Expect ACK tone pattern. Poll frequently — receive_ack_pattern()
			// scans only the tail of the buffer, so each poll is cheap (~120μs).
			// The ACK arrives after responder decode + prep + TX + audio latency;
			// polling adapts to any round-trip time without timing estimation.
			telecom_system->data_container.frames_to_read = 4;
		}
		else
		{
			// Fallback: expect short LDPC ctrl frame
			telecom_system->set_mfsk_ctrl_mode(true);
			telecom_system->data_container.frames_to_read =
				telecom_system->data_container.preamble_nSymb + telecom_system->get_active_nsymb();
		}
		connection_status=RECEIVING_ACKS_CONTROL;

		// ACK pattern detection uses dedicated ack_mfsk — no config switch needed
		if(ack_pattern_time_ms <= 0)
			load_configuration(ack_configuration, PHYSICAL_LAYER_ONLY,NO);

		// Recalculate timeout: guard delays from prior ACK detection can leave
		// receiving_timeout stale (e.g. 900ms), too short for the control round-trip.
		calculate_receiving_timeout();
		receiving_timer.start();
		printf("[CMD-RX] Entering receive mode: ack_cfg=%d recv_timeout=%d msg_tx_time=%d ctrl_tx_time=%d ack_batch=%d ftr=%d\n",
			ack_configuration, receiving_timeout, message_transmission_time_ms, ctrl_transmission_time_ms, ack_batch_size,
			telecom_system->data_container.frames_to_read.load());
		fflush(stdout);

		if(messages_control.data[0]==SET_CONFIG)
		{
			if(negotiated_configuration!= current_configuration)
			{
				data_configuration=negotiated_configuration;
				gear_shift_timer.start();
			}
			// Always wait for SET_CONFIG ACK (stay in RECEIVING_ACKS_CONTROL).
			// Previously jumped to TRANSMITTING_DATA when negotiated==current,
			// causing collision after BREAK (commander sent data before responder
			// finished processing SET_CONFIG).
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
		telecom_system->set_mfsk_ctrl_mode(false);  // data TX (full-length frames)
		pad_messages_batch_tx(data_batch_size);
		send_batch();
		if(ack_pattern_time_ms > 0)
		{
			// Expect ACK tone pattern — poll frequently (same as control path).
			telecom_system->data_container.frames_to_read = 4;
		}
		else
		{
			// Fallback: expect short LDPC ctrl frame
			telecom_system->set_mfsk_ctrl_mode(true);
			telecom_system->data_container.frames_to_read =
				telecom_system->data_container.preamble_nSymb + telecom_system->get_active_nsymb();
		}
		data_ack_received=NO;
		connection_status=RECEIVING_ACKS_DATA;
		// ACK pattern detection uses dedicated ack_mfsk — no config switch needed
		if(ack_pattern_time_ms <= 0)
			load_configuration(ack_configuration, PHYSICAL_LAYER_ONLY,NO);
		// Recalculate timeout: guard delays from prior ACK detection can leave
		// receiving_timeout stale, too short for the next ACK round-trip.
		calculate_receiving_timeout();
		receiving_timer.start();
	}
}

void cl_arq_controller::process_messages_rx_acks_control()
{
	if (receiving_timer.get_elapsed_time_ms()<receiving_timeout)
	{
		if(ack_pattern_time_ms > 0)
		{
			// Detect ACK tone pattern instead of decoding LDPC frame
			// Only call receive_ack_pattern while still waiting for the ACK.
			// Once ACKED, stop checking — re-detection would zero the buffer
			// and destroy the next frame's preamble arriving during the guard.
			if(messages_control.status==PENDING_ACK)
			{
				if(receive_ack_pattern())
				{
					printf("[CMD-ACK-PAT] Control ACK pattern detected!\n");
					fflush(stdout);
					// Flush old batch audio from playback buffer so responder
					// doesn't demodulate stale frames before the new batch.
					clear_buffer(playback_buffer);
					link_timer.start();
					watchdog_timer.start();
					gear_shift_timer.stop();
					gear_shift_timer.reset();
					messages_control.status=ACKED;
					stats.nAcked_control++;

					// Guard delay: wait for responder to finish ACK TX + config load.
					// ACK pattern ~363ms, CMD detects mid-way, RSP needs ~200ms after
					// ACK for config load. Guard + ~300ms CMD processing = turnaround.
					int guard = ptt_off_delay_ms + 200;
					receiving_timeout = (int)receiving_timer.get_elapsed_time_ms() + guard;
				}
			}
		}
		else
		{
			// Fallback: decode LDPC ACK frame
			this->receive();
			if(messages_rx_buffer.status==RECEIVED && messages_rx_buffer.type==ACK_CONTROL)
			{
				if(messages_rx_buffer.data[0]==messages_control.data[0] && messages_control.status==PENDING_ACK)
				{
					// Flush old batch audio from playback buffer so responder
					// doesn't demodulate stale frames before the new batch.
					clear_buffer(playback_buffer);
					{
					int copy_len = max_data_length+max_header_length-CONTROL_ACK_CONTROL_HEADER_LENGTH;
					if(copy_len > N_MAX/8) copy_len = N_MAX/8;
					for(int j=0;j<copy_len;j++)
					{
						messages_control.data[j]=messages_rx_buffer.data[j];
					}
				}
					link_timer.start();
					watchdog_timer.start();
					gear_shift_timer.stop();
					gear_shift_timer.reset();
					messages_control.status=ACKED;
					stats.nAcked_control++;

					// Wait for responder to finish remaining ACK batch frames
					{
						int drain = (int)receiving_timer.get_elapsed_time_ms()
							+ (ack_batch_size - 1) * ctrl_transmission_time_ms
							+ ptt_off_delay_ms + 200;
						if (drain < receiving_timeout)
							receiving_timeout = drain;
					}
				}
			}
			messages_rx_buffer.status=FREE;
		}
	}
	else
	{
		// Restore receiving_timeout if batch drain logic adjusted it
		calculate_receiving_timeout();
		// Only restore data config if we switched to ack config (LDPC ACK path)
		if(ack_pattern_time_ms <= 0)
			load_configuration(data_configuration, PHYSICAL_LAYER_ONLY,YES);
		if(messages_control.status==ACKED)
		{
			emergency_nack_count = 0;  // Channel working — reset BREAK counter
			process_control_commander();
		}
		else
		{
			// Break recovery phase 2: probe at target config failed
			if(break_recovery_phase == 2)
			{
				break_recovery_retries--;
				if(break_recovery_retries > 0)
				{
					printf("[BREAK-RECOVERY] Probe retry (%d left) at config %d\n",
						break_recovery_retries, current_configuration);
					fflush(stdout);
					receiving_timer.stop();
					receiving_timer.reset();
					// Force-clear: cleanup() skips PENDING_ACK status
					messages_control.status = FREE;
					add_message_control(SET_CONFIG);
					connection_status = TRANSMITTING_CONTROL;
					return;
				}
				else
				{
					// Probe failed — this config doesn't work.
					// BREAK back to ROBUST_0 and try lower target.
					printf("[BREAK-RECOVERY] Config %d failed probe, sending BREAK\n",
						current_configuration);
					fflush(stdout);
					receiving_timer.stop();
					receiving_timer.reset();
					// Force-clear: cleanup() skips PENDING_ACK status
					messages_control.status = FREE;
					emergency_previous_config = current_configuration;
					emergency_break_active = 1;
					emergency_break_retries = 3;
					break_recovery_phase = 0;  // BREAK ACK handler will set to 1
					send_break_pattern();
					telecom_system->data_container.frames_to_read = 4;
					calculate_receiving_timeout();
					receiving_timer.start();
					return;
				}
			}

			// Track control failures toward BREAK threshold.
			// Only during connected data exchange (not connection setup or turboshift).
			if(link_status == CONNECTED && turboshift_phase == TURBO_DONE
				&& gear_shift_on == YES && !emergency_break_active)
			{
				emergency_nack_count++;
				printf("[BREAK] Control failure #%d at config %d (threshold=%d)\n",
					emergency_nack_count, current_configuration, emergency_nack_threshold);
				fflush(stdout);

				if(emergency_nack_count >= emergency_nack_threshold
					&& !config_is_at_bottom(current_configuration, robust_enabled))
				{
					printf("[BREAK] Sending emergency BREAK pattern (control failure)\n");
					fflush(stdout);

					// Cancel pending control message
					messages_control.ack_timeout=0;
					messages_control.id=0;
					messages_control.length=0;
					messages_control.nResends=0;
					messages_control.status=FREE;
					messages_control.type=NONE;

					emergency_previous_config = current_configuration;
					emergency_break_active = 1;
					emergency_break_retries = 3;
					send_break_pattern();
					telecom_system->data_container.frames_to_read = 4;
					calculate_receiving_timeout();
					receiving_timer.start();
					return;
				}
			}
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
		if(ack_pattern_time_ms > 0)
		{
			// Detect ACK tone pattern
			// Only check while waiting — once detected, stop to avoid
			// buffer zeroing that destroys the next frame's preamble.
			if(data_ack_received==NO && receive_ack_pattern())
			{
				printf("[CMD-ACK-PAT] Data ACK pattern detected!\n");
				fflush(stdout);
				// Flush old batch audio from playback buffer so responder
				// doesn't demodulate stale frames before the new batch.
				clear_buffer(playback_buffer);
				link_timer.start();
				watchdog_timer.start();
				gear_shift_timer.stop();
				gear_shift_timer.reset();
				data_ack_received=YES;

				// Pattern = ACK for all pending messages (batch_size=1)
				for(int i=0; i<nMessages; i++)
				{
					if(messages_tx[i].status==PENDING_ACK)
					{
						register_ack(i);
					}
				}

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

				// Guard delay: wait for responder to finish ACK TX + config load
				int guard = ptt_off_delay_ms + 200;
				receiving_timeout = (int)receiving_timer.get_elapsed_time_ms() + guard;
			}
		}
		else
		{
			// Fallback: decode LDPC ACK frame
			this->receive();
			if(messages_rx_buffer.status==RECEIVED)
			{
				// Flush old batch audio from playback buffer so responder
				// doesn't demodulate stale frames before the new batch.
				clear_buffer(playback_buffer);
				link_timer.start();
				watchdog_timer.start();
				gear_shift_timer.stop();
				gear_shift_timer.reset();
				if(messages_rx_buffer.type==ACK_RANGE)
				{
					data_ack_received=YES;
					int start=(unsigned char)messages_rx_buffer.data[0];
					int end=(unsigned char)messages_rx_buffer.data[1];
					// Guard: start > end under garbage frames wraps unsigned char → infinite loop
					if(start <= end)
					{
						for(int i=start;i<=end;i++)
						{
							register_ack(i);
						}
					}
				}
				else if(messages_rx_buffer.type==ACK_MULTI)
				{
					data_ack_received=YES;
					// Clamp count to buffer bounds — garbage frames can have data[0]=255,
					// reading past the 200-byte messages_rx_buffer.data (Bug #15)
					int ack_count = (unsigned char)messages_rx_buffer.data[0];
					int max_acks = max_data_length + max_header_length - ACK_MULTI_ACK_RANGE_HEADER_LENGTH - 1;
					if(max_acks < 0) max_acks = 0;
					if(ack_count > max_acks) ack_count = max_acks;
					for(int i=0;i<ack_count;i++)
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
	}
	else if (data_ack_received==NO && !(last_message_sent_type==CONTROL && last_message_sent_code==REPEAT_LAST_ACK))
	{
		consecutive_data_acks = 0;  // Reset on failure
		if(ack_pattern_time_ms <= 0)
			load_configuration(data_configuration, PHYSICAL_LAYER_ONLY,YES);
		add_message_control(REPEAT_LAST_ACK);
	}
	else
	{
		if(ack_pattern_time_ms <= 0)
			load_configuration(data_configuration, PHYSICAL_LAYER_ONLY,YES);
		if (last_message_sent_type==CONTROL && last_message_sent_code==REPEAT_LAST_ACK)
		{
			stats.nNAcked_control++;
		}
		this->cleanup();

		// Emergency BREAK: track consecutive complete failures (data + REPEAT_LAST_ACK)
		if(data_ack_received == NO)
		{
			emergency_nack_count++;
			printf("[BREAK] Block failure #%d at config %d (threshold=%d)\n",
				emergency_nack_count, current_configuration, emergency_nack_threshold);
			fflush(stdout);

			// Trigger BREAK when threshold reached and not already at bottom
			if(emergency_nack_count >= emergency_nack_threshold
			   && !config_is_at_bottom(current_configuration, robust_enabled)
			   && !emergency_break_active
			   && turboshift_phase == TURBO_DONE
			   && gear_shift_on == YES)
			{
				printf("[BREAK] Sending emergency BREAK pattern\n");
				fflush(stdout);
				emergency_previous_config = current_configuration;
				emergency_break_active = 1;
				emergency_break_retries = 3;
				send_break_pattern();
				// Poll for ACK from responder
				telecom_system->data_container.frames_to_read = 4;
				calculate_receiving_timeout();
				receiving_timer.start();
				return;
			}
		}
		else
		{
			emergency_nack_count = 0;  // Reset on success
			break_drop_step = 1;
		}

		// Frame-level gearshift: after N consecutive successful data ACKs, shift up immediately
		if(data_ack_received==YES && gear_shift_on==YES && gear_shift_algorithm==SUCCESS_BASED_LADDER &&
			messages_control.status==FREE &&
			!config_is_at_top(current_configuration, robust_enabled))
		{
			consecutive_data_acks++;
			if(consecutive_data_acks >= frame_shift_threshold)
			{
				negotiated_configuration = config_ladder_up(current_configuration, robust_enabled);
				printf("[GEARSHIFT] FRAME UP: %d consecutive ACKs, config %d -> %d\n",
					consecutive_data_acks, current_configuration, negotiated_configuration);
				fflush(stdout);
				consecutive_data_acks = 0;

				// Put unsent messages back into TX FIFO for re-encoding at new config
				for(int i=0; i<nMessages; i++)
				{
					if(messages_tx[i].status == ADDED_TO_LIST || messages_tx[i].status == ADDED_TO_BATCH_BUFFER)
					{
						fifo_buffer_tx.push(messages_tx[i].data, messages_tx[i].length);
					}
					messages_tx[i].status = FREE;
				}
				fifo_buffer_backup.flush();
				block_under_tx = NO;

				add_message_control(SET_CONFIG);
				connection_status = TRANSMITTING_CONTROL;
				return;
			}
		}

		connection_status=TRANSMITTING_DATA;
	}
}

void cl_arq_controller::finish_turbo_direction()
{
	turboshift_active = false;

	if(turboshift_phase == TURBO_FORWARD)
	{
		// Forward direction probed. Advance to REVERSE (other side will probe).
		turboshift_phase = TURBO_REVERSE;
		printf("[TURBO] FORWARD complete: ceiling=%d, switching roles\n",
			turboshift_last_good);
		fflush(stdout);
		cleanup();
		add_message_control(SWITCH_ROLE);
		connection_status = TRANSMITTING_CONTROL;
	}
	else if(turboshift_phase == TURBO_REVERSE)
	{
		// Reverse direction probed. Switch back to original roles.
		printf("[TURBO] REVERSE complete: ceiling=%d, switching back\n",
			turboshift_last_good);
		fflush(stdout);
		turboshift_phase = TURBO_DONE;
		cleanup();
		add_message_control(SWITCH_ROLE);
		connection_status = TRANSMITTING_CONTROL;
	}
	else
	{
		// Already done (shouldn't happen)
		turboshift_phase = TURBO_DONE;
		connection_status = TRANSMITTING_DATA;
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
			// Reset per-phase so each handshake step gets its own timeout window
			connection_attempt_timer.reset();
			connection_attempt_timer.start();
			if(ack_pattern_time_ms > 0)
			{
				// ACK pattern carries no data, keep BROADCAST_ID
				// (responder's assigned_connection_id is in the ACK frame payload
				// which doesn't exist for tone patterns)
				this->connection_id=BROADCAST_ID;
				this->assigned_connection_id=BROADCAST_ID;
			}
			else
			{
				// Fallback: ACK frame carries responder's assigned connection_id
				this->connection_id=messages_control.data[1];
				this->assigned_connection_id=messages_control.data[1];
			}
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

			if(gear_shift_on==YES && gear_shift_algorithm==SNR_BASED)
			{
				this->link_status=NEGOTIATING;
				connection_status=TRANSMITTING_CONTROL;
				link_timer.start();
				watchdog_timer.start();
				gear_shift_timer.start();
			}
			else
			{
				// gear_shift off, or SUCCESS_BASED_LADDER (defers SET_CONFIG to post-block)
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
				watchdog_timer.start();
				link_timer.start();
				connection_attempt_timer.stop();
				connection_attempt_timer.reset();

				// Turboshift: start probing instead of jumping to data
				if(turboshift_active && gear_shift_on==YES && !config_is_at_top(current_configuration, robust_enabled))
				{
					turboshift_initiator = true;
					turboshift_phase = TURBO_FORWARD;
					turboshift_last_good = current_configuration;
					negotiated_configuration = config_ladder_up(current_configuration, robust_enabled);
					printf("[TURBO] Phase: FORWARD — probing commander->responder\n");
					printf("[TURBO] UP: config %d -> %d\n", current_configuration, negotiated_configuration);
					fflush(stdout);
					cleanup();
					add_message_control(SET_CONFIG);
					this->connection_status=TRANSMITTING_CONTROL;
				}
				else
				{
					turboshift_active = false;
					turboshift_phase = TURBO_DONE;
					this->connection_status=TRANSMITTING_DATA;
				}
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
			connection_attempt_timer.stop();
			connection_attempt_timer.reset();

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
							if(!config_is_at_top(current_configuration, robust_enabled))
							{
								negotiated_configuration=config_ladder_up(current_configuration, robust_enabled);
								printf("[GEARSHIFT] LADDER UP: success=%.0f%% > %.0f%%, config %d -> %d\n",
									last_transmission_block_stats.success_rate_data, gear_shift_up_success_rate_precentage,
									current_configuration, negotiated_configuration);
								fflush(stdout);
								cleanup();
								add_message_control(SET_CONFIG);
							}
							else
							{
								printf("[GEARSHIFT] LADDER: at top (config %d), success=%.0f%%\n",
									current_configuration, last_transmission_block_stats.success_rate_data);
								fflush(stdout);
								this->connection_status=TRANSMITTING_DATA;
							}
						}
						else if(last_transmission_block_stats.success_rate_data<gear_shift_down_success_rate_precentage)
						{
							if(!config_is_at_bottom(current_configuration, robust_enabled))
							{
								negotiated_configuration=config_ladder_down(current_configuration, robust_enabled);
								printf("[GEARSHIFT] LADDER DOWN: success=%.0f%% < %.0f%%, config %d -> %d\n",
									last_transmission_block_stats.success_rate_data, gear_shift_down_success_rate_precentage,
									current_configuration, negotiated_configuration);
								fflush(stdout);
								cleanup();
								add_message_control(SET_CONFIG);
							}
							else
							{
								printf("[GEARSHIFT] LADDER: at bottom (config %d), success=%.0f%%\n",
									current_configuration, last_transmission_block_stats.success_rate_data);
								fflush(stdout);
								this->connection_status=TRANSMITTING_DATA;
							}
							gear_shift_blocked_for_nBlocks=0;
						}
						else
						{
							printf("[GEARSHIFT] LADDER: hold config %d, success=%.0f%%\n",
								current_configuration, last_transmission_block_stats.success_rate_data);
							fflush(stdout);
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
				// Asymmetric gearshift: swap forward/reverse for the return path
				if(forward_configuration != CONFIG_NONE && reverse_configuration != CONFIG_NONE)
				{
					char tmp = forward_configuration;
					forward_configuration = reverse_configuration;
					reverse_configuration = tmp;

					if(forward_configuration != current_configuration)
					{
						data_configuration = forward_configuration;
						load_configuration(data_configuration, PHYSICAL_LAYER_ONLY, YES);
						printf("[GEARSHIFT] SWITCH_ROLE: loaded config %d for return path\n",
							forward_configuration);
					}
				}

				set_role(RESPONDER);
				this->link_status=CONNECTED;
				this->connection_status=RECEIVING;
				watchdog_timer.start();
				link_timer.start();
				last_message_received_type=NONE;
				last_message_sent_type=NONE;
				last_received_message_sequence=-1;
				// Reset RX state machine - wait for fresh data (prevents decode of self-received TX audio).
				// Frame completeness gating handles late arrivals adaptively.
				telecom_system->data_container.frames_to_read =
					telecom_system->data_container.preamble_nSymb + telecom_system->data_container.Nsymb;
				telecom_system->data_container.nUnder_processing_events = 0;
				telecom_system->receive_stats.mfsk_search_raw = 0;
			}
			else if (messages_control.data[0]==SET_CONFIG)
			{
				// SET_CONFIG ACK received: apply the new config now
				gear_shift_timer.stop();
				gear_shift_timer.reset();
				if(data_configuration != current_configuration)
				{
					messages_control_backup();
					load_configuration(data_configuration, PHYSICAL_LAYER_ONLY, YES);
					messages_control_restore();
					printf("[GEARSHIFT] SET_CONFIG ACKed, loaded config %d\n", data_configuration);
					fflush(stdout);

					// Re-fill TX messages for the new config's message sizes
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

				// Break recovery: reverse-turboshift probing
				if(break_recovery_phase == 1)
				{
					// Phase 1 complete: coordination at ROBUST_0 succeeded.
					// Target config loaded. Now probe it with SET_CONFIG at target.
					printf("[BREAK-RECOVERY] Phase 1 done, probing config %d (2 tries)\n",
						current_configuration);
					fflush(stdout);
					break_recovery_phase = 2;
					break_recovery_retries = 2;
					cleanup();
					add_message_control(SET_CONFIG);
					this->connection_status = TRANSMITTING_CONTROL;
				}
				else if(break_recovery_phase == 2)
				{
					// Phase 2 complete: target config works!
					printf("[BREAK-RECOVERY] Config %d verified, resuming data exchange\n",
						current_configuration);
					fflush(stdout);
					break_recovery_phase = 0;
					break_drop_step = 1;  // reset backoff on success
					this->connection_status = TRANSMITTING_DATA;
				}
				// Turboshift: keep climbing or finish direction
				else if(turboshift_active)
				{
					turboshift_last_good = current_configuration;
					turboshift_retries = 1;  // reset retry for next config
					if(!config_is_at_top(current_configuration, robust_enabled))
					{
						negotiated_configuration = config_ladder_up(current_configuration, robust_enabled);
						printf("[TURBO] UP: config %d -> %d\n",
							current_configuration, negotiated_configuration);
						fflush(stdout);
						cleanup();
						add_message_control(SET_CONFIG);
						this->connection_status=TRANSMITTING_CONTROL;
					}
					else
					{
						printf("[TURBO] Reached top at config %d\n", current_configuration);
						fflush(stdout);
						finish_turbo_direction();
					}
				}
				else
				{
					this->connection_status=TRANSMITTING_DATA;
				}
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
			// Reset RX state machine - wait for fresh data (prevents decode of self-received TX audio)
			telecom_system->data_container.frames_to_read =
				telecom_system->data_container.preamble_nSymb + telecom_system->data_container.Nsymb;
			telecom_system->data_container.nUnder_processing_events = 0;

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
			int filled = 0;
			int fill_limit = data_batch_size;  // Fill exactly one batch worth of messages
			for(int i=0;i<fill_limit;i++)
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
					filled++;
				}
				else
				{
					block_under_tx=YES;
					add_message_tx_data(DATA_SHORT, data_read_size, message_TxRx_byte_buffer);
					fifo_buffer_backup.push(message_TxRx_byte_buffer,data_read_size);
					filled++;
				}
			}
			printf("[DBG-FILL] Filled %d/%d messages (batch_size=%d, pop_size=%d)\n",
				filled, fill_limit, data_batch_size, max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH);
			fflush(stdout);
		}
		else if(block_under_tx==YES && message_batch_counter_tx==0 && get_nOccupied_messages()==0 && messages_control.status==FREE)
		{
			printf("[DBG-BLOCKEND] Adding BLOCK_END\n");
			fflush(stdout);
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
