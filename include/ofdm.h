/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2022 Fadi Jerji
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
#include "defines.h"
#include "fir_filter.h"

#define DATA 0
#define PILOT 1
#define CONFIG 2
#define ZERO 3
#define COPY_FIRST_COL 4
#define AUTO_SELLECT -1

#define DBPSK 0

#define UNKNOWN 0
#define MEASURED 1
#define INTERPOLATED 2

#define INTERPOLATION 0
#define DECIMATION 1


struct carrier
{
	std::complex <double> value;
	int type;

};

struct channel
{
	std::complex <double> value;
	int status;

};

class cl_pilot_configurator
{

public:
	cl_pilot_configurator();
	~cl_pilot_configurator();
	void configure();
	void init(int Nfft, int Nc, int Nsymb, struct carrier* _carrier);
	void deinit();
	void print();

	int Dx,Dy;
	int first_col,second_col,last_col,first_row,last_row;
	int first_row_zeros;
	int nData,nPilots,nConfig,nZeros;
	int Nfft, Nc, Nsymb, Nc_max;
	int pilot_mod;
	std::complex <double> *pilot_sequence;
	double pilot_boost;
	struct carrier* carrier;

private:
	struct carrier* virtual_carrier;


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

	std::complex <double> interpolate_linear(std::complex <double> a,double a_x,std::complex <double> b,double b_x,double x);
	std::complex <double> interpolate_bilinear(std::complex <double> a,double a_x,double a_y,std::complex <double> b,double b_x,double b_y,std::complex <double> c,double c_x,double c_y,std::complex <double> d,double d_x,double d_y,double x,double y);

	void interpolate_linear_col(int col);
	void interpolate_bilinear_matrix(int col1,int col2, int row1, int row2);

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
	double frequency_sync(std::complex <double>*in, double carrier_freq_width);
	void channel_equalizer(std::complex <double>* in, std::complex <double>* out);
	double measure_variance(std::complex <double>*in);
	double measure_signal_stregth(std::complex <double> *in, int nItems);
	double measure_SNR(std::complex <double>*in_s, std::complex <double>*in_n, int nItems);
	int time_sync(std::complex <double>*, int size, int interpolation_rate, int location_to_return);
	int symbol_sync(std::complex <double>*, int size, int interpolation_rate, int location_to_return);
	void rational_resampler(std::complex <double>* in, int in_size , std::complex <double>* out, int rate, int interpolation_decimation);
	void baseband_to_passband(std::complex <double>* in, int in_size, double* out, double sampling_frequency, double carrier_frequency, double carrier_amplitude, int interpolation_rate);
	void passband_to_baseband(double* in, int in_size, std::complex <double>* out, double sampling_frequency, double carrier_frequency, double carrier_amplitude, int decimation_rate);
	struct channel * estimated_channel;
	int Nfft,Nc,Nsymb;
	float gi;
	struct carrier* ofdm_frame;
	cl_pilot_configurator pilot_configurator;
	void fft(std::complex <double>* in, std::complex <double>* out, int _Nfft);
	int time_sync_Nsymb;
	double freq_offset_ignore_limit;
	int print_time_sync_status;
	cl_FIR FIR;
};



#endif
