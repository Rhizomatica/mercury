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

#include "physical_layer/awgn.h"

cl_awgn::cl_awgn()
{
}

cl_awgn::~cl_awgn()
{
}

void cl_awgn::set_seed(long seed)
{
	if(seed>0)
	{
		srand(seed);
	}
}

void cl_awgn::apply(std::complex <double> *in,std::complex <double> *out,float ampl,int nItems)
{

	float ampl_val=(ampl/sqrtf(2.0f));
	for(int i=0;i<nItems;i++)
	{
		out[i]=in[i]+ std::complex <double> ( ampl_val * awgn_value_generator(), ampl_val * awgn_value_generator());
	}
}

void cl_awgn::apply_with_delay(std::complex <double> *in,std::complex <double> *out,float ampl,int nItems, int delay)
{

	float ampl_val=(ampl/sqrtf(2.0f));
	for(int i=0;i<delay;i++)
	{
		out[i]=in[rand()%nItems]+ std::complex <double> ( ampl_val * awgn_value_generator(), ampl_val * awgn_value_generator());
	}
	for(int i=0;i<nItems;i++)
	{
		out[i+delay]=in[i]+ std::complex <double> ( ampl_val * awgn_value_generator(), ampl_val * awgn_value_generator());
	}
}

void cl_awgn::apply_with_delay(double *in,double *out,float ampl,int nItems, int delay)
{

	float ampl_val=(ampl/sqrtf(2.0f));
	for(int i=0;i<delay;i++)
	{
		out[i]=in[rand()%nItems]+ (double)(ampl_val * awgn_value_generator());
	}
	for(int i=0;i<nItems;i++)
	{
		out[i+delay]=in[i]+ (double)(ampl_val * awgn_value_generator());
	}
}

double  cl_awgn::awgn_value_generator()
{
	double x1;
	double x2;
	double result;

	x1=rand() / ((double)RAND_MAX);
	x2=rand() / ((double)RAND_MAX);

	result=sqrt(- log(x1)) * (sqrt(2) * cos(2.0 * M_PI * x2));

	return result;
	// based on  Box-Muller method
	//https://ieeexplore.ieee.org/abstract/document/953647
}






