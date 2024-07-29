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

#include "physical_layer/telecom_system.h"

cl_telecom_system::cl_telecom_system()
{
	receive_stats.iterations_done=-1;
	receive_stats.delay=0;
	receive_stats.delay_of_last_decoded_message=-1;
	receive_stats.time_peak_symb_location=0;
	receive_stats.time_peak_subsymb_location=0;
	receive_stats.sync_trials=0;
	receive_stats.phase_error_avg=0;
	receive_stats.freq_offset=0;
	receive_stats.freq_offset_of_last_decoded_message=0;
	receive_stats.message_decoded=NO;
	receive_stats.SNR=-99.9;
	receive_stats.signal_stregth_dbm=-999;

	time_sync_trials_max=20;
	use_last_good_time_sync=NO;
	use_last_good_freq_offset=NO;
	operation_mode=BER_PLOT_baseband;
	bit_interleaver_block_size=1;
	time_freq_interleaver_block_size=1;
	output_power_Watt=1;
	carrier_amplitude=sqrt(2.0);
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
	current_configuration=CONFIG_NONE;
	last_configuration=CONFIG_NONE;
	outer_code=NO_OUTER_CODE;
	outer_code_reserved_bits=0;
	bit_energy_dispersal_seed=0;
	pre_equalization_channel=NULL;
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

	int constellation_plot_counter=0;
	int constellation_plot_nFrames=10;
	float contellation[ofdm.pilot_configurator.nData*constellation_plot_nFrames][2]={0};

	while(lerror_rate.Frames_total<max_frame_no)
	{
		for(int i=0;i<nReal_data;i++)
		{
			data_container.data_bit[i]=rand()%2;
		}
		for(int i=0;i<nVirtual_data;i++)
		{
			data_container.data_bit[nReal_data+i]=data_container.data_bit[i];
		}

		ldpc.encode(data_container.data_bit,data_container.encoded_data);

		for(int i=0;i<ldpc.P;i++)
		{
			data_container.encoded_data[nReal_data+i]=data_container.encoded_data[i+ldpc.K];
		}

		interleaver(data_container.encoded_data,data_container.bit_interleaved_data,data_container.nBits,bit_interleaver_block_size);

		psk.mod(data_container.bit_interleaved_data,data_container.nBits,data_container.modulated_data);
		interleaver(data_container.modulated_data, data_container.ofdm_time_freq_interleaved_data, data_container.nData, time_freq_interleaver_block_size);
		ofdm.framer(data_container.ofdm_time_freq_interleaved_data,data_container.ofdm_framed_data);

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

		for(int i=0;i<data_container.Nsymb;i++)
		{
			ofdm.symbol_demod(&data_container.baseband_data[delay*0+i*data_container.Nofdm],&data_container.ofdm_symbol_demodulated_data[i*data_container.Nc]);
		}

		if(ofdm.channel_estimator==ZERO_FORCE)
		{
			ofdm.ZF_channel_estimator(data_container.ofdm_symbol_demodulated_data);
		}
		else if (ofdm.channel_estimator==LEAST_SQUARE)
		{
			ofdm.LS_channel_estimator(data_container.ofdm_symbol_demodulated_data);
		}

		if(ofdm.channel_estimator_amplitude_restoration==YES)
		{
			ofdm.restore_channel_amplitude();
			ofdm.channel_equalizer_without_amplitude_restoration(data_container.ofdm_symbol_demodulated_data,data_container.equalized_data_without_amplitude_restoration);
			ofdm.deframer(data_container.equalized_data_without_amplitude_restoration,data_container.ofdm_deframed_data_without_amplitude_restoration);
		}

		ofdm.channel_equalizer(data_container.ofdm_symbol_demodulated_data,data_container.equalized_data);

		variance=ofdm.measure_variance(data_container.ofdm_symbol_demodulated_data);

		ofdm.deframer(data_container.equalized_data,data_container.ofdm_deframed_data);
		deinterleaver(data_container.ofdm_deframed_data, data_container.ofdm_time_freq_deinterleaved_data, data_container.nData, time_freq_interleaver_block_size);
		psk.demod(data_container.ofdm_time_freq_deinterleaved_data,data_container.nBits,data_container.demodulated_data,variance);

		deinterleaver(data_container.demodulated_data,data_container.deinterleaved_data,data_container.nBits,bit_interleaver_block_size);


		for(int i=ldpc.P-1;i>=0;i--)
		{
			data_container.deinterleaved_data[i+nReal_data+nVirtual_data]=data_container.deinterleaved_data[i+nReal_data];
		}

		for(int i=0;i<nVirtual_data;i++)
		{
			data_container.deinterleaved_data[nReal_data+i]=data_container.deinterleaved_data[i];
		}


		ldpc.decode(data_container.deinterleaved_data,data_container.hd_decoded_data_bit);

		if(ofdm.channel_estimator_amplitude_restoration==YES)
		{
			for(int i=0;i<ofdm.pilot_configurator.nData;i++)
			{
				contellation[constellation_plot_counter*ofdm.pilot_configurator.nData+i][0]=data_container.ofdm_deframed_data_without_amplitude_restoration[i].real();
				contellation[constellation_plot_counter*ofdm.pilot_configurator.nData+i][1]=data_container.ofdm_deframed_data_without_amplitude_restoration[i].imag();
			}
		}
		else
		{
			for(int i=0;i<ofdm.pilot_configurator.nData;i++)
			{
				contellation[constellation_plot_counter*ofdm.pilot_configurator.nData+i][0]=data_container.ofdm_deframed_data[i].real();
				contellation[constellation_plot_counter*ofdm.pilot_configurator.nData+i][1]=data_container.ofdm_deframed_data[i].imag();
			}
		}

		constellation_plot_counter++;

		if(constellation_plot_counter==constellation_plot_nFrames)
		{
			constellation_plot_counter=0;
			constellation_plot.plot_constellation(&contellation[0][0],ofdm.pilot_configurator.nData*constellation_plot_nFrames);
		}


		lerror_rate.check(data_container.data_bit,data_container.hd_decoded_data_bit,nReal_data);
	}
	return lerror_rate;
}

cl_error_rate cl_telecom_system::passband_test_EsN0(float EsN0,int max_frame_no)
{
	cl_error_rate lerror_rate;
	float sigma=1.0/sqrt(pow(10,(EsN0/10)));
	int nReal_data=data_container.nBits-ldpc.P;
	int delay=0;

	if(data_container.Nfft==1024)
	{
		delay=100;
	}
	else
	{
		delay=50;
	}

	int constellation_plot_counter=0;
	int constellation_plot_nFrames=10;
	float contellation[ofdm.pilot_configurator.nData*constellation_plot_nFrames][2]={0};

	while(lerror_rate.Frames_total<max_frame_no)
	{
		for(int i=0;i<nReal_data-outer_code_reserved_bits;i++)
		{
			data_container.data_bit[i]=rand()%2;
		}
		bit_to_byte(data_container.data_bit,data_container.data_byte,nReal_data-outer_code_reserved_bits);
		this->transmit_byte(data_container.data_byte,(nReal_data-outer_code_reserved_bits)/8,data_container.passband_data,SINGLE_MESSAGE);
		awgn_channel.apply_with_delay(data_container.passband_data,data_container.passband_delayed_data,sigma,(data_container.Nofdm*(data_container.Nsymb+data_container.preamble_nSymb))*this->frequency_interpolation_rate,((data_container.preamble_nSymb+2)*data_container.Nofdm+delay)*frequency_interpolation_rate);
		this->receive_byte(data_container.passband_delayed_data,data_container.hd_decoded_data_byte);
		byte_to_bit(data_container.hd_decoded_data_byte,data_container.hd_decoded_data_bit,(nReal_data-outer_code_reserved_bits));

		if(ofdm.channel_estimator_amplitude_restoration==YES)
		{
			for(int i=0;i<ofdm.pilot_configurator.nData;i++)
			{
				contellation[constellation_plot_counter*ofdm.pilot_configurator.nData+i][0]=data_container.ofdm_deframed_data_without_amplitude_restoration[i].real();
				contellation[constellation_plot_counter*ofdm.pilot_configurator.nData+i][1]=data_container.ofdm_deframed_data_without_amplitude_restoration[i].imag();
			}
		}
		else
		{
			for(int i=0;i<ofdm.pilot_configurator.nData;i++)
			{
				contellation[constellation_plot_counter*ofdm.pilot_configurator.nData+i][0]=data_container.ofdm_deframed_data[i].real();
				contellation[constellation_plot_counter*ofdm.pilot_configurator.nData+i][1]=data_container.ofdm_deframed_data[i].imag();
			}
		}

		constellation_plot_counter++;

		if(constellation_plot_counter==constellation_plot_nFrames)
		{
			constellation_plot_counter=0;
			constellation_plot.plot_constellation(&contellation[0][0],ofdm.pilot_configurator.nData*constellation_plot_nFrames);
		}

		lerror_rate.check(data_container.data_bit,data_container.hd_decoded_data_bit,nReal_data-outer_code_reserved_bits);
	}
	return lerror_rate;
}

