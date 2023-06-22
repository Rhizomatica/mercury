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
#include "ofdm.h"


cl_ofdm::cl_ofdm()
{
	this->Nc=0;
	this->Nfft=0;
	this->Nsymb=0;
	this->gi=0;
	Ngi=0;
	ofdm_frame =0;
	zero_padded_data=0;
	iffted_data=0;
	gi_removed_data=0;
	ffted_data=0;
	estimated_channel=0;
	time_sync_Nsymb=1;
	freq_offset_ignore_limit=0.1;
	print_time_sync_status=NO;
}

cl_ofdm::~cl_ofdm()
{
	this->deinit();
}

void cl_ofdm::init(int Nfft, int Nc, int Nsymb, float gi)
{
	this->Nc=Nc;
	this->Nfft=Nfft;
	this->Nsymb=Nsymb;
	this->gi=gi;

	this->init();
}
void cl_ofdm::init()
{
	Ngi=Nfft*gi;

	ofdm_frame = new struct carrier[this->Nsymb*this->Nc];
	zero_padded_data=new std::complex <double>[Nfft];
	iffted_data=new std::complex <double>[Nfft];
	gi_removed_data=new std::complex <double>[Nfft];
	ffted_data=new std::complex <double>[Nfft];
	estimated_channel=new struct channel[this->Nsymb*this->Nc];

	pilot_configurator.init(this->Nfft, this->Nc,this->Nsymb,this->ofdm_frame);

	srand(0);

	int last_pilot=0;
	int pilot_value;
	if(this->pilot_configurator.pilot_mod==DBPSK)
	{
		for(int i=0;i<pilot_configurator.nPilots;i++)
		{
			pilot_value=rand()%2 ^ last_pilot;
			pilot_configurator.pilot_sequence[i]=std::complex <double>(2*pilot_value-1,0)*pilot_configurator.pilot_boost;
			last_pilot=pilot_value;
		}
	}

	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			(estimated_channel+i*Nc+j)->value=1;
		}
	}

	srand(time(0));

}

void cl_ofdm::deinit()
{
	this->Ngi=0;
	this->Nc=0;
	this->Nfft=0;
	this->Nsymb=0;
	this->gi=0;

	pilot_configurator.Dx=0;
	pilot_configurator.Dy=0;
	pilot_configurator.first_row=0;
	pilot_configurator.last_row=0;
	pilot_configurator.first_col=0;
	pilot_configurator.second_col=0;
	pilot_configurator.last_col=0;
	pilot_configurator.pilot_boost=0;
	pilot_configurator.first_row_zeros=NO;


	if(ofdm_frame!=NULL)
	{
		delete[] ofdm_frame;
		ofdm_frame=NULL;
	}
	if(zero_padded_data!=NULL)
	{
		delete[] zero_padded_data;
		zero_padded_data=NULL;
	}
	if(iffted_data!=NULL)
	{
		delete[] iffted_data;
		iffted_data=NULL;
	}
	if(gi_removed_data!=NULL)
	{
		delete[] gi_removed_data;
		gi_removed_data=NULL;
	}
	if(ffted_data!=NULL)
	{
		delete[] ffted_data;
		ffted_data=NULL;
	}
	if(estimated_channel!=NULL)
	{
		delete[] estimated_channel;
		estimated_channel=NULL;
	}

	pilot_configurator.deinit();
	pilot_configurator.pilot_sequence = new std::complex <double>[pilot_configurator.nPilots];

}

void cl_ofdm::zero_padder(std::complex <double>* in, std::complex <double>* out)
{
	int start_shift=1;
	for(int j=0;j<Nc/2;j++)
	{
		out[j+Nfft-Nc/2]=in[j];
	}

	for(int j=0;j<start_shift;j++)
	{
		out[j]=std::complex <double>(0,0);
	}

	for(int j=Nc/2+start_shift;j<Nfft-Nc/2;j++)
	{
		out[j]=std::complex <double>(0,0);
	}

	for(int j=Nc/2;j<Nc;j++)
	{
		out[j-Nc/2+start_shift]=in[j];
	}
}
void cl_ofdm::zero_depadder(std::complex <double>* in, std::complex <double>* out)
{
	int start_shift=1;
	for(int j=0;j<Nc/2;j++)
	{
		out[j]=in[j+Nfft-Nc/2];
	}
	for(int j=Nc/2;j<Nc;j++)
	{
		out[j]=in[j-Nc/2+start_shift];
	}
}
void cl_ofdm::gi_adder(std::complex <double>* in, std::complex <double>* out)
{
	for(int j=0;j<Nfft;j++)
	{
		out[j+Ngi]=in[j];
	}
	for(int j=0;j<Ngi;j++)
	{
		out[j]=in[j+Nfft-Ngi];
	}
}
void cl_ofdm::gi_remover(std::complex <double>* in, std::complex <double>* out)
{
	for(int j=0;j<Nfft;j++)
	{
		out[j]=in[j+Ngi];
	}
}

