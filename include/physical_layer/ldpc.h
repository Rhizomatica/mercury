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

#ifndef INC_LDPC_H_
#define INC_LDPC_H_

#include "ldpc_decoder_GBF.h"
#include "ldpc_decoder_SPA.h"
#include "mercury_ldpc.h"
#include "physical_defines.h"
#include <iostream>

class cl_ldpc
{
private:
	int Cwidth;
	int Vwidth;
	int dwidth;
	int *QCmatrixEnc;
	int *QCmatrixC;
	int *QCmatrixV;
	int *QCmatrixd;
	double* R;
	double* Q;
	int* V_pos;       // Pre-allocated SPA decoder workspace [P*Cwidth]



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
	int N,P,K; //!< N: the message size, P: the parity bit size, K: the information bit size (N=P+K).
	int standard;
	int framesize;
	float rate;
	int decoding_algorithm;
	float GBF_eta; //!< The GBF algorithms correction rate.
	int nIteration_max; //!< The maximum number of LDPC decoding iterations allowed.
	int print_nIteration;
	void init();
	void deinit();

	//! The LDPC encoding function, calculates and annex the parity bits to the original data.
	    /*!
	      \param data is the data to be protected by the LDPC code.
	      \param encoded_data is the concatenation of the original data with the LDPC parity bits.
	      \return None
	   */
	void encode(const int* data, int*  encoded_data);

	//! The LDPC decoding function, validates the message integrity and attempts to correct bit errors.
	    /*!
	      \param data is the received message.
	      \param encoded_data is the corrected data without the LDPC parity bits.
	      \return number of iterations used to decode the message, the message maybe corrupt if this reaches the max number of iterations allowed.
	   */
	int decode(const float* data,  int*  decoded_data);


};


#endif