void cl_telecom_system::transmit_byte(const int* data, int nBytes, double* out, int message_location)
{
	int nReal_data=data_container.nBits-ldpc.P;
	int msB=0,lsB=0;

	if(nBytes>(nReal_data-outer_code_reserved_bits)/8)
	{
		std::cout<<"message too long.. not sent."<<std::endl;
		return;
	}

	byte_to_bit((int*)data, data_container.data_bit, nBytes);

	if(outer_code==CRC16_MODBUS_RTU)
	{
		int crc=CRC16_MODBUS_RTU_calc((int*)data, nBytes);
		msB=(crc & 0xff00)>>8;
		lsB=crc & 0x00ff;
		byte_to_bit(&lsB, &data_container.data_bit[nBytes*8], 1);
		byte_to_bit(&msB, &data_container.data_bit[(nBytes+1)*8], 1);
	}

	for(int i=0;i<nReal_data-nBytes*8-outer_code_reserved_bits;i++)
	{
		data_container.data_bit[nBytes*8+outer_code_reserved_bits+i]=0;
	}

	transmit_bit(data_container.data_bit,out,message_location);
}

void cl_telecom_system::transmit_bit(const int* data, double* out, int message_location)
{
	int nVirtual_data=ldpc.N-data_container.nBits;
	int nReal_data=data_container.nBits-ldpc.P;
	float power_normalization=sqrt((double)(ofdm.Nfft*frequency_interpolation_rate));

	for(int i=0;i<nReal_data;i++)
	{
		data_container.data_bit[i]=data[i];
	}

	bit_energy_dispersal(data_container.data_bit, data_container.bit_energy_dispersal_sequence, data_container.data_bit_energy_dispersal, nReal_data);

	for(int i=0;i<nVirtual_data;i++)
	{
		data_container.data_bit_energy_dispersal[nReal_data+i]=data_container.data_bit_energy_dispersal[i];
	}

	ldpc.encode(data_container.data_bit_energy_dispersal,data_container.encoded_data);

	for(int i=0;i<ldpc.P;i++)
	{
		data_container.encoded_data[nReal_data+i]=data_container.encoded_data[i+ldpc.K];
	}

	interleaver(data_container.encoded_data,data_container.bit_interleaved_data,data_container.nBits,bit_interleaver_block_size);

	psk.mod(data_container.bit_interleaved_data,data_container.nBits,data_container.modulated_data);
	interleaver(data_container.modulated_data, data_container.ofdm_time_freq_interleaved_data, data_container.nData, time_freq_interleaver_block_size);
	ofdm.framer(data_container.ofdm_time_freq_interleaved_data,data_container.ofdm_framed_data);

	for(int i=0;i<data_container.preamble_nSymb*ofdm.Nc;i++)
	{
		data_container.preamble_data[i]=ofdm.ofdm_preamble[i].value;
	}

	for(int i=0;i<data_container.preamble_nSymb;i++)
	{
		for(int j=0;j<data_container.Nc;j++)
		{
			data_container.preamble_data[i*data_container.Nc+j]*=pre_equalization_channel[j].value;
		}
	}

	for(int i=0;i<data_container.Nsymb;i++)
	{
		for(int j=0;j<data_container.Nc;j++)
		{
			data_container.ofdm_framed_data[i*data_container.Nc+j]*=pre_equalization_channel[j].value;
		}
	}

	for(int i=0;i<data_container.preamble_nSymb;i++)
	{
		ofdm.symbol_mod(&data_container.preamble_data[i*data_container.Nc],&data_container.preamble_symbol_modulated_data[i*data_container.Nofdm]);
	}

	for(int i=0;i<data_container.Nsymb;i++)
	{
		ofdm.symbol_mod(&data_container.ofdm_framed_data[i*data_container.Nc],&data_container.ofdm_symbol_modulated_data[i*data_container.Nofdm]);
	}

	for(int j=0;j<data_container.Nofdm*data_container.preamble_nSymb;j++)
	{
		data_container.preamble_symbol_modulated_data[j]/=power_normalization;
		data_container.preamble_symbol_modulated_data[j]*=sqrt(output_power_Watt)*ofdm.preamble_configurator.boost;
	}

	for(int j=0;j<data_container.Nofdm*data_container.Nsymb;j++)
	{
		data_container.ofdm_symbol_modulated_data[j]/=power_normalization;
		data_container.ofdm_symbol_modulated_data[j]*=sqrt(output_power_Watt);
	}


	ofdm.baseband_to_passband(data_container.preamble_symbol_modulated_data,data_container.Nofdm*data_container.preamble_nSymb,data_container.passband_data_tx,sampling_frequency,carrier_frequency,carrier_amplitude,frequency_interpolation_rate);
	ofdm.baseband_to_passband(data_container.ofdm_symbol_modulated_data,data_container.Nofdm*data_container.Nsymb,&data_container.passband_data_tx[data_container.Nofdm*data_container.preamble_nSymb*frequency_interpolation_rate],sampling_frequency,carrier_frequency,carrier_amplitude,frequency_interpolation_rate);

	ofdm.peak_clip(data_container.passband_data_tx, data_container.Nofdm*data_container.preamble_nSymb*frequency_interpolation_rate,ofdm.preamble_papr_cut);
	ofdm.peak_clip(&data_container.passband_data_tx[data_container.Nofdm*data_container.preamble_nSymb*frequency_interpolation_rate], data_container.Nofdm*data_container.Nsymb*frequency_interpolation_rate,ofdm.data_papr_cut);

	if(message_location==NO_FILTER_MESSAGE)
	{
		for(int i=0;i<data_container.total_frame_size;i++)
		{
			*(out+i)=data_container.passband_data_tx[i];
		}
		return;
	}

	if(message_location==SINGLE_MESSAGE)
	{
		ofdm.FIR_tx1.apply(data_container.passband_data_tx,data_container.passband_data_tx_filtered_fir_1,data_container.total_frame_size);
		ofdm.FIR_tx2.apply(data_container.passband_data_tx_filtered_fir_1,data_container.passband_data_tx_filtered_fir_2,data_container.total_frame_size);

		for(int i=0;i<data_container.total_frame_size;i++)
		{
			*(out+i)=data_container.passband_data_tx_filtered_fir_2[i];
		}
		//		st_power_measurment power_measurment_preamble=ofdm.measure_signal_power_avg_papr(out, data_container.Nofdm*data_container.preamble_nSymb*frequency_interpolation_rate);
		//		st_power_measurment power_measurment_modulated_data=ofdm.measure_signal_power_avg_papr(&out[data_container.Nofdm*data_container.preamble_nSymb*frequency_interpolation_rate], data_container.Nofdm*data_container.Nsymb*frequency_interpolation_rate);
		//
		//		std::cout<<"preamble power: avg="<<power_measurment_preamble.avg;
		//		std::cout<<" max="<<power_measurment_preamble.max;
		//		std::cout<<" PAPR="<<power_measurment_preamble.papr_db<<" db";
		//		std::cout<<" mod_data power: avg="<<power_measurment_modulated_data.avg;
		//		std::cout<<" max="<<power_measurment_modulated_data.max;
		//		std::cout<<" PAPR="<<power_measurment_modulated_data.papr_db<<" db"<<std::endl;
		return;
	}


	if(message_location==FIRST_MESSAGE)
	{
		for(int i=0;i<data_container.total_frame_size;i++)
		{
			data_container.passband_data_tx_buffer[data_container.total_frame_size + i]=data_container.passband_data_tx[i];//TODO increasing value
			data_container.passband_data_tx_buffer[2 * data_container.total_frame_size + i]=data_container.passband_data_tx[i];
		}
	}

	if(message_location==MIDDLE_MESSAGE || message_location==FLUSH_MESSAGE)
	{
		for(int i=0;i<data_container.total_frame_size;i++)
		{
			data_container.passband_data_tx_buffer[2 * data_container.total_frame_size + i]=data_container.passband_data_tx[i];
		}
	}


	ofdm.FIR_tx1.apply(&data_container.passband_data_tx_buffer[data_container.total_frame_size/2],data_container.passband_data_tx_filtered_fir_1,2*data_container.total_frame_size);
	ofdm.FIR_tx2.apply(data_container.passband_data_tx_filtered_fir_1,data_container.passband_data_tx_filtered_fir_2,2*data_container.total_frame_size);

	for(int i=0;i<data_container.total_frame_size;i++)
	{
		*(out+i)=data_container.passband_data_tx_filtered_fir_2[data_container.total_frame_size/2+i];
	}
	shift_left(data_container.passband_data_tx_buffer, 3*data_container.total_frame_size, data_container.total_frame_size);


	int PAPR_Meas=NO;

	int MER_Meas=NO;

	if(PAPR_Meas==YES)
	{
		st_power_measurment power_measurment_preamble=ofdm.measure_signal_power_avg_papr(out, data_container.Nofdm*data_container.preamble_nSymb*frequency_interpolation_rate);
		st_power_measurment power_measurment_modulated_data=ofdm.measure_signal_power_avg_papr(&out[data_container.Nofdm*data_container.preamble_nSymb*frequency_interpolation_rate], data_container.Nofdm*data_container.Nsymb*frequency_interpolation_rate);
		std::cout<<"preamble power: avg="<<power_measurment_preamble.avg;
		std::cout<<" max="<<power_measurment_preamble.max;
		std::cout<<" PAPR="<<power_measurment_preamble.papr_db<<" db";
		std::cout<<" mod_data power: avg="<<power_measurment_modulated_data.avg;
		std::cout<<" max="<<power_measurment_modulated_data.max;
		std::cout<<" PAPR="<<power_measurment_modulated_data.papr_db<<" db"<<std::endl;
	}

	if(MER_Meas==YES)
	{
		ofdm.FIR_tx1.apply(data_container.passband_data_tx_buffer,&data_container.passband_data_tx_filtered_fir_1[data_container.total_frame_size/2],2.5*data_container.total_frame_size);
		ofdm.FIR_tx2.apply(data_container.passband_data_tx_filtered_fir_1,data_container.passband_data_tx_filtered_fir_2,2.5*data_container.total_frame_size);

		ofdm.passband_to_baseband(&data_container.passband_data_tx_filtered_fir_2[data_container.total_frame_size+data_container.total_frame_size/2],data_container.total_frame_size,data_container.baseband_data_interpolated,sampling_frequency,carrier_frequency,carrier_amplitude,1,&ofdm.FIR_rx_data);

		ofdm.rational_resampler(data_container.baseband_data_interpolated, (data_container.Nofdm*(data_container.Nsymb+data_container.preamble_nSymb))*frequency_interpolation_rate, data_container.baseband_data, data_container.interpolation_rate, DECIMATION);

		for(int i=0;i<data_container.Nsymb;i++)
		{
			ofdm.symbol_demod(&data_container.baseband_data[i*data_container.Nofdm+data_container.Nofdm*data_container.preamble_nSymb],&data_container.ofdm_symbol_demodulated_data[i*data_container.Nc]);
		}
		ofdm.automatic_gain_control(data_container.ofdm_symbol_demodulated_data);
		ofdm.ZF_channel_estimator(data_container.ofdm_symbol_demodulated_data);
		ofdm.channel_equalizer(data_container.ofdm_symbol_demodulated_data,data_container.equalized_data);
		ofdm.deframer(data_container.equalized_data,data_container.ofdm_deframed_data);

		float MER=ofdm.measure_SNR(data_container.ofdm_time_freq_interleaved_data,data_container.ofdm_deframed_data,data_container.nData);
		std::cout<<"MER ="<<MER<<std::endl;
	}

}

