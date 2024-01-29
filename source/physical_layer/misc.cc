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

#include "physical_layer/misc.h"


void shift_left(double* matrix, int size, int nShift)
{
	for(int j=0;j<size-nShift-1;j++)
	{
		matrix[j]=matrix[j+nShift];
	}
}

double get_angle(std::complex <double> value)
{
	double theta=0;

	if(value.real() >=0)
	{
		theta=atan(value.imag()/value.real());
	}
	else if (value.real() <0 && value.imag() >=0)
	{
		theta=atan(value.imag()/value.real()) +  M_PI;
	}
	else if (value.real() <0 && value.imag() <0)
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

