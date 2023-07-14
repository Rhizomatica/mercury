/*
 * Mercury: A configurable open-source software-defined modem.
 * Copyright (C) 2023 Fadi Jerji
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
#include "fir_filter.h"

cl_FIR::cl_FIR()
{
	filter_window=0;
	filter_nTaps=0;
	filter_transition_bandwidth=0;
	filter_cut_frequency=0;
	sampling_frequency=0;

	filter_coefficients=NULL;
}

cl_FIR::~cl_FIR()
{
	deinit();
}


void cl_FIR::design()
{
	filter_nTaps=(int)(4.0/(filter_transition_bandwidth/(sampling_frequency/2.0)));

	if(filter_nTaps%2==0)
	{
		filter_nTaps++;
	}
	filter_coefficients = new double[filter_nTaps];
	double sampling_interval=1.0/(sampling_frequency);
	double temp;

	filter_coefficients[filter_nTaps/2]=1;
	for(int i=0;i<filter_nTaps/2;i++)
	{
		temp=2*M_PI*filter_cut_frequency*(double)(filter_nTaps/2-i) *sampling_interval;

		filter_coefficients[i]=sin(temp)/temp;
		filter_coefficients[filter_nTaps-i-1]=filter_coefficients[i];
	}

	if(filter_window==HAMMING)
	{
		for(int i=0;i<filter_nTaps;i++)
		{
			filter_coefficients[i]*=0.54-0.46*cos(2.0*M_PI*(double)i/(filter_nTaps-1));
		}
	}
	else if(filter_window==HANNING)
	{
		for(int i=0;i<filter_nTaps;i++)
		{
			filter_coefficients[i]*=0.5-0.5*cos(2.0*M_PI*(double)i/(filter_nTaps-1));
		}
	}
	else if(filter_window==BLACKMAN)
	{
		for(int i=0;i<filter_nTaps;i++)
		{
			filter_coefficients[i]*=0.42-0.5*cos(2.0*M_PI*(double)i/filter_nTaps)+0.08*cos(4.0*M_PI*(double)i/filter_nTaps);
		}
	}
}

void cl_FIR::apply(std::complex <double>* in, std::complex <double>* out, int nItems)
{
	double acc_r,acc_im;
	for(int i=0;i<(nItems+filter_nTaps-1);i++)
	{
		acc_r=0;
		acc_im=0;
		for(int j=0;j<filter_nTaps;j++)
		{
			if((i-j)>=0 && (i-j)<nItems)
			{
				acc_r+=in[i-j].real()*filter_coefficients[j];
				acc_im+=in[i-j].imag()*filter_coefficients[j];
			}
		}

		if(i>=((int)(filter_nTaps-1)/2) && i<(nItems+(int)(filter_nTaps-1)/2))
		{
			out[i-(int)(filter_nTaps-1)/2].real(acc_r);
			out[i-(int)(filter_nTaps-1)/2].imag(acc_im);
		}
	}
}

void cl_FIR::deinit()
{
	filter_window=0;
	filter_nTaps=0;
	filter_transition_bandwidth=0;
	filter_cut_frequency=0;
	sampling_frequency=0;

	if(filter_coefficients!=NULL)
	{
		delete[] filter_coefficients;
		filter_coefficients=NULL;
	}
}