void cl_ofdm::fft(std::complex <double>* in, std::complex <double>* out)
{
	for(int i=0;i<Nfft;i++)
	{
		out[i]=in[i];
	}
	_fft(out,Nfft);

	for(int i=0;i<Nfft;i++)
	{
		out[i]=out[i]/(double)Nfft;
	}

}
void cl_ofdm::fft(std::complex <double>* in, std::complex <double>* out, int _Nfft)
{
	for(int i=0;i<_Nfft;i++)
	{
		out[i]=in[i];
	}
	_fft(out,_Nfft);

	for(int i=0;i<_Nfft;i++)
	{
		out[i]=out[i]/(double)_Nfft;
	}

}

void cl_ofdm::_fft(std::complex <double> *v, int n)
{
	if(n>1) {
		std::complex <double> *tmp=new std::complex <double>[n];
		int k,m;    std::complex <double> z, w, *vo, *ve;
		ve = tmp; vo = tmp+n/2;
		for(k=0; k<n/2; k++) {
			ve[k] = v[2*k];
			vo[k] = v[2*k+1];
		}
		_fft( ve, n/2 );
		_fft( vo, n/2 );
		for(m=0; m<n/2; m++) {
			w.real( cos(2*M_PI*m/(double)n));
			w.imag( -sin(2*M_PI*m/(double)n));
			z.real( w.real()*vo[m].real() - w.imag()*vo[m].imag());
			z.imag( w.real()*vo[m].imag() + w.imag()*vo[m].real());
			v[  m  ].real( ve[m].real() + z.real());
			v[  m  ].imag( ve[m].imag() + z.imag());
			v[m+n/2].real( ve[m].real() - z.real());
			v[m+n/2].imag( ve[m].imag() - z.imag());
		}
		if(tmp!=NULL)
		{
			delete tmp;
		}
	}
	//Ref:Wickerhauser, Mladen Victor,Mathematics for Multimedia, Birkhäuser Boston, January 2010, DOI: 10.1007/978-0-8176-4880-0, ISBNs 978-0-8176-4880-0, 978-0-8176-4879-4
	//https://www.math.wustl.edu/~victor/mfmm/
}

void cl_ofdm::ifft(std::complex <double>* in, std::complex <double>* out)
{
	for(int i=0;i<Nfft;i++)
	{
		out[i]=in[i];
	}
	_ifft(out,Nfft);
}

void cl_ofdm::ifft(std::complex <double>* in, std::complex <double>* out,int _Nfft)
{
	for(int i=0;i<_Nfft;i++)
	{
		out[i]=in[i];
	}
	_ifft(out,_Nfft);
}

void cl_ofdm::_ifft(std::complex <double>* v,int n)
{
	if(n>1) {
		std::complex <double> *tmp=new std::complex <double>[n];
		int k,m;    std::complex <double> z, w, *vo, *ve;
		ve = tmp; vo = tmp+n/2;
		for(k=0; k<n/2; k++) {
			ve[k] = v[2*k];
			vo[k] = v[2*k+1];
		}
		_ifft( ve, n/2);
		_ifft( vo, n/2);
		for(m=0; m<n/2; m++) {
			w.real( cos(2*M_PI*m/(double)n));
			w.imag( sin(2*M_PI*m/(double)n));
			z.real( w.real()*vo[m].real() - w.imag()*vo[m].imag());
			z.imag( w.real()*vo[m].imag() + w.imag()*vo[m].real());
			v[  m  ].real( ve[m].real() + z.real());
			v[  m  ].imag( ve[m].imag() + z.imag());
			v[m+n/2].real( ve[m].real() - z.real());
			v[m+n/2].imag( ve[m].imag() - z.imag());
		}
		if(tmp!=NULL)
		{
			delete tmp;
		}
	}
	//Ref:Wickerhauser, Mladen Victor,Mathematics for Multimedia, Birkhäuser Boston, January 2010, DOI: 10.1007/978-0-8176-4880-0, ISBNs 978-0-8176-4880-0, 978-0-8176-4879-4
	//https://www.math.wustl.edu/~victor/mfmm/
}

