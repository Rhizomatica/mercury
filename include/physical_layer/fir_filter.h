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

#ifndef INC_FIR_FILTER_H_
#define INC_FIR_FILTER_H_

#include <cmath>
#include <complex>
#include <iostream>

#define RECTANGULAR 0
#define HANNING 1
#define HAMMING 2
#define BLACKMAN 3

#define LPF 0
#define HPF 1
#define BPF 2

class cl_FIR
{
private:

	double* filter_coefficients;
	double filter_cut_frequency;


public:
	cl_FIR();
	~cl_FIR();

	void design();
	void apply(std::complex <double>* in, std::complex <double>* out, int nItems);
	void apply(double* in, double* out, int nItems);
	void deinit();

	int filter_window;
	double filter_transition_bandwidth;
	double lpf_filter_cut_frequency;
	double hpf_filter_cut_frequency;
	double sampling_frequency;
	int type;
	int filter_nTaps;
};




#endif
