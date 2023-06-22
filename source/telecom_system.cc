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
	tcp_socket_test.set_type(TYPE_SERVER);
	sampling_frequency=0;
	Shannon_limit=0;
	rbc=0;
	rb=0;
	Tf=0;
	frequency_interpolation_rate=0;
	Ts=0;
	Tu=0;
	LDPC_real_CR=0;
	bandwidth=0;
	M=0;
	carrier_frequency=0;
	current_configuration=CONFIG_0;
	last_configuration=CONFIG_0;
}


cl_telecom_system::~cl_telecom_system()
{

}
cl_error_rate cl_telecom_system::baseband_test_EsN0(float EsN0,int max_frame_no)
{
	cl_error_rate lerror_rate;
	float power_normalization=sqrt((double)ofdm.Nfft);
	float sigma=1.0/sqrt(pow(10,(EsN0/10)));
	float variance=1.0/(pow(10,(EsN0/10)));
	int nVirtual_data;
	int nReal_data;
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


		ldpc.decode(data_container.deinterleaved_data,data_container.hd_decoded_data);


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

	ofdm.passband_to_baseband((double*)data,(data_container.Nofdm*data_container.buffer_Nsymb)*frequency_interpolation_rate,data_container.baseband_data_interpolated,sampling_frequency,carrier_frequency+receive_stats.freq_offset,carrier_amplitude,1);
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
				ofdm.passband_to_baseband((double*)data,(data_container.Nofdm*data_container.buffer_Nsymb)*frequency_interpolation_rate,data_container.baseband_data_interpolated,sampling_frequency,carrier_frequency+receive_stats.freq_offset+freq_offset_measured,carrier_amplitude,1);
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

			int all_zero=YES;
			for(int i=0;i<nReal_data;i++)
			{
				if(data_container.hd_decoded_data[i]!=0)
				{
					all_zero=NO;
				}
			}
			if(receive_stats.iterations_done>(ldpc.nIteration_max-1) || all_zero==YES)
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
	ofdm.FIR.sampling_frequency=this->sampling_frequency;
	ofdm.FIR.design();
	data_container.set_size(ofdm.pilot_configurator.nData,ofdm.Nc,M,ofdm.Nfft,ofdm.Nfft*(1+ofdm.gi),ofdm.Nsymb,frequency_interpolation_rate);

	std::cout<<"nBits="<<data_container.nBits<<std::endl;
	std::cout<<"Shannon_limit="<<Shannon_limit<<" dB"<<std::endl;
	std::cout<<"rbc="<<rbc/1024.0<<" kbps"<<std::endl;
	std::cout<<"sampling_frequency="<<sampling_frequency<<" Sps"<<std::endl;
}

void cl_telecom_system::deinit()
{
	data_container.deinit();
	ofdm.FIR.deinit();
	ldpc.deinit();
	ofdm.deinit();
}

