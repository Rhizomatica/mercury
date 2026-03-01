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
	{
		int fill_end = max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH;
		if(fill_end > N_MAX/8) fill_end = N_MAX/8;
		for(int j=messages_rx[loc].length;j<fill_end;j++)
		{
			messages_rx[loc].data[j]=0;
		}
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
	// Fast HAIL scanning while LISTENING: use receive_hail_pattern() (~32ms cycles)
	// instead of slow receive() (multi-second LDPC frame captures).
	if(link_status == LISTENING && ack_pattern_time_ms > 0 && hail_detected == NO)
	{
		// Override large frames_to_read from LISTENING init — HAIL scanning
		// needs frames_to_read==0 to check the buffer. The audio callback
		// continuously fills the buffer regardless, so audio is always fresh.
		if(telecom_system->data_container.frames_to_read > 2)
		{
			MUTEX_LOCK(&capture_prep_mutex);
			telecom_system->data_container.frames_to_read = 2;
			MUTEX_UNLOCK(&capture_prep_mutex);
		}

		if(receive_hail_pattern())
		{
			printf("[HAIL] 'I am Mercury' beacon detected!\n");
			fflush(stdout);

			// Notify Winlink/trimode that incoming traffic detected
			std::string pending_str = "PENDING\r";
			tcp_socket_control.message->length = pending_str.length();
			for(int i = 0; i < (int)pending_str.length(); i++)
				tcp_socket_control.message->buffer[i] = pending_str[i];
			tcp_socket_control.transmit();

			// Respond with our own HAIL
			send_hail_pattern();
			printf("[HAIL] Responded with beacon\n");
			fflush(stdout);

			hail_detected = YES;

			// Prepare for START_CONNECTION (generous timeout for commander turnaround)
			set_receiving_timeout(3 * message_transmission_time_ms + 5000);
			receiving_timer.start();
			connection_status = RECEIVING;

			// Restore frames_to_read for full LDPC frame capture
			// (send_hail_pattern leaves ftr=2 which is too short for START_CONNECTION)
			telecom_system->data_container.frames_to_read =
				telecom_system->data_container.preamble_nSymb + telecom_system->data_container.Nsymb;
		}
		return; // Keep scanning (fast cycle) or just responded
	}

	if (receiving_timer.get_elapsed_time_ms()<receiving_timeout)
	{
		this->receive();

		// Emergency BREAK: commander signals "drop to ROBUST_0"
		// Only respond if we're actually connected — prevents all radios on a
		// frequency from ACKing someone else's BREAK.
		if(break_detected == YES && link_status != CONNECTED)
			break_detected = NO;
		if(break_detected == YES && link_status == CONNECTED)
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
			batch_rx_frame_count = 0;
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
					{
					int copy_len = max_data_length+max_header_length-CONTROL_ACK_CONTROL_HEADER_LENGTH;
					if(copy_len > N_MAX/8) copy_len = N_MAX/8;
					for(int j=0;j<copy_len;j++)
					{
						messages_control.data[j]=messages_rx_buffer.data[j];
					}
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
				batch_rx_frame_count++;

				{
					int rx_timeout;
					if(batch_rx_frame_count >= data_batch_size)
					{
						// All frames in batch decoded (count-based, not seq-based).
						// send_ack_pattern()'s OFDM wait prevents TX collision.
						// Note: seq order may differ from decode order when
						// beyond-bounds recovery skips then recovers frames.
						rx_timeout = ptt_on_delay_ms;
					}
					else
					{
						// More frames expected.  Add one msg_time margin for
						// FAIL recovery (false peaks in old frame body).
						int remaining = data_batch_size - messages_rx_buffer.sequence_number - 1;
						rx_timeout = remaining * message_transmission_time_ms
							+ time_left_to_send_last_frame + ptt_on_delay_ms
							+ message_transmission_time_ms;
					}
					set_receiving_timeout(rx_timeout);
				}
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

		// If we responded to HAIL but START_CONNECTION never arrived,
		// go back to HAIL scanning for the next beacon.
		if(link_status == LISTENING && hail_detected == YES)
		{
			printf("[HAIL] Timeout waiting for START_CONNECTION, resuming HAIL scan\n");
			fflush(stdout);
			hail_detected = NO;
		}
	}
}

void cl_arq_controller::process_messages_acknowledging_control()
{
	message_batch_counter_tx=0;
	if(g_verbose) { printf("[ACK-CTRL] status=%d (need %d=RECEIVED), ack_cfg=%d\n", messages_control.status, RECEIVED, ack_configuration); fflush(stdout); }
	if(messages_control.status==RECEIVED)
	{
		messages_control.type=ACK_CONTROL;
		messages_control.status=ACKED;
		stats.nAcks_sent_control++;

		// Bug #36: reset nUnder BEFORE ACK TX so only turnaround-period
		// nUnder is counted (not accumulated LISTENING nUnder).
		telecom_system->data_container.nUnder_processing_events = 0;

		if(ack_pattern_time_ms > 0)
		{
			// ACK pattern uses dedicated ack_mfsk — no config switch needed
			if(g_verbose) { printf("[ACK-CTRL] Sending ACK pattern (no config switch)\n"); fflush(stdout); }
			send_ack_pattern();
			// If config changed (e.g., SET_CONFIG), load the new data config now.
			// ACK was sent on old config (correct — commander is still on old config),
			// but we need to switch to new config before receiving data.
			if(data_configuration != current_configuration)
			{
				if(g_verbose) { printf("[ACK-CTRL] Loading new data config %d (was %d)\n", data_configuration, current_configuration); fflush(stdout); }
				load_configuration(data_configuration, PHYSICAL_LAYER_ONLY, YES);
			}
		}
		else
		{
			// Fallback: LDPC ACK needs ack_configuration for correct modulation
			if(g_verbose) { printf("[ACK-CTRL] Sending LDPC ACK, loading config %d...\n", ack_configuration); fflush(stdout); }
			load_configuration(ack_configuration, PHYSICAL_LAYER_ONLY,NO);
			messages_batch_tx[message_batch_counter_tx]=messages_control;
			message_batch_counter_tx++;
			telecom_system->set_mfsk_ctrl_mode(true);
			pad_messages_batch_tx(ack_batch_size);
			send_batch();
			load_configuration(data_configuration, PHYSICAL_LAYER_ONLY,YES);
		}
		// Capture frame + turnaround gap: CMD processing overhead only.
		// See acknowledging_data for detailed comment.
		// Must match buffer allocation (data_container.cc).
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

			int frame_symb = telecom_system->data_container.preamble_nSymb
				+ telecom_system->data_container.Nsymb;
			// OFDM ftr depends on command type:
			//   SWITCH_ROLE/SWITCH_BANDWIDTH: T_p = 118-167 symbols
			//     (new commander processes role switch + fills batch).
			//     Needs full buffer_Nsymb. See OFDM_BEYOND_BOUNDS.md §18.
			//   SET_CONFIG and others: T_p ≈ 30-44 symbols
			//     (commander detects ACK + guard + ptt_on).
			//     upper_bound = buffer_Nsymb - frame_symb suffices.
			// MFSK: frame_symb + 20 (no beyond-bounds issue, and
			//   buffer_Nsymb is 791-1225 which causes ACK timeouts).
			int ftr_val;
			if(is_ofdm_config(current_configuration))
			{
				char cmd = messages_control.data[0];
				int buf_nsymb = telecom_system->data_container.buffer_Nsymb.load();
				if(cmd == SWITCH_ROLE || cmd == SWITCH_BANDWIDTH)
					ftr_val = buf_nsymb;
				else
					ftr_val = buf_nsymb - frame_symb;  // = upper_bound
			}
			else
				ftr_val = frame_symb + 20;
			telecom_system->data_container.frames_to_read = ftr_val;
			telecom_system->data_container.nUnder_processing_events = 0;

			// === DIAG: gearshift ftr trace (remove after debug) ===
			{ int buf_Nsymb = telecom_system->data_container.buffer_Nsymb.load(); printf("[FTR-GEAR] CONFIG_%d ftr=%d (buffer_Nsymb=%d frame_symb=%d ofdm=%d)\n", current_configuration, ftr_val, buf_Nsymb, frame_symb, is_ofdm_config(current_configuration)); fflush(stdout); }
		}

		char ack_command = messages_control.data[0];  // Save before potential NB switch
		messages_control.status=FREE;
		batch_rx_frame_count = 0;
		connection_status=RECEIVING;
		connection_id=assigned_connection_id;

		// NB/WB auto-negotiation: deferred switch after START_CONNECTION ACK
		// Must happen after ACK is sent in WB (so commander can hear it)
		// but before receiving next message (TEST_CONNECTION in NB)
		if(link_status == CONNECTION_RECEIVED && session_narrowband && narrowband_enabled == NO)
		{
			printf("[NB-NEG] Responder: switching to narrowband after START_CONNECTION ACK\n");
			fflush(stdout);
			commander_configured_nb = NO;  // Save original WB for restore on disconnect
			switch_narrowband_mode(YES);
		}

		// BW negotiation: deferred WB switch after SWITCH_BANDWIDTH ACK
		if(wb_upgrade_pending)
		{
			printf("[BW-NEG] Responder: switching to WB after SWITCH_BANDWIDTH ACK\n");
			fflush(stdout);
			wb_upgrade_pending = false;
			switch_narrowband_mode(NO);
			// After WB config loads, the NB ftr=264 is still active but the
			// WB buffer is only 223 symbols. The leftover NB ftr causes the
			// first WB scan to happen after 264 symbols of total capture,
			// by which time the commander's preamble has scrolled out of the
			// smaller WB buffer. Reset ftr to the WB-appropriate value so
			// the first WB scan catches the preamble with margin.
			{
				int rx_frame = telecom_system->data_container.preamble_nSymb
				             + telecom_system->data_container.Nsymb;
				telecom_system->data_container.frames_to_read = rx_frame + 40;
				telecom_system->data_container.nUnder_processing_events = 0;
				printf("[BW-NEG] WB ftr reset to %d\n",
					telecom_system->data_container.frames_to_read.load());
				fflush(stdout);
			}
		}

		if (ack_command==SWITCH_ROLE)
		{
			set_role(COMMANDER);
			this->link_status=CONNECTED;
			// Clear messages_rx[] to prevent stale frames from previous phases
			// from being ACKed alongside legitimate data in the next batch.
			for(int i=0;i<nMessages;i++) messages_rx[i].status=FREE;
			messages_rx_buffer.status=FREE;
			cl_timer ptt_off_wait;
			ptt_off_wait.reset();
			ptt_off_wait.start();
			while(ptt_off_wait.get_elapsed_time_ms()<ptt_off_delay_ms);

			bool has_asymmetric = (forward_configuration != CONFIG_NONE &&
				reverse_configuration != CONFIG_NONE);

			// Save pre-swap config: this is the mutual config both sides
			// agreed on (e.g. settle config after BREAK recovery). The swap
			// below may load a stale reverse_configuration, corrupting
			// current_configuration before turboshift_last_good is set.
			int pre_switch_config = current_configuration;

			if(has_asymmetric)
			{
				// Asymmetric gearshift: swap forward/reverse for the return path
				char tmp = forward_configuration;
				forward_configuration = reverse_configuration;
				reverse_configuration = tmp;

				// During turboshift, skip the config load — both sides are at the
				// same mutual config and we'll probe from there. Loading the swapped
				// forward_configuration would corrupt current_configuration.
				if(turboshift_phase == TURBO_DONE || turboshift_phase == TURBO_REVERSE)
				{
					if(forward_configuration != current_configuration)
					{
						data_configuration = forward_configuration;
						load_configuration(data_configuration, PHYSICAL_LAYER_ONLY, YES);
					}

					printf("[GEARSHIFT] SWITCH_ROLE: transmitting at config %d\n",
						forward_configuration);
					fflush(stdout);
				}
				else
				{
					printf("[GEARSHIFT] SWITCH_ROLE during turboshift: staying at config %d\n",
						current_configuration);
					fflush(stdout);
				}
			}

			// Turboshift: start probing reverse direction as new commander
			if(has_asymmetric && turboshift_phase == TURBO_FORWARD && gear_shift_on == YES)
			{
				turboshift_phase = TURBO_REVERSE;
				turboshift_active = true;
				turboshift_last_good = pre_switch_config;

				if(!config_is_at_top(current_configuration, robust_enabled, narrowband_enabled == YES))
				{
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
						printf("[TURBO] Phase: REVERSE — probing responder->commander\n");
						printf("[TURBO] SNR-SUPERSHIFT: SNR=%.1f dB -> config %d -> %d (direct)\n",
							measurements.SNR_uplink, current_configuration, negotiated_configuration);
					}
					else
					{
						negotiated_configuration = config_ladder_up_n(current_configuration, 3, robust_enabled, narrowband_enabled == YES);
						printf("[TURBO] Phase: REVERSE — probing responder->commander\n");
						printf("[TURBO] SUPERSHIFT: config %d -> %d (step 3)\n",
							current_configuration, negotiated_configuration);
					}
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
		else if(ack_command==CLOSE_CONNECTION)
		{
			reset_session_state();
			load_configuration(init_configuration,FULL,YES);
			this->link_status=LISTENING;
			batch_rx_frame_count = 0;
			this->connection_status=RECEIVING;
			reset_all_timers();
			// Reset RX state machine - wait for fresh data (prevents decode of self-received TX audio)
			telecom_system->data_container.frames_to_read =
				telecom_system->data_container.preamble_nSymb + telecom_system->data_container.Nsymb;
			telecom_system->data_container.nUnder_processing_events = 0;

			fifo_buffer_tx.flush();
			fifo_buffer_backup.flush();
			fifo_buffer_rx.flush();
			messages_control.status=FREE;
		}
	}
}


void cl_arq_controller::process_messages_acknowledging_data()
{
	int nAck_messages=0;
	receiving_timer.stop();
	receiving_timer.reset();

	// Bug #36: reset nUnder BEFORE ACK TX so only turnaround-period
	// nUnder is counted (not accumulated frame-processing nUnder).
	telecom_system->data_container.nUnder_processing_events = 0;

	if(ack_pattern_time_ms > 0)
	{
		// Send ACK tone pattern (universal, all modes)
		if(repeating_last_ack==NO)
		{
			// Gate: require all expected frames before sending ACK.
			// Pattern ACK carries no per-frame info — commander marks ALL
			// pending frames as ACKED. If we only decoded a partial batch,
			// suppress ACK so commander times out and retransmits.
			//
			// Use actual RECEIVED slot count (immune to OFDM re-decode).
			// With compression + no zero-padding, expected frames < data_batch_size.
			// Derive expected count from compression header in frame 0.
			int rx_received = 0;
			for(int i = 0; i < data_batch_size; i++)
				if(messages_rx[i].status == RECEIVED) rx_received++;

			int expected = data_batch_size;  // Default for non-compressed
			if(compression_enabled
				&& messages_rx[0].status == RECEIVED
				&& messages_rx[0].length >= COMPRESS_HEADER_SIZE)
			{
				const unsigned char* hdr = (const unsigned char*)messages_rx[0].data;
				int hdr_comp = hdr[1] | (hdr[2] << 8);
				int total_compressed = COMPRESS_HEADER_SIZE + hdr_comp;
				int mf = max_data_length + max_header_length - DATA_LONG_HEADER_LENGTH;
				expected = (total_compressed + mf - 1) / mf;
				if(expected > data_batch_size) expected = data_batch_size;
				if(expected < 1) expected = 1;
			}

			// Diagnostic: show which sequence numbers were received
			{
				printf("[ACK-GATE-DIAG] rx=%d/%d exp=%d seqs:", rx_received, data_batch_size, expected);
				for(int i = 0; i < data_batch_size; i++)
					if(messages_rx[i].status == RECEIVED) printf(" %d", i);
				printf("\n"); fflush(stdout);
			}

			if(data_batch_size > 1 && rx_received < expected)
			{
				printf("[ACK-GATE] Suppressing: received %d/%d (expected %d)\n",
					rx_received, data_batch_size, expected);
				fflush(stdout);
				// Keep partial messages_rx (DON'T free) — add_message_rx_data
				// overwrites unconditionally, so retransmit fills in missing
				// frames while already-received frames are preserved.
				stats.nNAcked_data++;
				batch_rx_frame_count = 0;
				// Reset RX state for fresh retransmission capture
				telecom_system->data_container.frames_to_read =
					telecom_system->data_container.preamble_nSymb
					+ telecom_system->get_active_nsymb();
				telecom_system->data_container.nUnder_processing_events = 0;
				// DON'T reset search_raw here. If the timer fires mid-batch
				// while frames are still arriving, resetting to 0 causes
				// re-decode of already-received frames. Keep position so
				// the decode loop continues forward through the buffer.
				// Return to RECEIVING — commander will timeout and retransmit
				calculate_receiving_timeout();
				receiving_timer.start();
				connection_status=RECEIVING;
				return;
			}
			printf("[ACK-GATE] PASS: received %d/%d (expected %d)\n",
				rx_received, data_batch_size, expected);
			fflush(stdout);

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

		// send_ack_pattern() sets ftr = rx_frame + turnaround_symbols to cover
		// the full turnaround gap (ACK TX → commander ACK detect → encode →
		// batch TX → preamble arrives).  DO NOT override ftr here — the old
		// ftr=rx_frame approach caused the preamble to always land 19 symbols
		// past upper_bound (position=buf-33, upper=buf-52), producing the
		// persistent 22 OK / 9 FAIL pattern on VB-Cable benchmarks.
		telecom_system->set_mfsk_ctrl_mode(false);
		telecom_system->data_container.nUnder_processing_events = 0;

		if(g_verbose) {
			printf("[ACK-DATA] ftr=%d (from send_ack_pattern turnaround)\n",
				telecom_system->data_container.frames_to_read.load());
			fflush(stdout);
		}

		// BLOCK_END eliminated: flush data to application immediately after
		// sending pattern ACK. Commander finalizes locally in parallel.
		copy_data_to_buffer();
		messages_last_ack_bu.type=NONE;

		calculate_receiving_timeout();
		receiving_timer.start();
		batch_rx_frame_count = 0;
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

		batch_rx_frame_count = 0;
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
			int peer_flags = 0;
			destination_call_sign = callsign_unpack(&messages_control.data[2], &peer_flags);
			printf("[RX-CTRL] Unpacked commander callsign: '%s', flags=0x%02X\n", destination_call_sign.c_str(), peer_flags);
			fflush(stdout);

			// NB/WB auto-negotiation: NB always wins
			bool peer_narrowband = (peer_flags & 0x01) != 0;
			bool local_narrowband = (narrowband_enabled == YES);
			session_narrowband = peer_narrowband || local_narrowband;
			if(session_narrowband && !local_narrowband)
			{
				printf("[NB-NEG] Responder: commander wants NB, will switch after ACK\n");
				fflush(stdout);
			}
			else if(session_narrowband && local_narrowband && !peer_narrowband)
			{
				printf("[NB-NEG] Responder: local is NB, peer is WB, session=NB\n");
				fflush(stdout);
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

		// Read commander's capability from byte 5 of decoded LDPC frame.
		// Always present (LDPC decodes full block; unused bytes are zero-padded).
		// Backwards-compatible: old firmware doesn't fill byte 5 → decodes as 0 = no WB.
		peer_capability = (uint8_t)messages_control.data[5];
		printf("[BW-NEG] Commander capability: 0x%02X (WB=%s, COMPRESS=%s)\n",
			peer_capability,
			(peer_capability & CAP_WB_CAPABLE) ? "yes" : "no",
			(peer_capability & CAP_COMPRESSION) ? "yes" : "no");
		fflush(stdout);

		// Compression: enable if both sides support it
		compression_enabled = (local_capability & CAP_COMPRESSION) &&
		                      (peer_capability & CAP_COMPRESSION);
		if(compression_enabled)
		{
			compressor.init();
			printf("[COMPRESS] Enabled (both peers support compression)\n");
			fflush(stdout);
		}

		// B2F handler: init for Winlink LZHUF unroll/reroll
		b2f_handler.init();
		b2f_handler.unroll_enabled = (local_capability & CAP_B2F_UNROLL) &&
		                             (peer_capability & CAP_B2F_UNROLL);
		printf("[B2F] %s (local=0x%02X peer=0x%02X)\n",
			b2f_handler.unroll_enabled ? "Unroll ENABLED" : "Unroll DISABLED (peer lacks capability)",
			local_capability, peer_capability);
		fflush(stdout);

		tmp_SNR.f_SNR=(float)measurements.SNR_downlink;
		for(int i=0;i<4;i++)
		{
			messages_control.data[i+1]=tmp_SNR.char4_SNR[i];;
		}
		messages_control.data[5]=(char)local_capability;
		messages_control.length=6;

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
	else if(link_status==CONNECTED && (code==SET_CONFIG || code==BLOCK_END || code==FILE_END_ || code==SWITCH_ROLE || code==REPEAT_LAST_ACK || code==SWITCH_BANDWIDTH))
	{
		if(code==SWITCH_BANDWIDTH)
		{
			printf("[BW-NEG] Received SWITCH_BANDWIDTH (target=%d) my_mode=%d\n",
				(int)(unsigned char)messages_control.data[1], bandwidth_mode);
			fflush(stdout);
			if(bandwidth_mode == BW_NB_ONLY)
			{
				// Reject: don't ACK — commander will timeout and stay NB.
				// MFSK ACK patterns carry no data, so we can't signal rejection
				// through ACK content. Silence = rejection.
				printf("[BW-NEG] Rejecting WB upgrade (nb_only mode), not ACKing\n");
				fflush(stdout);
				wb_upgrade_pending = false;
				messages_control.status = FREE;  // Discard so next message can be received
				batch_rx_frame_count = 0;
				connection_status = RECEIVING;
				link_timer.start();
				watchdog_timer.start();
			}
			else
			{
				// Accept: standard ACK → deferred WB switch after ACK sent
				wb_upgrade_pending = true;
				connection_status = ACKNOWLEDGING_CONTROL;
				link_timer.start();
				watchdog_timer.start();
			}
		}
		else if(code==SET_CONFIG)
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
		// BLOCK_END eliminated — data flush now happens after pattern ACK in
		// process_messages_acknowledging_data(). Commander never sends BLOCK_END.
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
			reset_session_state();
			link_status=DISCONNECTING;
			connection_status=ACKNOWLEDGING_CONTROL;
			reset_all_timers();

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
				// Pop raw data from RX FIFO
				char rx_raw[MAX_BUFFER_SIZE];
				int rx_raw_len = fifo_buffer_rx.pop(rx_raw, max_data_length+max_header_length-DATA_LONG_HEADER_LENGTH);

				// B2F filter: parse incoming stream, reroll plaintext to LZHUF
				if(b2f_handler.is_initialized())
				{
					char b2f_buf[MAX_BUFFER_SIZE * 4]; // LZHUF can be larger than plaintext (rare)
					int b2f_len = b2f_handler.filter_rx(rx_raw, rx_raw_len,
						b2f_buf, sizeof(b2f_buf));
					if(b2f_len > 0)
					{
						memcpy(tcp_socket_data.message->buffer, b2f_buf, b2f_len);
						tcp_socket_data.message->length = b2f_len;
					}
					else
					{
						memcpy(tcp_socket_data.message->buffer, rx_raw, rx_raw_len);
						tcp_socket_data.message->length = rx_raw_len;
					}
				}
				else
				{
					memcpy(tcp_socket_data.message->buffer, rx_raw, rx_raw_len);
					tcp_socket_data.message->length = rx_raw_len;
				}

				tcp_socket_data.transmit();
			}
		}

	}
}