double cl_ofdm::frequency_sync(std::complex <double>*in, double carrier_freq_width)
{
	double frequency_offset_prec=0;

	std::complex <double> p1,p2,mul;
	std::complex <double> frame[Nfft];
	std::complex <double> frame_fft[Nfft],frame_depadded1[Nfft],frame_depadded2[Nfft];

	for(int i=0;i<Nfft/2;i++)
	{
		frame[i]=*(in+i);
		frame[i+Nfft/2]=*(in+i);
	}

	fft(frame,frame_fft);
	zero_depadder(frame_fft,frame_depadded1);

	for(int i=0;i<Nfft/2;i++)
	{
		frame[i]=*(in+i+Nfft/2);
		frame[i+Nfft/2]=*(in+i+Nfft/2);
	}

	fft(frame,frame_fft);
	zero_depadder(frame_fft,frame_depadded2);

	mul=0;
	for(int i=0;i<Nc;i++)
	{
		mul+=conj(frame_depadded2[i])*frame_depadded1[i];
	}
	frequency_offset_prec=2.0*(1.0/(2*M_PI))*atan(mul.imag()/mul.real());

	return (frequency_offset_prec*carrier_freq_width);

	//Ref1: P. H. Moose, "A technique for orthogonal frequency division multiplexing frequency offset correction," in IEEE Transactions on Communications, vol. 42, no. 10, pp. 2908-2914, Oct. 1994, doi: 10.1109/26.328961.
	//Ref2: T. M. Schmidl and D. C. Cox, "Robust frequency and timing synchronization for OFDM," in IEEE Transactions on Communications, vol. 45, no. 12, pp. 1613-1621, Dec. 1997, doi: 10.1109/26.650240.
}

void cl_ofdm::framer(std::complex <double>* in, std::complex <double>* out)
{
	int data_index=0;
	int pilot_index=0;
	for(int j=0;j<Nsymb;j++)
	{
		for(int k=0;k<Nc;k++)
		{
			if((ofdm_frame+j*this->Nc+k)->type==DATA)
			{
				out[j*Nc+k]=in[data_index];
				data_index++;
			}
			else if ((ofdm_frame+j*this->Nc+k)->type==PILOT)
			{
				out[j*Nc+k]=pilot_configurator.pilot_sequence[pilot_index];
				pilot_index++;
			}
			else if ((ofdm_frame+j*this->Nc+k)->type==ZERO)
			{
				out[j*Nc+k]=0;
			}
		}
	}

}

void cl_ofdm::deframer(std::complex <double>* in, std::complex <double>* out)
{
	int data_index=0;

	for(int j=0;j<Nsymb;j++)
	{
		for(int k=0;k<Nc;k++)
		{
			if((ofdm_frame+j*this->Nc+k)->type==DATA)
			{
				out[data_index]=in[j*Nc+k];
				data_index++;
			}
		}
	}
}


void cl_ofdm::symbol_mod(std::complex <double>*in, std::complex <double>*out)
{
	zero_padder(in,zero_padded_data);
	ifft(zero_padded_data,iffted_data);
	gi_adder(iffted_data, out);
}

void cl_ofdm::symbol_demod(std::complex <double>*in, std::complex <double>*out)
{
	gi_remover(in, gi_removed_data);
	fft(gi_removed_data,ffted_data);
	zero_depadder(ffted_data, out);
}

cl_pilot_configurator::cl_pilot_configurator()
{
	first_col=DATA;
	last_col=AUTO_SELLECT;
	second_col=DATA;
	first_row=DATA;
	last_row=DATA;
	Nc=0;
	Nsymb=0;
	Nc_max=0;
	nData=0;
	nPilots=0;
	nConfig=0;
	carrier=0;
	Dy=0;
	Dx=0;
	virtual_carrier=0;
	pilot_mod=DBPSK;
	pilot_sequence=0;
	pilot_boost=1.0;
	first_row_zeros=NO;
	Nfft=0;
	nZeros=0;
}

cl_pilot_configurator::~cl_pilot_configurator()
{
	if(virtual_carrier!=NULL)
	{
		delete virtual_carrier;
	}
}