void cl_telecom_system::TX_TEST_process_main()
{
	float sigma=1.0/sqrt(pow(10,((test_tx_AWGN_EsN0+test_tx_AWGN_EsN0_calibration)/10)));
	for(int i=0;i<data_container.nBits-ldpc.P;i++)
	{
		data_container.data[i]=rand()%2;
	}
	transmit(data_container.data,data_container.passband_data);
	awgn_channel.apply_with_delay(data_container.passband_data,data_container.passband_data,sigma,data_container.Nofdm*data_container.Nsymb*data_container.interpolation_rate,0);
	speaker.transfere(data_container.passband_data,data_container.Nofdm*data_container.interpolation_rate*ofdm.Nsymb);
}
void cl_telecom_system::RX_TEST_process_main()
{
	std::complex <double> data_fft[ofdm.pilot_configurator.nData];
	int tmp[N_MAX];
	int counter=0;
	int constellation_plot_counter=0;
	int constellation_plot_nFrames=1;
	float contellation[ofdm.pilot_configurator.nData*constellation_plot_nFrames][2]={0};

	std::cout << std::fixed;
	std::cout << std::setprecision(1);

	if(data_container.data_ready==1)
	{


		if(data_container.frames_to_read==0)
		{
			st_receive_stats test=receive((const double*)data_container.passband_delayed_data,tmp);
			shift_left(data_container.passband_delayed_data, data_container.Nofdm*data_container.interpolation_rate*data_container.buffer_Nsymb, data_container.Nofdm*data_container.interpolation_rate);

			for(int i=0;i<ofdm.pilot_configurator.nData;i++)
			{
				data_fft[i]=data_container.ofdm_deframed_data[i];
			}
			if (test.message_decoded==YES)
			{
				data_container.frames_to_read=data_container.Nsymb;
				std::cout<<"decoded in "<<test.iterations_done<<" data=";
				for(int i=0;i<10;i++)
				{
					std::cout<<tmp[i]<<",";
				}
				std::cout<<"delay="<<test.delay;
				std::cout<<" 1st symb loc="<<test.first_symbol_location;
				std::cout<<" freq_offset="<<receive_stats.freq_offset;
				std::cout<<" SNR="<<receive_stats.SNR<<" dB";
				std::cout<<" Signal Strength="<<receive_stats.signal_stregth_dbm<<" dBm ";
				std::cout<<std::endl;
			}
			else
			{
				std::cout<<"Syncing.. ";
				std::cout<<"delay="<<test.delay;
				std::cout<<" 1st symb loc="<<test.first_symbol_location;
				std::cout<<" freq_offset="<<receive_stats.freq_offset;
				std::cout<<" Signal Strength="<<receive_stats.signal_stregth_dbm<<" dBm ";
				std::cout<<std::endl;
			}

			for(int i=0;i<ofdm.pilot_configurator.nData;i++)
			{
				contellation[constellation_plot_counter*ofdm.pilot_configurator.nData+i][0]=data_fft[i].real();
				contellation[constellation_plot_counter*ofdm.pilot_configurator.nData+i][1]=data_fft[i].imag();
			}

			constellation_plot_counter++;

			if(constellation_plot_counter==constellation_plot_nFrames)
			{
				constellation_plot_counter=0;
				plot.plot_constellation(&contellation[0][0],ofdm.pilot_configurator.nData*constellation_plot_nFrames);
			}

			counter=0;
		}
		data_container.data_ready=0;
		counter++;

	}
	else
	{
		usleep(50000);
	}
}
void cl_telecom_system::TX_TCP_process_main()
{
	long int timeout_counter_ms=tcp_socket_test.timeout_ms;
	if (tcp_socket_test.get_status()==TCP_STATUS_ACCEPTED)
	{
		if(tcp_socket_test.receive()>0)
		{
			timeout_counter_ms=tcp_socket_test.timeout_ms;
			for(int i=0;i<tcp_socket_test.message->length;i++)
			{
				data_container.data[i]=tcp_socket_test.message->buffer[i];
			}

			for(int i=tcp_socket_test.message->length;i<data_container.nBits-ldpc.P;i++)
			{
				data_container.data[i]=rand()%2;
			}

			transmit(data_container.data,data_container.passband_data);
			speaker.transfere(data_container.passband_data,data_container.Nofdm*data_container.interpolation_rate*ofdm.Nsymb);
			speaker.transfere(data_container.passband_data,data_container.Nofdm*data_container.interpolation_rate*ofdm.Nsymb);


			//prepare the reply
			tcp_socket_test.message->buffer[0]='O';
			tcp_socket_test.message->buffer[1]='K';
			tcp_socket_test.message->length=2;
			tcp_socket_test.transmit();
		}
		else
		{
			timeout_counter_ms--;
			usleep(1000);
			if(timeout_counter_ms<1)
			{
				tcp_socket_test.check_incomming_connection();
				if (tcp_socket_test.get_status()==TCP_STATUS_ACCEPTED)
				{
					timeout_counter_ms=tcp_socket_test.timeout_ms;
				}
				else
				{
					usleep(1000);
				}
			}
		}

	}
	else
	{
		tcp_socket_test.check_incomming_connection();
		if (tcp_socket_test.get_status()==TCP_STATUS_ACCEPTED)
		{
			timeout_counter_ms=tcp_socket_test.timeout_ms;
		}
		else
		{
			usleep(1000);
		}
	}
}
void cl_telecom_system::RX_TCP_process_main()
{
	int received_message[N_MAX];
	std::cout << std::fixed;
	std::cout << std::setprecision(1);
	long timeout_counter_ms=tcp_socket_test.timeout_ms;
	st_receive_stats received_message_stats;
	if(data_container.data_ready==1)
	{
		data_container.data_ready=0;

		if(data_container.frames_to_read==0)
		{
			received_message_stats=receive((const double*)data_container.passband_delayed_data,received_message);
			shift_left(data_container.passband_delayed_data, data_container.Nofdm*data_container.interpolation_rate*data_container.buffer_Nsymb, data_container.Nofdm*data_container.interpolation_rate);


			if (received_message_stats.message_decoded==YES)
			{
				data_container.frames_to_read=data_container.Nsymb;

				if (tcp_socket_test.get_status()==TCP_STATUS_ACCEPTED)
				{

					for(int i=0;i<data_container.nBits;i++)
					{
						tcp_socket_test.message->buffer[i]=received_message[i];
					}
					tcp_socket_test.message->length=data_container.nBits;
					tcp_socket_test.transmit();
				}

				std::cout<<"decoded in "<<received_message_stats.iterations_done<<" data=";
				for(int i=0;i<10;i++)
				{
					std::cout<<received_message[i]<<",";
				}
				std::cout<<"delay="<<received_message_stats.delay;
				std::cout<<" 1st symb loc="<<received_message_stats.first_symbol_location;
				std::cout<<" freq_offset="<<receive_stats.freq_offset;
				std::cout<<" SNR="<<receive_stats.SNR<<" dB";
				std::cout<<" Signal Strength="<<receive_stats.signal_stregth_dbm<<" dBm ";

				std::cout<<std::endl;
			}
		}
	}
	if (tcp_socket_test.get_status()==TCP_STATUS_ACCEPTED)
	{
		if(tcp_socket_test.receive()>0)
		{
			timeout_counter_ms=tcp_socket_test.timeout_ms;
		}
		else
		{
			timeout_counter_ms--;
			usleep(1000);
			if(timeout_counter_ms<1)
			{
				tcp_socket_test.check_incomming_connection();
				if (tcp_socket_test.get_status()==TCP_STATUS_ACCEPTED)
				{
					timeout_counter_ms=tcp_socket_test.timeout_ms;
				}
				else
				{
					usleep(1000);
				}
			}
		}

	}
	else
	{
		tcp_socket_test.check_incomming_connection();
		if (tcp_socket_test.get_status()==TCP_STATUS_ACCEPTED)
		{
			timeout_counter_ms=tcp_socket_test.timeout_ms;
		}
		else
		{
			usleep(1000);
		}
	}
}
void cl_telecom_system::BER_PLOT_baseband_process_main()
{
	plot.open("BER");
	plot.reset("BER");
	int nPoints=25;
	float data_plot[nPoints][2];
	float data_plot_theo[nPoints][2];
	output_power_Watt=1;

	for(int ind=0;ind<nPoints;ind++)
	{
		float EsN0=(float)ind/1.0;

		data_plot[ind][0]=EsN0;

		data_plot[ind][1]=baseband_test_EsN0(EsN0,100).BER;

		data_plot_theo[ind][0]=EsN0 ;

		if(M==MOD_BPSK)
		{
			data_plot_theo[ind][1]=0.5*erfc(sqrt(pow(10,EsN0/10)));
		}
		else
		{
			data_plot_theo[ind][1]=(2.0/log2(M))*(1.0-1.0/sqrt(M))*erfc(sqrt(((3.0* log2(M))/(2.0*(M-1))) *pow(10,EsN0/10)/log2(M)));
		}
		std::cout<<EsN0<<";"<<data_plot[ind][1]<<std::endl;
	}

	plot.plot("BER Simulation",&data_plot[0][0],nPoints,"BER theoretical",&data_plot_theo[0][0],nPoints);
	plot.close();
}
void cl_telecom_system::BER_PLOT_passband_process_main()
{
	plot.open("BER");
	plot.reset("BER");
	int nPoints=25;
	float data_plot[nPoints][2];
	float data_plot_theo[nPoints][2];
	output_power_Watt=1;

	for(int ind=0;ind<nPoints;ind++)
	{
		float EsN0=(float)ind/1.0;

		data_plot[ind][0]=EsN0;

		data_plot[ind][1]=passband_test_EsN0(EsN0,100).BER;

		data_plot_theo[ind][0]=EsN0 ;

		if(M==MOD_BPSK)
		{
			data_plot_theo[ind][1]=0.5*erfc(sqrt(pow(10,EsN0/10)));
		}
		else
		{
			data_plot_theo[ind][1]=(2.0/log2(M))*(1.0-1.0/sqrt(M))*erfc(sqrt(((3.0* log2(M))/(2.0*(M-1))) *pow(10,EsN0/10)/log2(M)));
		}
		std::cout<<EsN0<<";"<<data_plot[ind][1]<<std::endl;
	}

	plot.plot("BER Simulation",&data_plot[0][0],nPoints,"BER theoretical",&data_plot_theo[0][0],nPoints);
	plot.close();
}

