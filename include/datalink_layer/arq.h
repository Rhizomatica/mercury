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

#ifndef ARQ_H_
#define ARQ_H_

#include "timer.h"
#include <unistd.h>
#include "tcp_socket.h"
#include "fifo_buffer.h"
#include "physical_layer/telecom_system.h"
#include "datalink_config.h"
#include "datalink_defines.h"
#include "common/common_defines.h"
#include "audioio/audioio.h"
#include "compression/mercury_compress.h"
#include "datalink_layer/b2f_handler.h"
#include "crypto/mercury_crypto.h"
#include <iomanip>

union u_SNR {
  float f_SNR;
  char char4_SNR[4];
};

// Base-36 callsign packing: fits up to 6 chars (A-Z, 0-9) into 5 bytes.
// Used by START_CONNECTION to avoid callsign truncation on small frames.
// Format: [1-bit flags][3-bit length][6 chars x 6 bits] = 40 bits = 5 bytes.
// Bit 39: narrowband flag (0=wideband, 1=narrowband).
// Bits 38-36: length (0-6). Bits 35-0: 6 chars x 6 bits.
//
// SSID is NOT carried in the pack — it's sent separately in TEST_CONNECTION
// (data[6]) so that 6-char callsigns are not truncated.
#define CALLSIGN_PACK_SIZE  5
#define SSID_NONE           0xFF

// SSID helpers: parse, format, and get SSID from "CALLSIGN-SSID" strings.
// SSID mapping: 0-15 = numeric (AX.25), 16=L, 17=T, 18=R, 19=X (Winlink/VARA)

inline int callsign_get_ssid(const std::string& callsign)
{
	size_t hyp = callsign.rfind('-');
	if(hyp == std::string::npos || hyp == callsign.size() - 1 || hyp == 0)
		return SSID_NONE;
	std::string ssid_str = callsign.substr(hyp + 1);
	if(ssid_str.size() == 1)
	{
		char c = ssid_str[0];
		if(c >= '0' && c <= '9') return c - '0';
		if(c == 'L' || c == 'l') return 16;
		if(c == 'T' || c == 't') return 17;
		if(c == 'R' || c == 'r') return 18;
		if(c == 'X' || c == 'x') return 19;
		return SSID_NONE;
	}
	else if(ssid_str.size() == 2 && ssid_str[0] >= '0' && ssid_str[0] <= '1'
	        && ssid_str[1] >= '0' && ssid_str[1] <= '9')
	{
		return (ssid_str[0] - '0') * 10 + (ssid_str[1] - '0');
	}
	return SSID_NONE;
}

inline std::string callsign_strip_ssid(const std::string& callsign)
{
	int ssid = callsign_get_ssid(callsign);
	if(ssid == SSID_NONE) return callsign;
	size_t hyp = callsign.rfind('-');
	return callsign.substr(0, hyp);
}

inline std::string callsign_format_ssid(const std::string& base, int ssid)
{
	if(ssid == SSID_NONE || ssid < 0) return base;
	std::string result = base + "-";
	if(ssid <= 15)
	{
		if(ssid >= 10) { result += (char)('0' + ssid / 10); result += (char)('0' + ssid % 10); }
		else result += (char)('0' + ssid);
	}
	else if(ssid == 16) result += 'L';
	else if(ssid == 17) result += 'T';
	else if(ssid == 18) result += 'R';
	else if(ssid == 19) result += 'X';
	else { result += (char)('0' + ssid / 10); result += (char)('0' + ssid % 10); }
	return result;
}

inline void callsign_pack(const char* callsign, int len, char* out, int flags = 0)
{
	if(len > 6) len = 6;
	uint64_t packed = ((uint64_t)(len & 0x7)) << 36;
	if(flags & 0x01) packed |= ((uint64_t)1) << 39;  // narrowband flag
	for(int i = 0; i < 6; i++)
	{
		int val = 0;
		if(i < len)
		{
			char c = callsign[i];
			if(c >= 'A' && c <= 'Z') val = c - 'A';
			else if(c >= 'a' && c <= 'z') val = c - 'a';
			else if(c >= '0' && c <= '9') val = c - '0' + 26;
		}
		packed |= ((uint64_t)(val & 0x3F)) << (30 - i * 6);
	}
	out[0] = (char)((packed >> 32) & 0xFF);
	out[1] = (char)((packed >> 24) & 0xFF);
	out[2] = (char)((packed >> 16) & 0xFF);
	out[3] = (char)((packed >> 8) & 0xFF);
	out[4] = (char)(packed & 0xFF);
}