void cl_pilot_configurator::init(int Nfft, int Nc, int Nsymb,struct carrier* _carrier)
{
	this->carrier=_carrier;
	this->Nc=Nc;
	this->Nsymb=Nsymb;
	this->Nfft=Nfft;
	if(Nc>Nsymb)
	{
		this->Nc_max=Nc;
	}
	else
	{
		this->Nc_max=Nsymb;
	}
	nData=Nc*Nsymb;
	virtual_carrier = new struct carrier[this->Nc_max*this->Nc_max];

	for(int j=0;j<this->Nc_max;j++)
	{
		for(int i=0;i<this->Nc_max;i++)
		{
			(virtual_carrier+j*this->Nc_max+i)->type=DATA;
		}

	}

	this->configure();

	pilot_sequence = new std::complex <double>[nPilots];

	this->print();
}

void cl_pilot_configurator::deinit()
{
	this->carrier=NULL;
	this->Nc=0;
	this->Nsymb=0;
	this->Nfft=0;
	this->Nc_max=0;
	this->nData=0;

	if(virtual_carrier!=NULL)
	{
		delete[] virtual_carrier;
		virtual_carrier=NULL;
	}
	if(pilot_sequence!=NULL)
	{
		delete[] pilot_sequence;
		pilot_sequence=NULL;
	}

}

void cl_pilot_configurator::configure()
{
	int x=0;
	int y=0;

	while(x<Nc_max && y<Nc_max)
	{
		(virtual_carrier+y*Nc_max+x)->type=PILOT;

		for(int j=y;j<Nc_max;j+=Dy)
		{
			(virtual_carrier+j*Nc_max+x)->type=PILOT;
		}
		for(int j=y;j>=0;j-=Dy)
		{
			(virtual_carrier+j*Nc_max+x)->type=PILOT;
		}

		y++;
		x+=Dx;

	}

	int pilot_count=0;
	for(int j=0;j<Nsymb;j++)
	{
		if ((virtual_carrier+j*Nc_max+Nc-1)->type==PILOT)
		{
			pilot_count++;
		}
	}

	if(last_col==AUTO_SELLECT && pilot_count<2)
	{
		last_col=COPY_FIRST_COL;
	}


	for(int j=0;j<Nc_max;j++)
	{
		if(first_row==PILOT)
		{
			(virtual_carrier+0*Nc_max+j)->type=PILOT;
		}
		if(last_row==PILOT)
		{
			(virtual_carrier+(Nsymb-1)*Nc_max+j)->type=PILOT;
		}
		if(first_col==PILOT)
		{
			(virtual_carrier+j*Nc_max+0)->type=PILOT;
		}
		if(last_col==PILOT)
		{
			(virtual_carrier+j*Nc_max+Nc-1)->type=PILOT;
		}
		if(last_col==COPY_FIRST_COL)
		{
			(virtual_carrier+j*Nc_max+Nc-1)->type=(virtual_carrier+j*Nc_max+0)->type;
		}
		if(second_col==CONFIG&&(virtual_carrier+j*Nc_max+1)->type!=PILOT)
		{
			(virtual_carrier+j*Nc_max+1)->type=CONFIG;
		}
	}

	if(first_row_zeros==YES)
	{
		int fft_zeros_tmp[Nfft];
		int fft_zeros_depadded_tmp[Nc];

		for(int j=0;j<Nfft;j++)
		{
			if(j%2==1)
			{
				fft_zeros_tmp[j]=0;
			}
			else
			{
				fft_zeros_tmp[j]=1;
			}
		}

		int start_shift=1;
		for(int j=0;j<Nc/2;j++)
		{
			fft_zeros_depadded_tmp[j]=fft_zeros_tmp[j+Nfft-Nc/2];
		}
		for(int j=Nc/2;j<Nc;j++)
		{
			fft_zeros_depadded_tmp[j]=fft_zeros_tmp[j-Nc/2+start_shift];
		}


		for(int j=0;j<Nc_max;j++)
		{
			if(fft_zeros_depadded_tmp[j]==0)
			{
				(virtual_carrier+0*Nc_max+j)->type=ZERO;
			}
		}
	}

	nZeros=0;
	nPilots=0;
	nConfig=0;
	for(int j=0;j<Nsymb;j++)
	{
		for(int i=0;i<Nc;i++)
		{

			(carrier + j*Nc+i)->type=(virtual_carrier+j*Nc_max+i)->type;

			if((virtual_carrier+j*Nc_max+i)->type==PILOT)
			{
				nPilots++;
				nData--;
			}
			if((virtual_carrier+j*Nc_max+i)->type==CONFIG)
			{
				nConfig++;
				nData--;
			}
			if((virtual_carrier+j*Nc_max+i)->type==ZERO)
			{
				nZeros++;
				nData--;
			}
		}
	}
}

