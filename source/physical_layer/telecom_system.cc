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
#include "audioio/audioio.h"
#include <chrono>

#ifdef MERCURY_GUI_ENABLED
#include "gui/gui_state.h"
#endif

extern cbuf_handle_t capture_buffer;
extern cbuf_handle_t playback_buffer;

// Test mode: artificial TX carrier offset in Hz (for testing frequency sync)
extern "C" double test_tx_carrier_offset;


cl_telecom_system::cl_telecom_system()
{
	receive_stats.iterations_done=-1;
	receive_stats.delay=0;
	receive_stats.delay_of_last_decoded_message=-1;
	receive_stats.mfsk_search_raw=0;
	receive_stats.time_peak_symb_location=0;
	receive_stats.time_peak_subsymb_location=0;
	receive_stats.sync_trials=0;
	receive_stats.phase_error_avg=0;
	receive_stats.freq_offset=0;
	receive_stats.freq_offset_of_last_decoded_message=0;
	receive_stats.message_decoded=NO;
	receive_stats.SNR=-99.9;
	receive_stats.signal_stregth_dbm=-999;
	receive_stats.mfsk_search_raw=0;

	time_sync_trials_max=20;
	use_last_good_time_sync=NO;
	use_last_good_freq_offset=NO;
	mfsk_fixed_delay=-1;
	test_puncture_nBits=0;
	ctrl_nBits=0;
	ctrl_nsymb=0;
	mfsk_ctrl_mode=false;
	ack_pattern_passband_samples=0;
	ack_pattern_detection_threshold=0.8;
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
			data_container.data_bit[i]=__random()%2;
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
	// For OFDM: sigma = 1/sqrt(10^(EsN0/10)) is the standard Es/N0 formula.
	// For MFSK: EsN0 parameter is treated as channel SNR (dB).
	// Sigma is calibrated by measuring actual transmitted signal power so that
	// SNR = P_signal / P_noise = P_signal / (sigma^2/2).
	float sigma = 0;
	bool sigma_calibrated = (M != MOD_MFSK);
	if(M != MOD_MFSK)
	{
		sigma = 1.0f / sqrt(pow(10.0f, (EsN0 / 10.0f)));
	}
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
	// For MFSK, nData can be very large (>15000) - skip constellation plot to avoid stack overflow
	int nDataPlot = (M == MOD_MFSK) ? 0 : ofdm.pilot_configurator.nData;
	float contellation[nDataPlot*constellation_plot_nFrames+1][2]={};

	while(lerror_rate.Frames_total<max_frame_no)
	{
		for(int i=0;i<nReal_data-outer_code_reserved_bits;i++)
		{
			data_container.data_bit[i]=__random()%2;
		}
		bit_to_byte(data_container.data_bit,data_container.data_byte,nReal_data-outer_code_reserved_bits);
		this->transmit_byte(data_container.data_byte,(nReal_data-outer_code_reserved_bits)/8,data_container.passband_data,SINGLE_MESSAGE);

		// MFSK: calibrate sigma from measured signal power (first frame only)
		// In-band channel SNR: SNR = P_sig / P_noise_inband
		// where P_noise_inband = P_noise_total * (BW_signal / f_nyquist)
		// P_noise_total per sample = sigma^2/2 (from AWGN apply)
		// sigma = sqrt(2 * P_sig * f_nyquist / (SNR_linear * BW_signal))
		if(!sigma_calibrated)
		{
			int nSamples = (data_container.Nofdm * (data_container.Nsymb + data_container.preamble_nSymb)) * frequency_interpolation_rate;
			double P_sig = 0;
			for(int i = 0; i < nSamples; i++)
			{
				P_sig += data_container.passband_data[i] * data_container.passband_data[i];
			}
			P_sig /= nSamples;
			double f_nyquist = sampling_frequency / 2.0;
			sigma = (float)sqrt(2.0 * P_sig * f_nyquist / (pow(10.0, EsN0 / 10.0) * bandwidth));
			sigma_calibrated = true;
		}

		awgn_channel.apply_with_delay(data_container.passband_data,data_container.passband_delayed_data,sigma,(data_container.Nofdm*(data_container.Nsymb+data_container.preamble_nSymb))*this->frequency_interpolation_rate,((data_container.preamble_nSymb+2)*data_container.Nofdm+delay)*frequency_interpolation_rate);
		if(M == MOD_MFSK)
		{
			mfsk_fixed_delay = ((data_container.preamble_nSymb+2)*data_container.Nofdm+delay)*frequency_interpolation_rate;
		}
		this->receive_byte(data_container.passband_delayed_data,data_container.hd_decoded_data_byte);
		mfsk_fixed_delay = -1;
		byte_to_bit(data_container.hd_decoded_data_byte,data_container.hd_decoded_data_bit,(nReal_data-outer_code_reserved_bits));

		if(nDataPlot > 0)
		{
			if(ofdm.channel_estimator_amplitude_restoration==YES)
			{
				for(int i=0;i<nDataPlot;i++)
				{
					contellation[constellation_plot_counter*nDataPlot+i][0]=data_container.ofdm_deframed_data_without_amplitude_restoration[i].real();
					contellation[constellation_plot_counter*nDataPlot+i][1]=data_container.ofdm_deframed_data_without_amplitude_restoration[i].imag();
				}
			}
			else
			{
				for(int i=0;i<nDataPlot;i++)
				{
					contellation[constellation_plot_counter*nDataPlot+i][0]=data_container.ofdm_deframed_data[i].real();
					contellation[constellation_plot_counter*nDataPlot+i][1]=data_container.ofdm_deframed_data[i].imag();
				}
			}

			constellation_plot_counter++;

			if(constellation_plot_counter==constellation_plot_nFrames)
			{
				constellation_plot_counter=0;
				constellation_plot.plot_constellation(&contellation[0][0],nDataPlot*constellation_plot_nFrames);
			}
		}

		lerror_rate.check(data_container.data_bit,data_container.hd_decoded_data_bit,nReal_data-outer_code_reserved_bits);
	}
	return lerror_rate;
}

int cl_telecom_system::get_frame_size_bytes()
{
    return (data_container.nBits - ldpc.P - outer_code_reserved_bits) / 8;
}

int cl_telecom_system::get_frame_size_bits()
{
    return data_container.nBits - ldpc.P - outer_code_reserved_bits;
}

