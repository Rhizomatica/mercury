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
#include "telecom_system.h"
#include <iostream>
cl_telecom_system::cl_telecom_system()
{
	time_sync_trials_max=20;
	receive_stats.time_sync_locked=NO;
	lock_time_sync=YES;
	operation_mode=BER_PLOT_baseband;
	receive_stats.phase_error_avg=0;
	receive_stats.freq_offset=0;
	receive_stats.freq_offset_locked=NO;
	receive_stats.SNR=-99.9;
	bit_interleaver_block_size=1;
	test_tx_AWGN_EsN0=500;
	test_tx_AWGN_EsN0_calibration=8;
	output_power_Watt=1;
	carrier_amplitude=sqrt(2.0);
	receive_stats.signal_stregth_dbm=-999;
	tcp_socket.set_type(TYPE_SERVER);
}


cl_telecom_system::~cl_telecom_system()
{
	if(filter_coefficients!=NULL)
	{
		delete filter_coefficients;
	}

}
cl_error_rate cl_telecom_system::baseband_test_EsN0(float EsN0,int max_frame_no)
{
	cl_error_rate lerror_rate;
	float power_normalization=sqrt((double)ofdm.Nfft);
	float sigma=1.0/sqrt(pow(10,(EsN0/10)));
	float variance=1.0/(pow(10,(EsN0/10)));
	int nVirtual_data;
	int nReal_data;
	int iterations_done;
	int delay=0;
	nVirtual_data=ldpc.N-data_container.nBits;
	nReal_data=data_container.nBits-ldpc.P;
	while(lerror_rate.Frames_total<max_frame_no)
	{
		for(int i=0;i<nReal_data;i++)
		{
			data_container.data[i]=rand()%2;
		}
		for(int i=0;i<nVirtual_data;i++)
		{
			data_container.data[nReal_data+i]=data_container.data[i];
		}

		ldpc.encode(data_container.data,data_container.encoded_data);

		for(int i=0;i<ldpc.P;i++)
		{
			data_container.encoded_data[nReal_data+i]=data_container.encoded_data[i+ldpc.K];
		}

		bit_interleaver(data_container.encoded_data,data_container.interleaved_data,data_container.nBits,bit_interleaver_block_size);

		psk.mod(data_container.interleaved_data,data_container.nBits,data_container.modulated_data);
		ofdm.framer(data_container.modulated_data,data_container.ofdm_framed_data);

		for(int i=0;i<data_container.Nsymb;i++)
		{
			ofdm.symbol_mod(&data_container.ofdm_framed_data[i*data_container.Nc],&data_container.ofdm_symbol_modulated_data[i*data_container.Nofdm]);
		}

		for(int j=0;j<data_container.Nofdm*data_container.Nsymb;j++)
		{
			data_container.ofdm_symbol_modulated_data[j]/=power_normalization;
		}


		awgn_channel.apply_with_delay(data_container.ofdm_symbol_modulated_data,data_container.baseband_data,sigma,data_container.Nofdm*data_container.Nsymb,0);


		for(int j=0;j<data_container.Nofdm*data_container.Nsymb;j++)
		{
			data_container.baseband_data[j]*=power_normalization;
		}

		for(int j=0;j<data_container.Nofdm*data_container.Nsymb;j++)
		{

			data_container.channaled_data[j]=data_container.baseband_data[j+delay*0];
		}

		for(int i=0;i<data_container.Nsymb;i++)
		{
			ofdm.symbol_demod(&data_container.channaled_data[i*data_container.Nofdm],&data_container.ofdm_symbol_demodulated_data[i*data_container.Nc]);
		}
		variance=ofdm.measure_variance(data_container.ofdm_symbol_demodulated_data);

		ofdm.channel_estimator_frame_time_frequency(data_container.ofdm_symbol_demodulated_data);
		ofdm.channel_equalizer(data_container.ofdm_symbol_demodulated_data,data_container.equalized_data);
		ofdm.deframer(data_container.equalized_data,data_container.ofdm_deframed_data);
		psk.demod(data_container.ofdm_deframed_data,data_container.nBits,data_container.demodulated_data,variance);

		bit_deinterleaver(data_container.demodulated_data,data_container.deinterleaved_data,data_container.nBits,bit_interleaver_block_size);


		for(int i=ldpc.P-1;i>=0;i--)
		{
			data_container.deinterleaved_data[i+nReal_data+nVirtual_data]=data_container.deinterleaved_data[i+nReal_data];
		}

		for(int i=0;i<nVirtual_data;i++)
		{
			data_container.deinterleaved_data[nReal_data+i]=data_container.deinterleaved_data[i];
		}


		iterations_done=ldpc.decode(data_container.deinterleaved_data,data_container.hd_decoded_data);


		lerror_rate.check(data_container.data,data_container.hd_decoded_data,nReal_data);
	}
	return lerror_rate;
}

