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

#include "physical_layer/error_rate.h"


cl_error_rate::cl_error_rate()
{
	BER=0;
	FER=0;
	Bits_total=0;
	Error_bits_total=0;
	Frames_total=0;
	Error_frames_total=0;
}

cl_error_rate::~cl_error_rate()
{
}

void cl_error_rate::reset()
{
	BER=0;
	FER=0;
	Bits_total=0;
	Error_bits_total=0;
	Frames_total=0;
	Error_frames_total=0;
}

void cl_error_rate::check(const int *in1,const int *in2,int nItems)
{
	int frame_error=0;
	for(int i=0;i<nItems;i++)
	{
		if(*(in1+i)!=*(in2+i))
		{
			Error_bits_total++;
			frame_error=1;
		}
		Bits_total++;
	}
	Frames_total++;
	if(frame_error==1)
	{
		Error_frames_total++;
	}
	BER=Error_bits_total/Bits_total;
	FER=Error_frames_total/Frames_total;
}