void cl_pilot_configurator::print()
{
	for(int j=0;j<Nsymb;j++)
	{
		for(int i=0;i<Nc;i++)
		{
			if((carrier+j*Nc+i)->type==PILOT)
			{
				std::cout<<"P ";
			}
			else if((carrier+j*Nc+i)->type==CONFIG)
			{
				std::cout<<"C ";
			}
			else if((carrier+j*Nc+i)->type==ZERO)
			{
				std::cout<<"Z ";
			}
			else if((carrier+j*Nc+i)->type==DATA)
			{
				std::cout<<". ";
			}
			else
			{
				std::cout<<"_ ";
			}
		}
		std::cout<<std::endl;

	}

	std::cout<<"nData="<<this->nData<<std::endl;
	std::cout<<"nPilots="<<this->nPilots<<std::endl;
	std::cout<<"nZeros="<<this->nZeros<<std::endl;
	std::cout<<"nConfig="<<this->nConfig<<std::endl;
}


std::complex <double> cl_ofdm::interpolate_linear(std::complex <double> a,double a_x,std::complex <double> b,double b_x,double x)
{
	std::complex <double> return_val;

	return_val=a+(b-a)*(x-a_x)/(b_x-a_x);

	return return_val;
}
std::complex <double> cl_ofdm::interpolate_bilinear(std::complex <double> a,double a_x,double a_y,std::complex <double> b,double b_x,double b_y,std::complex <double> c,double c_x,double c_y,std::complex <double> d,double d_x,double d_y,double x,double y)

{
	std::complex <double> e,f,return_val;

	e=this->interpolate_linear(a,a_x,b,b_x,x);
	f=this->interpolate_linear(c,c_x,d,d_x,x);

	return_val=this->interpolate_linear(e,a_y,f,c_y,y);


	return return_val;
}

void cl_ofdm::interpolate_linear_col(int col)
{
	int loc_start,loc_end,nLocations;

	loc_start=0;
	loc_end=Nsymb-1;
	nLocations=Nsymb-1;

	while(nLocations>0)
	{
		for(int i=loc_start;i<Nsymb;i++)
		{
			if(estimated_channel[i*Nc+col].status==MEASURED)
			{
				loc_start=i;
				break;
			}
		}
		for(int i=loc_start+1;i<Nsymb;i++)
		{
			if(estimated_channel[i*Nc+col].status==MEASURED)
			{
				loc_end=i;
				break;
			}
		}
		nLocations=loc_end-loc_start;

		for(int i=loc_start+1;i<loc_end;i++)
		{
			estimated_channel[i*Nc+col].value=interpolate_linear(estimated_channel[loc_start*Nc+col].value,loc_start,estimated_channel[loc_end*Nc+col].value,loc_end,i);
			estimated_channel[i*Nc+col].status=INTERPOLATED;
		}
		loc_start=loc_end;
	}


	loc_start=0;
	loc_end=Nsymb-1;
	for(int i=0;i<Nsymb;i++)
	{
		if(estimated_channel[i*Nc+col].status==MEASURED)
		{
			loc_start=i;
			break;
		}
	}
	for(int i=loc_start+1;i<Nsymb;i++)
	{
		if(estimated_channel[i*Nc+col].status==MEASURED)
		{
			loc_end=i;
			break;
		}
	}
	if(loc_start!=0)
	{
		for(int i=0;i<loc_start;i++)
		{
			estimated_channel[i*Nc+col].value=interpolate_linear(estimated_channel[loc_start*Nc+col].value,loc_start,estimated_channel[loc_end*Nc+col].value,loc_end,i);
			estimated_channel[i*Nc+col].status=INTERPOLATED;
		}
	}


	loc_end=0;
	loc_start=Nsymb-1;
	for(int i=Nsymb-1;i>=0;i--)
	{
		if(estimated_channel[i*Nc+col].status==MEASURED)
		{
			loc_end=i;
			break;
		}
	}
	for(int i=loc_end-1;i>=0;i--)
	{
		if(estimated_channel[i*Nc+col].status==MEASURED)
		{
			loc_start=i;
			break;
		}
	}
	if(loc_end!=Nsymb-1)
	{
		for(int i=Nsymb-1;i>loc_end;i--)
		{
			estimated_channel[i*Nc+col].value=interpolate_linear(estimated_channel[loc_start*Nc+col].value,loc_start,estimated_channel[loc_end*Nc+col].value,loc_end,i);
			estimated_channel[i*Nc+col].status=INTERPOLATED;
		}
	}
}


