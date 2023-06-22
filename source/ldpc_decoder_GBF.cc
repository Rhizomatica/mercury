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
#include "ldpc_decoder_GBF.h"

int decode_GBF(
		const float LLRi[],
		int LLRo[],
		int* C,
		int CWidth,
		int CWidthMax,
		int N,
		int K,
		int P,
		int nIteration_max,
		float eta
)
{

	int Cout[N_MAX];
	int LLRbin[N_MAX];
	float LLRtmp[N_MAX];
	int delta[N_MAX]={0};
	int iteration=0;
	int i,j,nOnes;


	for( i=0;i<N;i++)
	{
		LLRtmp[i]=LLRi[i];
	}

	for(iteration=0;iteration<nIteration_max;iteration++)
	{
		nOnes=0;
		for( i=0;i<N;i++)
		{
			LLRbin[i]= (LLRtmp[i]<0);
		}
		for (i=0;i<P;i++)
		{
			Cout[i]=LLRbin[*(C+i*CWidthMax+0)];
			for( j=1;j<CWidth;j++)
			{
				if(*(C+i*CWidthMax+j)!=-1)
				{
					Cout[i]^= LLRbin[*(C+i*CWidthMax+j)];
				}
			}

			nOnes+=Cout[i];

			for( j=0;j<CWidth;j++)
			{
				if(*(C+i*CWidthMax+j)!=-1)
				{
					delta[*(C+i*CWidthMax+j)]+=2*Cout[i]-1;
				}
			}
		}
		if(nOnes==0)
		{
			break;
		}
		for(int i=0;i<N;i++)
		{
			LLRtmp[i]+= (delta[i]>0)*(2*(LLRtmp[i]<0)-1)*delta[i]* eta;
			delta[i]=0;
		}
	}
	for( i=0;i<K;i++)
	{
		LLRo[i]=(LLRtmp[i]<0);
	}
	return iteration;
}

/* F. Jerji and C. Akamine, "Gradient Bit-Flipping LDPC Decoder for ATSC 3.0," 2019 IEEE International Symposium on Broadband Multimedia Systems and Broadcasting (BMSB), 2019, pp. 1-4, doi: 10.1109/BMSB47279.2019.8971839.
 * https://ieeexplore.ieee.org/document/8971839*/


