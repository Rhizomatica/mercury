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

#ifndef INC_TELECOM_SYSTEM_H_
#define INC_TELECOM_SYSTEM_H_

#include "data_container.h"
#include "psk.h"
#include "mfsk.h"
#include "awgn.h"
#include "error_rate.h"
#include "plot.h"
#include "ofdm.h"
#include "ldpc.h"
#include "interleaver.h"
#include "physical_config.h"
#include "physical_defines.h"
#include "misc.h"
#include "common/ring_buffer_posix.h"
#include <iomanip>


#if defined(_WIN32)
#define msleep(a) Sleep(a)
#else
#define msleep(a) usleep(a * 1000)
#endif

// TX gain table: amplitude boost per signal type × NB/WB mode
// Indexed as tx_gain[signal_type][nb_mode][nb_fir]
// nb_mode: 0=WB modulation (Nc=50), 1=NB modulation (Nc=10)
// nb_fir:  0=WB FIR filter,         1=NB FIR filter
// Currently mod and FIR always match; cross-entries exist for future per-mode tuning.
enum tx_signal_type {
	TX_SIG_MFSK_1S = 0,  // MFSK data frame, 1 stream  (ROBUST_0)
	TX_SIG_MFSK_2S,       // MFSK data frame, 2 streams (ROBUST_1, ROBUST_2)
	TX_SIG_OFDM,          // OFDM data frame (CONFIG_0..CONFIG_16)
	TX_SIG_ACK,           // ACK pattern
	TX_SIG_BREAK,         // BREAK pattern
	TX_SIG_COUNT
};

struct st_reinit_subsystems{
	int microphone=YES;
	int speaker=YES;
	int telecom_system=YES;
	int data_container=YES;
	int ofdm_FIR_rx_data=YES;
	int ofdm_FIR_rx_time_sync=YES;
	int ofdm_FIR_tx1=YES;
	int ofdm_FIR_tx2=YES;
	int ofdm=YES;
	int ldpc=YES;
	int psk=YES;
	int pre_equalization_channel=YES;
};

struct st_receive_stats{
	int iterations_done;
	int delay;
	int delay_of_last_decoded_message;
	int time_peak_symb_location;
	int time_peak_subsymb_location;
	int sync_trials;
	double phase_error_avg;
	double freq_offset;
	double freq_offset_of_last_decoded_message;
	int message_decoded;
	double SNR;
	double signal_stregth_dbm;
	st_power_measurment power_measurment;
	int crc;
	int all_zeros;
	int mfsk_search_raw;  // MFSK anti-re-decode: base search position (symbol units, pre-nUnder adjustment)
	int ofdm_search_raw;  // OFDM anti-re-decode: base search position (symbol units, pre-nUnder adjustment)
	bool ofdm_batch_active;  // true when consecutive OFDM frames expected (narrow BATCH window)
	int frame_overflow_symbols;  // >0: MFSK frame extends beyond captured audio by this many symbols
	double coarse_metric;  // Schmidl-Cox correlation metric from coarse time_sync (diagnostic)
	double ofdm_drift_per_frame;  // IIR-filtered prediction error (interp samples) for BATCH verify
};


class cl_telecom_system
{
private:


public:
	cl_telecom_system();
	~cl_telecom_system();
	cl_data_container data_container;
	cl_psk psk;
	cl_mfsk mfsk;
	cl_mfsk ack_mfsk;  // Dedicated MFSK instance for ACK pattern (always initialized, all modes)
	cl_awgn awgn_channel;
	cl_error_rate error_rate;
	cl_ofdm ofdm;
	cl_error_rate passband_test_EsN0(float EsN0,int max_frame_no);
	cl_error_rate baseband_test_EsN0(float EsN0,int max_frame_no);
	cl_ldpc ldpc;
	double sampling_frequency;
	double carrier_frequency;
	double carrier_amplitude;
	int frequency_interpolation_rate;
	int time_sync_trials_max;
	int use_last_good_time_sync;
	int use_last_good_freq_offset;
	int mfsk_fixed_delay;  // >= 0: bypass time_sync with this delay (BER test); -1: use time_sync
	int ofdm_forced_delay; // >= 0: override time_sync result (BER test, keeps passband_to_baseband); -1: normal
	int test_puncture_nBits;  // > 0: zero out LLRs past this position (punctured LDPC BER test); 0: disabled