void cl_ofdm::interpolate_bilinear_matrix(int col1,int col2, int row1, int row2)
{
	std::complex <double> a,b,c,d;
	double a_x,a_y,b_x,b_y,c_x,c_y,d_x,d_y;


	a=estimated_channel[row1*Nc+col1].value;
	b=estimated_channel[row1*Nc+col2].value;

	for(int i=col1+1;i<col2;i++)
	{
		estimated_channel[row1*Nc+i].value=this->interpolate_linear(a,col1,b,col2,i);
		estimated_channel[row1*Nc+i].status=INTERPOLATED;
	}

	for(int j=row1+1;j<row2;j++)
	{
		a=estimated_channel[(j-1)*Nc+col1].value;
		a_x=col1;
		a_y=j-1;
		b=estimated_channel[(j-1)*Nc+col2].value;
		b_x=col2;
		b_y=j-1;
		c=estimated_channel[(j+1)*Nc+col1].value;
		c_x=col1;
		c_y=j+1;
		d=estimated_channel[(j+1)*Nc+col2].value;
		d_x=col2;
		d_y=j+1;

		for(int i=col1+1;i<col2;i++)
		{
			estimated_channel[j*Nc+i].value=interpolate_bilinear(a,a_x,a_y,b,b_x,b_y,c,c_x,c_y,d,d_x,d_y,i,j);
			estimated_channel[j*Nc+i].status=INTERPOLATED;
		}

	}

	c=estimated_channel[row2*Nc+col1].value;
	d=estimated_channel[row2*Nc+col2].value;

	for(int i=col1+1;i<col2;i++)
	{
		estimated_channel[row2*Nc+i].value=this->interpolate_linear(c,col1,d,col2,i);
		estimated_channel[row2*Nc+i].status=INTERPOLATED;
	}

}

void cl_ofdm::channel_estimator_frame_time_frequency(std::complex <double>*in)
{
	int pilot_index=0;
	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			if((ofdm_frame+i*Nc+j)->type==PILOT)
			{
				(estimated_channel+i*Nc+j)->status=MEASURED;
				(estimated_channel+i*Nc+j)->value=*(in+i*Nc+j)/pilot_configurator.pilot_sequence[pilot_index];
				pilot_index++;
			}
			else
			{
				(estimated_channel+i*Nc+j)->status=UNKNOWN;
				(estimated_channel+i*Nc+j)->value=0;
			}
		}
	}

	for(int j=0;j<Nc;j++)
	{
		if(j%this->pilot_configurator.Dx==0)
		{
			interpolate_linear_col(j);
		}
		else if(j==Nc-1)
		{
			interpolate_linear_col(j);
		}
	}

	for(int j=0;j<Nc;j+=this->pilot_configurator.Dx)
	{
		if(j+this->pilot_configurator.Dx<Nc)
		{
			interpolate_bilinear_matrix(j,j+this->pilot_configurator.Dx,0,Nsymb-1);
		}
		else if(j!=Nc-1)
		{
			interpolate_bilinear_matrix(j,Nc-1,0,Nsymb-1);
		}
	}
}

double cl_ofdm::measure_variance(std::complex <double>*in)
{
	double variance=0;
	int pilot_index=0;
	std::complex <double> diff;
	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			if((ofdm_frame+i*Nc+j)->type==PILOT)
			{
				diff=*(in+i*Nc+j) -pilot_configurator.pilot_sequence[pilot_index];
				pilot_index++;
				variance+=pow(diff.real(),2)+pow(diff.imag(),2);
			}
		}

	}
	variance/=(double)pilot_index;

	return variance;
}

double cl_ofdm::measure_signal_stregth(std::complex <double>*in, int nItems)
{
	double signal_stregth=0;
	double signal_stregth_dbm=0;
	std::complex <double> value;

	for(int i=0;i<nItems;i++)
	{
		value=*(in+i);
		signal_stregth+=pow(value.real(),2)+pow(value.imag(),2);
	}
	signal_stregth/=nItems;

	signal_stregth_dbm=10.0*log10((signal_stregth)/0.001);

	return signal_stregth_dbm;
}