void cl_telecom_system::transmit_byte(int *data, int nBytes, double* out, int message_location)
{
	int nReal_data = data_container.nBits - ldpc.P;
	int msB = 0, lsB = 0; // TODO: type mismatch here.. we should covert this to uint8_t

	if(nBytes > (nReal_data-outer_code_reserved_bits) / 8)
	{
		std::cout<<"message too long.. not sent."<<std::endl;
		return;
	}

	byte_to_bit(data, data_container.data_bit, nBytes);

    // TODO: outer_code is byte aligned... better move this to transmit_bit not to waste bits
	if(outer_code==CRC16_MODBUS_RTU)
	{
		uint16_t crc = CRC16_MODBUS_RTU_calc((int*)data, nBytes);
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

void cl_telecom_system::transmit_bit(int* data, double* out, int message_location)
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

	if(M == MOD_MFSK)
	{
		// MFSK: bits → one-hot subcarrier vectors, directly to framed data
		// In ctrl mode, only modulate first ctrl_nBits interleaved bits (fewer symbols)
		int active_nbits = get_active_nbits();
		mfsk.mod(data_container.bit_interleaved_data, active_nbits, data_container.ofdm_framed_data);
	}
	else
	{
		psk.mod(data_container.bit_interleaved_data,data_container.nBits,data_container.modulated_data);
		interleaver(data_container.modulated_data, data_container.ofdm_time_freq_interleaved_data, data_container.nData, time_freq_interleaver_block_size);
		ofdm.framer(data_container.ofdm_time_freq_interleaved_data,data_container.ofdm_framed_data);
	}

	if(M == MOD_MFSK)
	{
		// MFSK preamble: known single-tone symbols (concentrated energy, detectable in weak signal)
		mfsk.generate_preamble(data_container.preamble_data, data_container.preamble_nSymb);
	}
	else
	{
		// OFDM preamble: broadband known symbols (all subcarriers)
		for(int i=0;i<data_container.preamble_nSymb*ofdm.Nc;i++)
		{
			data_container.preamble_data[i]=ofdm.ofdm_preamble[i].value;
		}
	}

	if(M != MOD_MFSK)
	{
		// Pre-equalization (OFDM only, not used for MFSK)
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
	}

	for(int i=0;i<data_container.preamble_nSymb;i++)
	{
		ofdm.symbol_mod(&data_container.preamble_data[i*data_container.Nc],&data_container.preamble_symbol_modulated_data[i*data_container.Nofdm]);
	}

	int active_nsymb = get_active_nsymb();

	for(int i=0;i<active_nsymb;i++)
	{
		ofdm.symbol_mod(&data_container.ofdm_framed_data[i*data_container.Nc],&data_container.ofdm_symbol_modulated_data[i*data_container.Nofdm]);
	}

	// MFSK amplitude boost: equalize drive level with OFDM
	// power_normalization assumes Nc active subcarriers (OFDM), but MFSK has only nStreams.
	// RMS scales as sqrt(nActive), so base boost = sqrt(Nc / nStreams).
	// -2 dB trim: OFDM peak_clip removes ~2 dB of energy; MFSK isn't clipped (low PAPR).
	double mfsk_boost = 1.0;
	if(M == MOD_MFSK)
	{
		mfsk_boost = sqrt((double)data_container.Nc / mfsk.nStreams) * pow(10.0, -2.0 / 20.0);
	}

	for(int j=0;j<data_container.Nofdm*data_container.preamble_nSymb;j++)
	{
		data_container.preamble_symbol_modulated_data[j]/=power_normalization;
		data_container.preamble_symbol_modulated_data[j]*=sqrt(output_power_Watt)*ofdm.preamble_configurator.boost*mfsk_boost;
	}

	for(int j=0;j<data_container.Nofdm*active_nsymb;j++)
	{
		data_container.ofdm_symbol_modulated_data[j]/=power_normalization;
		data_container.ofdm_symbol_modulated_data[j]*=sqrt(output_power_Watt)*mfsk_boost;
	}

	// Apply test TX carrier offset for frequency sync testing
	double tx_carrier = carrier_frequency + test_tx_carrier_offset;
	ofdm.baseband_to_passband(data_container.preamble_symbol_modulated_data,data_container.Nofdm*data_container.preamble_nSymb,data_container.passband_data_tx,sampling_frequency,tx_carrier,carrier_amplitude,frequency_interpolation_rate);
	ofdm.baseband_to_passband(data_container.ofdm_symbol_modulated_data,data_container.Nofdm*active_nsymb,&data_container.passband_data_tx[data_container.Nofdm*data_container.preamble_nSymb*frequency_interpolation_rate],sampling_frequency,tx_carrier,carrier_amplitude,frequency_interpolation_rate);

	ofdm.peak_clip(data_container.passband_data_tx, data_container.Nofdm*data_container.preamble_nSymb*frequency_interpolation_rate,ofdm.preamble_papr_cut);
	ofdm.peak_clip(&data_container.passband_data_tx[data_container.Nofdm*data_container.preamble_nSymb*frequency_interpolation_rate], data_container.Nofdm*active_nsymb*frequency_interpolation_rate,ofdm.data_papr_cut);

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

st_receive_stats cl_telecom_system::receive_bit(double *data, int* out)
{
	int nReal_data=data_container.nBits-ldpc.P;

	st_receive_stats tmp=receive_byte(data,data_container.hd_decoded_data_byte);
	byte_to_bit(data_container.hd_decoded_data_byte, out, nReal_data/8);

	return tmp;
}

st_receive_stats cl_telecom_system::receive_byte(double *data, int* out)
{

	float variance;
	int nVirtual_data=ldpc.N-data_container.nBits;
	int nReal_data=data_container.nBits-ldpc.P;
	double freq_offset_measured=0;
	receive_stats.message_decoded=NO;
	receive_stats.frame_overflow_symbols=0;
	receive_stats.sync_trials=0;

	int step=100;
	int pream_symb_loc;

	// Coarse frequency offset - starts at 0, only searched on trial 1 if trial 0 fails
	double coarse_freq_offset = 0.0;

	if(mfsk_fixed_delay >= 0)
	{
		// Known delay (BER test or overflow recapture) - bypass time_sync entirely.
		// BER test: delay set per-frame at line 293, clearing is safe.
		// Overflow recapture: delay pre-adjusted for buffer shift, use once.
		receive_stats.delay = mfsk_fixed_delay;
		mfsk_fixed_delay = -1;
		pream_symb_loc = receive_stats.delay / (data_container.Nofdm * data_container.interpolation_rate);
		if(pream_symb_loc < 1) { pream_symb_loc = 1; }
		receive_stats.signal_stregth_dbm = 0;
	}
	else
	{
		ofdm.passband_to_baseband((double*)data,data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate,data_container.baseband_data_interpolated,sampling_frequency,carrier_frequency,carrier_amplitude,1,&ofdm.FIR_rx_time_sync);

		receive_stats.signal_stregth_dbm=ofdm.measure_signal_stregth(data_container.baseband_data_interpolated, data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate);

		if(M == MOD_MFSK)
		{
			// MFSK: correlate against known preamble tone sequence
			// Anti-re-decode: skip past where previous preamble sits in buffer
			int search_start = receive_stats.mfsk_search_raw - data_container.nUnder_processing_events;
			if(search_start < 0) search_start = 0;
			receive_stats.delay=ofdm.time_sync_mfsk(data_container.baseband_data_interpolated,data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate,data_container.interpolation_rate,data_container.preamble_nSymb,mfsk.preamble_tones,mfsk.M,mfsk.nStreams,mfsk.stream_offsets,search_start);

		}
		else
		{
			TimeSyncResult coarse_result = ofdm.time_sync_preamble_with_metric(data_container.baseband_data_interpolated,data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate,data_container.interpolation_rate,0,step, 1);
			receive_stats.delay = coarse_result.delay;
			receive_stats.coarse_metric = coarse_result.correlation;
		}
		pream_symb_loc=receive_stats.delay/(data_container.Nofdm*data_container.interpolation_rate);
		if(pream_symb_loc<1){pream_symb_loc=1;}
	}

	// MFSK frame completeness check: if the frame extends beyond the buffer,
	// don't attempt decode — signal the ARQ layer to capture more audio.
	// This replaces fixed turnaround guard margins with adaptive recapture.
	if(M == MOD_MFSK && mfsk_fixed_delay < 0)
	{
		int sym_samples = data_container.Nofdm * frequency_interpolation_rate;
		int active_nsymb = get_active_nsymb();
		int frame_end_samples = receive_stats.delay + (data_container.preamble_nSymb + active_nsymb) * sym_samples;
		int buffer_samples = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;
		if(frame_end_samples > buffer_samples)
		{
			receive_stats.message_decoded = NO;
			int overflow_samples = frame_end_samples - buffer_samples;
			receive_stats.frame_overflow_symbols = (overflow_samples + sym_samples - 1) / sym_samples;
			return receive_stats;
		}
	}

	int lower_bound = data_container.preamble_nSymb;
	int upper_bound = data_container.buffer_Nsymb-(data_container.Nsymb+data_container.preamble_nSymb);

	if(M != MOD_MFSK)
	{
		printf("[OFDM-SYNC] coarse: pream_symb=%d delay=%d bounds=[%d,%d] metric=%.3f %s\n",
			pream_symb_loc, receive_stats.delay, lower_bound, upper_bound,
			receive_stats.coarse_metric,
			(pream_symb_loc > lower_bound && pream_symb_loc < upper_bound) ? "PASS" : "SKIP");
		fflush(stdout);
	}

	// Recovery for OFDM when preamble lands outside the valid bounds.
	// Even with full-buffer coarse search, Schmidl-Cox may peak at a position
	// too close to the start or end to extract a complete frame. Scan the full
	// buffer for signal energy and re-run Schmidl-Cox from the signal start.
	if(M != MOD_MFSK && !(pream_symb_loc > lower_bound && pream_symb_loc < upper_bound))
	{
		int sym_samples = data_container.Nofdm * frequency_interpolation_rate;
		int buf_samples = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;

		printf("[OFDM-SYNC] bounds-failed: pream_symb=%d, scanning full buffer for signal\n", pream_symb_loc);
		fflush(stdout);

		int signal_start_symb = -1;
		for(int s = lower_bound + 1; s < upper_bound; s++)
		{
			int offset = s * sym_samples;
			double e = 0.0;
			int cnt = 0;
			for(int i = 0; i < sym_samples && (offset + i) < buf_samples; i++)
			{
				double re = data_container.baseband_data_interpolated[offset + i].real();
				double im = data_container.baseband_data_interpolated[offset + i].imag();
				e += re*re + im*im;
				cnt++;
			}
			e = (cnt > 0) ? e / cnt : 0.0;
			if(e > 0.001)
			{
				signal_start_symb = s;
				break;
			}
		}

		if(signal_start_symb >= 0)
		{
			int search_start = signal_start_symb * sym_samples;
			int available = buf_samples - search_start;

			if(available > data_container.preamble_nSymb * sym_samples)
			{
				TimeSyncResult retry = ofdm.time_sync_preamble_with_metric(
					&data_container.baseband_data_interpolated[search_start],
					available, frequency_interpolation_rate, 0, step, 1);
				retry.delay += search_start;

				int retry_symb = retry.delay / sym_samples;
				if(retry_symb < 1) retry_symb = 1;

				double retry_energy = 0.0;
				int rcnt = 0;
				for(int i = 0; i < sym_samples && (retry.delay + i) < buf_samples; i++)
				{
					double re = data_container.baseband_data_interpolated[retry.delay + i].real();
					double im = data_container.baseband_data_interpolated[retry.delay + i].imag();
					retry_energy += re*re + im*im;
					rcnt++;
				}
				retry_energy = (rcnt > 0) ? retry_energy / rcnt : 0.0;

				printf("[OFDM-SYNC] bounds-skip: signal=%d retry=%d metric=%.3f energy=%.2e\n",
					signal_start_symb, retry_symb, retry.correlation, retry_energy);
				fflush(stdout);

				if(retry_energy >= 0.001 && retry.correlation >= 0.5
					&& retry_symb > lower_bound && retry_symb < upper_bound)
				{
					receive_stats.delay = retry.delay;
					receive_stats.coarse_metric = retry.correlation;
					pream_symb_loc = retry_symb;
				}
			}
		}
	}

	if(pream_symb_loc > lower_bound && pream_symb_loc < upper_bound)
	{
		// Signal energy gate: reject false preamble detections in silence.
		// Schmidl-Cox gives high correlation on near-zero noise (ratio of tiny
		// values is unstable). Check actual signal energy at the detected
		// preamble position before spending ~120ms on 3 LDPC decode trials.
		bool energy_ok = true;
		if(M != MOD_MFSK)
		{
			int sym_samples = data_container.Nofdm * frequency_interpolation_rate;
			int buf_samples = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;
			double energy_sum = 0.0;
			int count = 0;
			for(int i = 0; i < sym_samples && (receive_stats.delay + i) < buf_samples; i++)
			{
				double re = data_container.baseband_data_interpolated[receive_stats.delay + i].real();
				double im = data_container.baseband_data_interpolated[receive_stats.delay + i].imag();
				energy_sum += re*re + im*im;
				count++;
			}
			double mean_energy = (count > 0) ? energy_sum / count : 0.0;

			// Threshold: real OFDM signal has mean energy ~0.05-0.25;
			// VB-Cable silence has ~1e-10. Use 0.001 as conservative gate.
			if(mean_energy < 0.001)
			{
				printf("[OFDM-SYNC] energy=%.2e at delay=%d — silence, skipping decode\n",
					mean_energy, receive_stats.delay);
				fflush(stdout);
				energy_ok = false;
			}

			// Metric threshold: reject weak Schmidl-Cox peaks that pass the energy
			// gate but correspond to noise-on-signal, not a real OFDM preamble.
			// Real preambles: metric 0.80-0.93. Noise peaks: metric 0.09-0.17.
			// Threshold 0.5 provides wide margin between the two clusters.
			if(energy_ok && receive_stats.coarse_metric < 0.5)
			{
				printf("[OFDM-SYNC] metric=%.3f at delay=%d — weak peak, skipping decode\n",
					receive_stats.coarse_metric, receive_stats.delay);
				fflush(stdout);
				energy_ok = false;
			}

			// Silence-skip: when the best Schmidl-Cox peak lands in silence
			// (energy gate rejected), scan forward to find where signal actually
			// starts and re-run Schmidl-Cox from there. This handles
			// silence-preceded buffers (e.g. SET_CONFIG after ACK) where silence
			// or boundary peaks beat the real preamble in the initial search.
			// On real HF this rarely activates (noise floor > energy threshold),
			// but keeps Schmidl-Cox correlation for when it matters.
			if(!energy_ok)
			{
				int signal_start_symb = -1;
				for(int s = pream_symb_loc + 1; s < upper_bound; s++)
				{
					int offset = s * sym_samples;
					double e = 0.0;
					int cnt = 0;
					for(int i = 0; i < sym_samples && (offset + i) < buf_samples; i++)
					{
						double re = data_container.baseband_data_interpolated[offset + i].real();
						double im = data_container.baseband_data_interpolated[offset + i].imag();
						e += re*re + im*im;
						cnt++;
					}
					e = (cnt > 0) ? e / cnt : 0.0;
					if(e > 0.001)
					{
						signal_start_symb = s;
						break;
					}
				}

				if(signal_start_symb >= 0)
				{
					int search_start = signal_start_symb * sym_samples;
					int available = buf_samples - search_start;

					if(available > data_container.preamble_nSymb * sym_samples)
					{
						TimeSyncResult retry = ofdm.time_sync_preamble_with_metric(
							&data_container.baseband_data_interpolated[search_start],
							available, frequency_interpolation_rate, 0, step, 1);
						retry.delay += search_start;

						int retry_symb = retry.delay / sym_samples;
						if(retry_symb < 1) retry_symb = 1;

						// Check energy at the retry position
						double retry_energy = 0.0;
						int rcnt = 0;
						for(int i = 0; i < sym_samples && (retry.delay + i) < buf_samples; i++)
						{
							double re = data_container.baseband_data_interpolated[retry.delay + i].real();
							double im = data_container.baseband_data_interpolated[retry.delay + i].imag();
							retry_energy += re*re + im*im;
							rcnt++;
						}
						retry_energy = (rcnt > 0) ? retry_energy / rcnt : 0.0;

						printf("[OFDM-SYNC] silence-skip: orig=%d signal=%d retry=%d metric=%.3f energy=%.2e\n",
							pream_symb_loc, signal_start_symb, retry_symb, retry.correlation, retry_energy);
						fflush(stdout);

						if(retry_energy >= 0.001 && retry.correlation >= 0.5
							&& retry_symb > lower_bound && retry_symb < upper_bound)
						{
							receive_stats.delay = retry.delay;
							receive_stats.coarse_metric = retry.correlation;
							pream_symb_loc = retry_symb;
							energy_ok = true;
						}
					}
				}
			}
		}

		if(energy_ok)
		{
		int skip_h_count = 0;
		bool skip_h_recovery_attempted = false;
skip_h_retry_point:
		while (receive_stats.sync_trials<=time_sync_trials_max)
		{
			if(mfsk_fixed_delay >= 0)
			{
				// Known delay - skip all time_sync refinement
				receive_stats.delay = mfsk_fixed_delay;
			}
			else if(M == MOD_MFSK)
			{
				// MFSK: time_sync_mfsk already found optimal position, no refinement needed
				// Only one trial - spectral flatness sync is deterministic
				if(receive_stats.sync_trials > 0) break;
				// Trial 0: use delay from initial sync as-is
			}
			else if(receive_stats.sync_trials==time_sync_trials_max && use_last_good_time_sync==YES && receive_stats.delay_of_last_decoded_message!=-1)
			{
				receive_stats.delay=receive_stats.delay_of_last_decoded_message;
			}
			else if (receive_stats.sync_trials == 1 && g_gui_state.coarse_freq_sync_enabled.load())
			{
				// Trial 0 failed - try coarse frequency search before trial 1
				// Search ±30 Hz; Moose handles ±22 Hz residual at each,
				// giving ±52 Hz total coverage
				const double freq_search[] = {-30.0, 0.0, 30.0};
				const int n_search = 3;
				double best_correlation = 0.0;
				double best_offset = 0.0;
				int best_delay = receive_stats.delay;
				double zero_hz_correlation = 0.0;

				for (int i = 0; i < n_search; i++)
				{
					ofdm.passband_to_baseband((double*)data,
						data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate,
						data_container.baseband_data_interpolated,
						sampling_frequency,
						carrier_frequency + freq_search[i],
						carrier_amplitude, 1, &ofdm.FIR_rx_time_sync);

					TimeSyncResult ts_result = ofdm.time_sync_preamble_with_metric(
						data_container.baseband_data_interpolated,
						data_container.Nofdm * (2 * data_container.preamble_nSymb + data_container.Nsymb) * frequency_interpolation_rate,
						data_container.interpolation_rate, 0, step, 1);

					if (fabs(freq_search[i]) < 0.1)
						zero_hz_correlation = ts_result.correlation;

					if (ts_result.correlation > best_correlation)
					{
						best_correlation = ts_result.correlation;
						best_offset = freq_search[i];
						best_delay = ts_result.delay;
					}
				}

				// Apply only if non-zero offset is significantly better than 0 Hz
				if (fabs(best_offset) > 1.0 && best_correlation > 0.5 &&
				    best_correlation > zero_hz_correlation + 0.1)
				{
					coarse_freq_offset = best_offset;
					receive_stats.delay = best_delay;
					pream_symb_loc = receive_stats.delay / (data_container.Nofdm * data_container.interpolation_rate);
					if (pream_symb_loc < 1) { pream_symb_loc = 1; }
				}

				// Restore baseband for time sync (at possibly corrected frequency)
				ofdm.passband_to_baseband((double*)data,
					data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate,
					data_container.baseband_data_interpolated,
					sampling_frequency,
					carrier_frequency + coarse_freq_offset,
					carrier_amplitude, 1, &ofdm.FIR_rx_time_sync);

				// Fine time sync at the corrected frequency
				// Window = preamble+4 symbols (±2 sym search range) to handle coarse
				// Schmidl-Cox step=100 quantization + integer truncation of pream_symb_loc
				TimeSyncResult ts_result = ofdm.time_sync_preamble_with_metric(
					&data_container.baseband_data_interpolated[(pream_symb_loc-1)*data_container.Nofdm*frequency_interpolation_rate],
					(ofdm.preamble_configurator.Nsymb+4)*data_container.Nofdm*data_container.interpolation_rate,
					data_container.interpolation_rate,
					receive_stats.sync_trials, 1, time_sync_trials_max);
				receive_stats.delay = (pream_symb_loc-1)*data_container.Nofdm*frequency_interpolation_rate + ts_result.delay;
			}
			else
			{
				// Window = preamble+4 symbols (±2 sym search range) — see trial 1 comment
			receive_stats.delay=(pream_symb_loc-1)*data_container.Nofdm*frequency_interpolation_rate+ofdm.time_sync_preamble(&data_container.baseband_data_interpolated[(pream_symb_loc-1)*data_container.Nofdm*frequency_interpolation_rate],(ofdm.preamble_configurator.Nsymb+4)*data_container.Nofdm*data_container.interpolation_rate,data_container.interpolation_rate,receive_stats.sync_trials,1,time_sync_trials_max);
			}

			if(receive_stats.delay<0){receive_stats.delay=0;}

			// Clamp delay to prevent buffer overflow in rational_resampler
			{
				int buf_size = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;
				int frame_size = (data_container.Nofdm*(data_container.Nsymb+data_container.preamble_nSymb))*frequency_interpolation_rate;
				int max_delay = buf_size - frame_size;
				if(receive_stats.delay > max_delay)
				{
					receive_stats.delay = max_delay;
				}
			}

			// Post-fine-sync energy gate: Schmidl-Cox normalized correlation
			// ties at ~1.0 for positions where 3 of 4 preamble symbols overlap
			// signal (silence in the 4th cancels from numerator and denominator).
			// Sort returns earliest tied position, which may be in silence.
			// Fix: if energy at fine-sync delay is zero, advance by whole symbols
			// to find signal onset while preserving sub-symbol alignment.
			if(M != MOD_MFSK)
			{
				int sym_samples = data_container.Nofdm * frequency_interpolation_rate;
				int buf_samples = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;
				double fine_energy = 0.0;
				for(int i = 0; i < sym_samples && (receive_stats.delay + i) < buf_samples; i++)
					fine_energy += std::norm(data_container.baseband_data_interpolated[receive_stats.delay + i]);
				fine_energy /= sym_samples;
				if(fine_energy < 0.001)
				{
					int orig_delay = receive_stats.delay;
					for(int fwd = sym_samples; fwd <= 3*sym_samples; fwd += sym_samples)
					{
						int candidate = orig_delay + fwd;
						if(candidate + sym_samples > buf_samples) break;
						double e = 0.0;
						for(int i = 0; i < sym_samples; i++)
							e += std::norm(data_container.baseband_data_interpolated[candidate + i]);
						e /= sym_samples;
						if(e >= 0.001)
						{
							printf("[OFDM-SYNC] fine-energy-fix: delay %d->%d (fwd %d sym)\n",
								orig_delay, candidate, fwd / sym_samples);
							fflush(stdout);
							receive_stats.delay = candidate;
							break;
						}
					}
				}
			}

			// Use corrected carrier frequency if coarse sync applied
			double effective_carrier_freq = carrier_frequency + coarse_freq_offset;

			// DIAGNOSTIC: Save baseband snapshot from FIR_rx_time_sync before overwrite
			std::complex<double> ts_snap[4];
			if(M != MOD_MFSK && receive_stats.sync_trials == 0) {
				for(int k=0; k<4; k++)
					ts_snap[k] = data_container.baseband_data_interpolated[receive_stats.delay + k];
			}

			ofdm.passband_to_baseband((double*)data,data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate,data_container.baseband_data_interpolated,sampling_frequency,effective_carrier_freq,carrier_amplitude,1,&ofdm.FIR_rx_data);

			// DIAGNOSTIC: Compare FIR_rx_time_sync vs FIR_rx_data at delay position
			if(M != MOD_MFSK && receive_stats.sync_trials == 0) {
				printf("[FIR-CMP] delay=%d ts_fir: ", receive_stats.delay);
				for(int k=0; k<4; k++)
					printf("(%.6f,%.6f) ", ts_snap[k].real(), ts_snap[k].imag());
				printf("data_fir: ");
				for(int k=0; k<4; k++)
					printf("(%.6f,%.6f) ", data_container.baseband_data_interpolated[receive_stats.delay + k].real(), data_container.baseband_data_interpolated[receive_stats.delay + k].imag());
				printf("\n"); fflush(stdout);

				// Also check baseband energy around the extraction point
				double energy_at_delay = 0.0, energy_mid_frame = 0.0;
				int mid_offset = receive_stats.delay + (data_container.preamble_nSymb + data_container.Nsymb/2) * data_container.Nofdm * frequency_interpolation_rate;
				for(int k=0; k<256; k++) {
					energy_at_delay += std::norm(data_container.baseband_data_interpolated[receive_stats.delay + k]);
					if(mid_offset + k < data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate)
						energy_mid_frame += std::norm(data_container.baseband_data_interpolated[mid_offset + k]);
				}
				printf("[BB-ENERGY] at_delay=%.6f mid_frame=%.6f\n", energy_at_delay / 256, energy_mid_frame / 256);
				fflush(stdout);
			}

			ofdm.rational_resampler(&data_container.baseband_data_interpolated[receive_stats.delay], (data_container.Nofdm*(data_container.Nsymb+data_container.preamble_nSymb))*frequency_interpolation_rate, data_container.baseband_data, data_container.interpolation_rate, DECIMATION);


			if(receive_stats.sync_trials==time_sync_trials_max && use_last_good_freq_offset==YES && receive_stats.freq_offset_of_last_decoded_message!=0)
			{
				freq_offset_measured=receive_stats.freq_offset_of_last_decoded_message;
			}
			else
			{
				// Fine frequency sync (Moose algorithm) - ±0.5 subcarrier range
				// Note: Coarse offset already applied before this loop, so Moose measures residual
				// BUG FIX: baseband_data is at decimated (base) rate after rational_resampler,
				// so guard interval skip is Ngi samples, NOT Ngi*interpolation_rate.
				// The old code skipped Ngi*4=256=Nfft samples, reading across symbol boundaries.
				freq_offset_measured=ofdm.carrier_sampling_frequency_sync(&data_container.baseband_data[data_container.Ngi],bandwidth/(double)data_container.Nc,data_container.preamble_nSymb, sampling_frequency);
			}

			if(M == MOD_MFSK)
			{
				// MFSK: skip fine frequency correction (no channel estimation to compensate)
			}
			else if(fabs(freq_offset_measured)>ofdm.freq_offset_ignore_limit)
			{
				// Apply fine correction on top of coarse correction
				ofdm.passband_to_baseband((double*)data,(data_container.Nofdm*data_container.buffer_Nsymb)*frequency_interpolation_rate,data_container.baseband_data_interpolated,sampling_frequency,effective_carrier_freq+freq_offset_measured,carrier_amplitude,1,&ofdm.FIR_rx_data);
				ofdm.rational_resampler(&data_container.baseband_data_interpolated[receive_stats.delay], (data_container.Nofdm*(data_container.Nsymb+data_container.preamble_nSymb))*frequency_interpolation_rate, data_container.baseband_data, data_container.interpolation_rate, DECIMATION);
			}
			{
				// In ctrl mode, only demod ctrl_nsymb symbols; otherwise full Nsymb
				int rx_nsymb = get_active_nsymb();
				for(int i=0;i<rx_nsymb;i++)
				{
					ofdm.symbol_demod(&data_container.baseband_data[i*data_container.Nofdm+data_container.Nofdm*data_container.preamble_nSymb],&data_container.ofdm_symbol_demodulated_data[i*data_container.Nc]);
				}
			}


			if(M == MOD_MFSK)
			{
				// MFSK: non-coherent energy detection on FFT output → soft LLRs
				int rx_nbits = get_active_nbits();
				mfsk.demod(data_container.ofdm_symbol_demodulated_data, rx_nbits, data_container.demodulated_data);


				// Zero-pad LLRs beyond active bits (punctured positions = erasure)
				// This covers both ctrl mode and BER test puncturing
				int puncture_from = rx_nbits;
				if(test_puncture_nBits > 0 && test_puncture_nBits < puncture_from)
					puncture_from = test_puncture_nBits;
				for(int i = puncture_from; i < data_container.nBits; i++)
				{
					data_container.demodulated_data[i] = 0.0f;
				}

			}
			else
			{
				ofdm.automatic_gain_control(data_container.ofdm_symbol_demodulated_data);

				// DIAGNOSTIC: Print first few pilot subcarrier values after AGC
				{
					int diag_count = 0;
					printf("[PILOT-DIAG] trial=%d first_pilots_after_AGC: ", receive_stats.sync_trials);
					for(int si = 0; si < ofdm.Nsymb && diag_count < 5; si++) {
						for(int sc = 0; sc < ofdm.Nc && diag_count < 5; sc++) {
							if((ofdm.ofdm_frame + si*ofdm.Nc + sc)->type == PILOT) {
								std::complex<double> v = data_container.ofdm_symbol_demodulated_data[si*data_container.Nc + sc];
								printf("[%d,%d]=(%.3f,%.3f)|%.3f ", si, sc, v.real(), v.imag(), std::abs(v));
								diag_count++;
							}
						}
					}
					printf("\n"); fflush(stdout);
				}

				if(ofdm.channel_estimator==ZERO_FORCE)
				{
					ofdm.ZF_channel_estimator(data_container.ofdm_symbol_demodulated_data);
				}
				else if (ofdm.channel_estimator==LEAST_SQUARE)
				{
					ofdm.LS_channel_estimator(data_container.ofdm_symbol_demodulated_data);
				}

				// Channel estimate diagnostic (BEFORE equalizer clears status)
				double mean_H = -1.0;
				{
					double h_sum = 0, h_min = 1e9, h_max = 0;
					int h_measured = 0, h_interpolated = 0;
					for(int ci = 0; ci < ofdm.Nsymb * ofdm.Nc; ci++)
					{
						if(ofdm.estimated_channel[ci].status == MEASURED)
						{
							double h_abs = std::abs(ofdm.estimated_channel[ci].value);
							h_sum += h_abs;
							if(h_abs < h_min) h_min = h_abs;
							if(h_abs > h_max) h_max = h_abs;
							h_measured++;
						}
						else if(ofdm.estimated_channel[ci].status == INTERPOLATED)
						{
							h_interpolated++;
						}
					}
					if(h_measured > 0) mean_H = h_sum / h_measured;
					printf("[CHAN-EST] measured=%d interpolated=%d", h_measured, h_interpolated);
					if(h_measured > 0)
						printf(" mean_H=%.4f min_H=%.4f max_H=%.4f", mean_H, h_min, h_max);
					printf(" freq=%.1f metric=%.3f\n", freq_offset_measured, receive_stats.coarse_metric);
					fflush(stdout);
					// DIAGNOSTIC: Print first few H complex values on trial 0
					if(receive_stats.sync_trials == 0) {
						int hd = 0;
						printf("[H-DIAG] first_H: ");
						for(int ci = 0; ci < ofdm.Nsymb * ofdm.Nc && hd < 5; ci++) {
							if(ofdm.estimated_channel[ci].status == MEASURED) {
								printf("[%d,%d]=(%.4f,%.4f)|%.4f ",
									ci / ofdm.Nc, ci % ofdm.Nc,
									ofdm.estimated_channel[ci].value.real(),
									ofdm.estimated_channel[ci].value.imag(),
									std::abs(ofdm.estimated_channel[ci].value));
								hd++;
							}
						}
						printf("\n"); fflush(stdout);
					}
				}

				// Early exit: bad channel estimate means the preamble was a
				// boundary artifact or misaligned frame. LDPC decode would
				// waste ~35ms per trial and always fail.
				if(mean_H < 0.3)
				{
					skip_h_count++;
					printf("[OFDM-SYNC] trial %d SKIP-H: mean_H=%.4f too low, skipping LDPC\n",
						receive_stats.sync_trials, mean_H);
					fflush(stdout);
					receive_stats.sync_trials++;
					continue;
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
			}

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

            // TODO: FIX-ME
			receive_stats.crc=0xff;
			if(outer_code == CRC16_MODBUS_RTU && receive_stats.iterations_done > (ldpc.nIteration_max-1) && receive_stats.all_zeros == NO)
			{
				// the CRC should be calculated on ((nReal_data - outer_code_reserved_bits) / 8) no??
				receive_stats.crc=CRC16_MODBUS_RTU_calc(data_container.hd_decoded_data_byte, nReal_data/8);
				// should we compare the CRC (and for every frame?)
			}

			// TODO: FIX-ME receive_stats.crc is the crc itself, not the comparisson...
			if((receive_stats.iterations_done > (ldpc.nIteration_max-1) && receive_stats.crc != 0 ) || receive_stats.all_zeros==YES)
			{
				receive_stats.SNR=-99.9;
				receive_stats.message_decoded=NO;
				if(M != MOD_MFSK)
				{
					printf("[OFDM-SYNC] trial %d FAIL: delay=%d iter=%d all_zeros=%d freq_off=%.1f var=%.4f\n",
						receive_stats.sync_trials, receive_stats.delay,
						receive_stats.iterations_done, receive_stats.all_zeros,
						freq_offset_measured, variance);
					fflush(stdout);
				}
				receive_stats.sync_trials++;
			}
			else
			{
				if(M == MOD_MFSK)
				{
					// MFSK: no channel estimation, skip variance-based SNR
					// TODO: estimate SNR from peak tone energy vs noise energy
					receive_stats.SNR = 0.0;
				}
				else if(ofdm.channel_estimator==LEAST_SQUARE)
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
				// Only store freq offset for OFDM modes — MFSK runs the Moose estimator
				// on non-OFDM preamble data producing a garbage value (~45 Hz) that would
				// corrupt OFDM decoding after gearshift (use_last_good_freq_offset fallback).
				if(M != MOD_MFSK)
				{
					receive_stats.freq_offset_of_last_decoded_message=freq_offset_measured;
					receive_stats.freq_offset=freq_offset_measured;
				}

				receive_stats.delay_of_last_decoded_message=receive_stats.delay;
				break;
			}
		}

		// SKIP-H recovery: if all trials failed with low channel estimate,
		// the detected preamble was likely a false peak (e.g. residual MFSK
		// ACK tones looping back through VB-Cable). Search forward for a
		// later, real preamble and retry the trial loop.
		if(receive_stats.message_decoded != YES
			&& skip_h_count >= time_sync_trials_max + 1
			&& !skip_h_recovery_attempted)
		{
			skip_h_recovery_attempted = true;
			int sym_samples = data_container.Nofdm * frequency_interpolation_rate;
			int buf_samples = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;

			// Start searching 2 symbols past the false preamble
			int search_start_symb = pream_symb_loc + 2;
			int search_start = search_start_symb * sym_samples;
			int search_size = data_container.Nofdm *
				(2 * data_container.preamble_nSymb + data_container.Nsymb) *
				frequency_interpolation_rate;
			int available = buf_samples - search_start;
			if(available > search_size) available = search_size;

			if(search_start_symb < upper_bound
				&& available > data_container.preamble_nSymb * sym_samples)
			{
				// Re-run passband_to_baseband with time_sync FIR for fresh search
				ofdm.passband_to_baseband((double*)data,
					data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate,
					data_container.baseband_data_interpolated,
					sampling_frequency, carrier_frequency, carrier_amplitude,
					1, &ofdm.FIR_rx_time_sync);

				TimeSyncResult retry = ofdm.time_sync_preamble_with_metric(
					&data_container.baseband_data_interpolated[search_start],
					available, frequency_interpolation_rate, 0, step, 1);
				retry.delay += search_start;

				int retry_symb = retry.delay / sym_samples;
				if(retry_symb < 1) retry_symb = 1;

				// Check energy at the retry position
				double retry_energy = 0.0;
				int rcnt = 0;
				for(int i = 0; i < sym_samples && (retry.delay + i) < buf_samples; i++)
				{
					double re = data_container.baseband_data_interpolated[retry.delay + i].real();
					double im = data_container.baseband_data_interpolated[retry.delay + i].imag();
					retry_energy += re*re + im*im;
					rcnt++;
				}
				retry_energy = (rcnt > 0) ? retry_energy / rcnt : 0.0;

				printf("[OFDM-SYNC] SKIP-H recovery: orig=%d retry=%d metric=%.3f energy=%.2e\n",
					pream_symb_loc, retry_symb, retry.correlation, retry_energy);
				fflush(stdout);

				// No metric threshold here — the trial loop's mean_H check
				// validates the retry position. Fine time sync has ±2 symbol
				// range to correct from nearby positions.
				if(retry_energy >= 0.001
					&& retry_symb > lower_bound && retry_symb < upper_bound)
				{
					receive_stats.delay = retry.delay;
					receive_stats.coarse_metric = retry.correlation;
					pream_symb_loc = retry_symb;
					receive_stats.sync_trials = 0;
					skip_h_count = 0;
					coarse_freq_offset = 0.0;
					goto skip_h_retry_point;
				}
			}
		}
		} // end if(energy_ok)
	}

	if(ldpc.print_nIteration==YES)
	{
		std::cout<<"decoded in "<< receive_stats.iterations_done<<" iterations."<<std::endl;
	}

#ifdef MERCURY_GUI_ENABLED
	// Update GUI with receive statistics
	gui_update_receive_stats(receive_stats.SNR, receive_stats.signal_stregth_dbm, receive_stats.freq_offset);
#endif

	return receive_stats;
}

double cl_telecom_system::measure_signal_only(double *data)
{
	// Lightweight signal measurement - only passband to baseband + measure strength
	// No preamble detection or decoding
	ofdm.passband_to_baseband((double*)data,
		data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate,
		data_container.baseband_data_interpolated,
		sampling_frequency, carrier_frequency, carrier_amplitude,
		1, &ofdm.FIR_rx_time_sync);

	double signal_dbm = ofdm.measure_signal_stregth(
		data_container.baseband_data_interpolated,
		data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate);

	receive_stats.signal_stregth_dbm = signal_dbm;

#ifdef MERCURY_GUI_ENABLED
	gui_update_receive_stats(receive_stats.SNR, signal_dbm, receive_stats.freq_offset);
#endif

	return signal_dbm;
}

void cl_telecom_system::calculate_parameters()
{
	double nData_eff;
	double log2M_eff;

	if(M == MOD_MFSK)
	{
		nData_eff = ofdm.Nsymb;
		log2M_eff = mfsk.bits_per_symbol();
	}
	else
	{
		nData_eff = ofdm.pilot_configurator.nData;
		log2M_eff = log2(M);
	}

	LDPC_real_CR=(nData_eff*log2M_eff-(double)ldpc.P -(double)outer_code_reserved_bits)/(nData_eff*log2M_eff);
	Tu= ofdm.Nc/bandwidth;
	Ts= Tu*(1.0+ofdm.gi);
	Tf= Ts*(ofdm.Nsymb+ofdm.preamble_configurator.Nsymb);
	rb= nData_eff * log2M_eff /Tf;
	rbc= rb*LDPC_real_CR;
	if(M == MOD_MFSK)
		Shannon_limit= 0;
	else
		Shannon_limit= 10.0*log10((pow(2,(rb*ldpc.rate)/bandwidth)-1)*log2M_eff*bandwidth/rb);
	sampling_frequency=frequency_interpolation_rate*(bandwidth/ofdm.Nc)*ofdm.Nfft;
}

void cl_telecom_system::set_mfsk_ctrl_mode(bool enable)
{
	mfsk_ctrl_mode = enable && (M == MOD_MFSK) && (ctrl_nBits > 0) && (ctrl_nBits < data_container.nBits);
}

int cl_telecom_system::get_active_nsymb() const
{
	return (mfsk_ctrl_mode && ctrl_nsymb > 0) ? ctrl_nsymb : data_container.Nsymb;
}

int cl_telecom_system::get_active_nbits() const
{
	return (mfsk_ctrl_mode && ctrl_nBits > 0) ? ctrl_nBits : data_container.nBits;
}

// TX: Generate ACK pattern as passband audio (no LDPC, no interleaver — pure known tones)
// Returns number of passband samples written to out[]
int cl_telecom_system::generate_ack_pattern_passband(double* out)
{

	if(ack_pattern_passband_samples <= 0) return 0;

	int nsymb = cl_mfsk::ACK_PATTERN_NSYMB;
	float power_normalization = sqrt((double)(ofdm.Nfft * frequency_interpolation_rate));

	// Generate subcarrier-domain ACK pattern (nsymb * Nc complex values)
	// Reuse ofdm_framed_data buffer (allocated for Nsymb * Nc, nsymb=16 fits easily)
	// Always use dedicated ack_mfsk (M=16, nStreams=1) — config-independent
	ack_mfsk.generate_ack_pattern(data_container.ofdm_framed_data);


	// IFFT each symbol to time domain
	for(int i = 0; i < nsymb; i++)
	{
		ofdm.symbol_mod(&data_container.ofdm_framed_data[i * data_container.Nc],
			&data_container.ofdm_symbol_modulated_data[i * data_container.Nofdm]);
	}

	// Amplitude boost: equalize drive level with OFDM (sqrt(Nc/nStreams) - 2 dB trim)
	double ack_boost = sqrt((double)data_container.Nc / ack_mfsk.nStreams) * pow(10.0, -2.0 / 20.0);
	for(int j = 0; j < data_container.Nofdm * nsymb; j++)
	{
		data_container.ofdm_symbol_modulated_data[j] /= power_normalization;
		data_container.ofdm_symbol_modulated_data[j] *= sqrt(output_power_Watt) * ack_boost;
	}

	// Baseband to passband
	double tx_carrier = carrier_frequency;
	ofdm.baseband_to_passband(data_container.ofdm_symbol_modulated_data,
		data_container.Nofdm * nsymb, out,
		sampling_frequency, tx_carrier, carrier_amplitude, frequency_interpolation_rate);

	// Peak clipping
	ofdm.peak_clip(out, ack_pattern_passband_samples, ofdm.data_papr_cut);


	return ack_pattern_passband_samples;
}

// RX: Detect ACK pattern in passband audio buffer
// Returns detection metric (0.0 = noise, up to ACK_PATTERN_NSYMB = perfect)
double cl_telecom_system::detect_ack_pattern_from_passband(double* data, int size, int* out_matched)
{
	if(ack_pattern_passband_samples <= 0) return 0.0;

	// Passband to baseband (use FIR_rx_data for proper image rejection)
	ofdm.passband_to_baseband(data, size,
		data_container.baseband_data_interpolated,
		sampling_frequency, carrier_frequency, carrier_amplitude,
		1, &ofdm.FIR_rx_data);

	// Run matched-filter ACK detector — always use dedicated ack_mfsk (config-independent)
	double metric = ofdm.detect_ack_pattern(
		data_container.baseband_data_interpolated, size,
		data_container.interpolation_rate,
		cl_mfsk::ACK_PATTERN_NSYMB,
		ack_mfsk.ack_tones, cl_mfsk::ACK_PATTERN_LEN,
		ack_mfsk.tone_hop_step, ack_mfsk.M,
		ack_mfsk.nStreams, ack_mfsk.stream_offsets,
		out_matched);

	return metric;
}

// TX: Generate BREAK pattern as passband audio (identical to ACK but with break_tones)
int cl_telecom_system::generate_break_pattern_passband(double* out)
{
	if(ack_pattern_passband_samples <= 0) return 0;

	int nsymb = cl_mfsk::ACK_PATTERN_NSYMB;
	float power_normalization = sqrt((double)(ofdm.Nfft * frequency_interpolation_rate));

	ack_mfsk.generate_break_pattern(data_container.ofdm_framed_data);

	for(int i = 0; i < nsymb; i++)
	{
		ofdm.symbol_mod(&data_container.ofdm_framed_data[i * data_container.Nc],
			&data_container.ofdm_symbol_modulated_data[i * data_container.Nofdm]);
	}

	double ack_boost = sqrt((double)data_container.Nc / ack_mfsk.nStreams) * pow(10.0, -2.0 / 20.0);
	for(int j = 0; j < data_container.Nofdm * nsymb; j++)
	{
		data_container.ofdm_symbol_modulated_data[j] /= power_normalization;
		data_container.ofdm_symbol_modulated_data[j] *= sqrt(output_power_Watt) * ack_boost;
	}

	double tx_carrier = carrier_frequency;
	ofdm.baseband_to_passband(data_container.ofdm_symbol_modulated_data,
		data_container.Nofdm * nsymb, out,
		sampling_frequency, tx_carrier, carrier_amplitude, frequency_interpolation_rate);

	ofdm.peak_clip(out, ack_pattern_passband_samples, ofdm.data_papr_cut);

	return ack_pattern_passband_samples;
}

// RX: Detect BREAK pattern in passband audio buffer (uses break_tones instead of ack_tones)
double cl_telecom_system::detect_break_pattern_from_passband(double* data, int size, int* out_matched)
{
	if(ack_pattern_passband_samples <= 0) return 0.0;

	ofdm.passband_to_baseband(data, size,
		data_container.baseband_data_interpolated,
		sampling_frequency, carrier_frequency, carrier_amplitude,
		1, &ofdm.FIR_rx_data);

	double metric = ofdm.detect_ack_pattern(
		data_container.baseband_data_interpolated, size,
		data_container.interpolation_rate,
		cl_mfsk::ACK_PATTERN_NSYMB,
		ack_mfsk.break_tones, cl_mfsk::ACK_PATTERN_LEN,
		ack_mfsk.tone_hop_step, ack_mfsk.M,
		ack_mfsk.nStreams, ack_mfsk.stream_offsets,
		out_matched);

	return metric;
}

// ACK pattern detection test: sweep SNR, measure detection metric and false alarm rate
void cl_telecom_system::ack_pattern_detection_test()
{
	if(ack_pattern_passband_samples <= 0)
	{
		printf("[ACK_TEST] ack_pattern not configured\n");
		return;
	}

	int nsymb = cl_mfsk::ACK_PATTERN_NSYMB;
	int passband_samples = ack_pattern_passband_samples;
	int delay_samples = 2 * data_container.Nofdm * frequency_interpolation_rate;
	int rx_buffer_size = passband_samples + 2 * delay_samples;

	double* tx_passband = new double[passband_samples];
	double* rx_buffer = new double[rx_buffer_size];

	// Generate ACK pattern
	generate_ack_pattern_passband(tx_passband);

	// Measure signal power for sigma calibration
	double P_sig = 0;
	for(int i = 0; i < passband_samples; i++)
		P_sig += tx_passband[i] * tx_passband[i];
	P_sig /= passband_samples;

	double f_nyquist = sampling_frequency / 2.0;
	int nTrials = 20;

	// Note: max metric is ~ACK_PATTERN_NSYMB/2 due to carrier image (real passband roundtrip)
	// This is expected and consistent; threshold calibrated accordingly
	double max_clean_metric = nsymb / 2.0;

	printf("ACK_DETECT_TEST;max_clean=%.1f;SNR;mean_metric;min_metric;max_metric\n", max_clean_metric);
	fflush(stdout);

	// Sweep SNR from -20 to +5 dB
	for(double snr_db = -20.0; snr_db <= 5.0; snr_db += 1.0)
	{
		float sigma = (float)sqrt(2.0 * P_sig * f_nyquist / (pow(10.0, snr_db / 10.0) * bandwidth));
		double metric_sum = 0;
		double metric_min = 1e30;
		double metric_max = -1e30;

		for(int trial = 0; trial < nTrials; trial++)
		{
			// Zero buffer first (apply_with_delay only writes delay+nItems samples)
			for(int i = 0; i < rx_buffer_size; i++) rx_buffer[i] = 0.0;

			// Place TX signal with delay, add AWGN
			awgn_channel.apply_with_delay(tx_passband, rx_buffer, sigma,
				passband_samples, delay_samples);

			// Detect
			double metric = detect_ack_pattern_from_passband(rx_buffer, rx_buffer_size);
			metric_sum += metric;
			if(metric < metric_min) metric_min = metric;
			if(metric > metric_max) metric_max = metric;
		}

		printf("%.0f;%.3f;%.3f;%.3f\n", snr_db, metric_sum / nTrials, metric_min, metric_max);
		fflush(stdout);
	}

	// False alarm test: noise only (no signal)
	printf("ACK_FALSE_ALARM_TEST;threshold=%.1f\n", ack_pattern_detection_threshold);
	fflush(stdout);
	{
		int noise_trials = 20;
		int false_alarms = 0;
		double noise_metric_max = 0;
		// Use sigma for -10 dB SNR level noise power
		float sigma = (float)sqrt(2.0 * P_sig * f_nyquist / (pow(10.0, -10.0 / 10.0) * bandwidth));

		for(int trial = 0; trial < noise_trials; trial++)
		{
			// Generate noise-only buffer
			for(int i = 0; i < rx_buffer_size; i++)
				rx_buffer[i] = (sigma / sqrtf(2.0f)) * awgn_channel.awgn_value_generator();

			double metric = detect_ack_pattern_from_passband(rx_buffer, rx_buffer_size);
			if(metric > noise_metric_max) noise_metric_max = metric;
			if(metric >= ack_pattern_detection_threshold) false_alarms++;
		}

		printf("FALSE_ALARM;%d/%d;max_noise_metric=%.3f\n", false_alarms, noise_trials, noise_metric_max);
		fflush(stdout);
	}

	delete[] tx_passband;
	delete[] rx_buffer;
}

void cl_telecom_system::init()
{
	if(ofdm.Nc==AUTO_SELLECT)
	{
		ofdm.Nc=50;
	}
	if(ofdm.Nsymb==AUTO_SELLECT)
	{
		if(M==MOD_MFSK)
		{
			// MFSK: bits per symbol period = nBits * nStreams
			// Nsymb = LDPC codeword length / bits per symbol period
			ofdm.Nsymb = N_MAX / mfsk.bits_per_symbol();
		}
		else if(ofdm.pilot_configurator.pilot_density==HIGH_DENSITY)
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

	if(M!=MOD_MFSK && ofdm.pilot_configurator.Dx==AUTO_SELLECT)
	{
		if(M==MOD_BPSK){ofdm.pilot_configurator.Dx=1;}
		if(M==MOD_QPSK){ofdm.pilot_configurator.Dx=1;}
		if(M==MOD_8PSK){ofdm.pilot_configurator.Dx=1;}
		if(M==MOD_16QAM){ofdm.pilot_configurator.Dx=1;}
		if(M==MOD_32QAM){ofdm.pilot_configurator.Dx=1;}
		if(M==MOD_64QAM){ofdm.pilot_configurator.Dx=1;}
	}

	if(M!=MOD_MFSK && ofdm.pilot_configurator.Dy==AUTO_SELLECT)
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

	// MFSK doesn't use pilots, but pilot_configurator needs valid Dx/Dy
	if(M == MOD_MFSK)
	{
		if(ofdm.pilot_configurator.Dx == AUTO_SELLECT) ofdm.pilot_configurator.Dx = 1;
		if(ofdm.pilot_configurator.Dy == AUTO_SELLECT) ofdm.pilot_configurator.Dy = ofdm.Nsymb;
	}

	if(operation_mode==ARQ_MODE)
	{
		ofdm.pilot_configurator.print_on=NO;
		ofdm.preamble_configurator.print_on=NO;
	}

	if(reinit_subsystems.ofdm==YES)
	{
		printf("[PHY-DEBUG] Before ofdm.init(): Nsymb=%d Nc=%d Dx=%d Dy=%d density=%d M=%d\n",
			ofdm.Nsymb, ofdm.Nc, ofdm.pilot_configurator.Dx, ofdm.pilot_configurator.Dy,
			ofdm.pilot_configurator.pilot_density, M);
		fflush(stdout);
		ofdm.init();
		printf("[PHY-DEBUG] After ofdm.init(): nPilots=%d nData=%d ofdm_frame=%p\n",
			ofdm.pilot_configurator.nPilots, ofdm.pilot_configurator.nData,
			(void*)ofdm.ofdm_frame);
		// Verify PILOT count directly in ofdm_frame
		int verify_pilots = 0;
		for(int i = 0; i < ofdm.Nsymb * ofdm.Nc; i++)
			if(ofdm.ofdm_frame[i].type == PILOT) verify_pilots++;
		printf("[PHY-DEBUG] Direct pilot count in ofdm_frame: %d\n", verify_pilots);
		fflush(stdout);
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
		if(M == MOD_MFSK)
		{
			// MFSK: nData = Nsymb (no pilots)
			// Effective M = 2^(nBits*nStreams) so that nData*log2(M_eff) = N_MAX
			int M_eff = 1 << mfsk.bits_per_symbol();
			data_container.set_size(ofdm.Nsymb, ofdm.Nc, M_eff, ofdm.Nfft, ofdm.Nfft*(1+ofdm.gi), ofdm.Nsymb, ofdm.preamble_configurator.Nsymb, frequency_interpolation_rate);
		}
		else
		{
			data_container.set_size(ofdm.pilot_configurator.nData, ofdm.Nc, M, ofdm.Nfft, ofdm.Nfft*(1+ofdm.gi), ofdm.Nsymb, ofdm.preamble_configurator.Nsymb, frequency_interpolation_rate);
		}
		reinit_subsystems.data_container=NO;
	}

	if(reinit_subsystems.pre_equalization_channel==YES && M != MOD_MFSK)
	{
		pre_equalization_channel=new struct st_channel_complex[data_container.Nc];
		get_pre_equalization_channel();
		reinit_subsystems.pre_equalization_channel=NO;
	}

	__srandom (bit_energy_dispersal_seed);
	bit_energy_dispersal_seed = default_configurations_telecom_system.bit_energy_dispersal_seed;
	for(int i=0;i<ldpc.N;i++)
	{
		data_container.bit_energy_dispersal_sequence[i]=__random()%2;
	}

	receive_stats.iterations_done=-1;
	receive_stats.delay=0;
	receive_stats.delay_of_last_decoded_message=-1;
	receive_stats.mfsk_search_raw=0;
	receive_stats.time_peak_symb_location=0;
	receive_stats.time_peak_subsymb_location=0;
	receive_stats.sync_trials=0;
	receive_stats.phase_error_avg=0;
	receive_stats.freq_offset=0;
	receive_stats.freq_offset_of_last_decoded_message=0;
	receive_stats.message_decoded=NO;
	receive_stats.SNR=-99.9;
	receive_stats.signal_stregth_dbm=-999;

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

void cl_telecom_system::TX_RAND_process_main()
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
	tx_transfer(data_container.passband_data,data_container.Nofdm*data_container.interpolation_rate*(ofdm.Nsymb+ofdm.preamble_configurator.Nsymb));
}

void cl_telecom_system::TX_TEST_process_main()
{
    int nReal_data = data_container.nBits - ldpc.P;
    int frame_size = (nReal_data - outer_code_reserved_bits) / 8;

    static int counter = 0;

    for (int i = 0; i < frame_size; i++)
    {
        data_container.data_byte[i] = 0; // data_byte is an integer
    }
    data_container.data_byte[counter % frame_size] = 1;
    counter++;

    transmit_byte(data_container.data_byte, (nReal_data - outer_code_reserved_bits) / 8, data_container.passband_data, SINGLE_MESSAGE);

    tx_transfer(data_container.passband_data, data_container.Nofdm * data_container.interpolation_rate * (ofdm.Nsymb + ofdm.preamble_configurator.Nsymb));

}

void cl_telecom_system::TX_SHM_process_main(cbuf_handle_t buffer)
{
    static uint32_t spinner_anim = 0; char spinner[] = ".oOo";
    int nReal_data = data_container.nBits - ldpc.P;
    // int frame_size_bits = nReal_data - outer_code_reserved_bits;
    int frame_size = (nReal_data - outer_code_reserved_bits) / 8;
    // int input_buffer_size = 0;
    // std::cout<<"Extra unused bits: "<< frame_size_bits - (frame_size * 8)<<",";
    // std::cout<<std::endl;

    uint8_t data[frame_size];

    // check the data in the buffer, if smaller than frame size, transmits 0
    if ((int) size_buffer(buffer) >= frame_size)
    {
        // memset(data, 0, frame_size);
        read_buffer(buffer, data, frame_size);

        for (int i = 0; i < frame_size; i++)
        {
            data_container.data_byte[i] = data[i];
        }
    }
    // if there is no data in the buffer, just do nothing
    else
    {
		msleep(10);
		return;
    }

    transmit_byte(data_container.data_byte, (nReal_data - outer_code_reserved_bits) / 8, data_container.passband_data, SINGLE_MESSAGE);

    tx_transfer(data_container.passband_data, data_container.Nofdm * data_container.interpolation_rate * (ofdm.Nsymb + ofdm.preamble_configurator.Nsymb));

    printf("%c\033[1D", spinner[spinner_anim % 4]); spinner_anim++;
    fflush(stdout);
}


void cl_telecom_system::RX_RAND_process_main()
{
	std::complex <double> data_fft[ofdm.pilot_configurator.nData];
	int constellation_plot_counter=0;
	int constellation_plot_nFrames=1;
	float contellation[ofdm.pilot_configurator.nData*constellation_plot_nFrames][2]={0};
    int nReal_data = data_container.nBits - ldpc.P;
    int frame_size = (nReal_data - outer_code_reserved_bits) / 8;
    int out_data[N_MAX];

	int signal_period = data_container.Nofdm * data_container.buffer_Nsymb * data_container.interpolation_rate; // in samples
	int symbol_period = data_container. Nofdm * data_container.interpolation_rate;

	if(data_container.data_ready == 0)
	{
		msleep(1);
		return;
	}

	MUTEX_LOCK(&capture_prep_mutex);
	if (data_container.frames_to_read == 0)
	{

		memcpy(data_container.ready_to_process_passband_delayed_data, data_container.passband_delayed_data, signal_period * sizeof(double));

		st_receive_stats received_message_stats = receive_byte(data_container.ready_to_process_passband_delayed_data, out_data);

		if(received_message_stats.message_decoded == YES)
		{
			printf("Frame decoded in %d iterations. Data: \n", received_message_stats.iterations_done);

			for(int i = 0; i < frame_size; i++)
				printf("0x%x, ", out_data[i]);

			std::cout << std::endl;
			std::cout << std::dec;
			std::cout << " sync_trial=" << receive_stats.sync_trials;
			std::cout << " time_peak_subsymb_location=" << received_message_stats.delay % (data_container.Nofdm * data_container.interpolation_rate);
			std::cout << " time_peak_symb_location=" << received_message_stats.delay / (data_container.Nofdm * data_container.interpolation_rate);
			std::cout << " freq_offset=" << receive_stats.freq_offset;
			std::cout << " SNR=" << receive_stats.SNR << " dB";
			std::cout << " Signal Strength=" << receive_stats.signal_stregth_dbm << " dBm ";
			std::cout << std::endl;

			int end_of_current_message = received_message_stats.delay / symbol_period + data_container.Nsymb + data_container.preamble_nSymb;
			int frames_left_in_buffer = data_container.buffer_Nsymb - end_of_current_message;
			if(frames_left_in_buffer < 0)
				frames_left_in_buffer = 0;

			data_container.frames_to_read = data_container.Nsymb + data_container.preamble_nSymb - frames_left_in_buffer - data_container.nUnder_processing_events;

			if(data_container.frames_to_read > (data_container.Nsymb + data_container.preamble_nSymb) || data_container.frames_to_read < 0)
				data_container.frames_to_read = data_container.Nsymb + data_container.preamble_nSymb - frames_left_in_buffer;

			receive_stats.delay_of_last_decoded_message += (data_container.Nsymb + data_container.preamble_nSymb - data_container.frames_to_read) * symbol_period;

			data_container.nUnder_processing_events = 0;
		}
		else
		{
			if(data_container.frames_to_read == 0 && receive_stats.delay_of_last_decoded_message != -1)
			{
				receive_stats.delay_of_last_decoded_message -= symbol_period;
				if(receive_stats.delay_of_last_decoded_message < 0)
				{
					receive_stats.delay_of_last_decoded_message = -1;
				}
			}
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
	data_container.data_ready = 0;
	MUTEX_UNLOCK(&capture_prep_mutex);
}

void cl_telecom_system::RX_TEST_process_main()
{
    int out_data[N_MAX];
    int nReal_data = data_container.nBits - ldpc.P;
    int frame_size = (nReal_data - outer_code_reserved_bits) / 8;
	// int buff_size = data_container.Nofdm * data_container.buffer_Nsymb * data_container.interpolation_rate * 2;

	int signal_period = data_container.Nofdm * data_container.buffer_Nsymb * data_container.interpolation_rate; // in samples
	int symbol_period = data_container. Nofdm * data_container.interpolation_rate;

	if(data_container.data_ready == 0)
	{
		msleep(1);
		return;
	}

	MUTEX_LOCK(&capture_prep_mutex);
	if (data_container.frames_to_read == 0)
	{

		memcpy(data_container.ready_to_process_passband_delayed_data, data_container.passband_delayed_data, signal_period * sizeof(double));

		st_receive_stats received_message_stats = receive_byte(data_container.ready_to_process_passband_delayed_data, out_data);

		if(received_message_stats.message_decoded == YES)
		{
			printf("Frame decoded in %d iterations. Data: \n", received_message_stats.iterations_done);

			for(int i = 0; i < frame_size; i++)
				printf("0x%x, ", out_data[i]);

			std::cout << std::endl;
			std::cout << std::dec;
			std::cout << " sync_trial=" << receive_stats.sync_trials;
			std::cout << " time_peak_subsymb_location=" << received_message_stats.delay % (data_container.Nofdm * data_container.interpolation_rate);
			std::cout << " time_peak_symb_location=" << received_message_stats.delay / (data_container.Nofdm * data_container.interpolation_rate);
			std::cout << " freq_offset=" << receive_stats.freq_offset;
			std::cout << " SNR=" << receive_stats.SNR << " dB";
			std::cout << " Signal Strength=" << receive_stats.signal_stregth_dbm << " dBm ";
			std::cout << std::endl;

			int end_of_current_message = received_message_stats.delay / symbol_period + data_container.Nsymb + data_container.preamble_nSymb;
			int frames_left_in_buffer = data_container.buffer_Nsymb - end_of_current_message;
			if(frames_left_in_buffer < 0)
				frames_left_in_buffer = 0;

			data_container.frames_to_read = data_container.Nsymb + data_container.preamble_nSymb - frames_left_in_buffer - data_container.nUnder_processing_events;

			if(data_container.frames_to_read > (data_container.Nsymb + data_container.preamble_nSymb) || data_container.frames_to_read < 0)
				data_container.frames_to_read = data_container.Nsymb + data_container.preamble_nSymb - frames_left_in_buffer;

			receive_stats.delay_of_last_decoded_message += (data_container.Nsymb + data_container.preamble_nSymb - data_container.frames_to_read) * symbol_period;

			data_container.nUnder_processing_events = 0;
		}
		else
		{
			if(data_container.frames_to_read == 0 && receive_stats.delay_of_last_decoded_message != -1)
			{
				receive_stats.delay_of_last_decoded_message -= symbol_period;
				if(receive_stats.delay_of_last_decoded_message < 0)
				{
					receive_stats.delay_of_last_decoded_message = -1;
				}
			}
			//				std::cout<<" Signal Strength="<<receive_stats.signal_stregth_dbm<<" dBm ";
			//				std::cout<<std::endl;
		}

	}
	data_container.data_ready = 0;
	MUTEX_UNLOCK(&capture_prep_mutex);
}


void cl_telecom_system::RX_SHM_process_main(cbuf_handle_t buffer)
{
    static uint32_t spinner_anim = 0; char spinner[] = ".oOo";
	int out_data[N_MAX];
    int nReal_data = data_container.nBits - ldpc.P;
    int frame_size = (nReal_data - outer_code_reserved_bits) / 8;

	int signal_period = data_container.Nofdm * data_container.buffer_Nsymb * data_container.interpolation_rate; // in samples
	int symbol_period = data_container.Nofdm * data_container.interpolation_rate;

	// lock
	if(data_container.data_ready == 0)
	{
		msleep(1);
		return;
	}

	MUTEX_LOCK(&capture_prep_mutex);
	// Guard: deinit path zeros Nofdm/buffer_Nsymb under this mutex before freeing
	// buffers. If we see zero, buffers are being freed — skip processing.
	if(data_container.Nofdm == 0 || data_container.buffer_Nsymb == 0)
	{
		data_container.data_ready = 0;
		MUTEX_UNLOCK(&capture_prep_mutex);
		return;
	}
	// Recompute under mutex for consistency with guard check
	signal_period = data_container.Nofdm * data_container.buffer_Nsymb * data_container.interpolation_rate;
	symbol_period = data_container.Nofdm * data_container.interpolation_rate;
	if (data_container.frames_to_read == 0)
	{
#ifdef MERCURY_GUI_ENABLED
		// Apply live LDPC iteration limit from GUI
		int gui_ldpc_max = g_gui_state.ldpc_iterations_max.load();
		if (gui_ldpc_max >= 5 && gui_ldpc_max <= 50)
			ldpc.nIteration_max = gui_ldpc_max;
#endif

		memcpy(data_container.ready_to_process_passband_delayed_data, data_container.passband_delayed_data, signal_period * sizeof(double));

		auto proc_start = std::chrono::steady_clock::now();
		st_receive_stats received_message_stats = receive_byte(data_container.ready_to_process_passband_delayed_data, out_data);
		auto proc_end = std::chrono::steady_clock::now();
		double proc_ms = std::chrono::duration<double, std::milli>(proc_end - proc_start).count();

		// Frame period = (preamble + data symbols) in wall clock time
		double frame_samples = (double)(data_container.Nofdm * (data_container.Nsymb + data_container.preamble_nSymb) * data_container.interpolation_rate);
		double frame_ms = (frame_samples / 48000.0) * 1000.0;
		float load = (frame_ms > 0) ? (float)(proc_ms / frame_ms) : 0.0f;

#ifdef MERCURY_GUI_ENABLED
		g_gui_state.processing_load.store(load);
		size_t buf_used = size_buffer(capture_buffer);
		size_t buf_cap = circular_buf_capacity(capture_buffer);
		g_gui_state.buffer_fill_pct.store(buf_cap > 0 ? 100.0f * (float)buf_used / (float)buf_cap : 0.0f);
#endif

		if(received_message_stats.message_decoded == YES)
		{
			// printf("Frame decoded in %d iterations. Data: \n", received_message_stats.iterations_done);
			uint8_t data[frame_size];
			for(int i = 0; i < frame_size; i++)
			{
				data[i] = (uint8_t) out_data[i];
			}

			if ( frame_size <= (int) circular_buf_free_size(buffer) )
				write_buffer(buffer, data, frame_size);
			else
				printf("Decoded frame lost because of full buffer!\n");


			// Only display signal strength if in reasonable range (-150 to +50 dBm)
			if (receive_stats.signal_stregth_dbm >= -150 && receive_stats.signal_stregth_dbm <= 50)
				printf("\rSNR: %5.1f db  Level: %5.1f dBm  Load: %.2fx  Buf: %.0f%%  RX: %c",
					receive_stats.SNR, receive_stats.signal_stregth_dbm, load,
					(circular_buf_capacity(capture_buffer) > 0 ? 100.0 * size_buffer(capture_buffer) / circular_buf_capacity(capture_buffer) : 0.0),
					spinner[spinner_anim % 4]);
			else
				printf("\rSNR: %5.1f db  Load: %.2fx  Buf: %.0f%%  RX: %c",
					receive_stats.SNR, load,
					(circular_buf_capacity(capture_buffer) > 0 ? 100.0 * size_buffer(capture_buffer) / circular_buf_capacity(capture_buffer) : 0.0),
					spinner[spinner_anim % 4]);
			spinner_anim++;
			fflush(stdout);

			int end_of_current_message = received_message_stats.delay / symbol_period + data_container.Nsymb + data_container.preamble_nSymb;
			int frames_left_in_buffer = data_container.buffer_Nsymb - end_of_current_message;
			if(frames_left_in_buffer < 0)
				frames_left_in_buffer = 0;

			data_container.frames_to_read = data_container.Nsymb + data_container.preamble_nSymb - frames_left_in_buffer - data_container.nUnder_processing_events;

			if(data_container.frames_to_read > (data_container.Nsymb + data_container.preamble_nSymb) || data_container.frames_to_read < 0)
				data_container.frames_to_read = data_container.Nsymb + data_container.preamble_nSymb - frames_left_in_buffer;

			receive_stats.delay_of_last_decoded_message += (data_container.Nsymb + data_container.preamble_nSymb - data_container.frames_to_read) * symbol_period;

			data_container.nUnder_processing_events = 0;
		}
		else
		{
			if(data_container.frames_to_read == 0 && receive_stats.delay_of_last_decoded_message != -1)
			{
				receive_stats.delay_of_last_decoded_message -= symbol_period;
				if(receive_stats.delay_of_last_decoded_message < 0)
				{
					receive_stats.delay_of_last_decoded_message = -1;
				}
			}
			// Periodic status while scanning (every ~4 frames)
			if (spinner_anim % 4 == 0) {
				printf("\rLoad: %.2fx  Buf: %.0f%%  Scanning... %c",
					load,
					(circular_buf_capacity(capture_buffer) > 0 ? 100.0 * size_buffer(capture_buffer) / circular_buf_capacity(capture_buffer) : 0.0),
					spinner[spinner_anim % 4]);
				fflush(stdout);
			}
			spinner_anim++;
		}

	}
	data_container.data_ready = 0;
	MUTEX_UNLOCK(&capture_prep_mutex);
}


void cl_telecom_system::BER_PLOT_baseband_process_main()
{
	if(M == MOD_MFSK)
	{
		std::cout<<"PLOT_BASEBAND not supported for MFSK configs. Use PLOT_PASSBAND instead."<<std::endl;
		return;
	}
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
	// MFSK: sweep channel SNR from -25 to +5 dB in 1 dB steps (VARA claims ~-10 dB for SL1)
	// OFDM: sweep Es/N0 from -10 to +2.5 dB in 0.5 dB steps
	int nPoints = (M == MOD_MFSK) ? 31 : 25;
	int nFrames_per_point = (M == MOD_MFSK) ? 3 : 100;
	float data_plot[nPoints][2];
	float data_plot_theo[nPoints][2];
	output_power_Watt=1;
	float start_location = (M == MOD_MFSK) ? -25.0f : -10.0f;
	float step_size = (M == MOD_MFSK) ? 1.0f : 0.5f;

	for(int ind=0;ind<nPoints;ind++)
	{
		float EsN0=(float)(ind * step_size + start_location);

		data_plot[ind][0]=EsN0;

		data_plot[ind][1]=passband_test_EsN0(EsN0,nFrames_per_point).BER;

		data_plot_theo[ind][0]=EsN0 ;

		if(M==MOD_BPSK)
		{
			data_plot_theo[ind][1]=0.5*erfc(sqrt(pow(10,EsN0/10)));
		}
		else if(M==MOD_MFSK)
		{
			// MFSK theoretical: no simple closed-form, use 0 placeholder
			data_plot_theo[ind][1]=0;
		}
		else
		{
			data_plot_theo[ind][1]=(2.0/log2(M))*(1.0-1.0/sqrt(M))*erfc(sqrt(((3.0* log2(M))/(2.0*(M-1))) *pow(10,EsN0/10)/log2(M)));
		}
		std::cout<<EsN0<<";"<<data_plot[ind][1]<<std::endl;
	}

	BER_plot.plot("BER Simulation",&data_plot[0][0],nPoints,"BER theoretical",&data_plot_theo[0][0],nPoints);
	BER_plot.close();

	// Run ACK pattern detection test for MFSK modes
	if(M == MOD_MFSK)
	{
		ack_pattern_detection_test();
	}
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

	if(configuration<0 || (configuration>=NUMBER_OF_CONFIGS && !is_robust_config(configuration)))
	{
		return;
	}

	printf("[PHY] Loading configuration %d (was %d)\n", configuration, current_configuration);

	int _modulation = MOD_BPSK;
	float _ldpc_rate = 1/16.0f;
	int ofdm_preamble_configurator_Nsymb = 4;
	int ofdm_channel_estimator = LEAST_SQUARE;

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
	else if(configuration==ROBUST_0)
	{
		_modulation=MOD_MFSK;
		_ldpc_rate=1/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==ROBUST_1)
	{
		_modulation=MOD_MFSK;
		_ldpc_rate=1/16.0;  // Same FEC as ROBUST_0; speed comes from 2x parallel streams
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==ROBUST_2)
	{
		_modulation=MOD_MFSK;
		_ldpc_rate=4/16.0;  // Rate 1/4: 4x throughput vs ROBUST_1, waterfall at -8 dB
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
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
		reinit_subsystems.pre_equalization_channel=YES;
	}

	// MFSK: different ROBUST configs may change M or nStreams, need full reinit
	if(_modulation==MOD_MFSK && M==MOD_MFSK)
	{
		int new_mfsk_M, new_nStreams;
		if(configuration == ROBUST_0) { new_mfsk_M = 32; new_nStreams = 1; }
		else { new_mfsk_M = 16; new_nStreams = 2; } // ROBUST_1, ROBUST_2
		if(new_mfsk_M != mfsk.M || new_nStreams != mfsk.nStreams)
		{
			reinit_subsystems.telecom_system=YES;
			reinit_subsystems.data_container=YES;
			reinit_subsystems.ofdm=YES;
			reinit_subsystems.psk=YES;
		}
	}

	if(_ldpc_rate!=ldpc.rate)
	{
		reinit_subsystems.telecom_system=YES;
		reinit_subsystems.ldpc=YES;
	}

	if(reinit_subsystems.microphone==YES)
	{
        // why do we need this?
		// microphone.deinit();
	}
	if(reinit_subsystems.speaker==YES)
	{
        // why do we need this?
		// speaker.deinit();
	}

	if(reinit_subsystems.psk==YES)
	{
		psk.deinit();
	}
	if(reinit_subsystems.telecom_system==YES)
	{
		printf("[PHY-REINIT] About to deinit telecom_system (config %d -> %d)\n", last_configuration, configuration);
		fflush(stdout);

		// Zero audio-facing buffer parameters under the mutex BEFORE deinit
		// frees passband_delayed_data.  The audio capture_prep thread re-reads
		// these inside the same mutex, so it will see the zeroed values and
		// skip the buffer access, preventing use-after-free.
		// Only zero when data_container will actually be deinitialized and
		// re-allocated by set_size(). Otherwise (e.g., LDPC-only reinit where
		// buffers stay intact), Nofdm must stay valid for passband calculations.
		if(reinit_subsystems.data_container==YES)
		{
			MUTEX_LOCK(&capture_prep_mutex);
			data_container.Nofdm = 0;
			data_container.buffer_Nsymb = 0;
			data_container.data_ready = 0;
			MUTEX_UNLOCK(&capture_prep_mutex);
		}

		printf("[PHY-REINIT] Mutex zeroed, calling deinit...\n");
		fflush(stdout);
		this->deinit();
		printf("[PHY-REINIT] deinit complete (tid=%lu)\n", (unsigned long)GetCurrentThreadId());
		fflush(stdout);
	}

	printf("[PHY-REINIT] post-deinit A (tid=%lu)\n", (unsigned long)GetCurrentThreadId());
	fflush(stdout);
	printf("[PHY-M] _modulation=%d M_before=%.0f MOD_BPSK=%d MOD_MFSK=%d\n",
		_modulation, M, MOD_BPSK, MOD_MFSK);
	fflush(stdout);
	M=_modulation;
	printf("[PHY-M] M_after=%.0f\n", M);
	fflush(stdout);
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
	// MFSK non-coherent detection has no channel estimation to compensate
	// for timing errors, so we need to try more preamble correlation peaks.
	if (_modulation == MOD_MFSK) {
		time_sync_trials_max = 5;
	}
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


	if(reinit_subsystems.psk==YES)
	{
		if(M == MOD_MFSK)
		{
			int mfsk_M, mfsk_nStreams;
			if(current_configuration == ROBUST_0) { mfsk_M = 32; mfsk_nStreams = 1; }
			else { mfsk_M = 16; mfsk_nStreams = 2; } // ROBUST_1, ROBUST_2: 2x parallel 16-MFSK
			mfsk.init(mfsk_M, ofdm.Nc, mfsk_nStreams);
		}
		else
		{
			psk.set_predefined_constellation(M);
		}
		reinit_subsystems.psk=NO;
	}
	if(reinit_subsystems.telecom_system==YES)
	{
		printf("[PHY-REINIT] Calling init...\n");
		fflush(stdout);
		this->init();
		printf("[PHY-REINIT] init complete\n");
		fflush(stdout);
		reinit_subsystems.telecom_system=NO;
	}

	// Re-init MFSK now that ofdm.Nc is finalized (was AUTO_SELLECT during mfsk.init above)
	// This recalculates stream_offsets with the correct Nc value
	if(M == MOD_MFSK)
	{
		int mfsk_M, mfsk_nStreams;
		if(current_configuration == ROBUST_0) { mfsk_M = 32; mfsk_nStreams = 1; }
		else { mfsk_M = 16; mfsk_nStreams = 2; } // ROBUST_1, ROBUST_2
		mfsk.init(mfsk_M, ofdm.Nc, mfsk_nStreams);
	}

	bit_interleaver_block_size=data_container.nBits/10;
	time_freq_interleaver_block_size=data_container.nData/10;

	if(default_configurations_telecom_system.ofdm_time_sync_Nsymb==AUTO_SELLECT)
	{
		ofdm.time_sync_Nsymb=ofdm.Nsymb;
	}

	if(reinit_subsystems.microphone==YES)
	{
        // TODO: Do we need this?
#if 0
		microphone.baudrate = sampling_frequency;
		microphone.nbuffer_Samples = 2 * ofdm.Nfft*(1+ofdm.gi)*frequency_interpolation_rate*(ofdm.Nsymb+ofdm.preamble_configurator.Nsymb);
		microphone.frames_per_period = ofdm.Nfft*(1+ofdm.gi)*frequency_interpolation_rate;
		if(operation_mode != BER_PLOT_baseband &&
           operation_mode != BER_PLOT_passband &&
           operation_mode != TX_TEST)
		{
			microphone.init();
		}
		reinit_subsystems.microphone=NO;
#endif
	}

	if(reinit_subsystems.speaker==YES)
	{
        // TODO: Do we need this?
#if 0
		speaker.baudrate = sampling_frequency;
		speaker.nbuffer_Samples = 2 * ofdm.Nfft*(1+ofdm.gi)*frequency_interpolation_rate*(ofdm.Nsymb+ofdm.preamble_configurator.Nsymb);
		speaker.frames_per_period = ofdm.Nfft*(1+ofdm.gi)*frequency_interpolation_rate;
		if(operation_mode != BER_PLOT_baseband &&
           operation_mode != BER_PLOT_passband &&
           operation_mode != RX_TEST)
		{
			speaker.init();
		}
		reinit_subsystems.speaker=NO;
#endif
	}

	// Invalidate cached sync state from previous config - frame timing differs
	// between configs (preamble_nSymb varies 1-4), so old values would be wrong
	receive_stats.delay_of_last_decoded_message = -1;
	receive_stats.freq_offset_of_last_decoded_message = 0;
	receive_stats.mfsk_search_raw = 0;

	printf("[PHY] Config %d active: M=%.0f LDPC_rate=%.3f BW=%.0fHz Nc=%d Nsymb=%d nBits=%d\n",
		current_configuration, M, ldpc.rate, bandwidth,
		data_container.Nc, data_container.Nsymb, data_container.nBits);
	if(M == MOD_MFSK)
	{
		double _mfsk_boost = sqrt((double)data_container.Nc / mfsk.nStreams) * pow(10.0, -2.0 / 20.0);
		printf("[PHY] MFSK: M=%d nStreams=%d bps=%d offsets=[", mfsk.M, mfsk.nStreams, mfsk.bits_per_symbol());
		for(int i = 0; i < mfsk.nStreams; i++) printf("%s%d", i?",":"", mfsk.stream_offsets[i]);
		printf("] Nc=%d boost=%.1fdB\n", mfsk.Nc, 20.0*log10(_mfsk_boost));

		// Set up short control frame parameters for MFSK modes.
		// BER testing determined safe ctrl_nBits values (same waterfall as full frame):
		//   ROBUST_0 (rate 1/16, 32-MFSK×1): 1200 bits, waterfall -13 dB
		//   ROBUST_1 (rate 1/16, 16-MFSK×2): 1400 bits, waterfall -11 dB
		//   ROBUST_2 (rate 1/4): no puncturing (rate 1/4 can't tolerate it)
		int bps = mfsk.bits_per_symbol();
		if(current_configuration == ROBUST_0)
		{
			ctrl_nBits = 1200;
			ctrl_nsymb = ctrl_nBits / bps;  // 240
		}
		else if(current_configuration == ROBUST_1)
		{
			ctrl_nBits = 1400;
			ctrl_nsymb = ctrl_nBits / bps;  // 175
		}
		else
		{
			ctrl_nBits = 0;  // no puncturing
			ctrl_nsymb = 0;
		}
		mfsk_ctrl_mode = false;

		if(ctrl_nBits > 0)
			printf("[PHY] Ctrl frame: nBits=%d nsymb=%d (%.0f%% of data)\n",
				ctrl_nBits, ctrl_nsymb, 100.0 * ctrl_nsymb / data_container.Nsymb);

	}
	else
	{
		ctrl_nBits = 0;
		ctrl_nsymb = 0;
		mfsk_ctrl_mode = false;
	}

	// Universal ACK pattern: dedicated ack_mfsk with fixed M=16, nStreams=1 for ALL modes.
	// Config-independent: both sides always agree on ACK tone parameters,
	// so no config switching needed for ACK pattern TX/RX.
	ack_mfsk.init(16, data_container.Nc, 1);  // M=16, Nc=50, 1 stream centered in band

	ack_pattern_passband_samples = cl_mfsk::ACK_PATTERN_NSYMB * data_container.Nofdm * frequency_interpolation_rate;

	// Per-mode detection threshold (all using ack_mfsk: M=16, nStreams=1):
	// ROBUST_0 (-13 dB): low SNR, need conservative threshold
	// ROBUST_1/2 (-11/-8 dB): moderate SNR, standard threshold
	// OFDM (0 to +20 dB): high SNR, easy detection
	if(current_configuration == ROBUST_0)
		ack_pattern_detection_threshold = 0.65;
	else if(is_robust_config(current_configuration))
		ack_pattern_detection_threshold = 1.0;
	else
		ack_pattern_detection_threshold = 1.0;

	printf("[PHY] ACK pattern: %d symbols, %d passband samples (%.0f ms), threshold=%.2f\n",
		cl_mfsk::ACK_PATTERN_NSYMB, ack_pattern_passband_samples,
		1000.0 * ack_pattern_passband_samples / sampling_frequency,
		ack_pattern_detection_threshold);
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
			data_container.bit_interleaved_data[i]=__random()%2;
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