cl_error_rate cl_telecom_system::passband_test_EsN0(float EsN0,int max_frame_no)
{
	cl_error_rate lerror_rate;
	float sigma=1.0/sqrt(pow(10,(EsN0/10)));
	int nReal_data=data_container.nBits-ldpc.P;

	while(lerror_rate.Frames_total<max_frame_no)
	{
		for(int i=0;i<nReal_data;i++)
		{
			data_container.data[i]=rand()%2;
		}
		this->transmit(data_container.data,data_container.passband_data);
		awgn_channel.apply_with_delay(data_container.passband_data,data_container.passband_delayed_data,sigma,(data_container.Nofdm*data_container.Nsymb)*this->frequency_interpolation_rate,(data_container.Nofdm+50)*frequency_interpolation_rate);
		this->receive(data_container.passband_delayed_data,data_container.hd_decoded_data);
		lerror_rate.check(data_container.data,data_container.hd_decoded_data,nReal_data);
	}
	return lerror_rate;
}

void cl_telecom_system::transmit(const int* data, double* out)
{
	int nVirtual_data=ldpc.N-data_container.nBits;
	int nReal_data=data_container.nBits-ldpc.P;
	float power_normalization=sqrt((double)ofdm.Nfft);

	for(int i=0;i<nReal_data;i++)
	{
		data_container.data[i]=data[i];
	}

	for(int i=0;i<nVirtual_data;i++)
	{
		data_container.data[nReal_data+i]=data_container.data[i];
	}

	ldpc.encode(data_container.data,data_container.encoded_data);

	for(int i=0;i<ldpc.P;i++)
	{
		data_container.encoded_data[nReal_data+i]=data_container.encoded_data[i+ldpc.K];
	}

	bit_interleaver(data_container.encoded_data,data_container.interleaved_data,data_container.nBits,bit_interleaver_block_size);

	psk.mod(data_container.interleaved_data,data_container.nBits,data_container.modulated_data);
	ofdm.framer(data_container.modulated_data,data_container.ofdm_framed_data);

	for(int i=0;i<data_container.Nsymb;i++)
	{
		ofdm.symbol_mod(&data_container.ofdm_framed_data[i*data_container.Nc],&data_container.ofdm_symbol_modulated_data[i*data_container.Nofdm]);
	}

	for(int j=0;j<data_container.Nofdm*data_container.Nsymb;j++)
	{
		data_container.ofdm_symbol_modulated_data[j]/=sqrt(output_power_Watt)*power_normalization;
	}

	ofdm.baseband_to_passband(data_container.ofdm_symbol_modulated_data,data_container.Nofdm*data_container.Nsymb,out,sampling_frequency,carrier_frequency,carrier_amplitude,frequency_interpolation_rate);

}
st_receive_stats cl_telecom_system::receive(const double* data, int* out)
{
	float variance;
	int nVirtual_data=ldpc.N-data_container.nBits;
	int nReal_data=data_container.nBits-ldpc.P;
	double freq_offset_measured=0;
	receive_stats.message_decoded=NO;
	receive_stats.sync_trials=0;

	ofdm.passband_to_baseband((double*)data,(data_container.Nofdm*data_container.buffer_Nsymb)*frequency_interpolation_rate,data_container.baseband_data_interpolated,sampling_frequency,carrier_frequency+receive_stats.freq_offset,carrier_amplitude,1,filter_coefficients,filter_nTaps);
	receive_stats.signal_stregth_dbm=ofdm.measure_signal_stregth(data_container.baseband_data_interpolated, (data_container.Nofdm*data_container.buffer_Nsymb)*frequency_interpolation_rate);

	while (receive_stats.sync_trials<time_sync_trials_max)
	{
		if(receive_stats.time_sync_locked==NO)
		{
			receive_stats.delay=ofdm.time_sync(&data_container.baseband_data_interpolated[data_container.Nofdm*frequency_interpolation_rate],2*data_container.Nofdm*data_container.interpolation_rate,data_container.interpolation_rate,receive_stats.sync_trials);
			if(receive_stats.delay>=data_container.Nofdm*data_container.interpolation_rate)
			{
				receive_stats.delay-=data_container.Nofdm*data_container.interpolation_rate;
			}

		}
		receive_stats.first_symbol_location=ofdm.symbol_sync(&data_container.baseband_data_interpolated[data_container.Nofdm*frequency_interpolation_rate+receive_stats.delay],(data_container.Nofdm*data_container.Nsymb)*data_container.interpolation_rate-receive_stats.delay,data_container.interpolation_rate,0);

		if(receive_stats.first_symbol_location==0)
		{

			ofdm.rational_resampler(&data_container.baseband_data_interpolated[data_container.Nofdm*frequency_interpolation_rate+receive_stats.delay], (data_container.Nofdm*data_container.Nsymb)*frequency_interpolation_rate, data_container.baseband_data, data_container.interpolation_rate, DECIMATION);

			for(int j=0;j<data_container.Nofdm*data_container.Nsymb;j++)
			{

				data_container.channaled_data[j]=data_container.baseband_data[j];
			}

			freq_offset_measured=ofdm.frequency_sync(&data_container.channaled_data[data_container.Ngi*data_container.interpolation_rate],bandwidth/(double)data_container.Nc);

			if(abs(freq_offset_measured)>ofdm.freq_offset_ignore_limit)
			{
				ofdm.passband_to_baseband((double*)data,(data_container.Nofdm*data_container.buffer_Nsymb)*frequency_interpolation_rate,data_container.baseband_data_interpolated,sampling_frequency,carrier_frequency+receive_stats.freq_offset+freq_offset_measured,carrier_amplitude,1,filter_coefficients,filter_nTaps);
				receive_stats.delay=ofdm.time_sync(&data_container.baseband_data_interpolated[data_container.Nofdm*frequency_interpolation_rate],2*data_container.Nofdm*data_container.interpolation_rate,data_container.interpolation_rate,0);
				if(receive_stats.delay>=data_container.Nofdm*data_container.interpolation_rate)
				{
					receive_stats.delay-=data_container.Nofdm*data_container.interpolation_rate;
				}
				ofdm.rational_resampler(&data_container.baseband_data_interpolated[data_container.Nofdm*frequency_interpolation_rate+receive_stats.delay], (data_container.Nofdm*data_container.Nsymb)*frequency_interpolation_rate, data_container.baseband_data, data_container.interpolation_rate, DECIMATION);
				for(int j=0;j<data_container.Nofdm*data_container.Nsymb;j++)
				{
					data_container.channaled_data[j]=data_container.baseband_data[j];
				}
			}
			for(int i=0;i<data_container.Nsymb;i++)
			{
				ofdm.symbol_demod(&data_container.channaled_data[i*data_container.Nofdm],&data_container.ofdm_symbol_demodulated_data[i*data_container.Nc]);
			}
			variance=ofdm.measure_variance(data_container.ofdm_symbol_demodulated_data);

			ofdm.channel_estimator_frame_time_frequency(data_container.ofdm_symbol_demodulated_data);
			ofdm.channel_equalizer(data_container.ofdm_symbol_demodulated_data,data_container.equalized_data);
			ofdm.deframer(data_container.equalized_data,data_container.ofdm_deframed_data);
			psk.demod(data_container.ofdm_deframed_data,data_container.nBits,data_container.demodulated_data,variance);

			bit_deinterleaver(data_container.demodulated_data,data_container.deinterleaved_data,data_container.nBits,bit_interleaver_block_size);

			for(int i=ldpc.P-1;i>=0;i--)
			{
				data_container.deinterleaved_data[i+nReal_data+nVirtual_data]=data_container.deinterleaved_data[i+nReal_data];
			}

			for(int i=0;i<nVirtual_data;i++)
			{
				data_container.deinterleaved_data[nReal_data+i]=data_container.deinterleaved_data[i];
			}


			receive_stats.iterations_done=ldpc.decode(data_container.deinterleaved_data,data_container.hd_decoded_data);
			for(int i=0;i<nReal_data;i++)
			{
				*(out+i)=data_container.hd_decoded_data[i];
			}
			if(receive_stats.iterations_done>(ldpc.nIteration_max-1))
			{
				receive_stats.SNR=-99.9;
				receive_stats.message_decoded=NO;
				if(receive_stats.time_sync_locked==YES)
				{
					receive_stats.time_sync_locked=NO;
					if(ofdm.print_time_sync_status==YES)
					{
						std::cout<<"Time Sync Not Locked"<<std::endl;
					}
					receive_stats.sync_trials=0;
				}
				else
				{
					receive_stats.sync_trials++;
				}
			}
			else
			{
				for(int i=0;i<nVirtual_data;i++)
				{
					data_container.hd_decoded_data[nReal_data+i]=data_container.hd_decoded_data[i];
				}
				ldpc.encode(data_container.hd_decoded_data,data_container.encoded_data);
				for(int i=0;i<ldpc.P;i++)
				{
					data_container.encoded_data[nReal_data+i]=data_container.encoded_data[i+ldpc.K];
				}
				bit_interleaver(data_container.encoded_data,data_container.interleaved_data,data_container.nBits,bit_interleaver_block_size);
				psk.mod(data_container.interleaved_data,data_container.nBits,data_container.modulated_data);
				receive_stats.SNR=ofdm.measure_SNR(data_container.ofdm_deframed_data,data_container.modulated_data,data_container.nData);

				receive_stats.message_decoded=YES;
				receive_stats.freq_offset+=freq_offset_measured;

				if(receive_stats.time_sync_locked==NO&& lock_time_sync==YES)
				{
					receive_stats.time_sync_locked=YES;
					if(ofdm.print_time_sync_status==YES)
					{
						std::cout<<"Time Sync Locked"<<std::endl;
					}
				}
				break;
			}
		}
		else
		{
			receive_stats.sync_trials++;
		}
	}
	if(ldpc.print_nIteration==YES)
	{
		std::cout<<"decoded in "<< receive_stats.iterations_done<<" iterations."<<std::endl;
	}
	return receive_stats;
}

