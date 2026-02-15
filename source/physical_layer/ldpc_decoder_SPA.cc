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

#include "physical_layer/ldpc_decoder_SPA.h"

int decode_SPA(
		const float LLRi[],
		int LLRo[],
		int* C,
		int CWidth,
		int CWidthMax,
		int* V,
		int VWidth,
		int VWidthMax,
		int d[],
		int dWidth,
		double* R,
		double* Q,
		int* V_pos,
		int N,
		int K,
		int P,
		int nIteration_max
)
{
	int Cout[N_MAX];
	int LLRbin[N_MAX];
	int iteration=0;
	int i,j,nOnes;
	double LLRtmp[N_MAX];

	for( i=0;i<N;i++)
	{
		for(j=0;j<VWidth;j++)
		{
			*(R+i*VWidthMax+j)=0;
			*(Q+i*VWidthMax+j)=0;
		}
		LLRbin[i]= (LLRi[i]<0);
		LLRtmp[i]=LLRi[i];
	}

	nOnes=0;
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
	}
	if(nOnes!=0)
	{
		// Precompute V matrix positions to eliminate linear search in inner loop
		// V_pos[check * CWidthMax + col] = position of check in V[C[check][col]][]
		// V_pos buffer is pre-allocated by cl_ldpc::load() and passed in
		for(int ci=0;ci<P;ci++)
		{
			for(int cj=0;cj<CWidth;cj++)
			{
				int v=*(C+ci*CWidthMax+cj);
				if(v!=-1)
				{
					int pos=-1;
					for(int vk=0;vk<VWidth;vk++)
					{
						if(*(V+v*VWidthMax+vk)==ci)
						{
							pos=vk;
							break;
						}
					}
					V_pos[ci*CWidthMax+cj]=pos;
				}
				else
				{
					V_pos[ci*CWidthMax+cj]=-1;
				}
			}
		}

		int start=0;
		int end=0;
		int width=0;
		for (int section=0;section<dWidth;section+=2)
		{
			end+=d[section];
			width=d[section+1];

			for( i=start;i<end;i++)
			{
				for( j=0;j<width;j++)
				{
					*(Q+i*VWidthMax+j)=LLRi[i];
				}
			}
			start+=d[section];
		}

		int iindex,Cindex,i1index,i1;
		double temp;

		for(iteration=1;iteration<=nIteration_max;iteration++)
		{
			for ( iindex=0;iindex<P;iindex++)
			{
				for ( Cindex=0;Cindex<CWidth;Cindex++)
				{
					j=*(C+iindex*CWidthMax+Cindex);
					if(j!=-1)
					{
						temp=1;
						for ( i1index=0;i1index<CWidth;i1index++)
						{
							i1=*(C+iindex*CWidthMax+i1index);
							if(i1!=j && i1!=-1)
							{
								i=V_pos[iindex*CWidthMax+i1index];
								temp*=tanh(0.5* (double)*(Q+i1*VWidthMax+i));
							}

						}
						if(temp==1) // to avoid the limitation of the float/double
						{
							temp=0.9999999;
						}
						if(temp==-1)
						{
							temp=-0.9999999;
						}
						*(R+j*VWidthMax+V_pos[iindex*CWidthMax+Cindex])=2*atanh(temp);

					}

				}
			}

			for( i=0;i<N;i++)
			{
				LLRtmp[i]=LLRi[i];
				for ( j=0;j<VWidth;j++)
				{
					LLRtmp[i]+=*(R+i*VWidthMax+j);
				}
				LLRbin[i]= (LLRtmp[i]<0);
			}


			nOnes=0;
			for( i=0;i<P;i++)
			{
				Cout[i]=LLRbin[*(C+i*CWidthMax+0)];
				for( j=1;j<CWidth;j++)
				{
					if(*(C+i*CWidthMax+j)!=-1)
					{
						Cout[i]^=LLRbin[*(C+i*CWidthMax+j)];
					}
				}
				nOnes+=Cout[i];
			}

			if(nOnes==0)
			{
				break;
			}


			int start=0;
			int end=0;
			int width=0;
			for (int section=0;section<dWidth;section+=2)
			{
				end+=d[section];
				width=d[section+1];

				for( i=start;i<end;i++)
				{
					for( j=0;j<width;j++)
					{
						*(Q+i*VWidthMax+j)=LLRtmp[i]-*(R+i*VWidthMax+j);
					}
				}
				start+=d[section];
			}
		}
	}
	for( i=0;i<K;i++)
	{
		LLRo[i]=(LLRtmp[i]<0);
	}
	return iteration;

}

/* F. R. Kschischang, B. J. Frey, and H. . Loeliger, "Factor graphs and the sum-product algorithm," IEEE Transactions on Information Theory, vol. 47, no. 2, pp. 498–519, Feb 2001.
 * https://ieeexplore.ieee.org/document/910572
 */