double cl_ofdm::measure_SNR(std::complex <double>*in_s, std::complex <double>*in_n, int nItems)
{
	double variance=0;
	double SNR=0;
	std::complex <double> diff;
	for(int i=0;i<nItems;i++)
	{
		diff=*(in_n+i)-*(in_s+i);
		variance+=pow(diff.real(),2)+pow(diff.imag(),2);
	}
	variance/=nItems;
	SNR=-10.0*log10(variance);
	return SNR;
}

void cl_ofdm::channel_equalizer(std::complex <double>* in, std::complex <double>* out)
{
	for(int i=0;i<Nsymb;i++)
	{
		for(int j=0;j<Nc;j++)
		{
			*(out+i*Nc+j)=*(in+i*Nc+j) / (estimated_channel+i*Nc+j)->value;
		}
	}

}

int cl_ofdm::time_sync(std::complex <double>*in, int size, int interpolation_rate, int location_to_return)
{

	double corss_corr=0;
	double norm_a=0;
	double norm_b=0;

	int *corss_corr_loc=new int[size];
	double *corss_corr_vals=new double[size];
	int return_val;

	std::complex <double> *a_c, *b_c;

	for(int i=0;i<size;i++)
	{
		corss_corr_loc[i]=-1;
		corss_corr_vals[i]=0;
	}

	for(int i=0;i<size-(this->Ngi+this->Nfft)*interpolation_rate;i++)
	{
		a_c=in+i;
		b_c=in+i+this->Nfft*interpolation_rate;
		corss_corr=0;
		norm_a=0;
		norm_b=0;
		for(int j=0;j<Nsymb;j++)
		{
			if(j<time_sync_Nsymb)
			{
				for(int m=0;m<this->Ngi*interpolation_rate;m++)
				{
					corss_corr+=a_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].real()*b_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].real();
					norm_a+=a_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].real()*a_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].real();
					norm_b+=b_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].real()*b_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].real();

					corss_corr+=a_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].imag()*b_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].imag();
					norm_a+=a_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].imag()*a_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].imag();
					norm_b+=b_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].imag()*b_c[m+j*(this->Ngi+this->Nfft)*interpolation_rate].imag();
				}
			}
		}
		corss_corr=corss_corr/sqrt(norm_a*norm_b);
		corss_corr_vals[i]=corss_corr;
		corss_corr_loc[i]=i;
	}
	double tmp;
	int tmp_int;
	for(int i=0;i<size-(this->Ngi+this->Nfft)*interpolation_rate-1;i++)
	{
		for(int j=0;j<size-(this->Ngi+this->Nfft)*interpolation_rate-1;j++)
		{
			if (corss_corr_vals[j]<corss_corr_vals[j+1])
			{
				tmp=corss_corr_vals[j];
				corss_corr_vals[j]=corss_corr_vals[j+1];
				corss_corr_vals[j+1]=tmp;

				tmp_int=corss_corr_loc[j];
				corss_corr_loc[j]=corss_corr_loc[j+1];
				corss_corr_loc[j+1]=tmp_int;
			}
		}
	}
	return_val=corss_corr_loc[location_to_return];
	if(corss_corr_loc!=NULL)
	{
		delete corss_corr_loc;
	}
	if(corss_corr_vals!=NULL)
	{
		delete corss_corr_vals;
	}
	return return_val;
}