inline std::string callsign_unpack(const char* data, int* out_flags = nullptr)
{
	uint64_t packed = 0;
	packed |= ((uint64_t)(unsigned char)data[0]) << 32;
	packed |= ((uint64_t)(unsigned char)data[1]) << 24;
	packed |= ((uint64_t)(unsigned char)data[2]) << 16;
	packed |= ((uint64_t)(unsigned char)data[3]) << 8;
	packed |= ((uint64_t)(unsigned char)data[4]);
	int len = (int)((packed >> 36) & 0x7);  // 3 bits for length
	if(len > 6) len = 6;
	if(out_flags)
	{
		*out_flags = 0;
		if(packed & (((uint64_t)1) << 39)) *out_flags |= 0x01;  // narrowband
	}
	std::string result;
	for(int i = 0; i < len; i++)
	{
		int val = (int)((packed >> (30 - i * 6)) & 0x3F);
		if(val < 26) result += (char)('A' + val);
		else if(val < 36) result += (char)('0' + val - 26);
	}
	return result;
}


struct st_message
{
	int ack_timeout;
	int nResends;
	int length;
	char* data;
	char type;
	char id;
	char sequence_number;
	int status;
	cl_timer ack_timer;
};

struct st_stats
{
	  int nSent_data;
	  int nAcked_data;
	  int nReceived_data;
	  int nLost_data;
	  int nReSent_data;
	  int nAcks_sent_data;
	  int nNAcked_data;

	  int nSent_control;
	  int nAcked_control;
	  int nReceived_control;
	  int nLost_control;
	  int nReSent_control;
	  int nAcks_sent_control;
	  int nNAcked_control;

	  float success_rate_data;
};

struct st_measurements
{
	  double SNR_uplink;
	  double SNR_downlink;
	  double signal_stregth_dbm;
	  double frequency_offset;;
};


class cl_arq_controller
{

public:
	cl_arq_controller();
  ~cl_arq_controller();


  void set_nResends(int nResends);
  void set_ack_timeout_control(int ack_timeout_control);
  void set_ack_timeout_data(int ack_timeout_data);
  void set_receiving_timeout(int receiving_timeout);
  void set_link_timeout(int link_timeout);
  void set_nMessages(int nMessages);
  void set_max_buffer_length(int max_data_length, int max_message_length, int max_header_length);
  void set_ack_batch_size(int ack_batch_size);
  void set_data_batch_size(int data_batch_size);
  void set_control_batch_size(int control_batch_size);
  void set_role(int role);
  void calculate_receiving_timeout();
  void set_call_sign(std::string call_sign);

  int get_nOccupied_messages();
  int get_nFree_messages();
  int get_nTotal_messages();
  int get_nToSend_messages();
  int get_nPending_Ack_messages();
  int get_nReceived_messages();
  int get_nAcked_messages();

  void messages_control_backup();
  void messages_control_restore();

  int init(int tcp_base_port, int gear_shift_on, int initial_mode);

  uint8_t CRC8_calc(char* data_byte, int nItems);

	//! Updates timers values and check for timeouts.
	    /*!
	      \return None
	   */
  void update_status();
	//! removes any acked of failed messages.
	    /*!
	      \return None
	   */
  void cleanup();
  void finish_turbo_direction();

	//! registers the ack of a data message.
	    /*!
	      \param message_id is the id of the received message (its location in the buffer).
	      \return None
	   */
  void register_ack(int message_id);
  void pad_messages_batch_tx(int size);

  void process_main();

  void process_user_command(std::string command);
	//! Sends PPT on to the user.
	    /*!
	      \return None
	   */
  void ptt_on();
	//! Sends PPT off to the user.
	    /*!
	      \return None
	   */
  void ptt_off();

  void process_messages();


  void process_messages_commander();
  int add_message_control(char code);
  void process_messages_tx_control();
  int add_message_tx_data(char type, int length, char* data);
  void process_messages_tx_data();
	//! Sends a data or a control message to the other end (via ALSA driver).
	    /*!
	     * \param message the st_message structure to be sent.
	      \return None
	   */
  void send(st_message* message, int message_location);
  void send_batch();
  void send_ack_pattern();   // Level 3: TX short tone pattern instead of LDPC ACK
  bool receive_ack_pattern(); // Level 3: RX + detect ACK pattern, returns true if detected
  void send_break_pattern(); // Emergency BREAK: TX "drop to ROBUST_0" tone pattern
  void send_hail_pattern();    // TX "I am Mercury" beacon
  bool receive_hail_pattern(); // RX + detect HAIL beacon, returns true if detected
  void process_messages_rx_acks_control();
  void process_messages_rx_acks_data();
  void process_control_commander();
  void process_buffer_data_commander();
  void finalize_block_commander();