void cl_telecom_system::FIR_designer()
{
	filter_nTaps=(int)(4.0/(filter_transition_bandwidth/(sampling_frequency/2.0)));

	if(filter_nTaps%2==0)
	{
		filter_nTaps++;
	}
	filter_coefficients = new double[filter_nTaps];
	double sampling_interval=1.0/(sampling_frequency);
	double temp;

	filter_coefficients[filter_nTaps/2]=1;
	for(int i=0;i<filter_nTaps/2;i++)
	{
		temp=2*M_PI*filter_cut_frequency*(double)(filter_nTaps/2-i) *sampling_interval;

		filter_coefficients[i]=sin(temp)/temp;
		filter_coefficients[filter_nTaps-i-1]=filter_coefficients[i];
	}

	if(filter_window==HAMMING)
	{
		for(int i=0;i<filter_nTaps;i++)
		{
			filter_coefficients[i]*=0.54-0.46*cos(2.0*M_PI*(double)i/(filter_nTaps-1));
		}
	}
	else if(filter_window==HANNING)
	{
		for(int i=0;i<filter_nTaps;i++)
		{
			filter_coefficients[i]*=0.5-0.5*cos(2.0*M_PI*(double)i/(filter_nTaps-1));
		}
	}
	else if(filter_window==BLACKMAN)
	{
		for(int i=0;i<filter_nTaps;i++)
		{
			filter_coefficients[i]*=0.42-0.5*cos(2.0*M_PI*(double)i/filter_nTaps)+0.08*cos(4.0*M_PI*(double)i/filter_nTaps);
		}
	}
}

