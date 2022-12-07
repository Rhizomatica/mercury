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
#ifndef INC_LDPC_H_
#define INC_LDPC_H_

#include "ldpc_decoder_GBF.h"
#include "hermes_ldpc.h"
#include "defines.h"
#include <iostream>

class cl_ldpc
{
private:
	int Cwidth;
	int *QCmatrixEnc;
	int *QCmatrixC;



	float r;

	int standard_val;
	int decoding_algorithm_val;


	float eta_val;
	int nIteration_max_val;
	int print_nIteration_val;



	int update_code_parameters();

public:
	cl_ldpc();
	~cl_ldpc();
	int N,P,K;
	int standard;
	int framesize;
	float rate;
	int decoding_algorithm;
	float GBF_eta;
	int nIteration_max;
	int print_nIteration;
	void init();
	void encode(const int* data, int*  encoded_data);
	int decode(const float* data,  int*  decoded_data);


};


#endif
