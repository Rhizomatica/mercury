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
#include "ldpc.h"

cl_ldpc::cl_ldpc()
{
	standard_val=0;
	N=0;
	K=0;
	P=0;
	decoding_algorithm_val=0;
	eta_val=0;
	nIteration_max_val=0;
	print_nIteration_val=0;
	Cwidth=0;
	r=0;
	decoding_algorithm=0;
	rate=0;
	framesize=0;
	standard=0;
	nIteration_max=0;
	GBF_eta=0;
	print_nIteration=NO;

	Cwidth=0;
	QCmatrixC=NULL;
	QCmatrixEnc=NULL;
}

cl_ldpc::~cl_ldpc()
{
	deinit();
}

void cl_ldpc::init()
{
	standard_val=standard;
	N=framesize;
	K=(int)((float)N*rate);
	P=N-K;
	decoding_algorithm_val=decoding_algorithm;
	eta_val=GBF_eta;
	nIteration_max_val=nIteration_max;
	print_nIteration_val= print_nIteration;
	Cwidth=0;
	update_code_parameters();
}

void cl_ldpc::deinit()
{
	standard_val=0;
	N=0;
	K=0;
	P=0;
	decoding_algorithm_val=0;
	eta_val=0;
	nIteration_max_val=0;
	print_nIteration_val=0;
	Cwidth=0;

	Cwidth=0;
	QCmatrixC=NULL;
	QCmatrixEnc=NULL;

}


void cl_ldpc::encode(const int* data, int*  encoded_data)
 {
 	int CwidthMax=Cwidth-1;
 	int* QCmatrixEnc_;
 	QCmatrixEnc_=QCmatrixEnc;
 	for(int i=0;i<K;i++)
 	{
 		encoded_data[i]=data[i];
 	}

 	for(int i=0;i<P;i++)
 	{
 		encoded_data[i+K]=0;
 		for(int j=0;j<Cwidth-1;j++)
 		{
 			if(*(QCmatrixEnc_+i*CwidthMax+j)!=-1)
 			{
 				encoded_data[i+K]^=encoded_data[*(QCmatrixEnc_+i*CwidthMax+j)];
 			}
 		}
 	}
 }


 int cl_ldpc::update_code_parameters()
  {
  	int success=0;
  	if(standard_val==HERMES)
  	{
  		if(N==HERMES_NORMAL)
  		{
  			if(K==200)//rate == 2/16
  			{
  				Cwidth=hermes_normal_Cwidth_2_16;
  				QCmatrixC=&hermes_normal_QCmatrixC_2_16[0][0];
  				QCmatrixEnc=&hermes_normal_QCmatrixEnc_2_16[0][0];
  			}
  			else if(K==800)//rate == 8/16
  			{
  				Cwidth=hermes_normal_Cwidth_8_16;
  				QCmatrixC=&hermes_normal_QCmatrixC_8_16[0][0];
  				QCmatrixEnc=&hermes_normal_QCmatrixEnc_8_16[0][0];
  			}
  			else if(K==1400)//rate == 14/16
  			{
  				Cwidth=hermes_normal_Cwidth_14_16;
  				QCmatrixC=&hermes_normal_QCmatrixC_14_16[0][0];
  				QCmatrixEnc=&hermes_normal_QCmatrixEnc_14_16[0][0];
  			}
  			else
  			{
  				std::cout<<"K="<<K<<" Wrong Code Rate"<<std::endl<<"Exiting.."<<std::endl;
  				success=-1;
  				exit(1);
  			}

  		}

  	}
  	return success;
  }


 int cl_ldpc::decode(const float* data,  int*  decoded_data)
 {
	 int iterations_done=0;
 	if(decoding_algorithm_val==GBF)
 	{
 		iterations_done=decode_GBF(data,decoded_data,QCmatrixC,Cwidth,Cwidth,N,K,P,nIteration_max_val,eta_val);
 	}
 	return iterations_done;
 }
