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

#include "physical_layer/interleaver.h"


void interleaver(int* in, int* out, int nItems, int block_size)
{
	int nBlocks=nItems/block_size;

	for(int i=0;i<nBlocks;i++)
	{
		for(int j=0;j<block_size;j++)
		{
			out[j*nBlocks+i]=in[i*block_size+j];
		}
	}
	for(int i=nBlocks*block_size;i<nItems;i++)
	{
		out[i]=in[i];
	}
}

void interleaver(std::complex<double>* in, std::complex<double>* out, int nItems, int block_size)
{
	int nBlocks=nItems/block_size;

	for(int i=0;i<nBlocks;i++)
	{
		for(int j=0;j<block_size;j++)
		{
			out[j*nBlocks+i]=in[i*block_size+j];
		}
	}
	for(int i=nBlocks*block_size;i<nItems;i++)
	{
		out[i]=in[i];
	}
}

void deinterleaver(int* in, int* out, int nItems, int block_size)
{
	int nBlocks=nItems/block_size;

	for(int i=0;i<nBlocks;i++)
	{
		for(int j=0;j<block_size;j++)
		{
			out[i*block_size+j]=in[j*nBlocks+i];
		}
	}
	for(int i=nBlocks*block_size;i<nItems;i++)
	{
		out[i]=in[i];
	}
}

void deinterleaver(float* in, float* out, int nItems, int block_size)
{
	int nBlocks=nItems/block_size;

	for(int i=0;i<nBlocks;i++)
	{
		for(int j=0;j<block_size;j++)
		{
			out[i*block_size+j]=in[j*nBlocks+i];
		}
	}
	for(int i=nBlocks*block_size;i<nItems;i++)
	{
		out[i]=in[i];
	}
}

void deinterleaver(std::complex<double>* in, std::complex<double>* out, int nItems, int block_size)
{
	int nBlocks=nItems/block_size;

	for(int i=0;i<nBlocks;i++)
	{
		for(int j=0;j<block_size;j++)
		{
			out[i*block_size+j]=in[j*nBlocks+i];
		}
	}
	for(int i=nBlocks*block_size;i<nItems;i++)
	{
		out[i]=in[i];
	}
}

void bit_energy_dispersal(int* in, int* sequence, int* out, int nItems)
{
	for(int i=0;i<nItems;i++)
	{
		out[i]=in[i] ^ sequence[i];
	}
}

