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

#include "physical_layer/ldpc_decoder_SPA.h"

#define	find(matrix,matrix_size,value) \
		{ \
	for(loc_found=0;loc_found<matrix_size;loc_found++) \
	{ \
		if(*(matrix+loc_found)==value) \
		{break;} \
	} \
	if(loc_found==matrix_size) \
		{loc_found=-1;} \
		}

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
		int N,
		int K,
		int P,
		int nIteration_max
)
{
	int Cout[N_MAX];
	int LLRbin[N_MAX];
	int iteration=0;
	int i,j,k,nOnes;
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
								int loc_found;
								find(V+i1*VWidthMax,VWidth,iindex)
								i=loc_found;
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
						int loc_found;
						find(V+j*VWidthMax,VWidth,iindex)
						*(R+j*VWidthMax+loc_found)=2*atanh(temp);

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
						*(Q+i*VWidthMax+j)=LLRi[i]-*(R+i*VWidthMax+j);
						for( k=0;k<VWidth;k++)
						{
							*(Q+i*VWidthMax+j)+=*(R+i*VWidthMax+k);
						}
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

/* F. R. Kschischang, B. J. Frey, and H. . Loeliger, “Factor graphs and the sum-product algorithm,” IEEE Transactions on Information Theory, vol. 47, no. 2, pp. 498–519, Feb 2001.
 * https://ieeexplore.ieee.org/document/910572
 */