void cl_telecom_system::calculate_parameters()
{
	LDPC_real_CR=((double)ofdm.pilot_configurator.nData*log2(M)-(double)ldpc.P)/((double)ofdm.pilot_configurator.nData*log2(M));
	Tu= ofdm.Nc/bandwidth;
	Ts= Tu*(1.0+ofdm.gi);
	Tf= Ts*ofdm.Nsymb;
	rb= ofdm.pilot_configurator.nData * log2(M) /Tf;
	rbc= rb*LDPC_real_CR;
	Shannon_limit= 10.0*log10((pow(2,(rb*ldpc.rate)/bandwidth)-1)*log2(M)*bandwidth/rb);
	sampling_frequency=frequency_interpolation_rate*(bandwidth/ofdm.Nc)*ofdm.Nfft;
}

void cl_telecom_system::init()
{

	if(bandwidth==2500)
	{
		if(ofdm.Nc==AUTO_SELLECT)
		{
			ofdm.Nc=50;
		}
		if(ofdm.Nsymb==AUTO_SELLECT)
		{
			if(M==MOD_BPSK){ofdm.Nsymb=35;}
			if(M==MOD_QPSK){ofdm.Nsymb=17;}
			if(M==MOD_8QAM){ofdm.Nsymb=12;}
			if(M==MOD_16QAM){ofdm.Nsymb=9;}
			if(M==MOD_32QAM){ofdm.Nsymb=7;}
			if(M==MOD_64QAM){ofdm.Nsymb=6;}
		}

		if(ofdm.pilot_configurator.Dx==AUTO_SELLECT)
		{
			if(M==MOD_BPSK){ofdm.pilot_configurator.Dx=7;}
			if(M==MOD_QPSK){ofdm.pilot_configurator.Dx=7;}
			if(M==MOD_8QAM){ofdm.pilot_configurator.Dx=7;}
			if(M==MOD_16QAM){ofdm.pilot_configurator.Dx=7;}
			if(M==MOD_32QAM){ofdm.pilot_configurator.Dx=7;}
			if(M==MOD_64QAM){ofdm.pilot_configurator.Dx=7;}
		}

		if(ofdm.pilot_configurator.Dy==AUTO_SELLECT)
		{
			if(M==MOD_BPSK){ofdm.pilot_configurator.Dy=2;}
			if(M==MOD_QPSK){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_8QAM){ofdm.pilot_configurator.Dy=2;}
			if(M==MOD_16QAM){ofdm.pilot_configurator.Dy=2;}
			if(M==MOD_32QAM){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_64QAM){ofdm.pilot_configurator.Dy=2;}
		}
	}
	else if(bandwidth==2300)
	{
		if(ofdm.Nc==AUTO_SELLECT)
		{
			ofdm.Nc=46;
		}
		if(ofdm.Nsymb==AUTO_SELLECT)
		{
			if(M==MOD_BPSK){ofdm.Nsymb=37;}
			if(M==MOD_QPSK){ofdm.Nsymb=19;}
			if(M==MOD_8QAM){ofdm.Nsymb=13;}
			if(M==MOD_16QAM){ofdm.Nsymb=10;}
			if(M==MOD_32QAM){ofdm.Nsymb=8;}
			if(M==MOD_64QAM){ofdm.Nsymb=7;}
		}

		if(ofdm.pilot_configurator.Dx==AUTO_SELLECT)
		{
			if(M==MOD_BPSK){ofdm.pilot_configurator.Dx=5;}
			if(M==MOD_QPSK){ofdm.pilot_configurator.Dx=5;}
			if(M==MOD_8QAM){ofdm.pilot_configurator.Dx=5;}
			if(M==MOD_16QAM){ofdm.pilot_configurator.Dx=5;}
			if(M==MOD_32QAM){ofdm.pilot_configurator.Dx=5;}
			if(M==MOD_64QAM){ofdm.pilot_configurator.Dx=5;}
		}

		if(ofdm.pilot_configurator.Dy==AUTO_SELLECT)
		{
			if(M==MOD_BPSK){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_QPSK){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_8QAM){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_16QAM){ofdm.pilot_configurator.Dy=2;}
			if(M==MOD_32QAM){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_64QAM){ofdm.pilot_configurator.Dy=2;}
		}
	}

	ofdm.init();
	ldpc.init();
	calculate_parameters();
	FIR_designer();
	data_container.set_size(ofdm.pilot_configurator.nData,ofdm.Nc,M,ofdm.Nfft,ofdm.Nfft*(1+ofdm.gi),ofdm.Nsymb,frequency_interpolation_rate);

	std::cout<<"nBits="<<data_container.nBits<<std::endl;
	std::cout<<"Shannon_limit="<<Shannon_limit<<" dB"<<std::endl;
	std::cout<<"rbc="<<rbc/1024.0<<" kbps"<<std::endl;
	std::cout<<"sampling_frequency="<<sampling_frequency<<" Sps"<<std::endl;
}