st_receive_stats cl_telecom_system::receive_bit(const double* data, int* out)
{
	int nReal_data=data_container.nBits-ldpc.P;

	st_receive_stats tmp=receive_byte(data,data_container.hd_decoded_data_byte);
	byte_to_bit(data_container.hd_decoded_data_byte, out, nReal_data/8);

	return tmp;
}

st_receive_stats cl_telecom_system::receive_byte(const double* data, int* out)
{
	float variance;
	int nVirtual_data=ldpc.N-data_container.nBits;
	int nReal_data=data_container.nBits-ldpc.P;
	double freq_offset_measured=0;
	receive_stats.message_decoded=NO;
	receive_stats.sync_trials=0;

	ofdm.passband_to_baseband((double*)data,data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate,data_container.baseband_data_interpolated,sampling_frequency,carrier_frequency,carrier_amplitude,1,&ofdm.FIR_rx_time_sync);
	receive_stats.signal_stregth_dbm=ofdm.measure_signal_stregth(data_container.baseband_data_interpolated, data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate);

	int step=100;
	receive_stats.delay=ofdm.time_sync_preamble(data_container.baseband_data_interpolated,data_container.Nofdm*(2*data_container.preamble_nSymb+data_container.Nsymb)*frequency_interpolation_rate,data_container.interpolation_rate,0,step, 1);
	int pream_symb_loc=receive_stats.delay/(data_container.Nofdm*data_container.interpolation_rate);
	if(pream_symb_loc<1){pream_symb_loc=1;}

	if(pream_symb_loc>(ofdm.preamble_configurator.Nsymb+data_container.Nsymb/2) && pream_symb_loc<(data_container.buffer_Nsymb-(data_container.Nsymb+data_container.preamble_nSymb)))
	{
		while (receive_stats.sync_trials<=time_sync_trials_max)
		{
			if(receive_stats.sync_trials==time_sync_trials_max && use_last_good_time_sync==YES && receive_stats.delay_of_last_decoded_message!=-1)
			{
				receive_stats.delay=receive_stats.delay_of_last_decoded_message;
			}
			else
			{
				receive_stats.delay=(pream_symb_loc-1)*data_container.Nofdm*frequency_interpolation_rate+ofdm.time_sync_preamble(&data_container.baseband_data_interpolated[(pream_symb_loc-1)*data_container.Nofdm*frequency_interpolation_rate],(ofdm.preamble_configurator.Nsymb+2)*data_container.Nofdm*data_container.interpolation_rate,data_container.interpolation_rate,receive_stats.sync_trials,1,time_sync_trials_max);
			}

			if(receive_stats.delay<0){receive_stats.delay=0;}

			ofdm.passband_to_baseband((double*)data,data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate,data_container.baseband_data_interpolated,sampling_frequency,carrier_frequency,carrier_amplitude,1,&ofdm.FIR_rx_data);

			ofdm.rational_resampler(&data_container.baseband_data_interpolated[receive_stats.delay], (data_container.Nofdm*(data_container.Nsymb+data_container.preamble_nSymb))*frequency_interpolation_rate, data_container.baseband_data, data_container.interpolation_rate, DECIMATION);

			if(receive_stats.sync_trials==time_sync_trials_max && use_last_good_freq_offset==YES && receive_stats.freq_offset_of_last_decoded_message!=0)
			{
				freq_offset_measured=receive_stats.freq_offset_of_last_decoded_message;
			}
			else
			{
				freq_offset_measured=ofdm.carrier_sampling_frequency_sync(&data_container.baseband_data[data_container.Ngi*data_container.interpolation_rate],bandwidth/(double)data_container.Nc,data_container.preamble_nSymb, sampling_frequency);
			}

			if(abs(freq_offset_measured)>ofdm.freq_offset_ignore_limit)
			{
				ofdm.passband_to_baseband((double*)data,(data_container.Nofdm*data_container.buffer_Nsymb)*frequency_interpolation_rate,data_container.baseband_data_interpolated,sampling_frequency,carrier_frequency+freq_offset_measured,carrier_amplitude,1,&ofdm.FIR_rx_data);
				ofdm.rational_resampler(&data_container.baseband_data_interpolated[receive_stats.delay], (data_container.Nofdm*(data_container.Nsymb+data_container.preamble_nSymb))*frequency_interpolation_rate, data_container.baseband_data, data_container.interpolation_rate, DECIMATION);
			}
			for(int i=0;i<data_container.Nsymb;i++)
			{
				ofdm.symbol_demod(&data_container.baseband_data[i*data_container.Nofdm+data_container.Nofdm*data_container.preamble_nSymb],&data_container.ofdm_symbol_demodulated_data[i*data_container.Nc]);
			}

			ofdm.automatic_gain_control(data_container.ofdm_symbol_demodulated_data);

			if(ofdm.channel_estimator==ZERO_FORCE)
			{
				ofdm.ZF_channel_estimator(data_container.ofdm_symbol_demodulated_data);
			}
			else if (ofdm.channel_estimator==LEAST_SQUARE)
			{
				ofdm.LS_channel_estimator(data_container.ofdm_symbol_demodulated_data);
			}

			if(ofdm.channel_estimator_amplitude_restoration==YES)
			{
				ofdm.restore_channel_amplitude();
				ofdm.channel_equalizer_without_amplitude_restoration(data_container.ofdm_symbol_demodulated_data,data_container.equalized_data_without_amplitude_restoration);
				ofdm.deframer(data_container.equalized_data_without_amplitude_restoration,data_container.ofdm_deframed_data_without_amplitude_restoration);
			}

			ofdm.channel_equalizer(data_container.ofdm_symbol_demodulated_data,data_container.equalized_data);

			variance=ofdm.measure_variance(data_container.equalized_data);

			ofdm.deframer(data_container.equalized_data,data_container.ofdm_deframed_data);
			deinterleaver(data_container.ofdm_deframed_data, data_container.ofdm_time_freq_deinterleaved_data, data_container.nData, time_freq_interleaver_block_size);
			psk.demod(data_container.ofdm_time_freq_deinterleaved_data,data_container.nBits,data_container.demodulated_data,variance);

			deinterleaver(data_container.demodulated_data,data_container.deinterleaved_data,data_container.nBits,bit_interleaver_block_size);

			for(int i=ldpc.P-1;i>=0;i--)
			{
				data_container.deinterleaved_data[i+nReal_data+nVirtual_data]=data_container.deinterleaved_data[i+nReal_data];
			}

			for(int i=0;i<nVirtual_data;i++)
			{
				data_container.deinterleaved_data[nReal_data+i]=data_container.deinterleaved_data[i];
			}


			receive_stats.iterations_done=ldpc.decode(data_container.deinterleaved_data,data_container.hd_decoded_data_bit);

			bit_energy_dispersal(data_container.hd_decoded_data_bit, data_container.bit_energy_dispersal_sequence, data_container.hd_decoded_data_bit, nReal_data);

			bit_to_byte(data_container.hd_decoded_data_bit, data_container.hd_decoded_data_byte, nReal_data);

			receive_stats.all_zeros=YES;
			for(int i=0;i<nReal_data/8;i++)
			{
				if(data_container.hd_decoded_data_byte[i]!=0)
				{
					receive_stats.all_zeros=NO;
					break;
				}
			}

			for(int i=0;i<(nReal_data-outer_code_reserved_bits)/8;i++)
			{
				*(out+i)=data_container.hd_decoded_data_byte[i];
			}
			receive_stats.crc=0xff;
			if(outer_code==CRC16_MODBUS_RTU && receive_stats.iterations_done>(ldpc.nIteration_max-1) && receive_stats.all_zeros==NO)
			{
				receive_stats.crc=CRC16_MODBUS_RTU_calc(data_container.hd_decoded_data_byte, nReal_data/8);
			}

			if((receive_stats.iterations_done>(ldpc.nIteration_max-1) && receive_stats.crc!=0) || receive_stats.all_zeros==YES)
			{
				receive_stats.SNR=-99.9;
				receive_stats.message_decoded=NO;
				receive_stats.sync_trials++;
			}
			else
			{
				if(ofdm.channel_estimator==LEAST_SQUARE)
				{
					if(ofdm.channel_estimator_amplitude_restoration==YES)
					{
						variance=ofdm.measure_variance(data_container.equalized_data_without_amplitude_restoration);
					}
					receive_stats.SNR=10.0*log10(1.0/variance);
				}
				else if(ofdm.channel_estimator==ZERO_FORCE)
				{
					bit_energy_dispersal(data_container.hd_decoded_data_bit, data_container.bit_energy_dispersal_sequence, data_container.hd_decoded_data_bit, nReal_data);

					for(int i=0;i<nVirtual_data;i++)
					{
						data_container.hd_decoded_data_bit[nReal_data+i]=data_container.hd_decoded_data_bit[i];
					}
					ldpc.encode(data_container.hd_decoded_data_bit,data_container.encoded_data);
					for(int i=0;i<ldpc.P;i++)
					{
						data_container.encoded_data[nReal_data+i]=data_container.encoded_data[i+ldpc.K];
					}
					interleaver(data_container.encoded_data,data_container.bit_interleaved_data,data_container.nBits,bit_interleaver_block_size);
					psk.mod(data_container.bit_interleaved_data,data_container.nBits,data_container.modulated_data);
					interleaver(data_container.modulated_data, data_container.ofdm_time_freq_interleaved_data, data_container.nData, time_freq_interleaver_block_size);
					if(ofdm.channel_estimator_amplitude_restoration==YES)
					{
						receive_stats.SNR=ofdm.measure_SNR(data_container.ofdm_deframed_data_without_amplitude_restoration,data_container.ofdm_time_freq_interleaved_data,data_container.nData);
					}
					else
					{
						receive_stats.SNR=ofdm.measure_SNR(data_container.ofdm_deframed_data,data_container.ofdm_time_freq_interleaved_data,data_container.nData);
					}

				}

				receive_stats.message_decoded=YES;
				receive_stats.freq_offset_of_last_decoded_message=freq_offset_measured;
				receive_stats.freq_offset=freq_offset_measured;

				receive_stats.delay_of_last_decoded_message=receive_stats.delay;
				break;
			}
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
	LDPC_real_CR=((double)ofdm.pilot_configurator.nData*log2(M)-(double)ldpc.P -(double)outer_code_reserved_bits)/((double)ofdm.pilot_configurator.nData*log2(M));
	Tu= ofdm.Nc/bandwidth;
	Ts= Tu*(1.0+ofdm.gi);
	Tf= Ts*(ofdm.Nsymb+ofdm.preamble_configurator.Nsymb);
	rb= ofdm.pilot_configurator.nData * log2(M) /Tf;
	rbc= rb*LDPC_real_CR;
	Shannon_limit= 10.0*log10((pow(2,(rb*ldpc.rate)/bandwidth)-1)*log2(M)*bandwidth/rb);
	sampling_frequency=frequency_interpolation_rate*(bandwidth/ofdm.Nc)*ofdm.Nfft;
}

void cl_telecom_system::init()
{

	if(ofdm.Nc==AUTO_SELLECT)
	{
		ofdm.Nc=50;
	}
	if(ofdm.Nsymb==AUTO_SELLECT)
	{
		if(ofdm.pilot_configurator.pilot_density==HIGH_DENSITY)
		{
			if(M==MOD_BPSK){ofdm.Nsymb=48;} //48,1,3
			if(M==MOD_QPSK){ofdm.Nsymb=24;} //24,1,3
			if(M==MOD_8PSK){ofdm.Nsymb=16;} //16,1,3
			if(M==MOD_16QAM){ofdm.Nsymb=12;} //12,1,3
			if(M==MOD_32QAM){ofdm.Nsymb=9;}
			if(M==MOD_64QAM){ofdm.Nsymb=8;}
		}
		else if(ofdm.pilot_configurator.pilot_density==LOW_DENSITY)
		{
			if(M==MOD_BPSK){ofdm.Nsymb=40;} //40,1,5
			if(M==MOD_QPSK){ofdm.Nsymb=20;} //20,1,5
			if(M==MOD_8PSK){ofdm.Nsymb=16;} //13,1,5 (Nc=51)
			if(M==MOD_16QAM){ofdm.Nsymb=10;} //10,1,5
			if(M==MOD_32QAM){ofdm.Nsymb=9;} //Nc=53
			if(M==MOD_64QAM){ofdm.Nsymb=8;}
		}
	}

	if(ofdm.pilot_configurator.Dx==AUTO_SELLECT)
	{
		if(M==MOD_BPSK){ofdm.pilot_configurator.Dx=1;}
		if(M==MOD_QPSK){ofdm.pilot_configurator.Dx=1;}
		if(M==MOD_8PSK){ofdm.pilot_configurator.Dx=1;}
		if(M==MOD_16QAM){ofdm.pilot_configurator.Dx=1;}
		if(M==MOD_32QAM){ofdm.pilot_configurator.Dx=1;}
		if(M==MOD_64QAM){ofdm.pilot_configurator.Dx=1;}
	}

	if(ofdm.pilot_configurator.Dy==AUTO_SELLECT)
	{
		if(ofdm.pilot_configurator.pilot_density==HIGH_DENSITY)
		{
			if(M==MOD_BPSK){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_QPSK){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_8PSK){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_16QAM){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_32QAM){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_64QAM){ofdm.pilot_configurator.Dy=3;}
		}
		else if(ofdm.pilot_configurator.pilot_density==LOW_DENSITY)
		{
			if(M==MOD_BPSK){ofdm.pilot_configurator.Dy=5;}
			if(M==MOD_QPSK){ofdm.pilot_configurator.Dy=5;}
			if(M==MOD_8PSK){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_16QAM){ofdm.pilot_configurator.Dy=5;}
			if(M==MOD_32QAM){ofdm.pilot_configurator.Dy=3;}
			if(M==MOD_64QAM){ofdm.pilot_configurator.Dy=3;}
		}

	}

	if(operation_mode==ARQ_MODE)
	{
		ofdm.pilot_configurator.print_on=NO;
		ofdm.preamble_configurator.print_on=NO;
	}

	if(reinit_subsystems.ofdm==YES)
	{
		ofdm.init();
		reinit_subsystems.ofdm=NO;
	}

	if(reinit_subsystems.ldpc==YES)
	{
		ldpc.init();
		reinit_subsystems.ldpc=NO;
	}
	calculate_parameters();

	if(reinit_subsystems.ofdm_FIR_rx_data==YES)
	{
		ofdm.FIR_rx_data.sampling_frequency=this->sampling_frequency;
		ofdm.FIR_rx_data.design();
		reinit_subsystems.ofdm_FIR_rx_data=NO;
	}

	if(reinit_subsystems.ofdm_FIR_rx_time_sync==YES)
	{
		ofdm.FIR_rx_time_sync.sampling_frequency=this->sampling_frequency;
		ofdm.FIR_rx_time_sync.design();
		reinit_subsystems.ofdm_FIR_rx_time_sync=NO;
	}

	if(reinit_subsystems.ofdm_FIR_tx1==YES)
	{
		ofdm.FIR_tx1.sampling_frequency=this->sampling_frequency;
		ofdm.FIR_tx1.design();
		reinit_subsystems.ofdm_FIR_tx1=NO;
	}

	if(reinit_subsystems.ofdm_FIR_tx2==YES)
	{
		ofdm.FIR_tx2.sampling_frequency=this->sampling_frequency;
		ofdm.FIR_tx2.design();
		reinit_subsystems.ofdm_FIR_tx2=NO;
	}

	if(reinit_subsystems.data_container==YES)
	{
		data_container.set_size(ofdm.pilot_configurator.nData,ofdm.Nc,M,ofdm.Nfft,ofdm.Nfft*(1+ofdm.gi),ofdm.Nsymb,ofdm.preamble_configurator.Nsymb,frequency_interpolation_rate);
		reinit_subsystems.data_container=NO;
	}

	if(reinit_subsystems.pre_equalization_channel==YES)
	{
		pre_equalization_channel=new struct st_channel_complex[data_container.Nc];
		get_pre_equalization_channel();
		reinit_subsystems.pre_equalization_channel=NO;
	}

	srand(bit_energy_dispersal_seed);
	for(int i=0;i<ldpc.N;i++)
	{
		data_container.bit_energy_dispersal_sequence[i]=rand()%2;
	}
	srand(time(0)); // 2 consecutive srand()?

	receive_stats.iterations_done=-1;
	receive_stats.delay=0;
	receive_stats.delay_of_last_decoded_message=-1;
	receive_stats.time_peak_symb_location=0;
	receive_stats.time_peak_subsymb_location=0;
	receive_stats.sync_trials=0;
	receive_stats.phase_error_avg=0;
	receive_stats.freq_offset=0;
	receive_stats.freq_offset_of_last_decoded_message=0;
	receive_stats.message_decoded=NO;
	receive_stats.SNR=-99.9;
	receive_stats.signal_stregth_dbm=-999;

	if(operation_mode!=ARQ_MODE)
	{
		std::cout<<"nBits="<<data_container.nBits<<std::endl;
		std::cout<<"Shannon_limit="<<Shannon_limit<<" dB"<<std::endl;
		std::cout<<"rbc="<<rbc/1000.0<<" kbps"<<std::endl;
		std::cout<<"sampling_frequency="<<sampling_frequency<<" Sps"<<std::endl;
	}
}

void cl_telecom_system::deinit()
{
	if(reinit_subsystems.data_container==YES)
	{
		data_container.deinit();
	}
	if(reinit_subsystems.ofdm_FIR_rx_data==YES)
	{
		ofdm.FIR_rx_data.deinit();
	}
	if(reinit_subsystems.ofdm_FIR_rx_time_sync==YES)
	{
		ofdm.FIR_rx_time_sync.deinit();
	}
	if(reinit_subsystems.ofdm_FIR_tx1==YES)
	{
		ofdm.FIR_tx1.deinit();
	}
	if(reinit_subsystems.ofdm_FIR_tx2==YES)
	{
		ofdm.FIR_tx2.deinit();
	}
	if(reinit_subsystems.ldpc==YES)
	{
		ldpc.deinit();
	}
	if(reinit_subsystems.ofdm==YES)
	{
		ofdm.deinit();
	}
	if(reinit_subsystems.pre_equalization_channel==YES)
	{
		if(pre_equalization_channel!=NULL)
		{
			delete[] pre_equalization_channel;
			pre_equalization_channel=NULL;
		}
	}
}

void cl_telecom_system::TX_TEST_process_main()
{
	static int is_first_message=YES;
	for(int i=0;i<data_container.nBits-ldpc.P;i++)
	{
		data_container.data_bit[i]=rand()%2;
	}
	if(is_first_message==YES)
	{
		transmit_bit(data_container.data_bit,data_container.passband_data,FIRST_MESSAGE);
		is_first_message=NO;
	}
	else
	{
		transmit_bit(data_container.data_bit,data_container.passband_data,MIDDLE_MESSAGE);
	}
	speaker.transfere(data_container.passband_data,data_container.Nofdm*data_container.interpolation_rate*(ofdm.Nsymb+ofdm.preamble_configurator.Nsymb));
}

void cl_telecom_system::TX_BROADCAST_process_main()
{
    int nReal_data = data_container.nBits-ldpc.P;
    int frame_size = (nReal_data - outer_code_reserved_bits) / 8;
    printf("Frame size %d\n", frame_size);

    static int counter = 0;
    for (int i = 0; i < frame_size; i++)
    {
        data_container.data_byte[i] = 0;
    }
    data_container.data_byte[counter % frame_size] = 1;
    counter++;

    transmit_byte(data_container.data_byte,(nReal_data-outer_code_reserved_bits)/8,data_container.passband_data,SINGLE_MESSAGE);

	speaker.transfere(data_container.passband_data,data_container.Nofdm*data_container.interpolation_rate*(ofdm.Nsymb+ofdm.preamble_configurator.Nsymb));
}

void cl_telecom_system::RX_TEST_process_main()
{
	std::complex <double> data_fft[ofdm.pilot_configurator.nData];
	int tmp[N_MAX];
	int constellation_plot_counter=0;
	int constellation_plot_nFrames=1;
	float contellation[ofdm.pilot_configurator.nData*constellation_plot_nFrames][2]={0};

	std::cout << std::fixed;
	std::cout << std::setprecision(1);

	if(data_container.data_ready==1)
	{
		if(data_container.frames_to_read==0)
		{
			for(int i=0;i<data_container.Nofdm*data_container.buffer_Nsymb*data_container.interpolation_rate;i++)
			{
				data_container.ready_to_process_passband_delayed_data[i]=data_container.passband_delayed_data[i];
			}
			st_receive_stats received_message_stats=receive_byte((const double*)data_container.ready_to_process_passband_delayed_data,tmp);

			if(ofdm.channel_estimator_amplitude_restoration==YES)
			{
				for(int i=0;i<ofdm.pilot_configurator.nData;i++)
				{
					data_fft[i]=data_container.ofdm_deframed_data_without_amplitude_restoration[i];
				}
			}
			else
			{
				for(int i=0;i<ofdm.pilot_configurator.nData;i++)
				{
					data_fft[i]=data_container.ofdm_deframed_data[i];
				}
			}

			if (received_message_stats.message_decoded==YES)
			{
				std::cout<<"decoded in "<<received_message_stats.iterations_done<<" data=";
				std::cout<<std::hex;
				for(int i=0;i<5;i++)
				{
					std::cout<<"0x"<<tmp[i]<<",";
				}
				std::cout<<std::dec;
				std::cout<<" sync_trial="<<receive_stats.sync_trials;
				std::cout<<" time_peak_subsymb_location="<<received_message_stats.delay%(data_container.Nofdm*data_container.interpolation_rate);
				std::cout<<" time_peak_symb_location="<<received_message_stats.delay/(data_container.Nofdm*data_container.interpolation_rate);
				std::cout<<" freq_offset="<<receive_stats.freq_offset;
				std::cout<<" SNR="<<receive_stats.SNR<<" dB";
				std::cout<<" Signal Strength="<<receive_stats.signal_stregth_dbm<<" dBm ";
				std::cout<<std::endl;

				int end_of_current_message=received_message_stats.delay/(data_container.Nofdm*data_container.interpolation_rate)+data_container.Nsymb+data_container.preamble_nSymb;
				int frames_left_in_buffer=data_container.buffer_Nsymb-end_of_current_message;
				if(frames_left_in_buffer<0) {frames_left_in_buffer=0;}

				data_container.frames_to_read=data_container.Nsymb+data_container.preamble_nSymb-frames_left_in_buffer-data_container.nUnder_processing_events;

				if(data_container.frames_to_read>(data_container.Nsymb+data_container.preamble_nSymb) || data_container.frames_to_read<0)
				{
					data_container.frames_to_read=data_container.Nsymb+data_container.preamble_nSymb-frames_left_in_buffer;
				}

				receive_stats.delay_of_last_decoded_message+=(data_container.Nsymb+data_container.preamble_nSymb-(data_container.frames_to_read+data_container.nUnder_processing_events))*data_container.Nofdm*data_container.interpolation_rate;

				data_container.nUnder_processing_events=0;

			}
			else
			{
				if(data_container.data_ready==1 && data_container.frames_to_read==0 && receive_stats.delay_of_last_decoded_message!=-1)
				{
					receive_stats.delay_of_last_decoded_message-=data_container.Nofdm*data_container.interpolation_rate;
					if(receive_stats.delay_of_last_decoded_message<0)
					{
						receive_stats.delay_of_last_decoded_message=-1;
					}
				}
				//				std::cout<<"Syncing.. ";
				//				std::cout<<" delay="<<received_message_stats.delay;
				//				std::cout<<" time_peak_subsymb_location="<<received_message_stats.delay%(data_container.Nofdm*data_container.interpolation_rate);
				//				std::cout<<" time_peak_symb_location="<<received_message_stats.delay/(data_container.Nofdm*data_container.interpolation_rate);
				//				std::cout<<" freq_offset="<<receive_stats.freq_offset;
				//				std::cout<<" Signal Strength="<<receive_stats.signal_stregth_dbm<<" dBm ";
				//				std::cout<<std::endl;
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
				constellation_plot.plot_constellation(&contellation[0][0],ofdm.pilot_configurator.nData*constellation_plot_nFrames);
			}
		}
		data_container.data_ready=0;

	}
	else
	{
		usleep(1000);
	}
}

void cl_telecom_system::RX_BROADCAST_process_main()
{
	std::complex <double> data_fft[ofdm.pilot_configurator.nData];
	int tmp[N_MAX];
	int constellation_plot_counter=0;
	int constellation_plot_nFrames=1;
	float contellation[ofdm.pilot_configurator.nData*constellation_plot_nFrames][2]={0};
	int nReal_data = data_container.nBits-ldpc.P;
    int frame_size = (nReal_data - outer_code_reserved_bits) / 8;
    printf("Frame size %d\n", frame_size);


	std::cout << std::fixed;
	std::cout << std::setprecision(1);

	if(data_container.data_ready==1)
	{
		if(data_container.frames_to_read==0)
		{
			for(int i=0;i<data_container.Nofdm*data_container.buffer_Nsymb*data_container.interpolation_rate;i++)
			{
				data_container.ready_to_process_passband_delayed_data[i]=data_container.passband_delayed_data[i];
			}
			st_receive_stats received_message_stats=receive_byte((const double*)data_container.ready_to_process_passband_delayed_data, tmp);

			if(ofdm.channel_estimator_amplitude_restoration==YES)
			{
				for(int i=0;i<ofdm.pilot_configurator.nData;i++)
				{
					data_fft[i]=data_container.ofdm_deframed_data_without_amplitude_restoration[i];
				}
			}
			else
			{
				for(int i=0;i<ofdm.pilot_configurator.nData;i++)
				{
					data_fft[i]=data_container.ofdm_deframed_data[i];
				}
			}

			if (received_message_stats.message_decoded==YES)
			{
				std::cout<<"decoded in "<<received_message_stats.iterations_done<<" data=";
				std::cout<<std::hex;

				for(int i = 0; i < frame_size; i++)
				{
					std::cout<<"0x"<<tmp[i]<<",";
				}
				std::cout<<std::dec;
				std::cout<<" sync_trial="<<receive_stats.sync_trials;
				std::cout<<" time_peak_subsymb_location="<<received_message_stats.delay%(data_container.Nofdm*data_container.interpolation_rate);
				std::cout<<" time_peak_symb_location="<<received_message_stats.delay/(data_container.Nofdm*data_container.interpolation_rate);
				std::cout<<" freq_offset="<<receive_stats.freq_offset;
				std::cout<<" SNR="<<receive_stats.SNR<<" dB";
				std::cout<<" Signal Strength="<<receive_stats.signal_stregth_dbm<<" dBm ";
				std::cout<<std::endl;

				int end_of_current_message=received_message_stats.delay/(data_container.Nofdm*data_container.interpolation_rate)+data_container.Nsymb+data_container.preamble_nSymb;
				int frames_left_in_buffer=data_container.buffer_Nsymb-end_of_current_message;
				if(frames_left_in_buffer<0) {frames_left_in_buffer=0;}

				data_container.frames_to_read=data_container.Nsymb+data_container.preamble_nSymb-frames_left_in_buffer-data_container.nUnder_processing_events;

				if(data_container.frames_to_read>(data_container.Nsymb+data_container.preamble_nSymb) || data_container.frames_to_read<0)
				{
					data_container.frames_to_read=data_container.Nsymb+data_container.preamble_nSymb-frames_left_in_buffer;
				}

				receive_stats.delay_of_last_decoded_message+=(data_container.Nsymb+data_container.preamble_nSymb-(data_container.frames_to_read+data_container.nUnder_processing_events))*data_container.Nofdm*data_container.interpolation_rate;

				data_container.nUnder_processing_events=0;

			}
			else
			{
				if(data_container.data_ready==1 && data_container.frames_to_read==0 && receive_stats.delay_of_last_decoded_message!=-1)
				{
					receive_stats.delay_of_last_decoded_message-=data_container.Nofdm*data_container.interpolation_rate;
					if(receive_stats.delay_of_last_decoded_message<0)
					{
						receive_stats.delay_of_last_decoded_message=-1;
					}
				}
				//				std::cout<<"Syncing.. ";
				//				std::cout<<" delay="<<received_message_stats.delay;
				//				std::cout<<" time_peak_subsymb_location="<<received_message_stats.delay%(data_container.Nofdm*data_container.interpolation_rate);
				//				std::cout<<" time_peak_symb_location="<<received_message_stats.delay/(data_container.Nofdm*data_container.interpolation_rate);
				//				std::cout<<" freq_offset="<<receive_stats.freq_offset;
				//				std::cout<<" Signal Strength="<<receive_stats.signal_stregth_dbm<<" dBm ";
				//				std::cout<<std::endl;
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
				constellation_plot.plot_constellation(&contellation[0][0],ofdm.pilot_configurator.nData*constellation_plot_nFrames);
			}
		}
		data_container.data_ready=0;

	}
	else
	{
		usleep(1000);
	}
}

void cl_telecom_system::BER_PLOT_baseband_process_main()
{
	BER_plot.open("BER");
	BER_plot.reset("BER");
	int nPoints=25;
	float data_plot[nPoints][2];
	float data_plot_theo[nPoints][2];
	output_power_Watt=1;
	int start_location=-10;

	for(int ind=0;ind<nPoints;ind++)
	{
		float EsN0=(float)(ind/2.0+start_location);

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

	BER_plot.plot("BER Simulation",&data_plot[0][0],nPoints,"BER theoretical",&data_plot_theo[0][0],nPoints);
	BER_plot.close();
}
void cl_telecom_system::BER_PLOT_passband_process_main()
{
	BER_plot.open("BER");
	BER_plot.reset("BER");
	int nPoints=25;
	float data_plot[nPoints][2];
	float data_plot_theo[nPoints][2];
	output_power_Watt=1;
	int start_location=-10;

	for(int ind=0;ind<nPoints;ind++)
	{
		float EsN0=(float)(ind/2.0+start_location);

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

	BER_plot.plot("BER Simulation",&data_plot[0][0],nPoints,"BER theoretical",&data_plot_theo[0][0],nPoints);
	BER_plot.close();
}

void cl_telecom_system::load_configuration()
{
	this->load_configuration(default_configurations_telecom_system.init_configuration);
}

void cl_telecom_system::load_configuration(int configuration)
{
	if(configuration==current_configuration)
	{
		return;
	}

	if(configuration<0 || configuration>=NUMBER_OF_CONFIGS)
	{
		return;
	}


	int _modulation;
	float _ldpc_rate;
	int ofdm_preamble_configurator_Nsymb;
	int ofdm_channel_estimator;

	if(configuration==CONFIG_0)
	{
		_modulation=MOD_BPSK;
		_ldpc_rate=1/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_1)
	{
		_modulation=MOD_BPSK;
		_ldpc_rate=2/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_2)
	{
		_modulation=MOD_BPSK;
		_ldpc_rate=3/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_3)
	{
		_modulation=MOD_BPSK;
		_ldpc_rate=4/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_4)
	{
		_modulation=MOD_BPSK;
		_ldpc_rate=5/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_5)
	{
		_modulation=MOD_BPSK;
		_ldpc_rate=6/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_6)
	{
		_modulation=MOD_BPSK;
		_ldpc_rate=8/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_7)
	{
		_modulation=MOD_QPSK;
		_ldpc_rate=5/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_8)
	{
		_modulation=MOD_QPSK;
		_ldpc_rate=6/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_9)
	{
		_modulation=MOD_QPSK;
		_ldpc_rate=8/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_10)
	{
		_modulation=MOD_8PSK;
		_ldpc_rate=6/16.0;
		ofdm_preamble_configurator_Nsymb=3;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_11)
	{
		_modulation=MOD_8PSK;
		_ldpc_rate=8/16.0;
		ofdm_preamble_configurator_Nsymb=3;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_12)
	{
		_modulation=MOD_QPSK;
		_ldpc_rate=14/16.0;
		ofdm_preamble_configurator_Nsymb=3;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_13)
	{
		_modulation=MOD_16QAM;
		_ldpc_rate=8/16.0;
		ofdm_preamble_configurator_Nsymb=2;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_14)
	{
		_modulation=MOD_8PSK;
		_ldpc_rate=14/16.0;
		ofdm_preamble_configurator_Nsymb=2;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_15)
	{
		_modulation=MOD_16QAM;
		_ldpc_rate=14/16.0;
		ofdm_preamble_configurator_Nsymb=2;
		ofdm_channel_estimator=ZERO_FORCE;
	}
	else if(configuration==CONFIG_16)
	{
		_modulation=MOD_32QAM;
		_ldpc_rate=14/16.0;
		ofdm_preamble_configurator_Nsymb=1;
		ofdm_channel_estimator=ZERO_FORCE;
	}

	if(_modulation==MOD_BPSK || _modulation==MOD_QPSK || _modulation==MOD_8PSK)
	{
		ofdm.channel_estimator_amplitude_restoration=YES;
	}
	else
	{
		ofdm.channel_estimator_amplitude_restoration=NO;
	}

	if(current_configuration!=CONFIG_NONE)
	{
		reinit_subsystems.microphone=NO;
		reinit_subsystems.speaker=NO;
		reinit_subsystems.telecom_system=NO;
		reinit_subsystems.data_container=NO;
		reinit_subsystems.ofdm_FIR_rx_data=NO;
		reinit_subsystems.ofdm_FIR_rx_time_sync=NO;
		reinit_subsystems.ofdm_FIR_tx1=NO;
		reinit_subsystems.ofdm_FIR_tx2=NO;
		reinit_subsystems.ofdm=NO;
		reinit_subsystems.ldpc=NO;
		reinit_subsystems.psk=NO;
		reinit_subsystems.pre_equalization_channel=NO;
	}

	if(current_configuration==CONFIG_NONE)
	{
		last_configuration=configuration;
		current_configuration=configuration;
	}
	else
	{
		last_configuration=current_configuration;
		current_configuration=configuration;
	}

	if(_modulation!=M || ofdm_preamble_configurator_Nsymb!=ofdm.preamble_configurator.Nsymb)
	{
		reinit_subsystems.microphone=YES;
		reinit_subsystems.speaker=YES;
		reinit_subsystems.telecom_system=YES;
		reinit_subsystems.data_container=YES;
		reinit_subsystems.ofdm=YES;
		reinit_subsystems.psk=YES;
	}

	if(_ldpc_rate!=ldpc.rate)
	{
		reinit_subsystems.telecom_system=YES;
		reinit_subsystems.ldpc=YES;
	}

	if(reinit_subsystems.microphone==YES)
	{
		microphone.deinit();
	}
	if(reinit_subsystems.speaker==YES)
	{
		speaker.deinit();
	}
	if(reinit_subsystems.psk==YES)
	{
		psk.deinit();
	}
	if(reinit_subsystems.telecom_system==YES)
	{
		this->deinit();
	}

	M=_modulation;
	ldpc.rate=_ldpc_rate;
	ofdm.preamble_configurator.Nsymb=ofdm_preamble_configurator_Nsymb;
	ofdm.channel_estimator=ofdm_channel_estimator;

	awgn_channel.set_seed(rand());

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
	ofdm.pilot_configurator.boost=default_configurations_telecom_system.ofdm_pilot_configurator_pilot_boost;
	ofdm.pilot_configurator.seed=default_configurations_telecom_system.ofdm_pilot_configurator_seed;
	ofdm.pilot_configurator.pilot_density=default_configurations_telecom_system.ofdm_pilot_density;

	ofdm.preamble_configurator.nIdentical_sections=default_configurations_telecom_system.ofdm_preamble_configurator_nIdentical_sections;
	ofdm.preamble_configurator.modulation=default_configurations_telecom_system.ofdm_preamble_configurator_modulation;
	ofdm.preamble_configurator.boost=default_configurations_telecom_system.ofdm_preamble_configurator_boost;
	ofdm.preamble_configurator.seed=default_configurations_telecom_system.ofdm_preamble_configurator_seed;

	ofdm.freq_offset_ignore_limit=default_configurations_telecom_system.ofdm_freq_offset_ignore_limit;
	ofdm.start_shift=default_configurations_telecom_system.ofdm_start_shift;

	ofdm.preamble_papr_cut=default_configurations_telecom_system.ofdm_preamble_papr_cut;
	ofdm.data_papr_cut=default_configurations_telecom_system.ofdm_data_papr_cut;

	ofdm.LS_window_width=default_configurations_telecom_system.ofdm_LS_window_width;
	ofdm.LS_window_hight=default_configurations_telecom_system.ofdm_LS_window_hight;

	if(ofdm.LS_window_width%2==0)
	{
		ofdm.LS_window_width++;
	}
	if(ofdm.LS_window_hight%2==0)
	{
		ofdm.LS_window_hight++;
	}

	bit_energy_dispersal_seed=default_configurations_telecom_system.bit_energy_dispersal_seed;

	ldpc.standard=default_configurations_telecom_system.ldpc_standard;
	ldpc.framesize=default_configurations_telecom_system.ldpc_framesize;

	ldpc.decoding_algorithm=default_configurations_telecom_system.ldpc_decoding_algorithm;
	ldpc.GBF_eta=default_configurations_telecom_system.ldpc_GBF_eta;
	ldpc.nIteration_max=default_configurations_telecom_system.ldpc_nIteration_max;
	ldpc.print_nIteration=default_configurations_telecom_system.ldpc_print_nIteration;

	outer_code=default_configurations_telecom_system.outer_code;

	if(outer_code==CRC16_MODBUS_RTU)
	{
		outer_code_reserved_bits=16;
	}
	else
	{
		outer_code_reserved_bits=0;
	}

	bandwidth=default_configurations_telecom_system.bandwidth;
	time_sync_trials_max=default_configurations_telecom_system.time_sync_trials_max;
	use_last_good_time_sync=default_configurations_telecom_system.use_last_good_time_sync;
	use_last_good_freq_offset=default_configurations_telecom_system.use_last_good_freq_offset;
	frequency_interpolation_rate=default_configurations_telecom_system.frequency_interpolation_rate;
	carrier_frequency=default_configurations_telecom_system.carrier_frequency;
	output_power_Watt=default_configurations_telecom_system.output_power_Watt;

	ofdm.FIR_rx_data.filter_window=default_configurations_telecom_system.ofdm_FIR_rx_data_filter_window;
	ofdm.FIR_rx_data.filter_transition_bandwidth=default_configurations_telecom_system.ofdm_FIR_rx_data_filter_transition_bandwidth;
	ofdm.FIR_rx_data.lpf_filter_cut_frequency=default_configurations_telecom_system.ofdm_FIR_rx_data_lpf_filter_cut_frequency;
	ofdm.FIR_rx_data.type=default_configurations_telecom_system.ofdm_FIR_rx_data_filter_type;

	ofdm.FIR_rx_time_sync.filter_window=default_configurations_telecom_system.ofdm_FIR_rx_time_sync_filter_window;
	ofdm.FIR_rx_time_sync.filter_transition_bandwidth=default_configurations_telecom_system.ofdm_FIR_rx_time_sync_filter_transition_bandwidth;
	ofdm.FIR_rx_time_sync.lpf_filter_cut_frequency=default_configurations_telecom_system.ofdm_FIR_rx_time_sync_lpf_filter_cut_frequency;
	ofdm.FIR_rx_time_sync.type=default_configurations_telecom_system.ofdm_FIR_rx_time_sync_filter_type;


	ofdm.FIR_tx1.filter_window=default_configurations_telecom_system.ofdm_FIR_tx1_filter_window;
	ofdm.FIR_tx1.filter_transition_bandwidth=default_configurations_telecom_system.ofdm_FIR_tx1_filter_transition_bandwidth;
	ofdm.FIR_tx1.lpf_filter_cut_frequency=default_configurations_telecom_system.ofdm_FIR_tx1_lpf_filter_cut_frequency;
	ofdm.FIR_tx1.hpf_filter_cut_frequency=default_configurations_telecom_system.ofdm_FIR_tx1_hpf_filter_cut_frequency;
	ofdm.FIR_tx1.type=default_configurations_telecom_system.ofdm_FIR_tx1_filter_type;

	ofdm.FIR_tx2.filter_window=default_configurations_telecom_system.ofdm_FIR_tx2_filter_window;
	ofdm.FIR_tx2.filter_transition_bandwidth=default_configurations_telecom_system.ofdm_FIR_tx2_filter_transition_bandwidth;
	ofdm.FIR_tx2.lpf_filter_cut_frequency=default_configurations_telecom_system.ofdm_FIR_tx2_lpf_filter_cut_frequency;
	ofdm.FIR_tx2.hpf_filter_cut_frequency=default_configurations_telecom_system.ofdm_FIR_tx2_hpf_filter_cut_frequency;
	ofdm.FIR_tx2.type=default_configurations_telecom_system.ofdm_FIR_tx2_filter_type;


	constellation_plot.folder=default_configurations_telecom_system.plot_folder;
	BER_plot.folder=default_configurations_telecom_system.plot_folder;
	constellation_plot.plot_active=default_configurations_telecom_system.plot_plot_active;
	BER_plot.plot_active=default_configurations_telecom_system.plot_plot_active;


	microphone.dev_name=default_configurations_telecom_system.microphone_dev_name;
	speaker.dev_name=default_configurations_telecom_system.speaker_dev_name;


	data_container.sound_device_ptr=(void*)&microphone;
	microphone.type=default_configurations_telecom_system.microphone_type;
	microphone.channels=default_configurations_telecom_system.microphone_channels;
	microphone.data_container_ptr=&data_container;


	speaker.type=default_configurations_telecom_system.speaker_type;
	speaker.channels=default_configurations_telecom_system.speaker_channels;
	speaker.frames_to_leave_transmit_fct=default_configurations_telecom_system.speaker_frames_to_leave_transmit_fct;

	if(reinit_subsystems.psk==YES)
	{
		psk.set_predefined_constellation(M);
		reinit_subsystems.psk=NO;
	}
	if(reinit_subsystems.telecom_system==YES)
	{
		this->init();
		reinit_subsystems.telecom_system=NO;
	}

	bit_interleaver_block_size=data_container.nBits/10;
	time_freq_interleaver_block_size=data_container.nData/10;

	if(default_configurations_telecom_system.ofdm_time_sync_Nsymb==AUTO_SELLECT)
	{
		ofdm.time_sync_Nsymb=ofdm.Nsymb;
	}

	if(reinit_subsystems.microphone==YES)
	{
		microphone.baudrate=sampling_frequency;
		microphone.nbuffer_Samples=2 * ofdm.Nfft*(1+ofdm.gi)*frequency_interpolation_rate*(ofdm.Nsymb+ofdm.preamble_configurator.Nsymb);
		microphone.frames_per_period=ofdm.Nfft*(1+ofdm.gi)*frequency_interpolation_rate;
		if(operation_mode!=BER_PLOT_baseband && operation_mode!=BER_PLOT_passband && operation_mode!=TX_TEST)
		{
			microphone.init();
		}
		reinit_subsystems.microphone=NO;
	}

	if(reinit_subsystems.speaker==YES)
	{
		speaker.baudrate=sampling_frequency;
		speaker.nbuffer_Samples=2 * ofdm.Nfft*(1+ofdm.gi)*frequency_interpolation_rate*(ofdm.Nsymb+ofdm.preamble_configurator.Nsymb);
		speaker.frames_per_period=ofdm.Nfft*(1+ofdm.gi)*frequency_interpolation_rate;
		if(operation_mode!=BER_PLOT_baseband && operation_mode!=BER_PLOT_passband && operation_mode!=RX_TEST)
		{
			speaker.init();
		}
		reinit_subsystems.speaker=NO;
	}
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

	if(SNR>13.5)
	{
		configuration=CONFIG_16;
	}
	else if(SNR>12.5)
	{
		configuration=CONFIG_15;
	}
	else if(SNR>9)
	{
		configuration=CONFIG_14;
	}
	else if(SNR>7.5)
	{
		configuration=CONFIG_13;
	}
	else if(SNR>6.5)
	{
		configuration=CONFIG_12;
	}
	else if(SNR>4)
	{
		configuration=CONFIG_11;
	}
	else if(SNR>3)
	{
		configuration=CONFIG_10;
	}
	else if(SNR>1.5)
	{
		configuration=CONFIG_9;
	}
	else if(SNR>0.5)
	{
		configuration=CONFIG_8;
	}
	else if(SNR>-0.5)
	{
		configuration=CONFIG_7;
	}
	else if(SNR>-1.5)
	{
		configuration=CONFIG_6;
	}
	else if(SNR>-2.5)
	{
		configuration=CONFIG_5;
	}
	else if(SNR>-3.5)
	{
		configuration=CONFIG_4;
	}
	else if(SNR>-4.5)
	{
		configuration=CONFIG_3;
	}
	else if(SNR>-6)
	{
		configuration=CONFIG_2;
	}
	else if(SNR>-7.5)
	{
		configuration=CONFIG_1;
	}
	else
	{
		configuration=CONFIG_0;
	}

	return configuration;
}

void cl_telecom_system::get_pre_equalization_channel()
{
	int nTries=1000;
	for(int i=0;i<data_container.Nc;i++)
	{
		pre_equalization_channel[i].value=0;
	}

	for(int j=0;j<nTries;j++)
	{
		for(int i=0;i<data_container.Nc*log2(data_container.M);i++)
		{
			data_container.bit_interleaved_data[i]=rand()%2;
		}
		psk.mod(data_container.bit_interleaved_data,data_container.Nc*log2(data_container.M),data_container.modulated_data);

		ofdm.symbol_mod(data_container.modulated_data,data_container.preamble_symbol_modulated_data);
		ofdm.passband_start_sample=0;
		ofdm.baseband_to_passband(data_container.preamble_symbol_modulated_data,data_container.Nofdm,data_container.passband_data_tx,sampling_frequency,carrier_frequency,carrier_amplitude,frequency_interpolation_rate);

		ofdm.FIR_tx1.apply(data_container.passband_data_tx,data_container.passband_data_tx_filtered_fir_1,data_container.Nofdm*frequency_interpolation_rate);
		ofdm.FIR_tx2.apply(data_container.passband_data_tx_filtered_fir_1,data_container.passband_data_tx_filtered_fir_2,data_container.Nofdm*frequency_interpolation_rate);

		ofdm.passband_to_baseband(data_container.passband_data_tx_filtered_fir_2,data_container.Nofdm*frequency_interpolation_rate,data_container.baseband_data,sampling_frequency,carrier_frequency,carrier_amplitude,data_container.interpolation_rate,&ofdm.FIR_rx_data);
		ofdm.symbol_demod(data_container.baseband_data,data_container.ofdm_symbol_demodulated_data);

		for(int i=0;i<data_container.Nc;i++)
		{
			pre_equalization_channel[i].value+=data_container.modulated_data[i]/data_container.ofdm_symbol_demodulated_data[i];
		}
	}

	for(int i=0;i<data_container.Nc;i++)
	{
		pre_equalization_channel[i].value/=nTries;
	}

}
