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
				printf("[BREAK] ACK received! Dropping %d step(s): config %d -> %d (robust_enabled=%d)\n",
					break_drop_step, emergency_previous_config, target, robust_enabled);
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

				printf("[BREAK] Recovery phase 1: negotiated=%d forward=%d reverse=%d "
					"data_cfg=%d current=%d target=%d\n",
					negotiated_configuration, forward_configuration,
					reverse_configuration, data_configuration,
					current_configuration, target);
				fflush(stdout);

				{
					if(compression_enabled)
					{
						// Decompress messages_tx back to raw, push to FIFO
						// for re-compression at new config.
						restore_tx_from_compressed();
					}
					else
					{
						for(int i=nMessages-1; i>=0; i--)
						{
							if(messages_tx[i].status != FREE && messages_tx[i].length > 0)
								fifo_buffer_tx.push_front(messages_tx[i].data, messages_tx[i].length);
							messages_tx[i].status = FREE;
						}
						fifo_buffer_backup.flush();
					}
					block_under_tx = NO;
					int fifo_load = fifo_buffer_tx.get_size() - fifo_buffer_tx.get_free_size();
					printf("[BREAK] Saved data to FIFO (%d bytes total)\n", fifo_load);
					fflush(stdout);
				}

				// Force-clear: cleanup() skips PENDING_ACK status
				messages_control.status = FREE;
				add_message_control(SET_CONFIG);
				printf("[BREAK] SET_CONFIG queued: data[1]=%d data[2]=%d\n",
					(int)messages_control.data[1], (int)messages_control.data[2]);
				fflush(stdout);
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
				printf("[BREAK] EXHAUSTED state: emergency_prev=%d break_drop=%d robust=%d\n",
					emergency_previous_config, break_drop_step, robust_enabled);
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

				printf("[BREAK] EXHAUSTED recovery: negotiated=%d forward=%d data_cfg=%d target=%d\n",
					negotiated_configuration, forward_configuration, data_configuration, target);
				fflush(stdout);

				if(compression_enabled)
				{
					restore_tx_from_compressed();
				}
				else
				{
					for(int i=nMessages-1; i>=0; i--)
					{
						if(messages_tx[i].status != FREE && messages_tx[i].length > 0)
							fifo_buffer_tx.push_front(messages_tx[i].data, messages_tx[i].length);
						messages_tx[i].status = FREE;
					}
					fifo_buffer_backup.flush();
				}
				block_under_tx = NO;

				// Force-clear: cleanup() skips PENDING_ACK status
				messages_control.status = FREE;
				add_message_control(SET_CONFIG);
				printf("[BREAK] EXHAUSTED SET_CONFIG queued: data[1]=%d data[2]=%d\n",
					(int)messages_control.data[1], (int)messages_control.data[2]);
				fflush(stdout);
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
		// NB/WB auto-negotiation Phase 1: WB commander switches to NB
		// to probe for NB responders (first nb_probe_max attempts)
		if(connection_attempts == 0 && messages_control.status == FREE &&
		   commander_configured_nb >= 0 && commander_configured_nb != YES &&
		   nb_probe_max > 0)
		{
			printf("[NB-NEG] Commander: Phase 1 NB probe (WB commander)\n");
			fflush(stdout);
			switch_narrowband_mode(YES);
		}

		// NB probe switch-back: after nb_probe_max HAIL attempts in NB,
		// switch to WB so HAIL can reach the WB responder.
		// The START_CONNECTION retry path (line ~468) has its own switch-back,
		// but it's never reached when HAIL fails — HAIL failure prevents
		// START_CONNECTION from being queued.
		if(commander_configured_nb >= 0 && commander_configured_nb != YES &&
		   nb_probe_max > 0 && connection_attempts >= nb_probe_max &&
		   narrowband_enabled == YES)
		{
			printf("[NB-NEG] Commander: restoring WB after %d NB HAIL attempts\n",
				connection_attempts);
			fflush(stdout);
			switch_narrowband_mode(NO);
			// hail_detected is already NO — WB HAIL will be attempted below
		}

		// "I am Mercury" HAIL phase: fast MFSK beacons replace slow LDPC probes.
		// Send HAIL, listen for response. Repeat up to max_connection_attempts.
		// Only proceed to START_CONNECTION after HAIL response detected.
		// Directed HAIL: CRC suffix derived from responder's callsign prevents
		// multi-station collisions — only the target responder responds.
		if(ack_pattern_time_ms > 0 && hail_detected == NO)
		{
			telecom_system->ack_mfsk.set_hail_target(
				destination_call_sign.c_str(), destination_call_sign.length());
			send_hail_pattern();
			connection_attempts++;
			printf("[HAIL] Sent beacon %d of %d\n", connection_attempts, max_connection_attempts);
			fflush(stdout);

			// Listen for response (pattern time + responder turnaround)
			int listen_ms = 2 * ack_pattern_time_ms + 1500;
			cl_timer hail_listen;
			hail_listen.start();
			while(hail_listen.get_elapsed_time_ms() < listen_ms)
			{
				if(receive_hail_pattern())
				{
					printf("[HAIL] Response received — peer is Mercury\n");
					fflush(stdout);
					hail_detected = YES;
					break;
				}
				msleep(50);
			}

			if(hail_detected == NO)
				return; // Retry next cycle (max_connection_attempts check in update_status)
		}

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
			// Graceful disconnect (VARA-compatible): wait for TX FIFO to drain
			// before closing the link. Winlink sends FQ data then DISCONNECT;
			// we must deliver the FQ before tearing down the ARQ session.
			int fifo_pending = fifo_buffer_tx.get_size() - fifo_buffer_tx.get_free_size();
			if(fifo_pending > 0 || block_under_tx == YES)
			{
				// Data still pending — let normal data exchange drain it.
				// disconnect_requested stays YES, re-checked next cycle.
				static int disconnect_wait_prints = 0;
				if(disconnect_wait_prints++ % 50 == 0)
				{
					printf("[DISCONNECT] Waiting for TX drain: FIFO=%d bytes, block_under_tx=%d\n",
						fifo_pending, block_under_tx);
					fflush(stdout);
				}
			}
			else
			{
				printf("[DISCONNECT] TX drained, sending CLOSE_CONNECTION\n");
				fflush(stdout);
				disconnect_requested=NO;
				this->link_status=DISCONNECTING;
				messages_control.status=FREE;
				add_message_control(CLOSE_CONNECTION);
			}
		}
		else
		{
			reset_session_state();
			load_configuration(init_configuration,FULL,YES);

			// Switch to RESPONDER/LISTENING so we can receive incoming connections
			set_role(RESPONDER);
			this->link_status=LISTENING;
			this->connection_status=RECEIVING;

			reset_all_timers();
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
		// Key exchange gate: if encryption negotiated but not active, run key exchange first
		if(encryption_enabled && !cipher_suite.is_active() && cipher_suite.get_kx_phase() == KX_IDLE)
		{
			printf("[CRYPTO] Turboshift done, initiating key exchange\n");
			fflush(stdout);
			add_message_control(KEY_EXCHANGE_1);
			// connection_status set to TRANSMITTING_CONTROL by add_message_control
			return;
		}
		// Hold data until key exchange completes (encryption negotiated but not yet active)
		if(encryption_enabled && !cipher_suite.is_active())
		{
			return;
		}
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
			// CRC8 on full destination callsign (including SSID if present)
			messages_control.data[1]=CRC8_calc((char*)destination_call_sign.c_str(), destination_call_sign.length());
			// Pack base callsign (strip SSID — pack only supports A-Z, 0-9, 6 chars)
			// SSID is sent separately in TEST_CONNECTION
			std::string base_call = callsign_strip_ssid(my_call_sign);
			int pack_flags = 0;
			if (narrowband_enabled == YES || commander_configured_nb == YES)
				pack_flags |= 0x01;
			callsign_pack(base_call.c_str(), base_call.length(), &messages_control.data[2], pack_flags);
			messages_control.length=7;  // cmd(1) + CRC8(1) + packed_callsign(5)
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
			messages_control.data[5]=(char)local_capability;
			messages_control.data[6]=(char)callsign_get_ssid(my_call_sign);
			messages_control.length=7;
			messages_control.id=0;
		}
		else if(code==SWITCH_BANDWIDTH)
		{
			messages_control.data[0]=code;
			messages_control.data[1]=0;  // 0 = switch to WB
			messages_control.length=2;
			messages_control.id=0;
			// Fast fail: nb_only responders silently reject (don't ACK).
			// 2 retries keeps detection under ~10 seconds on MFSK modes.
			messages_control.nResends=2;
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

			printf("[GEARSHIFT] SET_CONFIG: forward=%d reverse=%d (SNR down=%.1f up=%.1f) link_status=%d\n",
				forward_configuration, reverse_configuration,
				measurements.SNR_downlink, measurements.SNR_uplink, (int)link_status);
			fflush(stdout);
#ifdef MERCURY_GUI_ENABLED
			{
				char buf[80];
				snprintf(buf, sizeof(buf), "[GEARSHIFT -> CONFIG_%d]", forward_configuration);
				gui_push_monitor_event(buf, true);
			}
#endif
		}
		else if(code==KEY_EXCHANGE_1)
		{
			// X25519 public key exchange (32 bytes)
			uint8_t pubkey[X25519_KEY_SIZE];
			if(cipher_suite.generate_x25519_keypair(pubkey) != 0)
			{
				printf("[CRYPTO] ERROR: X25519 keypair generation failed (RNG)\n");
				fflush(stdout);
				messages_control.status = FREE;
				return ERROR_;
			}
			messages_control.data[0] = code;
			memcpy(&messages_control.data[1], pubkey, X25519_KEY_SIZE);
			messages_control.length = 1 + X25519_KEY_SIZE;  // 33 bytes
			messages_control.id = 0;
			printf("[CRYPTO] Sending X25519 pubkey (32 bytes)\n");
			fflush(stdout);
		}
		else if(code==KEY_ACTIVATE)
		{
			messages_control.data[0] = code;
			// Append 8-byte key confirmation tag for PSK verification
			uint8_t confirm_tag[8];
			cipher_suite.compute_key_confirmation(confirm_tag);
			memcpy(&messages_control.data[1], confirm_tag, 8);
			messages_control.length = 9;
			messages_control.id = 0;
			printf("[CRYPTO] Sending KEY_ACTIVATE with confirmation tag\n");
			fflush(stdout);
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

				// NB/WB auto-negotiation: phase transitions
				if(commander_configured_nb >= 0 && nb_probe_max > 0)
				{
					if(commander_configured_nb == NO)
					{
						// WB commander: NB probes for attempts 0..nb_probe_max-1, then WB forever
						if(connection_attempts == nb_probe_max)
						{
							printf("[NB-NEG] Commander: Phase 2 - restoring WB\n");
							fflush(stdout);
							switch_narrowband_mode(NO);
							messages_control.status = FREE;
							return;
						}
					}
					else
					{
						// NB commander: alternate NB/WB in blocks of nb_probe_max
						// NB probes (0..1), WB probes (2..3), NB probes (4..5), ...
						// This ensures both NB and WB responders eventually get reached
						int phase = connection_attempts / nb_probe_max;
						int phase_start = phase * nb_probe_max;
						bool should_be_wb = (phase % 2 == 1);  // odd phases = WB
						if(connection_attempts == phase_start)
						{
							if(should_be_wb && narrowband_enabled == YES)
							{
								printf("[NB-NEG] Commander: switching to WB probe (attempt %d)\n", connection_attempts);
								fflush(stdout);
								switch_narrowband_mode(NO);
								messages_control.status = FREE;
								return;
							}
							else if(!should_be_wb && narrowband_enabled == NO)
							{
								printf("[NB-NEG] Commander: switching back to NB (attempt %d)\n", connection_attempts);
								fflush(stdout);
								switch_narrowband_mode(YES);
								messages_control.status = FREE;
								return;
							}
						}
					}
				}
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

		// Flush self-echo from BOTH the circular capture buffer and the
		// sliding passband buffer. On VB-Cable loopback, the CMD hears its
		// own 9-second TX as self-echo filling the ring buffer. Without
		// flushing capture_buffer, the capture thread drains stale self-echo
		// into passband_delayed_data BEFORE real-time audio (the RSP's ACK)
		// arrives — so the ACK is buried behind seconds of backlog. (Bug #38)
		circular_buf_reset(capture_buffer);
		{
			MUTEX_LOCK(&capture_prep_mutex);
			int signal_period = telecom_system->data_container.Nofdm
				* telecom_system->data_container.buffer_Nsymb
				* telecom_system->data_container.interpolation_rate;
			memset(telecom_system->data_container.passband_delayed_data, 0,
				2 * signal_period * sizeof(double));
			telecom_system->data_container.ring_write_index = 0;
			MUTEX_UNLOCK(&capture_prep_mutex);
		}

		if(messages_control.data[0] == KEY_EXCHANGE_1)
		{
			// KEY_EXCHANGE_1: receive LDPC ACK on data_configuration (OFDM).
			// Responder sends full OFDM frame with pubkey data.
			telecom_system->set_mfsk_ctrl_mode(false);
			telecom_system->data_container.frames_to_read =
				telecom_system->data_container.preamble_nSymb + telecom_system->data_container.Nsymb;
			printf("[CMD-RX] KEY_EXCHANGE_1: expecting LDPC ACK on config %d, ftr=%d\n",
				data_configuration,
				telecom_system->data_container.frames_to_read.load());
			fflush(stdout);
		}
		else if(ack_pattern_time_ms > 0)
		{
			// Expect ACK tone pattern. Start polling quickly (4 symbols ~90ms);
			// receive_ack_pattern() re-polls every 2 symbols until pattern arrives.
			// Large initial ftr delays first poll and pushes the OFDM turnaround
			// past the responder's buffer upper_bound (CONFIG_0 entry regression).
			telecom_system->data_container.frames_to_read = 4;
		}
		else
		{
			// Fallback: expect short LDPC ctrl frame on ack_configuration
			load_configuration(ack_configuration, PHYSICAL_LAYER_ONLY,NO);
			telecom_system->set_mfsk_ctrl_mode(true);
			telecom_system->data_container.frames_to_read =
				telecom_system->data_container.preamble_nSymb + telecom_system->get_active_nsymb();
		}
		connection_status=RECEIVING_ACKS_CONTROL;

		// Recalculate timeout: guard delays from prior ACK detection can leave
		// receiving_timeout stale (e.g. 900ms), too short for the control round-trip.
		calculate_receiving_timeout();
		receiving_timer.start();
		if(g_verbose) { printf("[CMD-RX] Entering receive mode: ack_cfg=%d recv_timeout=%d msg_tx_time=%d ctrl_tx_time=%d ack_batch=%d ftr=%d\n", ack_configuration, receiving_timeout, message_transmission_time_ms, ctrl_transmission_time_ms, ack_batch_size, telecom_system->data_container.frames_to_read.load()); fflush(stdout); }

		if(messages_control.data[0]==SWITCH_BANDWIDTH)
		{
			// Responder must fill its RX buffer before it can decode the
			// SWITCH_BANDWIDTH control frame.  For NB CONFIG_0 the buffer
			// fill time alone is ~14 s, so use the same 2×msg_tx basis as
			// the normal ACK timeout (calculate_receiving_timeout).
			receiving_timeout = 2 * message_transmission_time_ms
				+ ack_pattern_time_ms
				+ 2*ptt_on_delay_ms + 2*ptt_off_delay_ms + 3000;
			printf("[BW-NEG] SWITCH_BANDWIDTH recv_timeout=%d msg_tx=%d\n",
				receiving_timeout, message_transmission_time_ms);
			fflush(stdout);
		}

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

		// Flush self-echo from both ring buffer and sliding buffer (Bug #38).
		circular_buf_reset(capture_buffer);
		{
			MUTEX_LOCK(&capture_prep_mutex);
			int signal_period = telecom_system->data_container.Nofdm
				* telecom_system->data_container.buffer_Nsymb
				* telecom_system->data_container.interpolation_rate;
			memset(telecom_system->data_container.passband_delayed_data, 0,
				2 * signal_period * sizeof(double));
			telecom_system->data_container.ring_write_index = 0;
			MUTEX_UNLOCK(&capture_prep_mutex);
		}

		if(ack_pattern_time_ms > 0)
		{
			// Expect ACK tone pattern — start polling quickly (same as control path).
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
		if(ack_pattern_time_ms > 0 && messages_control.data[0] != KEY_EXCHANGE_1)
		{
			// Detect ACK tone pattern instead of decoding LDPC frame.
			// Keep checking until ACKED (not just PENDING_ACK): update_status()
			// may set ACK_TIMED_OUT before the receive window expires, but the
			// ACK pattern can still arrive within receiving_timeout.
			// KEY_EXCHANGE_1 must use LDPC path to receive responder's pubkey.
			if(messages_control.status != ACKED)
			{
				if(receive_ack_pattern())
				{
					printf("[CMD-ACK-PAT] Control ACK for code=%d detected! elapsed=%dms link=%d status=%d\n",
					(int)messages_control.data[0], (int)receiving_timer.get_elapsed_time_ms(), (int)link_status, (int)messages_control.status);
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

					// Guard delay: wait for responder to finish ACK TX + settle.
					// ptt_off covers the radio TX→RX transition; +200ms margin.
					int guard = ptt_off_delay_ms + 200;
					receiving_timeout = (int)receiving_timer.get_elapsed_time_ms() + guard;
				}
			}
		}
		else
		{
			// Decode LDPC ACK frame (also used for KEY_EXCHANGE_1 which
			// carries the responder's pubkey in the ACK data payload).
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
		// Restore data config if we switched to ack config (LDPC ACK path)
		if(ack_pattern_time_ms <= 0)
			load_configuration(data_configuration, PHYSICAL_LAYER_ONLY,YES);
		// KEY_EXCHANGE_1 stays on data_configuration — no restore needed
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

			// Break recovery phase 1: coordination SET_CONFIG at ROBUST_0 failed
			if(break_recovery_phase == 1)
			{
				break_recovery_retries--;
				if(break_recovery_retries > 0)
				{
					printf("[BREAK-RECOVERY] Phase 1 retry (%d left) at config %d\n",
						break_recovery_retries, current_configuration);
					fflush(stdout);
					receiving_timer.stop();
					receiving_timer.reset();
					messages_control.status = FREE;
					add_message_control(SET_CONFIG);
					connection_status = TRANSMITTING_CONTROL;
					return;
				}
				else
				{
					// Phase 1 exhausted — BREAK again to resync
					printf("[BREAK-RECOVERY] Phase 1 failed, re-sending BREAK\n");
					fflush(stdout);
					receiving_timer.stop();
					receiving_timer.reset();
					messages_control.status = FREE;
					// Keep emergency_previous_config unchanged (still targeting original settle config)
					emergency_break_active = 1;
					emergency_break_retries = 3;
					break_recovery_phase = 0;
					send_break_pattern();
					telecom_system->data_container.frames_to_read = 4;
					calculate_receiving_timeout();
					receiving_timer.start();
					return;
				}
			}

			// SWITCH_BANDWIDTH failure: responder is nb_only, didn't ACK.
			// Stay NB and start turboshift normally.
			if(wb_upgrade_pending && link_status == CONNECTED)
			{
				printf("[BW-NEG] SWITCH_BANDWIDTH not ACKed (peer is nb_only), staying NB\n");
				fflush(stdout);
				wb_upgrade_pending = false;

				// Force-clear control message
				messages_control.ack_timeout = 0;
				messages_control.id = 0;
				messages_control.length = 0;
				messages_control.nResends = 0;
				messages_control.status = FREE;
				messages_control.type = NONE;

				receiving_timer.stop();
				receiving_timer.reset();

				// Start turboshift in NB
				if(turboshift_active && gear_shift_on == YES &&
					!config_is_at_top(current_configuration, robust_enabled, narrowband_enabled == YES))
				{
					turboshift_initiator = true;
					turboshift_phase = TURBO_FORWARD;
					turboshift_last_good = current_configuration;

					int snr_target = -1;
					if(is_ofdm_config(current_configuration) && measurements.SNR_uplink > -90)
					{
						snr_target = get_configuration(measurements.SNR_uplink - SUPERSHIFT_MARGIN_DB);
						// Enforce bandwidth ceiling
						int cfg_ceiling = (narrowband_enabled == YES) ? NB_CONFIG_MAX : CONFIG_15;
						if(snr_target > cfg_ceiling)
							snr_target = cfg_ceiling;
					}

					if(snr_target > 0 && config_ladder_index(snr_target) > config_ladder_index(current_configuration))
					{
						negotiated_configuration = snr_target;
						printf("[TURBO] Phase: FORWARD — probing commander->responder (NB)\n");
						printf("[TURBO] SNR-SUPERSHIFT: SNR=%.1f dB -> config %d -> %d (direct)\n",
							measurements.SNR_uplink, current_configuration, negotiated_configuration);
					}
					else
					{
						negotiated_configuration = config_ladder_up_n(current_configuration, 3, robust_enabled, narrowband_enabled == YES);
						printf("[TURBO] Phase: FORWARD — probing commander->responder (NB)\n");
						printf("[TURBO] SUPERSHIFT: config %d -> %d (step 3)\n",
							current_configuration, negotiated_configuration);
					}
					fflush(stdout);
					cleanup();
					add_message_control(SET_CONFIG);
					connection_status = TRANSMITTING_CONTROL;
				}
				else
				{
					turboshift_active = false;
					turboshift_phase = TURBO_DONE;
					connection_status = TRANSMITTING_DATA;
				}
				return;
			}

			// PSK mismatch: KEY_ACTIVATE was sent to notify the responder.
			// Whether ACKed or timed out, disconnect now.
			if(psk_mismatch_pending && link_status == CONNECTED)
			{
				printf("[CRYPTO] KEY_ACTIVATE sent (PSK mismatch), disconnecting\n");
				fflush(stdout);
				psk_mismatch_pending = false;
				this->link_status = DROPPED;
				reset_session_state();
				return;
			}

			// Turboshift probe failure: handle directly (1 retry, then ceiling+BREAK).
			// Bypasses nResends sub-retries and gearshift_timeout for fast OTA response.
			// Guard: only during CONNECTED — during CONNECTING, the failed message is
			// START_CONNECTION, not a turboshift SET_CONFIG. Without this guard,
			// turboshift_active (true from init) hijacks the first NAck and replaces
			// START_CONNECTION with SET_CONFIG, preventing connection. (Bug #35)
			if(turboshift_active && this->role == COMMANDER && link_status == CONNECTED)
			{
				// Force-clear control message (prevent nResends sub-retries)
				messages_control.ack_timeout=0;
				messages_control.id=0;
				messages_control.length=0;
				messages_control.nResends=0;
				messages_control.status=FREE;
				messages_control.type=NONE;

				receiving_timer.stop();
				receiving_timer.reset();
				gear_shift_timer.stop();
				gear_shift_timer.reset();

				if(turboshift_retries > 0)
				{
					turboshift_retries--;
					printf("[TURBO] RETRY config %d (retries left: %d)\n",
						current_configuration, turboshift_retries);
					fflush(stdout);
					add_message_control(SET_CONFIG);
					connection_status = TRANSMITTING_CONTROL;
					return;
				}

				// Ceiling — send BREAK to resync both sides
				int failed_config = current_configuration;
				int settle_config = (turboshift_last_good >= 0) ?
					turboshift_last_good : init_configuration;

				printf("[TURBO] CEILING at config %d, sending BREAK to resync at %d\n",
					failed_config, settle_config);
				printf("[TURBO] CEILING state: turboshift_last_good=%d init_config=%d "
					"negotiated=%d data_cfg=%d current=%d\n",
					turboshift_last_good, init_configuration,
					negotiated_configuration, data_configuration, current_configuration);
				fflush(stdout);

				turboshift_active = false;
				data_configuration = settle_config;
				emergency_previous_config = settle_config;
				break_drop_step = 0;
				emergency_break_active = 1;
				emergency_break_retries = 3;
				emergency_nack_count = 0;

				for(int i=0; i<nMessages; i++)
					messages_tx[i].status = FREE;
				fifo_buffer_backup.flush();

				send_break_pattern();
				telecom_system->data_container.frames_to_read = 4;
				calculate_receiving_timeout();
				receiving_timer.start();
				return;
			}

			// Frame gearshift up failure: BREAK immediately, double threshold.
			// Only one attempt — no retries. Recover to the working config and
			// require 2x consecutive ACKs before trying to upshift again.
			if(messages_control.data[0] == SET_CONFIG
				&& turboshift_phase == TURBO_DONE
				&& !turboshift_active
				&& break_recovery_phase == 0
				&& !emergency_break_active)
			{
				gear_shift_timer.stop();
				gear_shift_timer.reset();

				int working_config = config_ladder_down(negotiated_configuration, robust_enabled);
				frame_shift_threshold *= 2;
				consecutive_data_acks = 0;

				{
					int fifo_load = fifo_buffer_tx.get_size() - fifo_buffer_tx.get_free_size();
					printf("[GEARSHIFT] FRAME UP FAILED: %d->%d NAck, BREAK to %d (threshold now %d, fifo=%d bytes)\n",
						working_config, negotiated_configuration, working_config, frame_shift_threshold, fifo_load);
				}
				fflush(stdout);

				// Cancel the failed control message
				messages_control.ack_timeout=0;
				messages_control.id=0;
				messages_control.length=0;
				messages_control.nResends=0;
				messages_control.status=FREE;
				messages_control.type=NONE;

				// Reset config state to the working config
				data_configuration = working_config;
				negotiated_configuration = working_config;

				// BREAK to resync — recover to the working config
				emergency_previous_config = working_config;
				break_drop_step = 0;
				emergency_break_active = 1;
				emergency_break_retries = 3;
				emergency_nack_count = 0;

				send_break_pattern();
				telecom_system->data_container.frames_to_read = 4;
				calculate_receiving_timeout();
				receiving_timer.start();
				return;
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

				// Pattern = ACK for all pending messages (batch_size=1).
				// Also catch ACK_TIMED_OUT: update_status() may fire before
				// the ACK pattern arrives within the receive window.
				for(int i=0; i<nMessages; i++)
				{
					if(messages_tx[i].status==PENDING_ACK || messages_tx[i].status==ACK_TIMED_OUT)
					{
						register_ack(i);
					}
				}

				if(messages_control.data[0]==REPEAT_LAST_ACK &&
				   (messages_control.status==PENDING_ACK || messages_control.status==ACK_TIMED_OUT))
				{
					this->messages_control.ack_timeout=0;
					this->messages_control.id=0;
					this->messages_control.length=0;
					this->messages_control.nResends=0;
					this->messages_control.status=FREE;
					this->messages_control.type=NONE;
					stats.nAcked_control++;
				}

				// Guard delay: wait for responder to finish full ACK TX + settle.
				// ptt_off covers the radio TX→RX transition; +200ms margin.
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
	else if (data_ack_received==NO && ack_pattern_time_ms <= 0 && !(last_message_sent_type==CONTROL && last_message_sent_code==REPEAT_LAST_ACK))
	{
		consecutive_data_acks = 0;  // Reset on failure

		// Frame gearshift just applied but data failed — BREAK immediately, no retry
		if(frame_gearshift_just_applied)
		{
			frame_gearshift_just_applied = false;
			int working_config = config_ladder_down(data_configuration, robust_enabled);
			frame_shift_threshold *= 2;

			printf("[GEARSHIFT] FRAME UP DATA FAILED: config %d can't pass data, BREAK to %d (threshold now %d)\n",
				data_configuration, working_config, frame_shift_threshold);
			fflush(stdout);

			// Preserve all pending data for resend at working config
			if(compression_enabled)
			{
				restore_tx_from_compressed();
			}
			else
			{
				for(int i=0; i<nMessages; i++)
				{
					if(messages_tx[i].status != FREE && messages_tx[i].length > 0)
						fifo_buffer_tx.push(messages_tx[i].data, messages_tx[i].length);
					messages_tx[i].status = FREE;
				}
				fifo_buffer_backup.flush();
			}
			block_under_tx = NO;

			int fifo_load = fifo_buffer_tx.get_size() - fifo_buffer_tx.get_free_size();
			printf("[GEARSHIFT] Saved data to FIFO: %d bytes pending\n", fifo_load);
			fflush(stdout);

			data_configuration = working_config;
			negotiated_configuration = working_config;

			emergency_previous_config = working_config;
			break_drop_step = 0;
			emergency_break_active = 1;
			emergency_break_retries = 3;
			emergency_nack_count = 0;

			send_break_pattern();
			telecom_system->data_container.frames_to_read = 4;
			calculate_receiving_timeout();
			receiving_timer.start();
			return;
		}

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
		if(data_ack_received == NO && ack_pattern_time_ms > 0)
		{
			// Pattern ACK timeout: REPEAT_LAST_ACK is useless (responder can't
			// receive control frames while waiting for data). Skip directly to
			// retransmit — avoids stuck loop where REPEAT_LAST_ACK was queued
			// but never sent (connection_status stayed RECEIVING_ACKS_DATA).
			printf("[CMD-ACK-PAT] Timeout: no ACK pattern detected, retransmitting\n");
			fflush(stdout);
			stats.nNAcked_data++;
			// Force all PENDING_ACK messages to ACK_TIMED_OUT so
			// process_messages_tx_data() can resend them immediately.
			// Without this, messages stay PENDING_ACK until their
			// individual ack_timeout fires (~13s), stalling the commander.
			for(int i=0; i<nMessages; i++)
			{
				if(messages_tx[i].status == PENDING_ACK)
					messages_tx[i].status = ACK_TIMED_OUT;
			}
		}
		this->cleanup();

		// Emergency BREAK: track consecutive complete failures (data + REPEAT_LAST_ACK)
		if(data_ack_received == NO)
		{
			consecutive_data_acks = 0;

			// Frame gearshift just applied but data failed — BREAK immediately
			if(frame_gearshift_just_applied && ack_pattern_time_ms > 0)
			{
				frame_gearshift_just_applied = false;
				int working_config = config_ladder_down(data_configuration, robust_enabled);
				frame_shift_threshold *= 2;

				printf("[GEARSHIFT] FRAME UP DATA FAILED (pat): config %d -> BREAK to %d (threshold %d)\n",
					data_configuration, working_config, frame_shift_threshold);
				fflush(stdout);

				for(int i=0; i<nMessages; i++)
				{
					if(messages_tx[i].status != FREE && messages_tx[i].length > 0)
						fifo_buffer_tx.push(messages_tx[i].data, messages_tx[i].length);
					messages_tx[i].status = FREE;
				}
				fifo_buffer_backup.flush();
				block_under_tx = NO;

				data_configuration = working_config;
				negotiated_configuration = working_config;
				emergency_previous_config = working_config;
				break_drop_step = 0;
				emergency_break_active = 1;
				emergency_break_retries = 3;
				emergency_nack_count = 0;

				send_break_pattern();
				telecom_system->data_container.frames_to_read = 4;
				calculate_receiving_timeout();
				receiving_timer.start();
				return;
			}

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
			frame_gearshift_just_applied = false;  // upshift survived — clear flag
		}

		// Auto-arm compression after B2F SID is ACKed
		if(data_ack_received==YES && b2f_compression_pending)
		{
			compression_enabled = true;
			b2f_compression_pending = false;
			printf("[COMPRESS] Armed after B2F ACK (commander)\n");
			fflush(stdout);
			// Enable streaming if both sides support it
			bool streaming_ok = (local_capability & CAP_STREAMING)
				&& (peer_capability & CAP_STREAMING);
			if(streaming_ok && !compressor.is_streaming())
				compressor.streaming_enable();
		}

		// Frame-level gearshift: after N consecutive successful data ACKs, shift up immediately
		{
			int proposed_frame = config_ladder_up(current_configuration, robust_enabled, narrowband_enabled == YES);
			bool frame_at_ceiling = (turboshift_phase == TURBO_DONE && turboshift_last_good >= 0
				&& config_ladder_index(proposed_frame) > config_ladder_index(turboshift_last_good));
		if(data_ack_received==YES && gear_shift_on==YES && gear_shift_algorithm==SUCCESS_BASED_LADDER &&
			messages_control.status==FREE &&
			!config_is_at_top(current_configuration, robust_enabled, narrowband_enabled == YES) && !frame_at_ceiling)
		{
			consecutive_data_acks++;
			if(consecutive_data_acks >= frame_shift_threshold)
			{
				negotiated_configuration = proposed_frame;
				printf("[GEARSHIFT] FRAME UP: %d consecutive ACKs, config %d -> %d\n",
					consecutive_data_acks, current_configuration, negotiated_configuration);
				fflush(stdout);
				consecutive_data_acks = 0;

				// Put all pending data back into TX FIFO for re-encoding at new config
				if(compression_enabled)
				{
					restore_tx_from_compressed();
				}
				else
				{
					for(int i=nMessages-1; i>=0; i--)
					{
						if(messages_tx[i].status != FREE && messages_tx[i].length > 0)
							fifo_buffer_tx.push_front(messages_tx[i].data, messages_tx[i].length);
						messages_tx[i].status = FREE;
					}
					fifo_buffer_backup.flush();
				}
				block_under_tx = NO;

				add_message_control(SET_CONFIG);
				connection_status = TRANSMITTING_CONTROL;
				return;
			}
		}
		} // frame-level gearshift ceiling scope

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
			// NB/WB auto-negotiation: NB commander in WB Phase 2 — switch back to NB
			if(commander_configured_nb == YES && narrowband_enabled == NO)
			{
				printf("[NB-NEG] Commander: NB commander connected via WB, switching to NB\n");
				fflush(stdout);
				switch_narrowband_mode(YES);
			}
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

			// Read responder's capability from byte 5.
			// With LDPC ACK: this is the responder's reply (correct).
			// With ACK pattern: no data payload, so this is our own TX data (assumes
			// symmetric capability — works when both sides use same bandwidth_mode).
			// Responder's SWITCH_BANDWIDTH handler rejects if nb_only as a safety net.
			peer_capability = (uint8_t)messages_control.data[5];
			printf("[BW-NEG] Responder capability: 0x%02X (WB=%s, COMPRESS=%s, ENCRYPT=%s)\n",
				peer_capability,
				(peer_capability & CAP_WB_CAPABLE) ? "yes" : "no",
				(peer_capability & CAP_COMPRESSION) ? "yes" : "no",
				(peer_capability & CAP_ENCRYPTION) ? "yes" : "no");
			fflush(stdout);

			// Read responder's SSID from byte 6 (confirmation — commander already knows it)
			{
				int peer_ssid = (uint8_t)messages_control.data[6];
				printf("[SSID] Responder SSID=%d (0xFF=none)\n", peer_ssid);
				fflush(stdout);
			}

			// Compression: defer unless force_compress CLI flag is set
			{
				bool both_support = (local_capability & CAP_COMPRESSION) &&
				                    (peer_capability & CAP_COMPRESSION);
				if(force_compress && both_support)
				{
					compression_enabled = true;
					compressor.init();
					printf("[COMPRESS] Force-enabled (--compress flag)\n");
					fflush(stdout);
				}
				else if(both_support)
				{
					compressor.init();  // Pre-init contexts, arm later on B2F detection
					printf("[COMPRESS] Deferred (waiting for B2F detection)\n");
					fflush(stdout);
				}
			}

			// B2F handler: init for Winlink LZHUF unroll/reroll
			b2f_handler.init();
			b2f_handler.unroll_enabled = (local_capability & CAP_B2F_UNROLL) &&
			                             (peer_capability & CAP_B2F_UNROLL);
			printf("[B2F] %s (local=0x%02X peer=0x%02X)\n",
				b2f_handler.unroll_enabled ? "Unroll ENABLED" : "Unroll DISABLED (peer lacks capability)",
				local_capability, peer_capability);
			fflush(stdout);

			// Encryption negotiation
			{
				bool both_support = (local_capability & CAP_ENCRYPTION) &&
				                    (peer_capability & CAP_ENCRYPTION);
				if (encryption_mode != ENCRYPT_OFF && both_support)
				{
					encryption_enabled = true;
					// Encryption requires batch-level assembly (compression path)
					if (!compression_enabled)
					{
						compression_enabled = true;
						printf("[CRYPTO] Forced compression ON (required for batch encryption)\n");
					}
					printf("[CRYPTO] Encryption negotiated (%s mode), key exchange after turboshift\n",
						encryption_mode == ENCRYPT_STRICT ? "SNDL-safe" : "classical-first");
				}
				else if (encryption_mode != ENCRYPT_OFF && !both_support)
				{
					printf("[CRYPTO] WARNING: Peer does not support encryption (peer_cap=0x%02X)\n",
						peer_capability);
				}
			}

			// Streaming compression negotiation
			{
				bool streaming_ok = (local_capability & CAP_STREAMING)
					&& (peer_capability & CAP_STREAMING)
					&& compression_enabled;
				if(streaming_ok)
				{
					compressor.streaming_enable();
				}
				else
				{
					printf("[STREAMING] Not enabled (local=0x%02X peer=0x%02X compress=%d)\n",
						local_capability, peer_capability, compression_enabled);
				}
			}
			fflush(stdout);

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
#ifdef MERCURY_GUI_ENABLED
				gui_set_monitor_callsigns(this->my_call_sign.c_str(),
				                          this->destination_call_sign.c_str());
				{
					char buf[128];
					snprintf(buf, sizeof(buf), "[CONNECTED %s <-> %s]",
					         this->my_call_sign.c_str(),
					         this->destination_call_sign.c_str());
					gui_push_monitor_event(buf, true);
				}
#endif
				watchdog_timer.start();
				link_timer.start();
				connection_attempt_timer.stop();
				connection_attempt_timer.reset();

				// BW negotiation: if we support WB and currently NB, propose WB upgrade.
				// Don't check peer_capability here — MFSK ACK patterns don't carry data,
				// so peer_capability is unreliable. The responder's accept/reject comes
				// back in the SWITCH_BANDWIDTH LDPC ACK (data[1]).
				bool we_want_wb = (local_capability & CAP_WB_CAPABLE);
				bool currently_nb = (narrowband_enabled == YES);

				if(we_want_wb && currently_nb)
				{
					printf("[BW-NEG] Both WB-capable, initiating WB upgrade\n");
					fflush(stdout);
					wb_upgrade_pending = true;
					cleanup();
					add_message_control(SWITCH_BANDWIDTH);
					this->connection_status=TRANSMITTING_CONTROL;
				}
				// Turboshift: start probing instead of jumping to data
				else if(turboshift_active && gear_shift_on==YES && !config_is_at_top(current_configuration, robust_enabled, narrowband_enabled == YES))
				{
					turboshift_initiator = true;
					turboshift_phase = TURBO_FORWARD;
					turboshift_last_good = current_configuration;

					// SNR-based supershift: if on OFDM with measured SNR, jump directly
					int snr_target = -1;
					if(is_ofdm_config(current_configuration) && measurements.SNR_uplink > -90)
					{
						snr_target = get_configuration(measurements.SNR_uplink - SUPERSHIFT_MARGIN_DB);
						int cfg_ceiling = (narrowband_enabled == YES) ? NB_CONFIG_MAX : CONFIG_15;
						if(snr_target > cfg_ceiling)
							snr_target = cfg_ceiling;
					}

					if(snr_target > 0 && config_ladder_index(snr_target) > config_ladder_index(current_configuration))
					{
						negotiated_configuration = snr_target;
						printf("[TURBO] Phase: FORWARD — probing commander->responder\n");
						printf("[TURBO] SNR-SUPERSHIFT: SNR=%.1f dB -> config %d -> %d (direct)\n",
							measurements.SNR_uplink, current_configuration, negotiated_configuration);
					}
					else
					{
						negotiated_configuration = config_ladder_up_n(current_configuration, 3, robust_enabled, narrowband_enabled == YES);
						printf("[TURBO] Phase: FORWARD — probing commander->responder\n");
						printf("[TURBO] SUPERSHIFT: config %d -> %d (step 3)\n", current_configuration, negotiated_configuration);
					}
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
		else if(this->link_status==CONNECTED && messages_control.data[0]==KEY_EXCHANGE_1)
		{
			// KEY_EXCHANGE_1 ACK: responder's X25519 pubkey (32 bytes at data[1..32])
			// + confirmation tag (8 bytes at data[33..40])
			printf("[CRYPTO] KEY_EXCHANGE_1 ACKed — received responder's X25519 pubkey\n");
			fflush(stdout);

			cipher_suite.compute_x25519_shared((const uint8_t*)&messages_control.data[1]);

			// Derive session key from X25519 (classical-only for now)
			cipher_suite.derive_session_key(
				my_call_sign.c_str(), destination_call_sign.c_str(),
				(psk_hex[0] != '\0') ? (const uint8_t*)psk_hex : NULL,
				(psk_hex[0] != '\0') ? (int)strlen(psk_hex) : 0,
				false);  // mlkem_done=false (X25519-only for now)

			// Verify responder's key confirmation tag (PSK mismatch detection)
			uint8_t our_tag[8];
			cipher_suite.compute_key_confirmation(our_tag);
			const uint8_t* peer_tag = (const uint8_t*)&messages_control.data[1 + X25519_KEY_SIZE];

			if(memcmp(our_tag, peer_tag, 8) != 0)
			{
				printf("[CRYPTO] PSK MISMATCH detected from KEY_EXCHANGE_1 ACK\n");
				fflush(stdout);

				// Report on control port
				const char* err_msg = "ENCRYPTION FAILURE PSK MISMATCH\r";
				int elen = (int)strlen(err_msg);
				for(int e=0; e<elen; e++)
					tcp_socket_control.message->buffer[e] = err_msg[e];
				tcp_socket_control.message->length = elen;
				tcp_socket_control.transmit();

#ifdef MERCURY_GUI_ENABLED
				g_gui_state.encryption_psk_mismatch.store(true);
				gui_push_monitor_event("[PSK MISMATCH — pre-shared key does not match, disconnecting]", true);
#endif
				// Still send KEY_ACTIVATE so the responder can also verify
				// the tag mismatch and disconnect cleanly on its side.
				messages_control.status = FREE;
				add_message_control(KEY_ACTIVATE);
				psk_mismatch_pending = true;

				watchdog_timer.start();
				link_timer.start();
			}
			else
			{
				printf("[CRYPTO] Key confirmation matches — sending KEY_ACTIVATE\n");
				fflush(stdout);

				messages_control.status = FREE;
				add_message_control(KEY_ACTIVATE);

				watchdog_timer.start();
				link_timer.start();
			}
		}
		else if(this->link_status==CONNECTED && messages_control.data[0]==KEY_ACTIVATE)
		{
			if(psk_mismatch_pending)
			{
				// KEY_ACTIVATE ACKed despite mismatch? Shouldn't happen, but disconnect.
				printf("[CRYPTO] KEY_ACTIVATE ACKed (PSK mismatch), disconnecting\n");
				fflush(stdout);
				psk_mismatch_pending = false;
				this->link_status = DROPPED;
				reset_session_state();
				return;
			}

			// KEY_ACTIVATE ACKed — encryption is now active on both sides
			cipher_suite.activate();
			tx_batch_counter = 0;
			rx_batch_counter = 0;
			consecutive_auth_failures = 0;
#ifdef MERCURY_GUI_ENABLED
			g_gui_state.encryption_active.store(true);
			g_gui_state.encryption_psk_mismatch.store(false);
#endif
			printf("[CRYPTO] KEY_ACTIVATE ACKed — encryption active (X25519 + ChaCha20-Poly1305)\n");
			fflush(stdout);

			// Report encryption state on control port
			{
				std::string str = "ENCRYPTION CLASSICAL\r";
				if (tcp_socket_control.get_status() == TCP_STATUS_ACCEPTED)
				{
					tcp_socket_control.message->length = str.length();
					for (int i = 0; i < (int)str.length(); i++)
						tcp_socket_control.message->buffer[i] = str[i];
					tcp_socket_control.transmit();
				}
			}

#ifdef MERCURY_GUI_ENABLED
			gui_push_monitor_event("[ENCRYPTION ACTIVE: X25519 + ChaCha20-Poly1305]", true);
#endif

			this->connection_status = TRANSMITTING_DATA;
			watchdog_timer.start();
			link_timer.start();
		}
		else if(this->link_status==CONNECTED && messages_control.data[0]==SWITCH_BANDWIDTH)
		{
			// ACK received = responder accepted (nb_only responders don't ACK at all)
			printf("[BW-NEG] SWITCH_BANDWIDTH accepted, switching to WB\n");
			fflush(stdout);
			wb_upgrade_pending = false;
			switch_narrowband_mode(NO);

			// Start turboshift in WB
			if(turboshift_active && gear_shift_on==YES && !config_is_at_top(current_configuration, robust_enabled, narrowband_enabled == YES))
			{
				turboshift_initiator = true;
				turboshift_phase = TURBO_FORWARD;
				turboshift_last_good = current_configuration;

				int snr_target = -1;
				if(is_ofdm_config(current_configuration) && measurements.SNR_uplink > -90)
				{
					snr_target = get_configuration(measurements.SNR_uplink - SUPERSHIFT_MARGIN_DB);
					// Enforce bandwidth ceiling
					int cfg_ceiling = (narrowband_enabled == YES) ? NB_CONFIG_MAX : CONFIG_15;
					if(snr_target > cfg_ceiling)
						snr_target = cfg_ceiling;
				}

				if(snr_target > 0 && config_ladder_index(snr_target) > config_ladder_index(current_configuration))
				{
					negotiated_configuration = snr_target;
					printf("[TURBO] Phase: FORWARD — probing commander->responder (post WB upgrade)\n");
					printf("[TURBO] SNR-SUPERSHIFT: SNR=%.1f dB -> config %d -> %d (direct)\n",
						measurements.SNR_uplink, current_configuration, negotiated_configuration);
				}
				else
				{
					negotiated_configuration = config_ladder_up_n(current_configuration, 3, robust_enabled, narrowband_enabled == YES);
					printf("[TURBO] Phase: FORWARD — probing commander->responder (post WB upgrade)\n");
					printf("[TURBO] SUPERSHIFT: config %d -> %d (step 3)\n", current_configuration, negotiated_configuration);
				}
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
		else if(this->link_status==CONNECTED)
		{
			if (messages_control.data[0]==FILE_END_)
			{
				this->connection_status=TRANSMITTING_DATA;
				std::cout<<"end of file acked"<<std::endl;
			}
			// BLOCK_END eliminated — pattern ACK / silence is sole flow control.
			// finalize_block_commander() called directly after data ACK.
			else if (messages_control.data[0]==SWITCH_ROLE)
			{
				// Asymmetric gearshift: swap forward/reverse for the return path
				if(forward_configuration != CONFIG_NONE && reverse_configuration != CONFIG_NONE)
				{
					char tmp = forward_configuration;
					forward_configuration = reverse_configuration;
					reverse_configuration = tmp;

					// During turboshift, both sides are at the same mutual config.
					// Loading the swapped forward_configuration would corrupt it
					// with a stale reverse_configuration from earlier probes.
					if(turboshift_phase == TURBO_DONE)
					{
						if(forward_configuration != current_configuration)
						{
							data_configuration = forward_configuration;
							load_configuration(data_configuration, PHYSICAL_LAYER_ONLY, YES);
							printf("[GEARSHIFT] SWITCH_ROLE: loaded config %d for return path\n",
								forward_configuration);
						}
					}
					else
					{
						printf("[GEARSHIFT] SWITCH_ROLE during turboshift: staying at config %d\n",
							current_configuration);
						fflush(stdout);
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
				// Clear messages_rx[] to prevent stale frames from previous phases
				// from being ACKed alongside legitimate data in the next batch.
				for(int i=0;i<nMessages;i++) messages_rx[i].status=FREE;
				messages_rx_buffer.status=FREE;
				// After SWITCH_ROLE, the buffer contains stale ACK pattern audio
				// from the ACK detection polling. MFSK ACK tones create false
				// OFDM preamble correlations → 10-15 LDPC FAILs before the real
				// frame shifts in. Flush the buffer to eliminate false detections.
				{
					MUTEX_LOCK(&capture_prep_mutex);
					int signal_period = telecom_system->data_container.Nofdm
						* telecom_system->data_container.buffer_Nsymb
						* telecom_system->data_container.interpolation_rate;
					memset(telecom_system->data_container.passband_delayed_data, 0,
						2 * signal_period * sizeof(double));
					telecom_system->data_container.ring_write_index = 0;
					MUTEX_UNLOCK(&capture_prep_mutex);
				}
				circular_buf_reset(capture_buffer);
				// SWITCH_ROLE: new commander needs ~120-170 symbols to process
				// role switch, fill batch, ptt_on, encode, TX. Ring buffer
				// preserves data in place (no shift_left drift), so we just
				// need enough callbacks for the turnaround. Old shift_left
				// value was buffer_Nsymb (223 = 4.7s!) to prevent drift.
				{
					int rx_frame = telecom_system->data_container.preamble_nSymb
						+ telecom_system->get_active_nsymb();
					telecom_system->data_container.frames_to_read = rx_frame + 10;
				}
				telecom_system->data_container.nUnder_processing_events = 0;
				telecom_system->receive_stats.mfsk_search_raw = 0;
				telecom_system->receive_stats.ofdm_search_raw = 0;
				telecom_system->receive_stats.ofdm_batch_active = false;
			}
			else if (messages_control.data[0]==SET_CONFIG)
			{
				// SET_CONFIG ACK received: apply the new config now
				gear_shift_timer.stop();
				gear_shift_timer.reset();
				int prev_configuration = current_configuration;  // save before load
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
					break_recovery_phase = 0;
					break_drop_step = 1;  // reset backoff on success

					if(turboshift_phase != TURBO_DONE)
					{
						// Turboshift ceiling recovery: both sides now at settle config.
						// Send SWITCH_ROLE to continue turboshift.
						printf("[BREAK-RECOVERY] Config %d verified, continuing turboshift\n",
							current_configuration);
						fflush(stdout);
						finish_turbo_direction();
					}
					else
					{
						int fifo_load = fifo_buffer_tx.get_size() - fifo_buffer_tx.get_free_size();
						printf("[BREAK-RECOVERY] Config %d verified, resuming data exchange (fifo=%d bytes, block_tx=%d)\n",
							current_configuration, fifo_load, block_under_tx);
						fflush(stdout);
						this->connection_status = TRANSMITTING_DATA;
					}
				}
				// Turboshift: keep climbing or finish direction
				else if(turboshift_active)
				{
					turboshift_last_good = prev_configuration;
					turboshift_retries = 1;  // reset retry for next config
					if(!config_is_at_top(current_configuration, robust_enabled, narrowband_enabled == YES))
					{
						// SNR-based supershift: on OFDM configs, use measured SNR to jump
						// directly to optimal config instead of blind stepping.
						int snr_target = -1;
						if(is_ofdm_config(current_configuration) && measurements.SNR_uplink > -90)
						{
							snr_target = get_configuration(measurements.SNR_uplink - SUPERSHIFT_MARGIN_DB);
							// Enforce bandwidth ceiling
							int cfg_ceiling = (narrowband_enabled == YES) ? NB_CONFIG_MAX : CONFIG_15;
							if(snr_target > cfg_ceiling)
								snr_target = cfg_ceiling;
						}

						if(snr_target > 0 && config_ladder_index(snr_target) > config_ladder_index(current_configuration))
						{
							negotiated_configuration = snr_target;
							printf("[TURBO] SNR-SUPERSHIFT: SNR=%.1f dB -> config %d -> %d (direct)\n",
								measurements.SNR_uplink, current_configuration, negotiated_configuration);
						}
						else
						{
							// ROBUST or no SNR: step 3 up the ladder
							negotiated_configuration = config_ladder_up_n(current_configuration, 3, robust_enabled, narrowband_enabled == YES);
							printf("[TURBO] SUPERSHIFT: config %d -> %d (step 3)\n",
								current_configuration, negotiated_configuration);
						}
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
					// Supershift re-trigger: after a ladder gearshift SET_CONFIG success,
					// fresh OFDM SNR is available. If it suggests we're far below optimal,
					// re-enter turboshift to jump ahead (rise fast, fall slow).
					bool retriggered = false;
					if(turboshift_phase == TURBO_DONE && gear_shift_on == YES &&
						is_ofdm_config(current_configuration) && measurements.SNR_uplink > -90)
					{
						int snr_ideal = get_configuration(measurements.SNR_uplink - SUPERSHIFT_MARGIN_DB);
						if(narrowband_enabled == YES && snr_ideal > NB_CONFIG_MAX)
							snr_ideal = NB_CONFIG_MAX;
						int gap = config_ladder_index(snr_ideal) - config_ladder_index(current_configuration);
						if(gap >= SUPERSHIFT_RETRIGGER_CONFIGS)
						{
							printf("[TURBO] RE-TRIGGER: SNR=%.1f dB suggests config %d (current %d, gap=%d)\n",
								measurements.SNR_uplink, snr_ideal, current_configuration, gap);
							fflush(stdout);
							turboshift_active = true;
							turboshift_phase = TURBO_FORWARD;
							turboshift_initiator = true;
							turboshift_last_good = current_configuration;
							turboshift_retries = 1;
							negotiated_configuration = snr_ideal;
							cleanup();
							add_message_control(SET_CONFIG);
							this->connection_status = TRANSMITTING_CONTROL;
							retriggered = true;
						}
					}
					if(!retriggered)
					{
						// Frame gearshift applied — if data fails immediately, BREAK
						if(data_configuration != prev_configuration)
							frame_gearshift_just_applied = true;
						this->connection_status=TRANSMITTING_DATA;
					}
				}
				watchdog_timer.start();
				link_timer.start();
			}
		}
		else if(this->link_status==DISCONNECTING && messages_control.data[0]==CLOSE_CONNECTION)
		{
			reset_session_state();
			load_configuration(init_configuration,FULL,YES);
			this->link_status=LISTENING;
			this->connection_status=RECEIVING;
			reset_all_timers();
			// Reset RX state machine - wait for fresh data (prevents decode of self-received TX audio)
			telecom_system->data_container.frames_to_read =
				telecom_system->data_container.preamble_nSymb + telecom_system->data_container.Nsymb;
			telecom_system->data_container.nUnder_processing_events = 0;

			fifo_buffer_tx.flush();
			fifo_buffer_backup.flush();
			fifo_buffer_rx.flush();

			// Reset messages_control so new CONNECT commands can work
			messages_control.status=FREE;

			set_role(RESPONDER);

			std::string str="DISCONNECTED\r";
			tcp_socket_control.message->length=str.length();

			for(int i=0;i<tcp_socket_control.message->length;i++)
			{
				tcp_socket_control.message->buffer[i]=str[i];
			}
			tcp_socket_control.transmit();
		}

		// Bug #41: After processing any ACKed control message, free the slot so
		// the next handshake message can be queued. Without this, messages_control
		// stays ACKED after START_CONNECTION ACK, and add_message_control(TEST_CONNECTION)
		// in process_messages_commander returns ERROR_ (status != FREE).
		// cleanup() is idempotent: branches that already call it (SET_CONFIG, etc.)
		// will find status=FREE on the second call, which is a no-op.
		cleanup();
	}
}

// Shared BLOCK_END state transitions: reset block, flush backup, stats, gearshift.
// Called from both explicit BLOCK_END ACK handler and implicit path (batch_size==1).
void cl_arq_controller::finalize_block_commander()
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

	// Streaming: commit context after successful data ACK
	if(compressor.is_streaming())
		compressor.commit_pending();

#ifdef MERCURY_GUI_ENABLED
	if(batch_uncompressed_size > 0)
		gui_add_throughput_bytes_tx(batch_uncompressed_size);
	batch_uncompressed_size = 0;
#endif

	if(last_transmission_block_stats.nSent_data > 0)
		last_transmission_block_stats.success_rate_data=100*(1-((float)last_transmission_block_stats.nReSent_data/(float)last_transmission_block_stats.nSent_data));
	else
		last_transmission_block_stats.success_rate_data=100;
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
				{
					int proposed = config_ladder_up(current_configuration, robust_enabled, narrowband_enabled == YES);
					bool at_ceiling = (turboshift_phase == TURBO_DONE && turboshift_last_good >= 0
						&& config_ladder_index(proposed) > config_ladder_index(turboshift_last_good));
				if(!config_is_at_top(current_configuration, robust_enabled, narrowband_enabled == YES) && !at_ceiling)
				{
					negotiated_configuration=proposed;
					printf("[GEARSHIFT] LADDER UP: success=%.0f%% > %.0f%%, config %d -> %d\n",
						last_transmission_block_stats.success_rate_data, gear_shift_up_success_rate_precentage,
						current_configuration, negotiated_configuration);
					fflush(stdout);
					cleanup();
					add_message_control(SET_CONFIG);
				}
				else
				{
					if(at_ceiling)
						printf("[GEARSHIFT] LADDER: at turboshift ceiling %d (config %d), success=%.0f%%\n",
							turboshift_last_good, current_configuration, last_transmission_block_stats.success_rate_data);
					else
						printf("[GEARSHIFT] LADDER: at top (config %d), success=%.0f%%\n",
							current_configuration, last_transmission_block_stats.success_rate_data);
					fflush(stdout);
					this->connection_status=TRANSMITTING_DATA;
				}
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

void cl_arq_controller::process_buffer_data_commander()
{
	int data_read_size;
	if(role==COMMANDER && link_status==CONNECTED && connection_status==TRANSMITTING_DATA)
	{
		// Key exchange gate: don't fill FIFO data while key exchange is in progress.
		// process_commander() gates process_messages_tx_data(), but this function
		// is called separately from process_messages() and needs its own gate.
		if(encryption_enabled && !cipher_suite.is_active())
			return;

		if( fifo_buffer_tx.get_size()!=fifo_buffer_tx.get_free_size() && block_under_tx==NO)
		{
			int max_frame = max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH;

			if(compression_enabled)
			{
				// --- Batch-level compression with adaptive sizing ---
				// Pop raw data based on estimated compression ratio, compress,
				// iteratively add more data until batch is 85%+ full.
				int batch_capacity = data_batch_size * max_frame;

				// Reserve space for encryption auth tag if active
				int crypto_overhead = 0;
				if(cipher_suite.is_active())
					crypto_overhead = AUTH_TAG_SIZE;
				batch_capacity -= crypto_overhead;

				// Initial guess based on running ratio estimate.
				// Staging can be up to COMPRESS_WORKSPACE_SIZE (64KB) because
				// high-ratio compressors (zstd/PPMd) can shrink 33KB+ to <1.5KB.
				const int staging_max = 65535;  // Header orig_size is uint16; cap to prevent overflow
				int chdr_size = compressor.get_header_size();
				int initial_pop = (int)(batch_capacity * compress_ratio_estimate);
				if(initial_pop > staging_max) initial_pop = staging_max;
				if(initial_pop < batch_capacity - chdr_size)
					initial_pop = batch_capacity - chdr_size;

				char staging[COMPRESS_WORKSPACE_SIZE];
				int raw_size = fifo_buffer_tx.pop(staging, initial_pop);
				if(raw_size == 0)
				{
					last_transmission_block_stats.nSent_data=0;
					last_transmission_block_stats.nReSent_data=0;
				}
				else
				{
					char comp_buf[16384];
					int comp_size = 0;
					bool compress_ok = false;

					// Adaptive fill loop: compress, check fill ratio, add more.
					// Streaming mode: single compress call (no retry). The PPMd
					// model advances on each compress_block call. Retrying with
					// different data sizes causes TX/RX model desync because RX
					// only decompresses the final data once.
					int max_iter = compressor.is_streaming() ? 1 : 4;
					for(int iter = 0; iter < max_iter; iter++)
					{
						comp_size = compressor.compress_block(
							staging, raw_size, comp_buf, batch_capacity);

						if(comp_size > 0)
						{
							// Compression succeeded — check fill ratio
							float fill_ratio = (float)comp_size / (float)batch_capacity;
							if(fill_ratio >= 0.85f || iter == max_iter - 1)
							{
								compress_ok = true;
								break;  // Good enough
							}
							// Under-filled: estimate how much more raw data to add
							int comp_payload = comp_size - chdr_size;
							if(comp_payload <= 0) { compress_ok = true; break; }
							float current_ratio = (float)raw_size / (float)comp_payload;
							int remaining_comp = batch_capacity - comp_size;
							int more_raw = (int)(remaining_comp * current_ratio);
							if(more_raw < 1) { compress_ok = true; break; }
							if(raw_size + more_raw > staging_max)
								more_raw = staging_max - raw_size;
							if(more_raw <= 0) { compress_ok = true; break; }

							int got = fifo_buffer_tx.pop(staging + raw_size, more_raw);
							if(got == 0) { compress_ok = true; break; }  // FIFO empty
							raw_size += got;
							// Loop back to compress with more data
						}
						else if(comp_size == -1)
						{
							// Doesn't fit — push back excess and accept as-is
							if(compressor.is_streaming())
							{
								// Streaming: push back 40%, use raw (model already advanced)
								int pushback = raw_size * 2 / 5;
								if(pushback < 1) pushback = 1;
								fifo_buffer_tx.push_front(
									staging + raw_size - pushback, pushback);
								raw_size -= pushback;
								// Model is desynced — reset streaming
								compressor.streaming_reset();
								break;
							}
							int pushback = raw_size * 2 / 5;
							if(pushback < 1) pushback = 1;
							fifo_buffer_tx.push_front(
								staging + raw_size - pushback, pushback);
							raw_size -= pushback;
							if(raw_size < 1) break;
							// Loop back to retry compression
						}
						else
						{
							// Error (0) — fall through to raw
							break;
						}
					}

					if(compress_ok && comp_size > 0)
					{
						// Update running ratio estimate (EMA)
						int comp_payload = comp_size - chdr_size;
						if(comp_payload > 0)
						{
							float measured = (float)raw_size / (float)comp_payload;
							compress_ratio_estimate = 0.7f * compress_ratio_estimate + 0.3f * measured;
						}
						fifo_buffer_backup.push(staging, raw_size);
						batch_uncompressed_size = raw_size;
						// Streaming: save raw data pending ACK confirmation
						if(compressor.is_streaming())
							compressor.set_pending_raw((unsigned char*)staging, raw_size);
#ifdef MERCURY_GUI_ENABLED
						// Monitor tap: plaintext before compression
						gui_push_monitor_text(staging, raw_size, true);
						// Push algo to GUI (read from compressed header byte 0)
						g_gui_state.compression_algo.store((int)(unsigned char)comp_buf[0]);
#endif
					}
					else
					{
						// compress_block() error fallback — wrap raw data in ALGO_RAW.
						// Cap to batch capacity minus header.
						int hdr_sz = compressor.get_header_size();
						int max_raw = batch_capacity - hdr_sz;
						if(raw_size > max_raw)
						{
							fifo_buffer_tx.push_front(
								staging + max_raw,
								raw_size - max_raw);
							raw_size = max_raw;
						}
						comp_buf[0] = COMPRESS_ALGO_RAW;
						comp_buf[1] = (char)(raw_size & 0xFF);
						comp_buf[2] = (char)((raw_size >> 8) & 0xFF);
						comp_buf[3] = (char)(raw_size & 0xFF);
						comp_buf[4] = (char)((raw_size >> 8) & 0xFF);
						if(compressor.is_streaming())
						{
							// CRC16 for streaming desync detection
							uint16_t crc = 0xFFFF;
							for(int j = 0; j < raw_size; j++)
							{
								crc ^= (unsigned char)staging[j];
								for(int b = 0; b < 8; b++)
									crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
							}
							comp_buf[5] = (char)(crc & 0xFF);
							comp_buf[6] = (char)((crc >> 8) & 0xFF);
						}
						memcpy(comp_buf + hdr_sz, staging, raw_size);
						comp_size = hdr_sz + raw_size;
						fifo_buffer_backup.push(staging, raw_size);
						batch_uncompressed_size = raw_size;
						// Streaming: save raw data pending ACK confirmation
						if(compressor.is_streaming())
							compressor.set_pending_raw((unsigned char*)staging, raw_size);
#ifdef MERCURY_GUI_ENABLED
						// Monitor tap: plaintext (RAW fallback path)
						gui_push_monitor_text(staging, raw_size, true);
#endif
					}

					// --- Encrypt batch (after compression, before frame split) ---
					if(cipher_suite.is_active() && comp_size > 0)
					{
						// Always use full 16-byte auth tag — encryption only runs
						// after turboshift at OFDM speeds where 16 bytes is negligible.
						int tag_size = AUTH_TAG_SIZE;

						// Pad compressed data so encrypted output fills all
						// data_batch_size frames.  The ACK gate can't peek at
						// the compression header when it's encrypted, so it
						// expects data_batch_size unique frames.  Without
						// padding, compression may produce fewer frames and
						// the duplicate-ID padding causes ACK gate suppression.
						// Receiver decrypts, reads comp_size from header, and
						// ignores the zero padding beyond it.
						int target_comp = data_batch_size * max_frame - tag_size;
						if(comp_size < target_comp && target_comp <= (int)sizeof(comp_buf))
						{
							memset(comp_buf + comp_size, 0, target_comp - comp_size);
							comp_size = target_comp;
						}

						uint32_t tx_direction = (original_role == COMMANDER)
							? DIRECTION_CMD_TO_RSP : DIRECTION_RSP_TO_CMD;
						printf("[CRYPTO-TX] Encrypting %d bytes, counter=%llu dir=%u tag=%d config=%d\n",
							comp_size, (unsigned long long)tx_batch_counter,
							tx_direction, tag_size, (int)data_configuration);
						fflush(stdout);
						char enc_buf[16384];
						int enc_size = cipher_suite.encrypt(
							(const uint8_t*)comp_buf, comp_size,
							(uint8_t*)enc_buf, sizeof(enc_buf),
							tx_batch_counter, tx_direction,
							tag_size);
						if(enc_size > 0)
						{
							memcpy(comp_buf, enc_buf, enc_size);
							comp_size = enc_size;
							tx_batch_counter++;
						}
						else
						{
							printf("[CRYPTO] Encrypt failed (batch %llu)\n",
								(unsigned long long)tx_batch_counter);
							fflush(stdout);
						}
					}

					// With encryption, comp_buf is padded + encrypted to fill
					// all data_batch_size frames exactly (no duplicate IDs).
					// Without encryption, pad_messages_batch_tx() fills
					// remaining batch slots with duplicates.

					// Ensure contiguous IDs 0..data_batch_size-1
					for(int i = 0; i < data_batch_size; i++)
					{
						if(messages_tx[i].status != FREE)
							messages_tx[i].status = FREE;
					}

					// Split comp_buf into frames
					int pos = 0;
					int frame_num = 0;
					while(pos < comp_size)
					{
						int chunk = comp_size - pos;
						if(chunk > max_frame) chunk = max_frame;
						block_under_tx = YES;
						int result;
						if(chunk == max_frame)
							result = add_message_tx_data(DATA_LONG, chunk, comp_buf + pos);
						else
							result = add_message_tx_data(DATA_SHORT, chunk, comp_buf + pos);
						if(result != SUCCESSFUL)
						{
							printf("[COMPRESS-TX] ERROR: add_message_tx_data failed at frame %d (result=%d)\n",
								frame_num, result);
							fflush(stdout);
						}
						pos += chunk;
						frame_num++;
					}
					printf("[COMPRESS-TX] %d raw -> %d comp (%d frames), ratio_est=%.2f, fill=%.0f%%\n",
						raw_size, comp_size, frame_num, compress_ratio_estimate,
						100.0f * comp_size / batch_capacity);
					fflush(stdout);
				}
			}
			else
			{
				// --- No compression: original per-frame loop ---
				int filled = 0;
				int fill_limit = data_batch_size;
				batch_uncompressed_size = 0;
				for(int i=0;i<fill_limit;i++)
				{
					data_read_size=fifo_buffer_tx.pop(message_TxRx_byte_buffer, max_frame);
					if(data_read_size==0)
					{
						last_transmission_block_stats.nSent_data=0;
						last_transmission_block_stats.nReSent_data=0;
						break;
					}
					fifo_buffer_backup.push(message_TxRx_byte_buffer, data_read_size);
					batch_uncompressed_size += data_read_size;
#ifdef MERCURY_GUI_ENABLED
					// Monitor tap: plaintext (no compression path)
					gui_push_monitor_text(message_TxRx_byte_buffer, data_read_size, true);
#endif
					block_under_tx=YES;
					if(data_read_size==max_frame)
						add_message_tx_data(DATA_LONG, data_read_size, message_TxRx_byte_buffer);
					else
						add_message_tx_data(DATA_SHORT, data_read_size, message_TxRx_byte_buffer);
					filled++;
				}
			}
		}
		else if(block_under_tx==YES && message_batch_counter_tx==0 && get_nOccupied_messages()==0 && messages_control.status==FREE)
		{
			// BLOCK_END eliminated: pattern ACK / silence is sole flow control.
			// Commander finalizes immediately after all data frames ACKed.
			// Responder flushes data to app after sending pattern ACK.
			// Downshift on decode failure handled by emergency BREAK (threshold=2).
			finalize_block_commander();
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
