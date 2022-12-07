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
#include <psk.h>



cl_psk::cl_psk()

{
	constellation=0;
	nBits=0;
	nSymbols=0;
}

cl_psk::~cl_psk()
{
	if(constellation!=NULL)
	{
		delete constellation;
	}
}

void cl_psk::set_predefined_constellation(int M)
{
	std::complex <double>* constellation =new std::complex <double>[M];
	if(M==MOD_BPSK)
	{
		constellation[0]=std::complex <double> ( 1 , 0 );
		constellation[1]=std::complex <double> ( -1 , 0 );
	}
	else if(M==MOD_QPSK)
	{
		constellation[0]=std::complex <double> ( -1,1);
		constellation[1]=std::complex <double> ( -1,-1);
		constellation[2]=std::complex <double> ( 1,1);
		constellation[3]=std::complex <double> ( 1,-1);

	}
	else if(M==MOD_8QAM)
	{
		constellation[0]=std::complex <double> ( -3,1);
		constellation[1]=std::complex <double> ( -3,-1);
		constellation[2]=std::complex <double> ( -1,1);
		constellation[3]=std::complex <double> ( -1,-1);
		constellation[4]=std::complex <double> ( 3,1);
		constellation[5]=std::complex <double> ( 3,-1);
		constellation[6]=std::complex <double> ( 1,1);
		constellation[7]=std::complex <double> ( 1,-1);
	}
	else if(M==MOD_16QAM)
	{
		constellation[0]=std::complex <double> ( -3,3);
		constellation[1]=std::complex <double> ( -3,1);
		constellation[2]=std::complex <double> ( -3,-3);
		constellation[3]=std::complex <double> ( -3,-1);
		constellation[4]=std::complex <double> ( -1,3);
		constellation[5]=std::complex <double> ( -1,1);
		constellation[6]=std::complex <double> ( -1,-3);
		constellation[7]=std::complex <double> ( -1,-1);
		constellation[8]=std::complex <double> ( 3,3);
		constellation[9]=std::complex <double> ( 3,1);
		constellation[10]=std::complex <double> ( 3,-3);
		constellation[11]=std::complex <double> ( 3,-1);
		constellation[12]=std::complex <double> ( 1,3);
		constellation[13]=std::complex <double> ( 1,1);
		constellation[14]=std::complex <double> ( 1,-3);
		constellation[15]=std::complex <double> ( 1,-1);
	}
	else if(M==MOD_32QAM)
	{
		constellation[0]=std::complex <double> ( -3,5);
		constellation[1]=std::complex <double> ( -1,5);
		constellation[2]=std::complex <double> ( -3,-5);
		constellation[3]=std::complex <double> ( -1,-5);
		constellation[4]=std::complex <double> ( -5,3);
		constellation[5]=std::complex <double> ( -5,1);
		constellation[6]=std::complex <double> ( -5,-3);
		constellation[7]=std::complex <double> ( -5,-1);
		constellation[8]=std::complex <double> ( -1,3);
		constellation[9]=std::complex <double> ( -1,1);
		constellation[10]=std::complex <double> ( -1,-3);
		constellation[11]=std::complex <double> ( -1,-1);
		constellation[12]=std::complex <double> ( -3,3);
		constellation[13]=std::complex <double> ( -3,1);
		constellation[14]=std::complex <double> ( -3,-3);
		constellation[15]=std::complex <double> ( -3,-1);
		constellation[16]=std::complex <double> ( 3,5);
		constellation[17]=std::complex <double> ( 1,5);
		constellation[18]=std::complex <double> ( 3,-5);
		constellation[19]=std::complex <double> ( 1,-5);
		constellation[20]=std::complex <double> ( 5,3);
		constellation[21]=std::complex <double> ( 5,1);
		constellation[22]=std::complex <double> ( 5,-3);
		constellation[23]=std::complex <double> ( 5,-1);
		constellation[24]=std::complex <double> ( 1,3);
		constellation[25]=std::complex <double> ( 1,1);
		constellation[26]=std::complex <double> ( 1,-3);
		constellation[27]=std::complex <double> ( 1,-1);
		constellation[28]=std::complex <double> ( 3,3);
		constellation[29]=std::complex <double> ( 3,1);
		constellation[30]=std::complex <double> ( 3,-3);
		constellation[31]=std::complex <double> ( 3,-1);

	}
	else if(M==MOD_64QAM)
	{
		constellation[0]=std::complex <double> ( -7,7);
		constellation[1]=std::complex <double> ( -7,5);
		constellation[2]=std::complex <double> ( -7,1);
		constellation[3]=std::complex <double> ( -7,3);
		constellation[4]=std::complex <double> ( -7,-7);
		constellation[5]=std::complex <double> ( -7,-5);
		constellation[6]=std::complex <double> ( -7,-1);
		constellation[7]=std::complex <double> ( -7,-3);
		constellation[8]=std::complex <double> ( -5,7);
		constellation[9]=std::complex <double> ( -5,5);
		constellation[10]=std::complex <double> ( -5,1);
		constellation[11]=std::complex <double> ( -5,3);
		constellation[12]=std::complex <double> ( -5,-7);
		constellation[13]=std::complex <double> ( -5,-5);
		constellation[14]=std::complex <double> ( -5,-1);
		constellation[15]=std::complex <double> ( -5,-3);
		constellation[16]=std::complex <double> ( -1,7);
		constellation[17]=std::complex <double> ( -1,5);
		constellation[18]=std::complex <double> ( -1,1);
		constellation[19]=std::complex <double> ( -1,3);
		constellation[20]=std::complex <double> ( -1,-7);
		constellation[21]=std::complex <double> ( -1,-5);
		constellation[22]=std::complex <double> ( -1,-1);
		constellation[23]=std::complex <double> ( -1,-3);
		constellation[24]=std::complex <double> ( -3,7);
		constellation[25]=std::complex <double> ( -3,5);
		constellation[26]=std::complex <double> ( -3,1);
		constellation[27]=std::complex <double> ( -3,3);
		constellation[28]=std::complex <double> ( -3,-7);
		constellation[29]=std::complex <double> ( -3,-5);
		constellation[30]=std::complex <double> ( -3,-1);
		constellation[31]=std::complex <double> ( -3,-3);
		constellation[32]=std::complex <double> ( 7,7);
		constellation[33]=std::complex <double> ( 7,5);
		constellation[34]=std::complex <double> ( 7,1);
		constellation[35]=std::complex <double> ( 7,3);
		constellation[36]=std::complex <double> ( 7,-7);
		constellation[37]=std::complex <double> ( 7,-5);
		constellation[38]=std::complex <double> ( 7,-1);
		constellation[39]=std::complex <double> ( 7,-3);
		constellation[40]=std::complex <double> ( 5,7);
		constellation[41]=std::complex <double> ( 5,5);
		constellation[42]=std::complex <double> ( 5,1);
		constellation[43]=std::complex <double> ( 5,3);
		constellation[44]=std::complex <double> ( 5,-7);
		constellation[45]=std::complex <double> ( 5,-5);
		constellation[46]=std::complex <double> ( 5,-1);
		constellation[47]=std::complex <double> ( 5,-3);
		constellation[48]=std::complex <double> ( 1,7);
		constellation[49]=std::complex <double> ( 1,5);
		constellation[50]=std::complex <double> ( 1,1);
		constellation[51]=std::complex <double> ( 1,3);
		constellation[52]=std::complex <double> ( 1,-7);
		constellation[53]=std::complex <double> ( 1,-5);
		constellation[54]=std::complex <double> ( 1,-1);
		constellation[55]=std::complex <double> ( 1,-3);
		constellation[56]=std::complex <double> ( 3,7);
		constellation[57]=std::complex <double> ( 3,5);
		constellation[58]=std::complex <double> ( 3,1);
		constellation[59]=std::complex <double> ( 3,3);
		constellation[60]=std::complex <double> ( 3,-7);
		constellation[61]=std::complex <double> ( 3,-5);
		constellation[62]=std::complex <double> ( 3,-1);
		constellation[63]=std::complex <double> ( 3,-3);
	}
	set_constellation(constellation,M);

}

