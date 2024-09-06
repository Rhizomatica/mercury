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

#include "physical_layer/misc.h"


void shift_left(double* matrix, int size, int nShift)
{
	// TODO: double check this
	for(int j=0;j<size-nShift-1;j++)
	{
		matrix[j]=matrix[j+nShift];
	}
}

double get_angle(std::complex <double> value)
{
	double theta=0;

	if(value.real() == 0) // check for divide by zero
	{
		theta = M_PI / 2;
	}
	else if(value.real() > 0)
	{
		theta=atan(value.imag()/value.real());
	}
	else if (value.real() < 0 && value.imag() >= 0)
	{
		theta=atan(value.imag()/value.real()) +  M_PI;
	}
	else if (value.real() < 0 && value.imag() < 0)
	{
		theta=atan(value.imag()/value.real()) -  M_PI;
	}

	return theta;
}

double get_amplitude(std::complex <double> value)
{
	double amplitude;
	amplitude=sqrt(pow(value.real(),2)+pow(value.imag(),2));
	return amplitude;
}

std::complex <double> set_complex(double amplitude, double theta)
{
	std::complex <double> value;
	value.real(amplitude*cos(theta));
	value.imag(amplitude*sin(theta));
	return value;
}

void matrix_multiplication(std::complex <double>* a, int a_width, int a_hight, std::complex <double>* b, int b_width, int b_hight, std::complex <double>* c)
{
	if(a_width!=b_hight)
	{
		return;
	}

	for(int i=0;i<a_hight;i++)
	{
		for(int j=0;j<b_width;j++)
		{
			*(c+i*b_width+j)=0;
			for(int k=0;k<a_width;k++)
			{
				*(c+i*b_width+j)+=*(a+i*a_width+k) * *(b+k*b_width+j);
			}
		}
	}
}

void byte_to_bit(int* data_byte, int* data_bit, int nBytes)
{
	int mask;
	for(int i=0;i<nBytes;i++)
	{
		mask=0x01;
		for(int j=0;j<8;j++)
		{
			data_bit[i*8+j]=((data_byte[i]&mask)==mask);
			mask=mask<<1;
		}
	}
}

void bit_to_byte(int* data_bit, int* data_byte, int nBits)
{
	int mask;
	for(int i=0;i<nBits/8;i++)
	{
		data_byte[i]=0x00;
		mask=0x01;
		for(int j=0;j<8;j++)
		{
			data_byte[i]|=data_bit[i*8+j]*mask;
			mask=mask<<1;
		}
	}
	if(nBits%8!=0)
	{
		data_byte[nBits/8]=0x00;
		mask=0x01;
		for(int j=0;j<nBits%8;j++)
		{
			data_byte[nBits/8]|=data_bit[nBits-(nBits%8)+j]*mask;
			mask=mask<<1;
		}
	}
}
