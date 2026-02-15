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

#include "physical_layer/ldpc.h"

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
	QCmatrixV=NULL;
	QCmatrixd=NULL;
	Q=NULL;
	Vwidth=0;
	R=NULL;
	V_pos=NULL;
	dwidth=0;
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

	if(R!=NULL)
	{
		delete[] R;
		R=NULL;
	}
	if(Q!=NULL)
	{
		delete[] Q;
		Q=NULL;
	}
	if(V_pos!=NULL)
	{
		delete[] V_pos;
		V_pos=NULL;
	}

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
  	if(standard_val==MERCURY)
  	{
  		if(N==MERCURY_NORMAL)
  		{
  			if(K==100)//rate == 1/16
  			{
  				Cwidth=mercury_normal_Cwidth_1_16;
  				Vwidth=mercury_normal_Vwidth_1_16;
  				dwidth=mercury_normal_dwidth_1_16;
  				QCmatrixC=&mercury_normal_QCmatrixC_1_16[0][0];
  				QCmatrixEnc=&mercury_normal_QCmatrixEnc_1_16[0][0];
  				QCmatrixV=&mercury_normal_QCmatrixV_1_16[0][0];
  				QCmatrixd=&mercury_normal_QCmatrixd_1_16[0];
  				R=new double [N*Vwidth];
  				Q=new double [N*Vwidth];
  			}
  			else if(K==200)//rate == 2/16
  			{
//  				Cwidth=mercury_met_Cwidth_2_16;
//  				Vwidth=mercury_met_Vwidth_2_16;
//  				dwidth=mercury_met_dwidth_2_16;
//  				QCmatrixC=&mercury_met_QCmatrixC_2_16[0][0];
//  				QCmatrixEnc=&mercury_met_QCmatrixEnc_2_16[0][0];
//  				QCmatrixV=&mercury_met_QCmatrixV_2_16[0][0];
//  				QCmatrixd=&mercury_met_QCmatrixd_2_16[0];

  				Cwidth=mercury_normal_Cwidth_2_16;
  				Vwidth=mercury_normal_Vwidth_2_16;
  				dwidth=mercury_normal_dwidth_2_16;
  				QCmatrixC=&mercury_normal_QCmatrixC_2_16[0][0];
  				QCmatrixEnc=&mercury_normal_QCmatrixEnc_2_16[0][0];
  				QCmatrixV=&mercury_normal_QCmatrixV_2_16[0][0];
  				QCmatrixd=&mercury_normal_QCmatrixd_2_16[0];
  				R=new double [N*Vwidth];
  				Q=new double [N*Vwidth];
  			}
  			else if(K==300)//rate == 3/16
  			{
  				Cwidth=mercury_normal_Cwidth_3_16;
  				Vwidth=mercury_normal_Vwidth_3_16;
  				dwidth=mercury_normal_dwidth_3_16;
  				QCmatrixC=&mercury_normal_QCmatrixC_3_16[0][0];
  				QCmatrixEnc=&mercury_normal_QCmatrixEnc_3_16[0][0];
  				QCmatrixV=&mercury_normal_QCmatrixV_3_16[0][0];
  				QCmatrixd=&mercury_normal_QCmatrixd_3_16[0];
  				R=new double [N*Vwidth];
  				Q=new double [N*Vwidth];
  			}
  			else if(K==400)//rate == 4/16
  			{
  				Cwidth=mercury_normal_Cwidth_4_16;
  				Vwidth=mercury_normal_Vwidth_4_16;
  				dwidth=mercury_normal_dwidth_4_16;
  				QCmatrixC=&mercury_normal_QCmatrixC_4_16[0][0];
  				QCmatrixEnc=&mercury_normal_QCmatrixEnc_4_16[0][0];
  				QCmatrixV=&mercury_normal_QCmatrixV_4_16[0][0];
  				QCmatrixd=&mercury_normal_QCmatrixd_4_16[0];
  				R=new double [N*Vwidth];
  				Q=new double [N*Vwidth];
  			}
  			else if(K==500)//rate == 5/16
  			{
  				Cwidth=mercury_normal_Cwidth_5_16;
  				Vwidth=mercury_normal_Vwidth_5_16;
  				dwidth=mercury_normal_dwidth_5_16;
  				QCmatrixC=&mercury_normal_QCmatrixC_5_16[0][0];
  				QCmatrixEnc=&mercury_normal_QCmatrixEnc_5_16[0][0];
  				QCmatrixV=&mercury_normal_QCmatrixV_5_16[0][0];
  				QCmatrixd=&mercury_normal_QCmatrixd_5_16[0];
  				R=new double [N*Vwidth];
  				Q=new double [N*Vwidth];
  			}
  			else if(K==600)//rate == 6/16
  			{
  				Cwidth=mercury_normal_Cwidth_6_16;
  				Vwidth=mercury_normal_Vwidth_6_16;
  				dwidth=mercury_normal_dwidth_6_16;
  				QCmatrixC=&mercury_normal_QCmatrixC_6_16[0][0];
  				QCmatrixEnc=&mercury_normal_QCmatrixEnc_6_16[0][0];
  				QCmatrixV=&mercury_normal_QCmatrixV_6_16[0][0];
  				QCmatrixd=&mercury_normal_QCmatrixd_6_16[0];
  				R=new double [N*Vwidth];
  				Q=new double [N*Vwidth];
  			}
  			else if(K==800)//rate == 8/16
  			{
  				Cwidth=mercury_normal_Cwidth_8_16;
  				Vwidth=mercury_normal_Vwidth_8_16;
  				dwidth=mercury_normal_dwidth_8_16;
  				QCmatrixC=&mercury_normal_QCmatrixC_8_16[0][0];
  				QCmatrixEnc=&mercury_normal_QCmatrixEnc_8_16[0][0];
  				QCmatrixV=&mercury_normal_QCmatrixV_8_16[0][0];
  				QCmatrixd=&mercury_normal_QCmatrixd_8_16[0];
  				R=new double [N*Vwidth];
  				Q=new double [N*Vwidth];
  			}
  			else if(K==1400)//rate == 14/16
  			{
  				Cwidth=mercury_normal_Cwidth_14_16;
  				Vwidth=mercury_normal_Vwidth_14_16;
  				dwidth=mercury_normal_dwidth_14_16;
  				QCmatrixC=&mercury_normal_QCmatrixC_14_16[0][0];
  				QCmatrixEnc=&mercury_normal_QCmatrixEnc_14_16[0][0];
  				QCmatrixV=&mercury_normal_QCmatrixV_14_16[0][0];
  				QCmatrixd=&mercury_normal_QCmatrixd_14_16[0];
  				R=new double [N*Vwidth];
  				Q=new double [N*Vwidth];
  			}
  			else
  			{
  				std::cout<<"K="<<K<<" Wrong Code Rate"<<std::endl<<"Exiting.."<<std::endl;
  				success=-1;
  				exit(1);
  			}
  			if(R==NULL || Q==NULL)
  			{
  				std::cout<<"Memory allocation error"<<std::endl;
  				exit(2);
  			}
  			// Pre-allocate V_pos workspace for SPA decoder (eliminates per-frame heap churn)
  			V_pos=new int[P*Cwidth];
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
 	else if(decoding_algorithm_val==SPA)
 	{
 		iterations_done=decode_SPA(data,decoded_data,QCmatrixC,Cwidth,Cwidth, QCmatrixV,Vwidth,Vwidth,QCmatrixd,dwidth,R,Q,V_pos,N,K,P,nIteration_max_val);
 	}
 	return iterations_done;
 }