	// MFSK short control frames: punctured LDPC for ACK/control messages
	int ctrl_nBits;    // interleaved bits to transmit for ctrl frames (0 = no puncturing)
	int ctrl_nsymb;    // MFSK symbols for ctrl frames
	bool mfsk_ctrl_mode;  // true: TX/RX uses shorter ctrl frame parameters
	void set_mfsk_ctrl_mode(bool enable);
	int get_active_nsymb() const;  // ctrl_nsymb when mfsk_ctrl_mode, else Nsymb
	int get_active_nbits() const;  // ctrl_nBits when mfsk_ctrl_mode, else nBits

	// ACK pattern: short known-tone sequence for pattern-based ACK
	int ack_pattern_passband_samples;    // = ack_mfsk.ack_pattern_nsymb * Nofdm * freq_interp_rate
	double ack_pattern_detection_threshold;  // metric threshold for detection
	int generate_ack_pattern_passband(double* out);  // TX: returns samples written
	double detect_ack_pattern_from_passband(double* data, int size, int* out_matched = nullptr);  // RX: returns metric
	void ack_pattern_detection_test();  // SNR sweep + false alarm test

	// BREAK pattern: emergency "drop to ROBUST_0" signal (different tones from ACK)
	int generate_break_pattern_passband(double* out);  // TX: returns samples written
	double detect_break_pattern_from_passband(double* data, int size, int* out_matched = nullptr);  // RX: returns metric

	// HAIL pattern: "I am Mercury" beacon (different tones from ACK and BREAK)
	int generate_hail_pattern_passband(double* out);  // TX: returns samples written
	double detect_hail_pattern_from_passband(double* data, int size, int* out_matched = nullptr);  // RX: returns metric

	st_receive_stats receive_stats;

	int operation_mode;

	double output_power_Watt;

	void transmit_bit(int *data, double *out, int message_location);
	st_receive_stats receive_bit(double *data, int *out);

	void transmit_byte(int* data, int nBytes, double *out, int message_location);
	st_receive_stats receive_byte(double *data, int* out);

	// Lightweight signal measurement only (no decoding)
	double measure_signal_only(double *data);


	double M;
	double bandwidth;
	double LDPC_real_CR;
	double Tu;
	double Ts;
	double Tf;
	double rb;
	double rbc;
	double Shannon_limit;

	int bit_interleaver_block_size;
	int time_freq_interleaver_block_size;

	void calculate_parameters();

	void init();
	void deinit();
	cl_plot BER_plot, constellation_plot;

	void TX_RAND_process_main();
	void RX_RAND_process_main();
	void TX_TEST_process_main();
	void RX_TEST_process_main();
	void TX_SHM_process_main(cbuf_handle_t buffer);
	void RX_SHM_process_main(cbuf_handle_t buffer);
	void BER_PLOT_baseband_process_main();
	void BER_PLOT_passband_process_main();

	void load_configuration();
	void load_configuration(int configuration);
	int last_configuration;
	int current_configuration;
	void return_to_last_configuration();
	char get_configuration(double SNR);

	int get_frame_size_bytes();
	int get_frame_size_bits();

	struct st_channel_complex *pre_equalization_channel;
	void get_pre_equalization_channel();

	cl_configuration_telecom_system default_configurations_telecom_system;

	int outer_code;
	int outer_code_reserved_bits;

	int bit_energy_dispersal_seed;

	int narrowband_enabled;  // 0=wideband (Nc=50, BW=2344 Hz), 1=narrowband (Nc=10, BW=469 Hz)

	// TX gain table: per signal-type × NB/WB mode amplitude scalars
	double tx_gain[TX_SIG_COUNT][2][2];  // [signal_type][nb_mod][nb_fir]
	double get_tx_gain(tx_signal_type sig) const;
	void init_tx_gain_defaults();
	void print_tx_gain_table() const;

	st_reinit_subsystems reinit_subsystems;

};



#endif