void cl_psk::set_constellation(std::complex <double> *_constellation, int size)
{
	float power_normalization_value=0;

	constellation=new std::complex <double>[size];
	nSymbols=size;
	nBits=(int)log2(nSymbols);

	for(int i=0;i<size;i++)
	{
		constellation[i]=*(_constellation+i);
	}

	for(int i=0;i<nSymbols;i++)
	{
		power_normalization_value+=pow(constellation[i].real(),2)+pow(constellation[i].imag(),2);
	}
	power_normalization_value=1/(sqrt(power_normalization_value/nSymbols));

	for(int i=0;i<nSymbols;i++)
	{
		constellation[i]*=power_normalization_value;
	}
}


void cl_psk::mod(const int *in,int nItems,std::complex <double> *out)
{
	for(int i=0;i<nItems;i+=nBits)
	{
		unsigned int const_loc=0;
		for(int j=0;j<nBits;j++)
		{
			const_loc+=*(in+i+j);
			const_loc<<=1;
		}
		const_loc>>=1;
		*(out+i/nBits)=constellation[const_loc];
	}
}





void cl_psk::demod(const std::complex <double> *in,int nItems,float *out,float variance)
{

	float* D=new float[nSymbols];
	float* LLR=new float[nBits];
	float Dmin0,Dmin1;
	unsigned int mask;

	for(int i=0;i<nItems;i+=nBits)
	{

		for(int j=0;j<nSymbols;j++)
		{
			D[j]=pow((real(*(in+i/nBits))-real(constellation[j])),2)+pow((imag(*(in+i/nBits))-imag(constellation[j])),2);
		}

		mask=1;
		for(int k=0;k<nBits;k++)
		{
			Dmin0=D[0];
			Dmin1=D[mask];

			for(int j=0;j<nSymbols;j++)
			{
				if((j & mask)==0)
				{
					if(D[j]<Dmin0)
					{
						Dmin0=D[j];
					}
				}
				if((j & mask)==mask)
				{
					if(D[j]<Dmin1)
					{
						Dmin1=D[j];
					}
				}
			}
			LLR[k]=((1/variance)*(Dmin1-Dmin0));
			mask<<=1;
		}
		for(int j=0;j<nBits;j++)
		{
			*(out+i+j)=LLR[nBits-j-1];
		}
	}

	if(D!=NULL)
	{
		delete D;
	}
	if(LLR!=NULL)
	{
		delete LLR;
	}

}


