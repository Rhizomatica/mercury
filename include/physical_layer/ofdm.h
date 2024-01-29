/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022-2024 Fadi Jerji
 * Author: Fadi Jerji
 * Email: fadi.jerji@  <gmail.com, rhizomatica.org, caisresearch.com, ieee.org>
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
	void fft(std::complex <double>* in, std::complex <double>* out);
	void ifft(std::complex <double>* in, std::complex <double>* out);
	void ifft(std::complex <double>* in, std::complex <double>* out, int _Nfft);

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
	void channel_estimator_frame(std::complex <double>*in);
	void channel_estimator_frame_time_frequency(std::complex <double>*in);
	double frequency_sync(std::complex <double>*in, double carrier_freq_width, int preamble_nSymb);
	void channel_equalizer(std::complex <double>* in, std::complex <double>* out);
	void automatic_gain_control(std::complex <double>*in);
	double measure_variance(std::complex <double>*in);
	double measure_signal_stregth(std::complex <double> *in, int nItems);
	void peak_clip(double *in, int nItems, double papr);
	void peak_clip(std::complex <double> *in, int nItems, double papr);
	double measure_SNR(std::complex <double>*in_s, std::complex <double>*in_n, int nItems);
	int time_sync(std::complex <double>*in, int size, int interpolation_rate, int location_to_return);
	int time_sync_preamble(std::complex <double>*in, int size, int interpolation_rate, int location_to_return, int step);
	int symbol_sync(std::complex <double>*, int size, int interpolation_rate, int location_to_return);
	void rational_resampler(std::complex <double>* in, int in_size , std::complex <double>* out, int rate, int interpolation_decimation);
	void baseband_to_passband(std::complex <double>* in, int in_size, double* out, double sampling_frequency, double carrier_frequency, double carrier_amplitude, int interpolation_rate);
	void passband_to_baseband(double* in, int in_size, std::complex <double>* out, double sampling_frequency, double carrier_frequency, double carrier_amplitude, int decimation_rate);
	struct st_channel_complex * estimated_channel;
	int Nfft,Nc,Nsymb;
	float gi;
	struct st_carrier* ofdm_frame;
	struct st_carrier* ofdm_preamble;
	cl_pilot_configurator pilot_configurator;
	cl_preamble_configurator preamble_configurator;
	void fft(std::complex <double>* in, std::complex <double>* out, int _Nfft);
	int time_sync_Nsymb;
	double freq_offset_ignore_limit;
	cl_FIR FIR_rx;
	cl_FIR FIR_tx1, FIR_tx2;
	int start_shift;
	long unsigned passband_start_sample;

	double preamble_papr_cut;
	double data_papr_cut;

	int channel_estimator;
};



#endif