  void process_messages_responder();
	//! Adds the received data message to the buffer.
	    /*!
	     * \param type is the message type.
	     * \param id is message id.
	     * \param length is the message content length.
	     * \param data is the message content.
	     *  \return SUCESSFUL or ERROR
	   */
  int add_message_rx_data(char type, char id, int length, char* data);
	//! Prepares control ack message.
	    /*!
	      \return None
	   */
  void process_messages_rx_data_control();
	//! Prepares control ack message.
	    /*!
	      \return None
	   */
  void process_messages_acknowledging_control();
	//! Prepares data ack message.
	    /*!
	      \return None
	   */
  void process_messages_acknowledging_data();
  void process_control_responder();
  void process_buffer_data_responder();

  void copy_data_to_buffer();
  void restore_backup_buffer_data();
  void restore_tx_from_compressed();  // Decompress messages_tx back to raw in fifo_buffer_tx

	//! Receives a data or a control message from the other end (via ALSA driver).
	    /*!
	      \return None
	   */
  void receive();

	//! Prints debug information.
	    /*!
	      \return None
	   */
  void print_stats();

  void reset_all_timers();
  void reset_session_state();

  cl_configuration_arq default_configuration_ARQ;


  int message_transmission_time_ms;
  int ctrl_transmission_time_ms;
  int ack_pattern_time_ms;  // Level 3: ACK pattern TX duration (ms)
  int data_batch_size;
  int control_batch_size;
  int ack_batch_size;
  int batch_rx_frame_count;  // Total data frames decoded in current RX batch (including padding duplicates)
  int block_ready;
  int block_under_tx;
  int max_message_length;
  int max_data_length;
  int max_header_length;

  int connection_status;
  int link_status;
  int role;
  int original_role;
  char connection_id;
  char assigned_connection_id;


  cl_tcp_socket tcp_socket_control;
  cl_tcp_socket tcp_socket_data;


  cl_timer watchdog_timer;
  cl_timer link_timer;
  cl_timer receiving_timer;
  cl_timer print_stats_timer;
  cl_timer gear_shift_timer;
  cl_timer switch_role_timer;
  cl_timer switch_role_test_timer;
  cl_timer connection_attempt_timer;

  float print_stats_frequency_hz;

  int message_batch_counter_tx;

  char* message_TxRx_byte_buffer;
  struct st_message messages_rx_buffer;

  struct st_message messages_last_ack_bu;
  struct st_message messages_control_bu;
  struct st_message messages_control;
  struct st_message* messages_batch_tx;

  int ack_timeout_control;
  int ack_timeout_data;
  int link_timeout;
  int watchdog_timeout;
  int receiving_timeout;
  int switch_role_timeout;
  int switch_role_test_timeout;
  int gearshift_timeout;
  int connection_timeout;

  std::string destination_call_sign;

  cl_fifo_buffer fifo_buffer_tx;
  cl_fifo_buffer fifo_buffer_rx;
  cl_fifo_buffer fifo_buffer_backup;

  cl_telecom_system* telecom_system;

  char data_configuration;
  char init_configuration;
  char last_data_configuration;
  char current_configuration;
  char ack_configuration;
  char negotiated_configuration;
  char forward_configuration;   // Commander→Responder TX speed (asymmetric gearshift)
  char reverse_configuration;   // Responder→Commander TX speed (after SWITCH_ROLE)

  int gear_shift_on;
  int robust_enabled;
  int narrowband_enabled;  // 0=wideband (2344 Hz), 1=narrowband (469 Hz)
  int commander_configured_nb;  // commander's original NB setting (-1=unset, YES/NO)
  int nb_probe_max;             // max NB probe attempts before fallback (default 2)
  bool session_narrowband;      // negotiated NB for this session (NB always wins)
  int bandwidth_mode;           // BW_AUTO=0, BW_NB_ONLY=1
  uint8_t local_capability;    // CAP_WB_CAPABLE | CAP_COMPRESSION
  uint8_t peer_capability;     // Received from peer via TEST_CONNECTION
  bool wb_upgrade_pending;     // True between SWITCH_BANDWIDTH send and ACK
  cl_compressor compressor;           // Block compression (PPMd + zstd)
  bool compression_enabled;           // Negotiated: both sides have CAP_COMPRESSION
  bool force_compress;                // CLI -F on: always enable compression (skip B2F detection)
  bool b2f_compression_pending;       // B2F SID detected, arm compression on next data ACK
  cl_b2f_handler b2f_handler;         // B2F protocol handler (Winlink LZHUF unroll/reroll)
  float compress_ratio_estimate;      // Running compression ratio (raw/compressed), init 2.0
  int batch_uncompressed_size;        // Uncompressed bytes in current TX batch (for throughput)

