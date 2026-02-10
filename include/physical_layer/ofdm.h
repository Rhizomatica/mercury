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

#ifndef INC_OFDM_H_
#define INC_OFDM_H_

#include <complex>
#include <math.h>
#include <iostream>
#include "misc.h"
#include "physical_defines.h"
#include "fir_filter.h"
#include "plot.h"
#include "psk.h"
#include "interpolator.h"

// Result structure for time sync that includes correlation quality
struct TimeSyncResult {
	int delay;           // Sample delay to preamble start
	double correlation;  // Normalized correlation (0.0 to 1.0)
};


class cl_pilot_configurator
{

public:
	cl_pilot_configurator();
	~cl_pilot_configurator();
	void configure();
	void init(int Nfft, int Nc, int Nsymb, struct st_carrier* _carrier, int start_shift);
	void deinit();
	void print();

	int Dx,Dy;
	int first_col,second_col,last_col,first_row,last_row;
	int nData,nPilots,nConfig;
	int Nfft, Nc, Nsymb, Nc_max;
	int modulation;
	int seed;
	std::complex <double> *sequence;
	double boost;
	struct st_carrier* carrier;
	int print_on;
	int pilot_density;

private:
	struct st_carrier* virtual_carrier;
	int start_shift;


};

class cl_preamble_configurator
{

public:
	cl_preamble_configurator();
	~cl_preamble_configurator();
	void configure();
	void init(int Nfft, int Nc, struct st_carrier* _carrier, int start_shift);
	void deinit();
	void print();

	int nZeros,nPreamble;
	int Nfft, Nc, Nsymb, nIdentical_sections;
	int modulation;
	int seed;
	std::complex <double> *sequence;
	double boost;
	struct st_carrier* carrier;
	int print_on;

private:
	int start_shift;


};

class cl_ofdm
{
private:

	int Ngi;
	void zero_padder(std::complex <double>* in, std::complex <double>* out);
	void zero_depadder(std::complex <double>* in, std::complex <double>* out);
	void gi_adder(std::complex <double>* in, std::complex <double>* out);
	void gi_remover(std::complex <double>* in, std::complex <double>* out);
	void _fft(std::complex <double> *v, int n);
	void _ifft(std::complex <double> *v, int n);
	void _fft_fast(std::complex <double> *v, int n);
	void _ifft_fast(std::complex <double> *v, int n);
	void fft(std::complex <double>* in, std::complex <double>* out);
	void ifft(std::complex <double>* in, std::complex <double>* out);
	void ifft(std::complex <double>* in, std::complex <double>* out, int _Nfft);

	// Optimized FFT: precomputed twiddle factors
	std::complex<double>* fft_twiddle;     // Twiddle factors for FFT
	std::complex<double>* fft_scratch;     // Scratch buffer for in-place FFT
	int* fft_bit_rev;                      // Bit-reversal permutation table
	int fft_twiddle_size;                  // Size of twiddle table
	void init_fft_tables(int n);
	void deinit_fft_tables();

	std::complex <double> *zero_padded_data,*iffted_data;
	std::complex <double> *gi_removed_data,*ffted_data;

public:
	cl_ofdm();
	~cl_ofdm();
	void init();
	void init(int Nfft, int Nc, int Nsymb, float gi);
	void deinit();
	void symbol_mod(std::complex <double>*in, std::complex <double>*out);
	void symbol_demod(std::complex <double>*in, std::complex <double>*out);
	void framer(std::complex <double>* in, std::complex <double>* out);
	void deframer(std::complex <double>* in, std::complex <double>* out);
	void ZF_channel_estimator(std::complex <double>*in);
	void LS_channel_estimator(std::complex <double>*in);
	void restore_channel_amplitude();
	double carrier_sampling_frequency_sync(std::complex <double>*in, double carrier_freq_width, int preamble_nSymb, double sampling_frequency);
	double frequency_sync_coarse(std::complex<double>* in, double subcarrier_spacing, int search_range_subcarriers = 0, int interpolation_rate = 1);
	void channel_equalizer(std::complex <double>* in, std::complex <double>* out);
	void channel_equalizer_without_amplitude_restoration(std::complex <double>* in,std::complex <double>* out);

	void automatic_gain_control(std::complex <double>*in);
	double measure_variance(std::complex <double>*in);
	double measure_signal_stregth(std::complex <double> *in, int nItems);
	st_power_measurment measure_signal_power_avg_papr(double *in, int nItems);
	void peak_clip(double *in, int nItems, double papr);
	void peak_clip(std::complex <double> *in, int nItems, double papr);
	double measure_SNR(std::complex <double>*in_s, std::complex <double>*in_n, int nItems);
	int time_sync(std::complex <double>*in, int size, int interpolation_rate, int location_to_return);
	int time_sync_preamble(std::complex <double>*in, int size, int interpolation_rate, int location_to_return, int step, int nTrials_max);
	TimeSyncResult time_sync_preamble_with_metric(std::complex <double>*in, int size, int interpolation_rate, int location_to_return, int step, int nTrials_max);
	int time_sync_mfsk(std::complex<double>* baseband_interp, int buffer_size_interp, int interpolation_rate, int preamble_nSymb, const int* preamble_tones, int mfsk_M, int nStreams, const int* stream_offsets, int search_start_symb = 0);
	double detect_ack_pattern(std::complex<double>* baseband_interp, int buffer_size_interp, int interpolation_rate, int ack_nsymb, const int* ack_tones, int ack_pattern_len, int tone_hop_step, int mfsk_M, int nStreams, const int* stream_offsets);
	int symbol_sync(std::complex <double>*, int size, int interpolation_rate, int location_to_return);
	void rational_resampler(std::complex <double>* in, int in_size , std::complex <double>* out, int rate, int interpolation_decimation);
	void baseband_to_passband(std::complex <double>* in, int in_size, double* out, double sampling_frequency, double carrier_frequency, double carrier_amplitude, int interpolation_rate);
	void passband_to_baseband(double* in, int in_size, std::complex <double>* out, double sampling_frequency, double carrier_frequency, double carrier_amplitude, int decimation_rate, cl_FIR* filter);
	struct st_channel_complex * estimated_channel, *estimated_channel_without_amplitude_restoration;
	int Nfft,Nc,Nsymb;
	float gi;
	struct st_carrier* ofdm_frame;
	struct st_carrier* ofdm_preamble;
	cl_pilot_configurator pilot_configurator;
	cl_preamble_configurator preamble_configurator;
	void fft(std::complex <double>* in, std::complex <double>* out, int _Nfft);
	int time_sync_Nsymb;
	double freq_offset_ignore_limit;
	cl_FIR FIR_rx_data,FIR_rx_time_sync;
	cl_FIR FIR_tx1, FIR_tx2;
	int start_shift;
	long unsigned passband_start_sample;

	double preamble_papr_cut;
	double data_papr_cut;

	int channel_estimator;
	int channel_estimator_amplitude_restoration;
	int LS_window_width;
	int LS_window_hight;

	// Pre-allocated buffers for passband_to_baseband (avoids new/delete per call)
	std::complex<double>* p2b_l_data;
	std::complex<double>* p2b_data_filtered;
	int p2b_buffer_size;
};



#endif
