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
#ifndef INC_AWGN_H_
#define INC_AWGN_H_

#include <complex>
#include <stdlib.h>
#include <cmath>


class cl_awgn
{

public:
	cl_awgn();
	~cl_awgn();

	void set_seed(long seed);
	void apply(std::complex <double> *in,std::complex <double> *out,float ampl,int nItems);
	void apply_with_delay(std::complex <double> *in,std::complex <double> *out,float ampl,int nItems, int delay);
	void apply_with_delay(double *in,double *out,float ampl,int nItems, int delay);
	double  awgn_value_generator();

};


#endif