void cl_telecom_system::load_configuration()
{
	this->load_configuration(default_configurations_telecom_system.current_configuration);
}

void cl_telecom_system::load_configuration(int configuration)
{
	if(configuration!=CONFIG_0 && configuration!=CONFIG_1 && configuration!=CONFIG_2 && configuration!=CONFIG_3 && configuration!=CONFIG_4 && configuration!=CONFIG_5 && configuration!=CONFIG_6)
	{
		return;
	}

	last_configuration=current_configuration;
	current_configuration=configuration;

	microphone.deinit();
	speaker.deinit();
	this->deinit();

	if(configuration==CONFIG_0)
	{
		M=MOD_BPSK;
		ldpc.rate=2/16.0;
	}
	else if(configuration==CONFIG_1)
	{
		M=MOD_BPSK;
		ldpc.rate=14/16.0;
	}
	else if(configuration==CONFIG_2)
	{
		M=MOD_QPSK;
		ldpc.rate=14/16.0;
	}
	else if(configuration==CONFIG_3)
	{
		M=MOD_8QAM;
		ldpc.rate=14/16.0;
	}
	else if(configuration==CONFIG_4)
	{
		M=MOD_16QAM;
		ldpc.rate=14/16.0;
	}
	else if(configuration==CONFIG_5)
	{
		M=MOD_32QAM;
		ldpc.rate=14/16.0;
	}
	else if(configuration==CONFIG_6)
	{
		M=MOD_64QAM;
		ldpc.rate=14/16.0;
	}


	awgn_channel.set_seed(rand());
	test_tx_AWGN_EsN0_calibration=default_configurations_telecom_system.test_tx_AWGN_EsN0_calibration;
	test_tx_AWGN_EsN0=default_configurations_telecom_system.test_tx_AWGN_EsN0;

	tcp_socket_test.port=default_configurations_telecom_system.tcp_socket_test_port;
	tcp_socket_test.timeout_ms=default_configurations_telecom_system.tcp_socket_test_timeout_ms;

	ofdm.Nc=default_configurations_telecom_system.ofdm_Nc;
	ofdm.Nfft=default_configurations_telecom_system.ofdm_Nfft;
	ofdm.gi=default_configurations_telecom_system.ofdm_gi;
	ofdm.Nsymb=default_configurations_telecom_system.ofdm_Nsymb;
	ofdm.pilot_configurator.Dx=default_configurations_telecom_system.ofdm_pilot_configurator_Dx;
	ofdm.pilot_configurator.Dy=default_configurations_telecom_system.ofdm_pilot_configurator_Dy;
	ofdm.pilot_configurator.first_row=default_configurations_telecom_system.ofdm_pilot_configurator_first_row;
	ofdm.pilot_configurator.last_row=default_configurations_telecom_system.ofdm_pilot_configurator_last_row;
	ofdm.pilot_configurator.first_col=default_configurations_telecom_system.ofdm_pilot_configurator_first_col;
	ofdm.pilot_configurator.second_col=default_configurations_telecom_system.ofdm_pilot_configurator_second_col;
	ofdm.pilot_configurator.last_col=default_configurations_telecom_system.ofdm_pilot_configurator_last_col;
	ofdm.pilot_configurator.pilot_boost=default_configurations_telecom_system.ofdm_pilot_configurator_pilot_boost;
	ofdm.pilot_configurator.first_row_zeros=default_configurations_telecom_system.ofdm_pilot_configurator_first_row_zeros;
	ofdm.print_time_sync_status=default_configurations_telecom_system.ofdm_print_time_sync_status;
	ofdm.freq_offset_ignore_limit=default_configurations_telecom_system.ofdm_freq_offset_ignore_limit;



	ldpc.standard=default_configurations_telecom_system.ldpc_standard;
	ldpc.framesize=default_configurations_telecom_system.ldpc_framesize;

	ldpc.decoding_algorithm=default_configurations_telecom_system.ldpc_decoding_algorithm;
	ldpc.GBF_eta=default_configurations_telecom_system.ldpc_GBF_eta;
	ldpc.nIteration_max=default_configurations_telecom_system.ldpc_nIteration_max;
	ldpc.print_nIteration=default_configurations_telecom_system.ldpc_print_nIteration;

	bandwidth=default_configurations_telecom_system.bandwidth;
	time_sync_trials_max=default_configurations_telecom_system.time_sync_trials_max;
	lock_time_sync=default_configurations_telecom_system.lock_time_sync;
	frequency_interpolation_rate=default_configurations_telecom_system.frequency_interpolation_rate;
	carrier_frequency=default_configurations_telecom_system.carrier_frequency;
	output_power_Watt=default_configurations_telecom_system.output_power_Watt;
	ofdm.FIR.filter_window=default_configurations_telecom_system.ofdm_FIR_filter_window;
	ofdm.FIR.filter_transition_bandwidth=default_configurations_telecom_system.ofdm_FIR_filter_transition_bandwidth;
	ofdm.FIR.filter_cut_frequency=default_configurations_telecom_system.ofdm_FIR_filter_cut_frequency;


	plot.folder=default_configurations_telecom_system.plot_folder;
	plot.plot_active=default_configurations_telecom_system.plot_plot_active;


	microphone.dev_name=default_configurations_telecom_system.microphone_dev_name;
	speaker.dev_name=default_configurations_telecom_system.speaker_dev_name;


	data_container.sound_device_ptr=(void*)&microphone;
	microphone.type=default_configurations_telecom_system.microphone_type;
	microphone.channels=default_configurations_telecom_system.microphone_channels;
	microphone.data_container_ptr=&data_container;


	speaker.type=default_configurations_telecom_system.speaker_type;
	speaker.channels=default_configurations_telecom_system.speaker_channels;
	speaker.frames_to_leave_transmit_fct=default_configurations_telecom_system.speaker_frames_to_leave_transmit_fct;

	psk.set_predefined_constellation(M);
	this->init();

	bit_interleaver_block_size=data_container.nBits/10;
	ofdm.time_sync_Nsymb=ofdm.Nsymb;

	microphone.baudrate=sampling_frequency;
	microphone.nbuffer_Samples=2 * ofdm.Nfft*(1+ofdm.gi)*frequency_interpolation_rate*ofdm.Nsymb;
	microphone.frames_per_period=ofdm.Nfft*(1+ofdm.gi)*frequency_interpolation_rate;
	microphone.init();

	speaker.baudrate=sampling_frequency;
	speaker.nbuffer_Samples=2 * ofdm.Nfft*(1+ofdm.gi)*frequency_interpolation_rate*ofdm.Nsymb;
	speaker.frames_per_period=ofdm.Nfft*(1+ofdm.gi)*frequency_interpolation_rate;
	speaker.init();
}

void cl_telecom_system::return_to_last_configuration()
{
	int tmp;
	this->load_configuration(last_configuration);
	tmp= last_configuration;
	last_configuration=current_configuration;
	current_configuration=tmp;
}

char cl_telecom_system::get_configuration(double SNR)
{
	char configuration;

	if(SNR>40)
	{
		configuration=CONFIG_6;
	}
	else if(SNR>35)
	{
		configuration=CONFIG_5;
	}
	else if(SNR>33)
	{
		configuration=CONFIG_4;
	}
	else if(SNR>30)
	{
		configuration=CONFIG_3;
	}
	else if(SNR>20)
	{
		configuration=CONFIG_2;
	}
	else if(SNR>10)
	{
		configuration=CONFIG_1;
	}
	else
	{
		configuration=CONFIG_0;
	}

	return configuration;
}