  // Encryption (hybrid PQ: X25519 + ML-KEM-768 + ChaCha20-Poly1305)
  cl_cipher_suite cipher_suite;       // Per-connection cipher state (ephemeral keys, session key)
  int encryption_mode;                // ENCRYPT_OFF, ENCRYPT_STRICT, ENCRYPT_FAST
  bool encryption_enabled;            // Negotiated: both sides have CAP_ENCRYPTION and mode != OFF
  uint64_t tx_batch_counter;          // Monotonic counter for encrypt nonces (TX direction)
  uint64_t rx_batch_counter;          // Monotonic counter for decrypt nonces (RX direction)
  int consecutive_auth_failures;      // Auth failures since last success (3 → disconnect)
  uint8_t* kx_data_buf;              // Buffer for ML-KEM key exchange data (1184 or 1088 bytes)
  int kx_data_len;                    // Length of pending key exchange data
  char psk_hex[129];                  // Pre-shared key (hex string, up to 64 bytes = 128 hex chars)

  int gear_shift_algorithm;
  double gear_shift_up_success_rate_precentage;
  double gear_shift_down_success_rate_precentage;
  int gear_shift_block_for_nBlocks_total;
  int gear_shift_blocked_for_nBlocks;
  int consecutive_data_acks;       // Frame-level gearshift: consecutive successful data ACKs
  int frame_shift_threshold;       // Shift up after this many consecutive ACKs (default 3)
  bool frame_gearshift_just_applied;  // true after frame upshift ACKed — BREAK on first data failure

  // Turboshift: bidirectional probing phase before data exchange
  enum TurboshiftPhase { TURBO_FORWARD, TURBO_REVERSE, TURBO_DONE };
  TurboshiftPhase turboshift_phase;
  bool turboshift_active;          // true = currently probing (climbing the ladder)
  int turboshift_last_good;        // last config that decoded successfully (-1 = none)
  bool turboshift_initiator;       // true = I started turboshift (original commander)
  int turboshift_retries;          // retries left at current config (0 = ceiling)

  // Emergency BREAK: drop to ROBUST_0 when current config is undecodable
  int emergency_nack_count;       // consecutive failed data blocks
  int emergency_nack_threshold;   // trigger threshold (default 2)
  int emergency_break_active;     // 1 = BREAK sent, waiting for ACK
  int emergency_break_retries;    // retries left for current BREAK attempt
  int emergency_previous_config;  // config that was failing
  int break_drop_step;            // ladder steps to drop (1,2,4,4,4...)
  int break_recovery_phase;       // 0=off, 1=coord at ROBUST_0, 2=probing target
  int break_recovery_retries;     // probe attempts remaining (2 total)
  int break_detected;             // YES if BREAK pattern detected by responder
  int hail_detected;              // YES if HAIL beacon detected (responder LISTENING)
  int hail_sent;                  // YES if commander has sent HAIL in current CONNECTING phase

  int ptt_on_delay_ms;
  int ptt_off_delay_ms;
  int pilot_tone_ms;   // Duration of pilot tone before OFDM (0=disabled)
  int pilot_tone_hz;   // Frequency of pilot tone (250=out of band, 1500=in band)
  double time_left_to_send_last_frame;

  int disconnect_requested;

  int connection_attempts;
  int max_connection_attempts;

  int exit_on_disconnect;
  int had_control_connection;

  // GUI measurement getters
  double get_snr_uplink() const { return measurements.SNR_uplink; }
  double get_snr_downlink() const { return measurements.SNR_downlink; }

private:
  int nMessages;
  struct st_message* messages_tx;
  struct st_message* messages_rx;


  struct st_message* messages_batch_ack;


  std::string my_call_sign;
  std::string user_command_buffer;


  st_stats stats, last_transmission_block_stats;
  st_measurements measurements;

  int nResends;

  char get_configuration(double SNR);
  void load_configuration(int configuration, int level, int backup_configuration);
  void switch_narrowband_mode(int nb_enabled);
  void return_to_last_configuration();
  int init_messages_buffers();
  int deinit_messages_buffers();
  void check_buffer_canaries(const char* caller);

  char last_received_message_sequence;
  char last_message_sent_type;
  char last_message_sent_code;

  char last_message_received_type;
  char last_message_received_code;

  int data_ack_received;
  int repeating_last_ack;

};


#endif