int cl_ofdm::symbol_sync(std::complex <double>*in, int size, int interpolation_rate, int location_to_return)
{

	double corss_corr=0;
	double norm_a=0;
	double norm_b=0;

	int *corss_corr_loc=new int[Nsymb];
	double *corss_corr_vals=new double[Nsymb];
	int return_val;

	std::complex <double> *a_c, *b_c, a, b;

	for(int i=0;i<Nsymb;i++)
	{
		corss_corr_loc[i]=-1;
		corss_corr_vals[i]=0;
	}

	for(int i=0;i<Nsymb;i++)
	{
		a_c=in+i*(Nfft+Ngi)*interpolation_rate;
		b_c=in+i*(Nfft+Ngi)*interpolation_rate+(Nfft/2)*interpolation_rate;
		corss_corr=0;
		norm_a=0;
		norm_b=0;
		for(int m=0;m<(this->Nfft/2)*interpolation_rate;m++)
		{
			corss_corr+=a_c[m].real()*b_c[m].real();
			norm_a+=a_c[m].real()*a_c[m].real();
			norm_b+=b_c[m].real()*b_c[m].real();

			corss_corr+=a_c[m].imag()*b_c[m].imag();
			norm_a+=a_c[m].imag()*a_c[m].imag();
			norm_b+=b_c[m].imag()*b_c[m].imag();
		}
		corss_corr=corss_corr/sqrt(norm_a*norm_b);

		if(corss_corr<0)
		{
			corss_corr_vals[i]=-corss_corr;
		}
		else
		{
			corss_corr_vals[i]=corss_corr;
		}
		corss_corr_loc[i]=i;

	}
	double tmp;
	int tmp_int;
	for(int i=0;i<Nsymb-1;i++)
	{
		for(int j=0;j<Nsymb-1;j++)
		{
			if (corss_corr_vals[j]<corss_corr_vals[j+1])
			{
				tmp=corss_corr_vals[j];
				corss_corr_vals[j]=corss_corr_vals[j+1];
				corss_corr_vals[j+1]=tmp;

				tmp_int=corss_corr_loc[j];
				corss_corr_loc[j]=corss_corr_loc[j+1];
				corss_corr_loc[j+1]=tmp_int;
			}
		}
	}
	return_val=corss_corr_loc[location_to_return];
	if(corss_corr_loc!=NULL)
	{
		delete corss_corr_loc;
	}
	if(corss_corr_vals!=NULL)
	{
		delete corss_corr_vals;
	}
	return return_val;
}

void cl_ofdm::rational_resampler(std::complex <double>* in, int in_size, std::complex <double>* out, int rate, int interpolation_decimation)
{
	if (interpolation_decimation==DECIMATION)
	{
		int index=0;
		for(int i=0;i<in_size;i+=rate)
		{
			*(out+index)=*(in+i);
			index++;
		}
	}
	else if (interpolation_decimation==INTERPOLATION)
	{
		for(int i=0;i<in_size-1;i++)
		{
			for(int j=0;j<rate;j++)
			{
				*(out+i*rate+j)=interpolate_linear(*(in+i),0,*(in+i+1),rate,j);
			}
		}
		for(int j=0;j<rate;j++)
		{
			*(out+(in_size-1)*rate+j)=interpolate_linear(*(in+in_size-2),0,*(in+in_size-1),rate,rate+j);
		}
	}
}

void cl_ofdm::baseband_to_passband(std::complex <double>* in, int in_size, double* out, double sampling_frequency, double carrier_frequency, double carrier_amplitude,int interpolation_rate)
{
	double sampling_interval=1.0/sampling_frequency;
	std::complex <double> *data_interpolated= new std::complex <double>[in_size*interpolation_rate];
	rational_resampler( in, in_size, data_interpolated, interpolation_rate, INTERPOLATION);
	for(int i=0;i<in_size*interpolation_rate;i++)
	{
		out[i]=data_interpolated[i].real()*carrier_amplitude*cos(2*M_PI*carrier_frequency*(double)i * sampling_interval);
		out[i]+=data_interpolated[i].imag()*carrier_amplitude*sin(2*M_PI*carrier_frequency*(double)i * sampling_interval);
	}
	if(data_interpolated!=NULL)
	{
	delete data_interpolated;
	}
}
void cl_ofdm::passband_to_baseband(double* in, int in_size, std::complex <double>* out, double sampling_frequency, double carrier_frequency, double carrier_amplitude, int decimation_rate)
{
	double sampling_interval=1.0/sampling_frequency;

	std::complex <double> *l_data= new std::complex <double>[in_size];
	std::complex <double> *data_filtered= new std::complex <double>[in_size];

	for(int i=0;i<in_size;i++)
	{
		l_data[i].real(in[i]*carrier_amplitude*cos(2*M_PI*carrier_frequency*(double)i * sampling_interval));
		l_data[i].imag(in[i]*carrier_amplitude*sin(2*M_PI*carrier_frequency*(double)i * sampling_interval));
	}

	FIR.apply(l_data,data_filtered,in_size);

	rational_resampler(data_filtered, in_size, out, decimation_rate, DECIMATION);
	if(l_data!=NULL)
	{
	delete l_data;
	}
	if(data_filtered!=NULL)
	{
		delete data_filtered;
	}
}
