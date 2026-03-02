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
#include "debug/canary_guard.h"
#include <chrono>
#ifdef MERCURY_GUI_ENABLED
#include "gui/gui_state.h"
#endif
#if defined(_WIN32)
#include <windows.h>
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
	receive_stats.ofdm_search_raw=0;
	receive_stats.ofdm_batch_active=false;
	receive_stats.ofdm_drift_per_frame=0.0;
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
	receive_stats.ofdm_search_raw=0;
	receive_stats.ofdm_batch_active=false;
	receive_stats.ofdm_drift_per_frame=0.0;

	time_sync_trials_max=20;
	use_last_good_time_sync=NO;
	use_last_good_freq_offset=NO;
	mfsk_fixed_delay=-1;
	ofdm_forced_delay=-1;
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
	narrowband_enabled=NO;
	pre_equalization_channel=NULL;
	init_tx_gain_defaults();
}


cl_telecom_system::~cl_telecom_system()
{

}

void cl_telecom_system::init_tx_gain_defaults()
{
	// Compute default boost values from the original formula:
	//   max_Nc * trim / sqrt(Nc * nStreams)
	// where max_Nc=50, trim=pow(10,-2/20)=0.7943 (OFDM clip headroom offset)
	const double max_Nc = 50.0;
	const double trim = pow(10.0, -2.0 / 20.0);  // -2 dB = 0.7943

	// WB: Nc=50, NB: Nc=10
	const double Nc_wb = 50.0, Nc_nb = 10.0;

	// All gains calibrated via VB-Cable measurement (calibrate_gain.py + play_all_modes.py)
	// Target: WB CONFIG_0 peak level (-11.4 dBFS on VB-Cable)
	// Corrections applied as gain_old * pow(10, delta_dB/20)

	// [nb_mod=0] WB modulation
	// MFSK_1S was -10.8 → need -0.6 dB: 5.617 * 0.933 = 5.24
	// MFSK_2S was -11.3 → need -0.1 dB: essentially unchanged
	tx_gain[TX_SIG_MFSK_1S][0][0] = 5.24;
	tx_gain[TX_SIG_MFSK_1S][0][1] = 5.24;
	tx_gain[TX_SIG_MFSK_2S][0][0] = max_Nc * trim / sqrt(Nc_wb * 2.0);  // 3.97, already -11.3
	tx_gain[TX_SIG_MFSK_2S][0][1] = tx_gain[TX_SIG_MFSK_2S][0][0];
	tx_gain[TX_SIG_OFDM]   [0][0] = 1.0;  // -11.4, the reference
	tx_gain[TX_SIG_OFDM]   [0][1] = 1.0;
	tx_gain[TX_SIG_ACK]    [0][0] = 5.24;
	tx_gain[TX_SIG_ACK]    [0][1] = 5.24;
	tx_gain[TX_SIG_BREAK]  [0][0] = 5.24;
	tx_gain[TX_SIG_BREAK]  [0][1] = 5.24;

	// [nb_mod=1] NB modulation
	// Scale from WB calibrated values by sqrt(Nc_wb/Nc_nb) = sqrt(5) = 2.236
	// NB has fewer subcarriers → single MFSK tone needs more boost to match OFDM power.
	// Previous values (23.8, 16.9) were ~2x too high, causing 57% clipping → BER=0.5.
	double nb_scale = sqrt(Nc_wb / Nc_nb);  // sqrt(5) = 2.236
	tx_gain[TX_SIG_MFSK_1S][1][0] = tx_gain[TX_SIG_MFSK_1S][0][0] * nb_scale;  // 5.24 * 2.236 = 11.72
	tx_gain[TX_SIG_MFSK_1S][1][1] = tx_gain[TX_SIG_MFSK_1S][1][0];
	tx_gain[TX_SIG_MFSK_2S][1][0] = tx_gain[TX_SIG_MFSK_2S][0][0] * nb_scale;  // 3.97 * 2.236 = 8.88
	tx_gain[TX_SIG_MFSK_2S][1][1] = tx_gain[TX_SIG_MFSK_2S][1][0];
	tx_gain[TX_SIG_OFDM]   [1][0] = 1.0 * pow(10.0, 7.3 / 20.0);  // 2.317, already -11.5
	tx_gain[TX_SIG_OFDM]   [1][1] = tx_gain[TX_SIG_OFDM][1][0];
	tx_gain[TX_SIG_ACK]    [1][0] = tx_gain[TX_SIG_MFSK_1S][1][0];  // single tone, same as MFSK 1S
	tx_gain[TX_SIG_ACK]    [1][1] = tx_gain[TX_SIG_ACK][1][0];
	tx_gain[TX_SIG_BREAK]  [1][0] = tx_gain[TX_SIG_MFSK_1S][1][0];
	tx_gain[TX_SIG_BREAK]  [1][1] = tx_gain[TX_SIG_BREAK][1][0];
}

double cl_telecom_system::get_tx_gain(tx_signal_type sig) const
{
	int nb = (narrowband_enabled == YES) ? 1 : 0;
	return tx_gain[sig][nb][nb];  // mod and FIR always match currently
}

void cl_telecom_system::print_tx_gain_table() const
{
	static const char* sig_names[TX_SIG_COUNT] = {
		"MFSK_1S", "MFSK_2S", "OFDM   ", "ACK    ", "BREAK  "
	};
	static const char* mode_names[2] = { "WB", "NB" };

	printf("[TX-GAIN] Gain table (signal × mod × fir):\n");
	for(int sig = 0; sig < TX_SIG_COUNT; sig++)
	{
		for(int nb_mod = 0; nb_mod < 2; nb_mod++)
		{
			for(int nb_fir = 0; nb_fir < 2; nb_fir++)
			{
				const char* marker = (nb_mod == nb_fir) ? " <" : "  ";
				printf("[TX-GAIN]   %s  %s_mod  %s_fir  = %.4f%s\n",
					sig_names[sig], mode_names[nb_mod], mode_names[nb_fir],
					tx_gain[sig][nb_mod][nb_fir], marker);
			}
		}
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

		// Always use fully-equalized data for visualization (tight clusters).
		// Amplitude restoration still helps the decoder via the separate path.
		for(int i=0;i<ofdm.pilot_configurator.nData;i++)
		{
			contellation[constellation_plot_counter*ofdm.pilot_configurator.nData+i][0]=data_container.ofdm_deframed_data[i].real();
			contellation[constellation_plot_counter*ofdm.pilot_configurator.nData+i][1]=data_container.ofdm_deframed_data[i].imag();
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
		else
		{
			// OFDM BER test: use known delay position and skip freq sync.
			// Also needed for NB to prevent the NB freq estimator from running
			// on synthetic passband data (no real channel offset to measure).
			ofdm_forced_delay = ((data_container.preamble_nSymb+2)*data_container.Nofdm+delay)*frequency_interpolation_rate;
		}
		this->receive_byte(data_container.passband_delayed_data,data_container.hd_decoded_data_byte);
		mfsk_fixed_delay = -1;
		ofdm_forced_delay = -1;
		byte_to_bit(data_container.hd_decoded_data_byte,data_container.hd_decoded_data_bit,(nReal_data-outer_code_reserved_bits)/8);

		if(nDataPlot > 0)
		{
			// Always use fully-equalized data for visualization (tight clusters).
			for(int i=0;i<nDataPlot;i++)
			{
				contellation[constellation_plot_counter*nDataPlot+i][0]=data_container.ofdm_deframed_data[i].real();
				contellation[constellation_plot_counter*nDataPlot+i][1]=data_container.ofdm_deframed_data[i].imag();
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
	int msB = 0, lsB = 0;
	int frame_size = (nReal_data - outer_code_reserved_bits) / 8;

	if(nBytes > frame_size)
	{
		std::cout<<"message too long.. not sent."<<std::endl;
		return;
	}

	byte_to_bit(data, data_container.data_bit, nBytes);

	// Zero-pad data to full frame_size BEFORE CRC so that CRC covers a
	// fixed-size block.  RX self-check: CRC16([frame_size bytes + CRC]) = 0.
	for(int i = nBytes * 8; i < frame_size * 8; i++)
	{
		data_container.data_bit[i] = 0;
	}

	if(outer_code == CRC16_MODBUS_RTU)
	{
		// Pad byte array too (data may alias data_container.data_byte)
		for(int i = nBytes; i < frame_size; i++)
			data[i] = 0;
		uint16_t crc = CRC16_MODBUS_RTU_calc(data, frame_size);
		msB = (crc & 0xff00) >> 8;
		lsB = crc & 0x00ff;
		byte_to_bit(&lsB, &data_container.data_bit[frame_size * 8], 1);
		byte_to_bit(&msB, &data_container.data_bit[(frame_size + 1) * 8], 1);
	}

	// Zero any remaining non-byte-aligned waste bits after CRC
	for(int i = frame_size * 8 + outer_code_reserved_bits; i < nReal_data; i++)
	{
		data_container.data_bit[i] = 0;
	}

	transmit_bit(data_container.data_bit, out, message_location);
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

#ifdef MERCURY_GUI_ENABLED
		// Accumulate tone energies across ALL symbols for full-packet view
		{
			int nSymbols = active_nbits / (mfsk.nBits * mfsk.nStreams);
			double gui_E[2][64] = {};
			int gui_peak[2] = {};
			for (int st = 0; st < mfsk.nStreams && st < 2; st++)
			{
				for (int s = 0; s < nSymbols; s++)
				{
					int hop = (s * mfsk.tone_hop_step) % mfsk.M;
					for (int m = 0; m < mfsk.M && m < 64; m++)
					{
						std::complex<double> val = data_container.ofdm_framed_data[
							s * data_container.Nc + mfsk.stream_offsets[st] + ((m + hop) % mfsk.M)];
						gui_E[st][m] += val.real() * val.real() + val.imag() * val.imag();
					}
				}
				// Peak = last symbol's active tone (current tone indicator)
				if (nSymbols > 0) {
					int last_s = nSymbols - 1;
					int hop = (last_s * mfsk.tone_hop_step) % mfsk.M;
					double max_e = -1.0;
					for (int m = 0; m < mfsk.M && m < 64; m++)
					{
						std::complex<double> val = data_container.ofdm_framed_data[
							last_s * data_container.Nc + mfsk.stream_offsets[st] + ((m + hop) % mfsk.M)];
						double e = val.real() * val.real() + val.imag() * val.imag();
						if (e > max_e) { max_e = e; gui_peak[st] = m; }
					}
				}
			}
			gui_push_mfsk_tones(gui_E, gui_peak, mfsk.M, mfsk.nStreams, true);
		}
#endif
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
		// === DIAG: pre_eq at TX time (remove after debug) ===
		{
			static int tx_preeq_count = 0;
			static int tx_preeq_last_config = -1;
			if(tx_preeq_last_config != current_configuration) { tx_preeq_count = 0; tx_preeq_last_config = current_configuration; }
			if(tx_preeq_count < 2)
			{
				tx_preeq_count++;
				printf("[TX-PREEQ] CONFIG_%d pre_eq[0..4]=(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f)\n",
					current_configuration,
					pre_equalization_channel[0].value.real(), pre_equalization_channel[0].value.imag(),
					pre_equalization_channel[1].value.real(), pre_equalization_channel[1].value.imag(),
					pre_equalization_channel[2].value.real(), pre_equalization_channel[2].value.imag(),
					pre_equalization_channel[3].value.real(), pre_equalization_channel[3].value.imag(),
					pre_equalization_channel[4].value.real(), pre_equalization_channel[4].value.imag());
				fflush(stdout);
			}
		}
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

	// TX gain from calibration table (replaces computed mfsk_boost formula)
	double mfsk_boost = 1.0;
	if(M == MOD_MFSK)
	{
		tx_signal_type sig = (mfsk.nStreams == 1) ? TX_SIG_MFSK_1S : TX_SIG_MFSK_2S;
		mfsk_boost = get_tx_gain(sig);
	}
	else
	{
		mfsk_boost = get_tx_gain(TX_SIG_OFDM);
	}

	// Preamble boost: OFDM uses sqrt(2) boost for detection headroom;
	// MFSK preamble is already a concentrated single tone — no boost needed.
	double preamble_boost = (M == MOD_MFSK) ? 1.0 : ofdm.preamble_configurator.boost;

	for(int j=0;j<data_container.Nofdm*data_container.preamble_nSymb;j++)
	{
		data_container.preamble_symbol_modulated_data[j]/=power_normalization;
		data_container.preamble_symbol_modulated_data[j]*=sqrt(output_power_Watt)*preamble_boost*mfsk_boost;
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

		// TX-PEAK: measure peak passband amplitude (fires once per config)
		{
			static int peak_last_config = -1;
			if(peak_last_config != current_configuration)
			{
				peak_last_config = current_configuration;
				double peak = 0, rms_sum = 0;
				for(int i = 0; i < data_container.total_frame_size; i++)
				{
					double s = fabs(*(out+i));
					if(s > peak) peak = s;
					rms_sum += s * s;
				}
				double rms = sqrt(rms_sum / data_container.total_frame_size);
				printf("[TX-PEAK] CONFIG_%d peak=%.6f rms=%.6f papr=%.1fdB total_samples=%d\n",
					current_configuration, peak, rms, 20.0*log10(peak/rms), data_container.total_frame_size);
			}
		}

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
	receive_stats.frame_data_missing=false;
	receive_stats.sync_trials=0;

	// Timing breakdown
	double timing_pb_tsync_ms = 0, timing_pb_data_ms = 0, timing_ldpc_ms = 0;
	auto timing_total_start = std::chrono::steady_clock::now();

	int step=100;
	int pream_symb_loc;

	// Coarse frequency offset - starts at 0, only searched on trial 1 if trial 0 fails
	double coarse_freq_offset = 0.0;

	if(mfsk_fixed_delay >= 0)
	{
		// Known delay (BER test) - bypass time_sync entirely.
		// Delay set per-frame in passband_test_EsN0(), cleared after use.
		// Adjust for nUnder: in BER mode nUnder is always 0 (no capture thread),
		// so this is a no-op, but kept for safety.
		int nUnder_adj = data_container.nUnder_processing_events.load();
		int symbol_period = data_container.Nofdm * frequency_interpolation_rate;
		int adjusted_delay = mfsk_fixed_delay - nUnder_adj * symbol_period;
		if(adjusted_delay < 0) adjusted_delay = 0;

		receive_stats.delay = adjusted_delay;
		mfsk_fixed_delay = -1;
		pream_symb_loc = receive_stats.delay / (data_container.Nofdm * data_container.interpolation_rate);
		if(pream_symb_loc < 1) { pream_symb_loc = 1; }
		receive_stats.signal_stregth_dbm = 0;
	}
	else
	{
		// Quick passband energy diagnostic (first 3 calls per config)
		{
			static int pb_diag_count = 0;
			static int pb_diag_last_key = -1;
			int pb_key = current_configuration * 10 + narrowband_enabled;
			if(pb_key != pb_diag_last_key) { pb_diag_count = 0; pb_diag_last_key = pb_key; }
			if(pb_diag_count < 3)
			{
				pb_diag_count++;
				int pb_total = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;
				double e_total = 0, pk = 0;
				for(int i = 0; i < pb_total; i++)
				{
					double v = ((double*)data)[i];
					e_total += v*v;
					if(fabs(v) > pk) pk = fabs(v);
				}
				printf("[PB-ENERGY] rms=%.6f peak=%.6f samples=%d output_power=%.3f\n",
					sqrt(e_total/pb_total), pk, pb_total, output_power_Watt);
			}
		}
		auto t0_pb = std::chrono::steady_clock::now();
		{
			static int p2b_diag = 0;
			if(p2b_diag < 5) {
				p2b_diag++;
				printf("[P2B-DIAG] M=%.0f carrier_freq=%.1f carrier_amp=%.6f fs=%.0f interp=%d buf_samples=%d\n",
					M, carrier_frequency, carrier_amplitude, sampling_frequency,
					frequency_interpolation_rate,
					data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate);
				fflush(stdout);
			}
		}
		ofdm.passband_to_baseband((double*)data,data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate,data_container.baseband_data_interpolated,sampling_frequency,carrier_frequency,carrier_amplitude,1,&ofdm.FIR_rx_time_sync);
		auto t1_pb = std::chrono::steady_clock::now();
		timing_pb_tsync_ms = std::chrono::duration<double, std::milli>(t1_pb - t0_pb).count();

		receive_stats.signal_stregth_dbm=ofdm.measure_signal_stregth(data_container.baseband_data_interpolated, data_container.Nofdm*data_container.buffer_Nsymb*frequency_interpolation_rate);

		// Pre-scan passband for signal region to constrain OFDM preamble search.
		// After ACK TX + buffer flush, the buffer is: [zeros | VB-Cable silence | signal | silence].
		// VB-Cable silence has a DC offset that produces high GI+halfsym correlation
		// (metric ~0.97), competitive with real preambles. Scanning peak passband
		// amplitude identifies where real signal starts, letting us skip the silence.
		// Uses peak absolute value (like BUF-ENERGY), NOT mean squared — OFDM has
		// ~10-12 dB PAPR with 50 subcarriers, so mean squared is ~0.02 even when
		// peak is ~0.5. Threshold 0.1 peak: silence ~0, TX ramp ~0.02, signal 0.3+.
		int signal_start_symb = 0;
		if(M != MOD_MFSK)
		{
			int sym_samples = data_container.Nofdm * frequency_interpolation_rate;
			int buf_samples = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;
			double max_peak_seen = 0.0;
			for(int s = 0; s < data_container.buffer_Nsymb; s++)
			{
				int offset = s * sym_samples;
				double peak = 0.0;
				for(int i = 0; i < sym_samples && (offset + i) < buf_samples; i++)
				{
					double v = fabs(data[offset + i]);
					if(v > peak) peak = v;
				}
				if(peak > max_peak_seen) max_peak_seen = peak;
				if(peak > 0.1)
				{
					signal_start_symb = s;
					if(g_verbose)
						printf("[PRESCAN] signal_start_symb=%d peak=%.4f\n", s, peak);
					break;
				}
			}
			if(g_verbose && signal_start_symb == 0)
				printf("[PRESCAN] no signal found, max_peak=%.6f\n", max_peak_seen);
		}

		if(M == MOD_MFSK)
		{
			// MFSK preamble detection
			// Anti-re-decode: skip past where previous preamble sits in buffer
			int search_start = receive_stats.mfsk_search_raw - data_container.nUnder_processing_events;
			if(search_start < 0) search_start = 0;
			double mfsk_sync_metric = 0;

			if(ofdm.mfsk_corr_template != NULL)
			{
				// Waveform cross-correlation (2000:1 noise discrimination, NB+WB)
				receive_stats.delay = ofdm.time_sync_mfsk_corr(
					data_container.baseband_data_interpolated,
					data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate,
					data_container.interpolation_rate,
					search_start, &mfsk_sync_metric);
			}
			else
			{
				// WB: FFT energy ratio detection
				receive_stats.delay = ofdm.time_sync_mfsk(
					data_container.baseband_data_interpolated,
					data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate,
					data_container.interpolation_rate,
					data_container.preamble_nSymb, mfsk.preamble_tones,
					mfsk.M, mfsk.nStreams, mfsk.stream_offsets,
					search_start, &mfsk_sync_metric);
			}

			if(receive_stats.delay < 0)
			{
				st_receive_stats no_preamble = {};
				no_preamble.message_decoded = NO;
				no_preamble.delay = -1;
				no_preamble.signal_stregth_dbm = receive_stats.signal_stregth_dbm;
				return no_preamble;
			}

		}
		else
		{
			// Unified OFDM preamble detection (NB + WB, initial + batch).
			//
			// Schmidl-Cox autocorrelation: exploits L-sample periodicity of the
			// preamble (even-only subcarriers WB, every-2nd NB). Correlates the
			// received signal with itself — immune to audio path distortion.
			//   Coarse: GI stride over full buffer (halfsym_2phase)
			//   Fine: baseband stride within ±1 GI of coarse peak
			//
			// Metric: energy-weighted normalized correlation → [0,1].
			// Amplitude-independent: works at any RX gain / HF fading level.
			double preamble_detect_threshold = 0.15;

			// BER test: known delay bypasses detection entirely (same as mfsk_fixed_delay).
			// The forced delay positions the preamble exactly; no detection needed.
			if(ofdm_forced_delay >= 0)
			{
				// BER test: known delay bypasses detection (same as mfsk_fixed_delay).
				receive_stats.delay = ofdm_forced_delay;
				receive_stats.coarse_metric = 10.0;

				// Detection self-test: verify matched filter on first BER frame only.
				// Runs once per config to catch template mismatches without spamming
				// on low-SNR frames (where noise makes detection impossible).
				{
					static int selftest_config = -1;
					if(selftest_config != current_configuration)
					{
						selftest_config = current_configuration;
						int interp_st = data_container.interpolation_rate;
						int sym_st = data_container.Nofdm * interp_st;
						int buf_interp_st = data_container.Nofdm * data_container.buffer_Nsymb * interp_st;
						int margin = 4 * sym_st;
						int st_start = ofdm_forced_delay - margin;
						if(st_start < 0) st_start = 0;
						int st_size = 2 * margin + data_container.preamble_nSymb * sym_st;
						if(st_start + st_size > buf_interp_st) st_size = buf_interp_st - st_start;
						if(st_size > 0)
						{
							TimeSyncResult selftest = ofdm.time_sync_preamble_halfsym(
								&data_container.baseband_data_interpolated[st_start],
								st_size, interp_st, interp_st);
							int detected_delay = st_start + selftest.delay;
							int gi_interp_st = data_container.Ngi * interp_st;
							printf("[BER-DET] config=%d metric=%.4f delay=%d expected=%d %s\n",
								current_configuration,
								selftest.correlation, detected_delay, ofdm_forced_delay,
								(selftest.correlation < preamble_detect_threshold
								 || abs(detected_delay - ofdm_forced_delay) > gi_interp_st)
								? "WARN-lowSNR" : "OK");
						}
					}
				}
			}
			else
			{

			int interp = data_container.interpolation_rate;
			int sym_samples = data_container.Nofdm * interp;
			int buf_interp = data_container.Nofdm * data_container.buffer_Nsymb * interp;
			int ofdm_skip = receive_stats.ofdm_search_raw - data_container.nUnder_processing_events;
			if(ofdm_skip < 0) ofdm_skip = 0;

			int search_offset, search_size;

			bool batch_verified = false;

			if(receive_stats.ofdm_batch_active && receive_stats.ofdm_search_raw > 0)
			{
				// BATCH mode: predict + verify. After successful decode, the next
				// preamble position is predictable from ofdm_skip. Try a tiny
				// verify window first; fall back to wider search on failure.
				int gi_interp = data_container.Ngi * interp;
				int preamble_interp = data_container.preamble_nSymb * sym_samples;
				int predicted_pos = ofdm_skip * sym_samples + (int)receive_stats.ofdm_drift_per_frame;
				if(predicted_pos < 0) predicted_pos = 0;

				// Verify in ±2*gi_interp window around prediction
				int verify_start = predicted_pos - 2 * gi_interp;
				if(verify_start < 0) verify_start = 0;
				int verify_size = 4 * gi_interp + preamble_interp;
				if(verify_start + verify_size > buf_interp)
					verify_size = buf_interp - verify_start;

				if(verify_size > 0)
				{
					TimeSyncResult verify = ofdm.time_sync_preamble_halfsym(
						&data_container.baseband_data_interpolated[verify_start],
						verify_size, interp, interp);

					if(verify.correlation >= preamble_detect_threshold)
					{
						// Prediction verified — use directly, skip wider search
						receive_stats.delay = verify_start + verify.delay;
						receive_stats.coarse_metric = verify.correlation;
						int drift = (int)receive_stats.delay - predicted_pos;
						receive_stats.ofdm_drift_per_frame = 0.8 * receive_stats.ofdm_drift_per_frame + 0.2 * drift;
						batch_verified = true;
					}
				}

				if(!batch_verified)
				{
					// Prediction failed — search full remaining buffer from
					// ofdm_skip onwards (same as initial search).  The narrow
					// forward_look=40 cap was causing preambles >40 symbols
					// past ofdm_skip to be missed, especially after turnaround
					// gaps where the next preamble arrives much later.
					int effective_start = (ofdm_skip > signal_start_symb) ? ofdm_skip : signal_start_symb;
					search_offset = effective_start * sym_samples;
					search_size = buf_interp - search_offset;
					receive_stats.ofdm_drift_per_frame = 0.0;
				}
			}
			else
			{
				// INITIAL search: full buffer, skip past decoded frames.
				int effective_start = (ofdm_skip > signal_start_symb) ? ofdm_skip : signal_start_symb;
				search_offset = effective_start * sym_samples;
				search_size = buf_interp - search_offset;
			}

			if(!batch_verified)
			{
				// Schmidl-Cox autocorrelation: two-phase (coarse at GI stride,
				// fine at baseband stride). Correlates signal with itself —
				// immune to audio path distortion.
				//
				// Early exit (0.5): find the FIRST preamble above threshold
				// instead of the maximum. Prevents later frames with higher
				// energy-weighted metric from shadowing earlier ones (seq=00)
				// when multiple back-to-back frames are in the buffer.
				// Batch prediction handles sequential frames after first lock.
				TimeSyncResult matched = ofdm.time_sync_preamble_halfsym_2phase(
					&data_container.baseband_data_interpolated[search_offset],
					search_size, interp, 0.5);

				receive_stats.delay = search_offset + matched.delay;
				receive_stats.coarse_metric = matched.correlation;

				if(matched.correlation < preamble_detect_threshold)
				{
					// No preamble found — exit batch mode.  Don't reset
					// ofdm_search_raw here; the ARQ FAIL handler will
					// decrement it via ftr.  But signal that the full
					// search came up empty so the ARQ layer can reset
					// search_raw faster (e.g. ftr=frame_symb).
					receive_stats.ofdm_batch_active = false;
				}
			}

			if (g_verbose) printf("[OFDM-SYNC] %s %s offset=%d size=%d metric=%.4f delay=%d thr=%.1f\n",
				narrowband_enabled ? "NB" : "WB",
				receive_stats.ofdm_batch_active ? "BATCH" : "INIT",
				search_offset, search_size, receive_stats.coarse_metric, (int)receive_stats.delay,
				preamble_detect_threshold);

			} // end else (non-forced-delay detection)
		}
		pream_symb_loc=receive_stats.delay/(data_container.Nofdm*data_container.interpolation_rate);
		if(pream_symb_loc<1){pream_symb_loc=1;}

	}

	// MFSK frame completeness check: if the frame extends beyond the buffer,
	// don't attempt decode — signal the ARQ layer to capture more audio.
	// Skip in BER test mode (mfsk_fixed_delay >= 0) where buffers are pre-sized.
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
	double preamble_detect_threshold = 0.15;

	if(M != MOD_MFSK)
	{
		if (g_verbose)
			printf("[OFDM-SYNC] coarse: pream_symb=%d delay=%d bounds=[%d,%d] metric=%.3f %s\n",
				pream_symb_loc, receive_stats.delay, lower_bound, upper_bound,
				receive_stats.coarse_metric,
				(pream_symb_loc > lower_bound && pream_symb_loc <= upper_bound) ? "PASS" : "SKIP");
		fflush(stdout);
	}

	// Recovery for OFDM when preamble lands outside the valid bounds.
	// Even with full-buffer coarse search, Schmidl-Cox may peak at a position
	// too close to the start or end to extract a complete frame. Scan the
	// buffer for signal energy and re-run Schmidl-Cox from the signal start.
	//
	// IMPORTANT: Respect anti-re-decode. When ofdm_search_raw has advanced past
	// upper_bound, the search region has no room for complete frames. The recovery
	// must NOT rescan from the start (which would re-find already-decoded frames).
	if(M != MOD_MFSK && !(pream_symb_loc > lower_bound && pream_symb_loc <= upper_bound))
	{
		// Anti-re-decode check: if skip is past upper_bound, there are no
		// un-decoded frames left in the buffer. Skip recovery → FAIL → buffer shift.
		int ofdm_eff = receive_stats.ofdm_search_raw - data_container.nUnder_processing_events;
		if(ofdm_eff < 0) ofdm_eff = 0;
		if(ofdm_eff > upper_bound)
		{
			// No room for new frames — force FAIL
			pream_symb_loc = 0;
		}
		else
		{

		int sym_samples = data_container.Nofdm * frequency_interpolation_rate;
		int buf_samples = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;

		if (g_verbose) printf("[OFDM-SYNC] bounds-failed: pream_symb=%d, scanning full buffer for signal\n", pream_symb_loc);
		fflush(stdout);

		// Start scan from anti-re-decode position to avoid re-finding old preambles
		int scan_start = lower_bound + 1;
		if(ofdm_eff > scan_start) scan_start = ofdm_eff;
		int signal_start_symb = -1;
		for(int s = scan_start; s <= upper_bound; s++)
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
			// Constrain GI retry window (same as silence-skip recovery)
			int max_search = (data_container.preamble_nSymb + 4) * sym_samples;
			if(available > max_search) available = max_search;

			if(available > data_container.preamble_nSymb * sym_samples)
			{
				// Schmidl-Cox autocorrelation: preamble L-sample periodicity
				// discriminates preamble from data (data has no L-period).
				TimeSyncResult retry = ofdm.time_sync_preamble_halfsym(
					&data_container.baseband_data_interpolated[search_start],
					available, data_container.interpolation_rate,
					data_container.interpolation_rate);
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

				if (g_verbose)
					printf("[OFDM-SYNC] bounds-skip: signal=%d retry=%d metric=%.3f energy=%.2e\n",
						signal_start_symb, retry_symb, retry.correlation, retry_energy);
				fflush(stdout);

				if(retry_energy >= 0.001 && retry.correlation >= preamble_detect_threshold
					&& retry_symb > lower_bound && retry_symb <= upper_bound)
				{
					receive_stats.delay = retry.delay;
					receive_stats.coarse_metric = retry.correlation;
					pream_symb_loc = retry_symb;
				}
			}
		}
		} // else: recovery with anti-re-decode scan
	}

	int skip_h_count = 0;
	if(pream_symb_loc > lower_bound && pream_symb_loc <= upper_bound)
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
				if (g_verbose)
					printf("[OFDM-SYNC] energy=%.2e at delay=%d — silence, skipping decode\n",
						mean_energy, receive_stats.delay);
				fflush(stdout);
				energy_ok = false;
			}

			// Metric threshold: reject weak peaks that correspond to data symbols.
			// Schmidl-Cox preamble: ~0.5-1.0. Data: ~0.01-0.05 (no L-period).
			// Threshold 0.10 blocks data peaks while allowing degraded preambles.
			if(energy_ok && receive_stats.coarse_metric < 0.10)
			{
				if (g_verbose)
					printf("[OFDM-SYNC] metric=%.3f at delay=%d — weak peak, skipping decode\n",
						receive_stats.coarse_metric, receive_stats.delay);
				fflush(stdout);
				energy_ok = false;
			}

			// Silence-skip: when the best Schmidl-Cox peak lands in silence
			// (energy gate rejected), scan forward to find where signal actually
			// starts and re-run with GI correlation from there. This handles
			// silence-preceded buffers (e.g. SET_CONFIG after ACK) where silence
			// or boundary peaks beat the real preamble in the initial search.
			// Both NB and WB now use halfsym which searches the whole buffer,
			// so this rarely activates. Kept as safety net for edge cases.
			if(!energy_ok)
			{
				int signal_start_symb = -1;
				for(int s = pream_symb_loc + 1; s <= upper_bound; s++)
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
					// Constrain GI retry to preamble + 4 symbols around signal start.
					// Searching the full remaining buffer can find a later frame
					// repetition past upper_bound, causing the retry to fail bounds.
					int max_search = (data_container.preamble_nSymb + 4) * sym_samples;
					if(available > max_search) available = max_search;

					if(available > data_container.preamble_nSymb * sym_samples)
					{
						// Schmidl-Cox autocorrelation: preamble L-sample periodicity
						// discriminates preamble from data (data has no L-period).
						TimeSyncResult retry = ofdm.time_sync_preamble_halfsym(
							&data_container.baseband_data_interpolated[search_start],
							available, data_container.interpolation_rate,
							data_container.interpolation_rate);
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

						if (g_verbose)
							printf("[OFDM-SYNC] silence-skip: orig=%d signal=%d retry=%d metric=%.3f energy=%.2e\n",
								pream_symb_loc, signal_start_symb, retry_symb, retry.correlation, retry_energy);
						fflush(stdout);

						if(retry_energy >= 0.001 && retry.correlation >= preamble_detect_threshold
							&& retry_symb > lower_bound && retry_symb <= upper_bound)
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

		// Data energy gate (after all recovery paths): preamble has energy
		// but data symbols may be silence — frame partially captured, with
		// preamble at trailing edge and data still arriving in the pipeline.
		// Runs after silence-skip recovery so the final pream position is used.
		if(energy_ok && M != MOD_MFSK)
		{
			int sym_samples_de = data_container.Nofdm * frequency_interpolation_rate;
			int buf_samples_de = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;
			int data_offset = receive_stats.delay + data_container.preamble_nSymb * sym_samples_de;
			double data_e = 0.0;
			int d_count = 0;
			int check_len = 4 * sym_samples_de; // first 4 data symbols
			for(int i = 0; i < check_len && (data_offset + i) < buf_samples_de; i++)
			{
				double re = data_container.baseband_data_interpolated[data_offset + i].real();
				double im = data_container.baseband_data_interpolated[data_offset + i].imag();
				data_e += re*re + im*im;
				d_count++;
			}
			data_e = (d_count > 0) ? data_e / d_count : 0.0;

			// DIAG: compare passband vs baseband energy at preamble and data positions
			{
				int pream_offset = receive_stats.delay;
				double pb_pream = 0, pb_data = 0, bb_pream = 0;
				int pream_len = data_container.preamble_nSymb * sym_samples_de;
				for(int i = 0; i < pream_len && (pream_offset + i) < buf_samples_de; i++) {
					double v = ((double*)data)[pream_offset + i];
					pb_pream += v*v;
					double re = data_container.baseband_data_interpolated[pream_offset + i].real();
					double im = data_container.baseband_data_interpolated[pream_offset + i].imag();
					bb_pream += re*re + im*im;
				}
				pb_pream /= pream_len;
				bb_pream /= pream_len;
				for(int i = 0; i < check_len && (data_offset + i) < buf_samples_de; i++) {
					double v = ((double*)data)[data_offset + i];
					pb_data += v*v;
				}
				pb_data = (d_count > 0) ? pb_data / d_count : 0.0;
				printf("[ENERGY-DIAG] pream: pb=%.4e bb=%.4e | data: pb=%.4e bb=%.4e | delay=%d data_off=%d buf=%d\n",
					pb_pream, bb_pream, pb_data, data_e, receive_stats.delay, data_offset, buf_samples_de);
				fflush(stdout);
			}

			if(data_e < 0.001)
			{
				printf("[OFDM-SYNC] data_energy=%.2e at pream=%d delay=%d — frame incomplete, skipping decode\n",
					data_e, pream_symb_loc, receive_stats.delay);
				fflush(stdout);
				energy_ok = false;
				receive_stats.frame_data_missing = true;
			}
		}

		if(energy_ok)
		{
		if(M != MOD_MFSK && g_verbose)
		{
			printf("[OFDM-ENTRY] metric=%.3f delay=%d symb=%d %s Nc=%d Nsymb=%d\n",
				receive_stats.coarse_metric, receive_stats.delay, pream_symb_loc,
				narrowband_enabled ? "NB" : "WB", ofdm.Nc, ofdm.Nsymb);
			fflush(stdout);
		}

		skip_h_count = 0;
		double mean_H = -1.0;
		bool skip_h_recovery_attempted = false;
skip_h_retry_point:
		while (receive_stats.sync_trials<=time_sync_trials_max)
		{
			if(mfsk_fixed_delay >= 0)
			{
				// Known delay - skip all time_sync refinement
				receive_stats.delay = mfsk_fixed_delay;
			}
			else if(ofdm_forced_delay >= 0)
			{
				// OFDM forced delay (BER test): skip sync refinement, use known position
				receive_stats.delay = ofdm_forced_delay;
				if(receive_stats.sync_trials > 0) break;  // one trial only
			}
			else if(M == MOD_MFSK)
			{
				// MFSK: time_sync_mfsk already found optimal position, no refinement needed
				// Only one trial - spectral flatness sync is deterministic
				if(receive_stats.sync_trials > 0) break;
				// Trial 0: use delay from initial sync as-is
			}
			else if(narrowband_enabled)
			{
				// NB OFDM: halfsym detection provides timing, Moose handles freq sync.
				// NB preamble uses even-only subcarriers (Bug #41), enabling half-symbol
				// repetition. Single trial for now; coarse freq search possible in future.
				if(receive_stats.sync_trials > 0) break;
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

				int sym_samp = data_container.Nofdm * frequency_interpolation_rate;
				for (int i = 0; i < n_search; i++)
				{
					ofdm.passband_to_baseband((double*)data,
						data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate,
						data_container.baseband_data_interpolated,
						sampling_frequency,
						carrier_frequency + freq_search[i],
						carrier_amplitude, 1, &ofdm.FIR_rx_time_sync);

					// FFT fine search around known preamble position.
					// FFT metric discriminates preamble vs data (4× ratio)
					// and drops sharply with frequency error — ideal for
					// comparing offsets.
					int base_off = (pream_symb_loc > 0 ? pream_symb_loc - 1 : 0) * sym_samp;
					int buf_lim = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;
					int avail = buf_lim - base_off;
					int need = (data_container.preamble_nSymb + 4) * sym_samp;
					if(avail > need) avail = need;
					TimeSyncResult ts_result = ofdm.time_sync_preamble_halfsym(
						&data_container.baseband_data_interpolated[base_off],
						avail, data_container.interpolation_rate,
						data_container.interpolation_rate);
					ts_result.delay += base_off;

					if (fabs(freq_search[i]) < 0.1)
						zero_hz_correlation = ts_result.correlation;

					if (ts_result.correlation > best_correlation)
					{
						best_correlation = ts_result.correlation;
						best_offset = freq_search[i];
						best_delay = ts_result.delay;
					}
				}

				// Apply only if non-zero offset is significantly better than 0 Hz.
				// Schmidl-Cox metric range: 0-1 (~0.8+ at preamble, ~0.05 at data).
				// Delta 0.08 = meaningful improvement over 0 Hz baseline.
				if (fabs(best_offset) > 1.0 && best_correlation > preamble_detect_threshold &&
				    best_correlation > zero_hz_correlation + 0.08)
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

				// Schmidl-Cox fine time sync at the corrected frequency
				{
					int sym_samp = data_container.Nofdm * frequency_interpolation_rate;
					int base_off = (pream_symb_loc > 0 ? pream_symb_loc - 1 : 0) * sym_samp;
					int buf_lim = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;
					int avail = buf_lim - base_off;
					int need = (data_container.preamble_nSymb + 4) * sym_samp;
					if(avail > need) avail = need;
					TimeSyncResult ts_result = ofdm.time_sync_preamble_halfsym(
						&data_container.baseband_data_interpolated[base_off],
						avail, data_container.interpolation_rate,
						data_container.interpolation_rate);
					receive_stats.delay = base_off + ts_result.delay;
				}
			}
			else
			{
				// GI+halfsym fine timing for Moose freq sync alignment.
				// Coarse detection finds the correct symbol, but Moose needs
				// the halfsym alignment that even-only preamble subcarriers
				// provide. GI+halfsym finds a position where mean_H≥0.30,
				// allowing LDPC decode. See PHASE2_FFT_REPLACEMENT.md §8.
				TimeSyncResult fine_result = ofdm.time_sync_preamble_with_metric(
					&data_container.baseband_data_interpolated[(pream_symb_loc-1)*data_container.Nofdm*frequency_interpolation_rate],
					(ofdm.preamble_configurator.Nsymb+4)*data_container.Nofdm*data_container.interpolation_rate,
					data_container.interpolation_rate, receive_stats.sync_trials, 1, time_sync_trials_max);
				receive_stats.delay = (pream_symb_loc-1)*data_container.Nofdm*frequency_interpolation_rate + fine_result.delay;
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
							if (g_verbose)
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

			// Compute extraction range before data FIR so we can scope it.
			int extraction_delay = receive_stats.delay;
			int buf_size_interp = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;
			int frame_size_interp = (data_container.Nofdm*(data_container.Nsymb+data_container.preamble_nSymb))*frequency_interpolation_rate;
			if(extraction_delay < 0) extraction_delay = 0;
			if(extraction_delay > buf_size_interp - frame_size_interp)
				extraction_delay = buf_size_interp - frame_size_interp;

			// Scoped data FIR: only process the frame region + FIR margin.
			// The time_sync FIR already processed the full buffer for preamble search;
			// the data FIR only needs the frame. Saves 80-90% for high configs
			// where frame (10 symbols) is much smaller than buffer (113 symbols).
			int fir_margin = ofdm.FIR_rx_data.filter_nTaps * frequency_interpolation_rate;
			int pb_start = extraction_delay - fir_margin;
			if(pb_start < 0) pb_start = 0;
			int pb_end = extraction_delay + frame_size_interp + fir_margin;
			if(pb_end > buf_size_interp) pb_end = buf_size_interp;
			int pb_size = pb_end - pb_start;

			auto t2_pb = std::chrono::steady_clock::now();
			ofdm.passband_to_baseband(&data[pb_start],pb_size,&data_container.baseband_data_interpolated[pb_start],sampling_frequency,effective_carrier_freq,carrier_amplitude,1,&ofdm.FIR_rx_data,pb_start);
			auto t3_pb = std::chrono::steady_clock::now();
			timing_pb_data_ms += std::chrono::duration<double, std::milli>(t3_pb - t2_pb).count();

			ofdm.rational_resampler(&data_container.baseband_data_interpolated[extraction_delay], frame_size_interp, data_container.baseband_data, data_container.interpolation_rate, DECIMATION);

			if(ofdm_forced_delay >= 0)
			{
				// BER test: true freq offset is 0, skip estimation
				freq_offset_measured = 0;
			}
			else if(receive_stats.sync_trials==time_sync_trials_max && use_last_good_freq_offset==YES && receive_stats.freq_offset_of_last_decoded_message!=0)
			{
				freq_offset_measured=receive_stats.freq_offset_of_last_decoded_message;
			}
			else if(narrowband_enabled)
			{
				// NB: skip fine freq sync entirely. Both Moose and
				// carrier_frequency_sync_nb are unreliable for NB:
				// - Moose: only 2 of 5 preamble SCs survive duplication-FFT
				// - carrier_frequency_sync_nb: gives -17 Hz on zero-offset channel
				//   after preamble changed to even-only subcarriers (Bug #41)
				// Coarse sync residual (±7.5 Hz max) is handled by ZF estimator
				// (no cross-pilot averaging → immune to phase rotation).
				freq_offset_measured = 0;
				if(g_verbose)
					printf("[NB-FREQ] skipped (relying on coarse sync + ZF)\n");
			}
			else
			{
				// Fine frequency sync (Moose algorithm) - ±0.5 subcarrier range
				// Note: Coarse offset already applied before this loop, so Moose measures residual
				// BUG FIX: baseband_data is at decimated (base) rate after rational_resampler,
				// so guard interval skip is Ngi samples, NOT Ngi*interpolation_rate.
				// The old code skipped Ngi*4=256=Nfft samples, reading across symbol boundaries.
				freq_offset_measured=ofdm.carrier_sampling_frequency_sync(&data_container.baseband_data[data_container.Ngi],bandwidth/(double)data_container.Nc,data_container.preamble_nSymb, sampling_frequency);
				if(g_verbose)
					printf("[WB-FREQ] Moose=%.4f Hz\n", freq_offset_measured);
			}

			// Clamp fine freq correction to ±subcarrier_spacing/4.
			// Coarse sync handles larger offsets; any Moose/NB estimate beyond this
			// is likely wrong and would introduce ICI rather than correct it.
			{
				double max_correction = bandwidth / (double)data_container.Nc / 4.0;
				if(freq_offset_measured > max_correction) freq_offset_measured = max_correction;
				if(freq_offset_measured < -max_correction) freq_offset_measured = -max_correction;
			}

			if(M == MOD_MFSK)
			{
				// MFSK: skip fine frequency correction (no channel estimation to compensate)
			}
			else if(fabs(freq_offset_measured)>ofdm.freq_offset_ignore_limit)
			{
				// Apply fine correction on top of coarse correction (scoped to frame region)
				auto t6_pb = std::chrono::steady_clock::now();
				ofdm.passband_to_baseband(&data[pb_start],pb_size,&data_container.baseband_data_interpolated[pb_start],sampling_frequency,effective_carrier_freq+freq_offset_measured,carrier_amplitude,1,&ofdm.FIR_rx_data,pb_start);
				auto t7_pb = std::chrono::steady_clock::now();
				timing_pb_data_ms += std::chrono::duration<double, std::milli>(t7_pb - t6_pb).count();
				ofdm.rational_resampler(&data_container.baseband_data_interpolated[extraction_delay], frame_size_interp, data_container.baseband_data, data_container.interpolation_rate, DECIMATION);
			}
			{
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

#ifdef MERCURY_GUI_ENABLED
				// Accumulate tone energies across ALL symbols for full-packet view
				{
					int nSymbols = rx_nbits / (mfsk.nBits * mfsk.nStreams);
					double gui_E[2][64] = {};
					int gui_peak[2] = {};
					for (int st = 0; st < mfsk.nStreams && st < 2; st++)
					{
						for (int s = 0; s < nSymbols; s++)
						{
							int hop = (s * mfsk.tone_hop_step) % mfsk.M;
							for (int m = 0; m < mfsk.M && m < 64; m++)
							{
								std::complex<double> val = data_container.ofdm_symbol_demodulated_data[
									s * data_container.Nc + mfsk.stream_offsets[st] + ((m + hop) % mfsk.M)];
								gui_E[st][m] += val.real() * val.real() + val.imag() * val.imag();
							}
						}
						// Peak = last symbol's active tone
						if (nSymbols > 0) {
							int last_s = nSymbols - 1;
							int hop = (last_s * mfsk.tone_hop_step) % mfsk.M;
							double max_e = -1.0;
							for (int m = 0; m < mfsk.M && m < 64; m++)
							{
								std::complex<double> val = data_container.ofdm_symbol_demodulated_data[
									last_s * data_container.Nc + mfsk.stream_offsets[st] + ((m + hop) % mfsk.M)];
								double e = val.real() * val.real() + val.imag() * val.imag();
								if (e > max_e) { max_e = e; gui_peak[st] = m; }
							}
						}
					}
					gui_push_mfsk_tones(gui_E, gui_peak, mfsk.M, mfsk.nStreams, false);
				}
#endif

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
				if(narrowband_enabled)
					ofdm.CPE_correction(data_container.ofdm_symbol_demodulated_data);

				if(ofdm.channel_estimator==ZERO_FORCE)
				{
					ofdm.ZF_channel_estimator(data_container.ofdm_symbol_demodulated_data);
				}
				else if (ofdm.channel_estimator==LEAST_SQUARE)
				{
					ofdm.LS_channel_estimator(data_container.ofdm_symbol_demodulated_data);
				}

				mean_H = -1.0;
				int h_count = 0;
				{
					double h_sum = 0;
					for(int ci = 0; ci < ofdm.Nsymb * ofdm.Nc; ci++)
					{
						if(ofdm.estimated_channel[ci].status == MEASURED)
						{
							h_sum += std::abs(ofdm.estimated_channel[ci].value);
							h_count++;
						}
					}
					if(h_count > 0) mean_H = h_sum / h_count;
				}
				{
					double mean_H_threshold = 0.50;
					if(mean_H < mean_H_threshold)
					{
						skip_h_count++;
						if (g_verbose)
						{
							printf("[OFDM-SYNC] trial %d SKIP-H: mean_H=%.4f too low (threshold=%.2f), skipping LDPC\n",
								receive_stats.sync_trials, mean_H, mean_H_threshold);
							// Diagnostic: print first 8 pilot |H| values and positions
							if(skip_h_count <= 2) {
								int p_printed = 0;
								printf("[H-DIAG] Nc=%d Nsymb=%d h_count=%d delay=%d FIR_cut=%.1f bw=%.1f\n",
									ofdm.Nc, ofdm.Nsymb, h_count, receive_stats.delay,
									ofdm.FIR_rx_data.lpf_filter_cut_frequency, bandwidth);
								for(int ci = 0; ci < ofdm.Nsymb * ofdm.Nc && p_printed < 8; ci++) {
									if(ofdm.estimated_channel[ci].status == MEASURED) {
										printf("[H-DIAG]  pilot[%d/%d] |H|=%.6f phase=%.1f\n",
											ci/ofdm.Nc, ci%ofdm.Nc,
											std::abs(ofdm.estimated_channel[ci].value),
											std::arg(ofdm.estimated_channel[ci].value) * 180.0 / M_PI);
										p_printed++;
									}
								}
								fflush(stdout);
							}
						}
						fflush(stdout);
						receive_stats.sync_trials++;
						continue;
					}
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

			auto t4_ldpc = std::chrono::steady_clock::now();
			receive_stats.iterations_done=ldpc.decode(data_container.deinterleaved_data,data_container.hd_decoded_data_bit);
			auto t5_ldpc = std::chrono::steady_clock::now();
			timing_ldpc_ms += std::chrono::duration<double, std::milli>(t5_ldpc - t4_ldpc).count();

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

			// CRC16 self-check: compute CRC over [data + CRC_LSB + CRC_MSB] = nReal_data/8 bytes.
			// For correct data, CRC16_MODBUS_RTU of [message || appended_CRC] = 0.
			// Check on ALL frames (not just LDPC failures) to catch wrong-codeword convergence.
			receive_stats.crc=0;
			if(outer_code == CRC16_MODBUS_RTU && receive_stats.all_zeros == NO)
			{
				receive_stats.crc=CRC16_MODBUS_RTU_calc(data_container.hd_decoded_data_byte, nReal_data/8);
			}

			if(receive_stats.all_zeros==YES ||
			   (outer_code == CRC16_MODBUS_RTU && receive_stats.crc != 0) ||
			   (outer_code != CRC16_MODBUS_RTU && receive_stats.iterations_done > (ldpc.nIteration_max-1)))
			{
				receive_stats.SNR=-99.9;
				receive_stats.message_decoded=NO;
				if(M != MOD_MFSK)
				{
					if (g_verbose)
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
					// ZF SNR measurement: re-encode decoded bits to get ideal
					// modulated symbols, then compare with received symbols.
					// Must re-apply bit_energy_dispersal (undo the descrambling
					// done earlier) since the LDPC encoder expects scrambled data.
					// Use a temp buffer to avoid corrupting hd_decoded_data_bit
					// (which the BER test compares against the original data).
					int temp_bits[N_MAX];
					memcpy(temp_bits, data_container.hd_decoded_data_bit, nReal_data * sizeof(int));
					bit_energy_dispersal(temp_bits, data_container.bit_energy_dispersal_sequence, temp_bits, nReal_data);

					for(int i=0;i<nVirtual_data;i++)
					{
						temp_bits[nReal_data+i]=temp_bits[i];
					}
					ldpc.encode(temp_bits,data_container.encoded_data);
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

#ifdef MERCURY_GUI_ENABLED
				// Push fully-equalized data for visualization (tight clusters).
				// Amplitude restoration still helps the decoder via the separate path.
				if (M != MOD_MFSK) {
					gui_push_constellation(data_container.ofdm_deframed_data, data_container.nData, (int)M, false);
				} else {
					gui_push_constellation(nullptr, 0, (int)M, true);
				}
#endif

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

				// Schmidl-Cox autocorrelation: preamble L-sample periodicity
				// discriminates preamble from data (data has no L-period).
				TimeSyncResult retry = ofdm.time_sync_preamble_halfsym(
					&data_container.baseband_data_interpolated[search_start],
					available, data_container.interpolation_rate,
					data_container.interpolation_rate);
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

				if (g_verbose)
					printf("[OFDM-SYNC] SKIP-H recovery: orig=%d retry=%d metric=%.3f energy=%.2e\n",
						pream_symb_loc, retry_symb, retry.correlation, retry_energy);
				fflush(stdout);

				// Schmidl-Cox metric >= threshold = confirmed preamble.
				// The trial loop's mean_H check provides additional validation.
				if(retry_energy >= 0.001 && retry.correlation >= preamble_detect_threshold
					&& retry_symb > lower_bound && retry_symb <= upper_bound)
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

		// Diagnostic: decode failure analysis
		if(receive_stats.message_decoded != YES)
		{
			const char* fail_type = (skip_h_count > 0) ? "SKIP-H" : "LDPC";
			// Measure energy at preamble and data positions
			int sym_samp = data_container.Nofdm * frequency_interpolation_rate;
			int buf_samp = data_container.Nofdm * data_container.buffer_Nsymb * frequency_interpolation_rate;
			double pream_energy = 0.0, data_energy = 0.0;
			int pream_start = pream_symb_loc * sym_samp;
			int data_start = (pream_symb_loc + data_container.preamble_nSymb) * sym_samp;
			for(int i = 0; i < sym_samp && (pream_start + i) < buf_samp; i++) {
				double re = data_container.baseband_data_interpolated[pream_start + i].real();
				double im = data_container.baseband_data_interpolated[pream_start + i].imag();
				pream_energy += re*re + im*im;
			}
			pream_energy /= sym_samp;
			for(int i = 0; i < 4*sym_samp && (data_start + i) < buf_samp; i++) {
				double re = data_container.baseband_data_interpolated[data_start + i].real();
				double im = data_container.baseband_data_interpolated[data_start + i].imag();
				data_energy += re*re + im*im;
			}
			data_energy /= (4*sym_samp);
			printf("[FAIL-DIAG] %s: metric=%.3f pream_sym=%d skip_h=%d/%d delay=%d mean_H=%.4f pE=%.2e dE=%.2e\n",
				fail_type, receive_stats.coarse_metric, pream_symb_loc,
				skip_h_count, time_sync_trials_max + 1,
				receive_stats.delay, mean_H, pream_energy, data_energy);
			fflush(stdout);
		}

		} // end if(energy_ok)

	}

	if(ldpc.print_nIteration==YES)
	{
		std::cout<<"decoded in "<< receive_stats.iterations_done<<" iterations."<<std::endl;
	}

	// Timing breakdown (printed periodically to avoid log flood)
	{
		auto timing_total_end = std::chrono::steady_clock::now();
		double timing_total_ms = std::chrono::duration<double, std::milli>(timing_total_end - timing_total_start).count();
		double frame_samples = (double)(data_container.Nofdm * (data_container.Nsymb + data_container.preamble_nSymb) * data_container.interpolation_rate);
		double frame_ms = (frame_samples / 48000.0) * 1000.0;
		static int timing_print_counter = 0;
		if(timing_print_counter++ % 20 == 0)
		{
			printf("[TIMING] total=%.1fms (%.0f%% of %.0fms frame) | pb_tsync=%.1f pb_data=%.1f ldpc=%.1f other=%.1f | buf_symb=%d frame_symb=%d iter=%d\n",
				timing_total_ms, frame_ms > 0 ? 100.0*timing_total_ms/frame_ms : 0, frame_ms,
				timing_pb_tsync_ms, timing_pb_data_ms, timing_ldpc_ms,
				timing_total_ms - timing_pb_tsync_ms - timing_pb_data_ms - timing_ldpc_ms,
				(int)data_container.buffer_Nsymb,
				data_container.preamble_nSymb + data_container.Nsymb,
				receive_stats.iterations_done);
			fflush(stdout);
		}
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

	int nsymb = ack_mfsk.ack_pattern_nsymb;
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

	// TX gain from calibration table
	double ack_boost = get_tx_gain(TX_SIG_ACK);
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
// Returns detection metric (0.0 = noise, up to ack_pattern_nsymb = perfect)
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
		ack_mfsk.ack_pattern_nsymb,
		ack_mfsk.ack_tones, ack_mfsk.ack_pattern_len,
		ack_mfsk.tone_hop_step, ack_mfsk.M,
		ack_mfsk.nStreams, ack_mfsk.stream_offsets,
		out_matched);

	return metric;
}

// TX: Generate BREAK pattern as passband audio (identical to ACK but with break_tones)
int cl_telecom_system::generate_break_pattern_passband(double* out)
{
	if(ack_pattern_passband_samples <= 0) return 0;

	int nsymb = ack_mfsk.ack_pattern_nsymb;
	float power_normalization = sqrt((double)(ofdm.Nfft * frequency_interpolation_rate));

	ack_mfsk.generate_break_pattern(data_container.ofdm_framed_data);

	for(int i = 0; i < nsymb; i++)
	{
		ofdm.symbol_mod(&data_container.ofdm_framed_data[i * data_container.Nc],
			&data_container.ofdm_symbol_modulated_data[i * data_container.Nofdm]);
	}

	// TX gain from calibration table
	double brk_boost = get_tx_gain(TX_SIG_BREAK);
	for(int j = 0; j < data_container.Nofdm * nsymb; j++)
	{
		data_container.ofdm_symbol_modulated_data[j] /= power_normalization;
		data_container.ofdm_symbol_modulated_data[j] *= sqrt(output_power_Watt) * brk_boost;
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
		ack_mfsk.ack_pattern_nsymb,
		ack_mfsk.break_tones, ack_mfsk.ack_pattern_len,
		ack_mfsk.tone_hop_step, ack_mfsk.M,
		ack_mfsk.nStreams, ack_mfsk.stream_offsets,
		out_matched);

	return metric;
}

// TX: Generate HAIL pattern as passband audio (prefix + optional directed suffix)
int cl_telecom_system::generate_hail_pattern_passband(double* out)
{
	if(ack_pattern_passband_samples <= 0) return 0;

	int nsymb = ack_mfsk.hail_detect_nsymb;
	int hail_samples = nsymb * data_container.Nofdm * frequency_interpolation_rate;
	float power_normalization = sqrt((double)(ofdm.Nfft * frequency_interpolation_rate));

	ack_mfsk.generate_hail_pattern(data_container.ofdm_framed_data);

	for(int i = 0; i < nsymb; i++)
	{
		ofdm.symbol_mod(&data_container.ofdm_framed_data[i * data_container.Nc],
			&data_container.ofdm_symbol_modulated_data[i * data_container.Nofdm]);
	}

	double hail_boost = get_tx_gain(TX_SIG_ACK);  // same gain as ACK
	for(int j = 0; j < data_container.Nofdm * nsymb; j++)
	{
		data_container.ofdm_symbol_modulated_data[j] /= power_normalization;
		data_container.ofdm_symbol_modulated_data[j] *= sqrt(output_power_Watt) * hail_boost;
	}

	double tx_carrier = carrier_frequency;
	ofdm.baseband_to_passband(data_container.ofdm_symbol_modulated_data,
		data_container.Nofdm * nsymb, out,
		sampling_frequency, tx_carrier, carrier_amplitude, frequency_interpolation_rate);

	ofdm.peak_clip(out, hail_samples, ofdm.data_papr_cut);

	return hail_samples;
}

// RX: Detect HAIL pattern in passband audio buffer (prefix + optional directed suffix)
double cl_telecom_system::detect_hail_pattern_from_passband(double* data, int size, int* out_matched)
{
	if(ack_pattern_passband_samples <= 0) return 0.0;

	ofdm.passband_to_baseband(data, size,
		data_container.baseband_data_interpolated,
		sampling_frequency, carrier_frequency, carrier_amplitude,
		1, &ofdm.FIR_rx_data);

	double metric = ofdm.detect_ack_pattern(
		data_container.baseband_data_interpolated, size,
		data_container.interpolation_rate,
		ack_mfsk.hail_detect_nsymb,
		ack_mfsk.hail_detect_tones, ack_mfsk.hail_detect_nsymb,
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

	int nsymb = ack_mfsk.ack_pattern_nsymb;
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

	// With carrier image recovery (direct + mirror bin energy), max metric ≈ ack_pattern_nsymb
	double max_clean_metric = (double)nsymb;

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
		ofdm.Nc = narrowband_enabled ? 10 : 50;
	}

	// Recompute bandwidth-dependent parameters from actual Nc
	// physical_config.cc computed these with hardcoded Nc=50; recompute for actual Nc.
	// IMPORTANT: Do NOT modify default_configurations_telecom_system here — it holds the
	// original physical config values and must remain pristine across NB/WB transitions.
	// Only update member variables; load_configuration() + init() will recompute each time.
	{
		double bw = 48000.0 * ofdm.Nc / ofdm.Nfft / frequency_interpolation_rate;
		double bw_original = 48000.0 * 50.0 / ofdm.Nfft / frequency_interpolation_rate;
		// Keep carrier_frequency at the WB center (~1472 Hz) regardless of bandwidth.
		// NB signal spans cf ± bw/2 (e.g. 1238-1706 Hz), centered near 1500 Hz.
		// Old formula: cf = offset + bw/2 + 300, which put NB at ~534 Hz (bottom of passband).
		double cf = default_configurations_telecom_system.carrier_frequency;

		// FIR transition bandwidth scaling for narrowband (Option A — split FIR).
		//
		// GI preservation constraint: filter_nTaps <= Ngi*interp = 64 samples.
		// nTaps = 2*fs/transition_BW, so transition_BW >= 1500 Hz for 64 taps.
		// Any filter in the TX→RX chain that processes the passband/baseband signal
		// with more taps than the GI window will destroy the GI-copy property,
		// making Schmidl-Cox preamble detection fail.
		//
		// TIME-SYNC RX FIR: Keep transition at 3000 Hz (33 taps). GI-safe.
		// DATA RX FIR: Narrow transition (600 Hz, 161 taps). Runs AFTER time sync
		//   determines the delay, so GI structure is irrelevant.
		// TX FIRs: Keep transition at 1000 Hz (97 taps, same as WB). The WB TX
		//   filter at 97 taps slightly exceeds the 64-sample GI, but the outermost
		//   Hamming-windowed taps contribute negligible energy. 481 taps (from
		//   scaling to 200 Hz) would be catastrophic — 7.5x the GI window.
		double bw_ratio = bw / bw_original;

		// Set member variables directly for correct NB/WB FIR design.
		// load_configuration() already copied defaults → members with WB values;
		// we override them here before FIR design() (called below in init).
		bandwidth = bw;
		carrier_frequency = cf;
		ofdm.FIR_rx_time_sync.lpf_filter_cut_frequency = 0.9 * bw / 2;
		// Transition must end before the -2fc conjugate image to avoid
		// carrier-phase-dependent matched-filter degradation (Bug #54).
		// Heterodyne produces image at 2*cf ± cutoff. Lowest image freq:
		//   2*cf - cutoff = 2*1500 - 0.9*bw/2.
		// Transition must fit between cutoff and that image frequency:
		//   max_transition = (2*cf - cutoff) - cutoff = 2*(cf - cutoff).
		// WB: 2*(1500-1055)=891 Hz, use 85% → ~757 Hz (~127 taps).
		//   Stopband starts at 1055+757=1812 Hz, image at 1945 Hz.
		//   133 Hz of stopband before image → ~20 dB rejection.
		//   50% (215 taps) gave identical results, not worth the CPU.
		// NB: 2*(1500-211)=2578 Hz, cap at 2000 → 49 taps (unchanged).
		{
			double cutoff_ts = 0.9 * bw / 2.0;
			double image_gap = 2.0 * (cf - cutoff_ts);
			ofdm.FIR_rx_time_sync.filter_transition_bandwidth = std::min(image_gap * 0.85, 2000.0);
		}
		ofdm.FIR_rx_data.lpf_filter_cut_frequency = 1.0 * bw / 2;
		ofdm.FIR_rx_data.filter_transition_bandwidth = 3000 * bw_ratio;  // narrow: 161 taps, tight
		ofdm.FIR_tx1.lpf_filter_cut_frequency = cf + bw / 2;
		ofdm.FIR_tx1.hpf_filter_cut_frequency = cf - bw / 2;
		ofdm.FIR_tx1.filter_transition_bandwidth = 1000;  // WB default: 97 taps, GI-tolerable
		ofdm.FIR_tx2.lpf_filter_cut_frequency = cf + bw / 2;
		ofdm.FIR_tx2.hpf_filter_cut_frequency = cf - bw / 2;
		ofdm.FIR_tx2.filter_transition_bandwidth = 1000;  // WB default: 97 taps, GI-tolerable
		if(g_verbose) {
			printf("[INIT-DIAG] Nc=%d bw=%.1f cf=%.1f FIR_data_cut=%.1f FIR_ts_cut=%.1f nb=%d\n",
				ofdm.Nc, bw, cf, ofdm.FIR_rx_data.lpf_filter_cut_frequency,
				ofdm.FIR_rx_time_sync.lpf_filter_cut_frequency, narrowband_enabled);
			fflush(stdout);
		}
	}

	// Nsymb scales with Nc: narrowband (Nc=10) needs 5× more symbols than wideband (Nc=50)
	int nc_scale = 50 / ofdm.Nc;

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
			if(M==MOD_BPSK){ofdm.Nsymb=48 * nc_scale;}
			if(M==MOD_QPSK){ofdm.Nsymb=24 * nc_scale;}
			if(M==MOD_8PSK){ofdm.Nsymb=16 * nc_scale;}
			if(M==MOD_16QAM){ofdm.Nsymb=12 * nc_scale;}
			if(M==MOD_32QAM){ofdm.Nsymb=9 * nc_scale;}
			if(M==MOD_64QAM){ofdm.Nsymb=8 * nc_scale;}
		}
		else if(ofdm.pilot_configurator.pilot_density==LOW_DENSITY)
		{
			if(M==MOD_BPSK){ofdm.Nsymb=40 * nc_scale;}
			if(M==MOD_QPSK){ofdm.Nsymb=20 * nc_scale;}
			if(M==MOD_8PSK){ofdm.Nsymb=16 * nc_scale;}
			if(M==MOD_16QAM){ofdm.Nsymb=10 * nc_scale;}
			if(M==MOD_32QAM){ofdm.Nsymb=9 * nc_scale;}
			if(M==MOD_64QAM){ofdm.Nsymb=8 * nc_scale;}
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
		pre_equalization_channel=CNEW(struct st_channel_complex, data_container.Nc, "ts.pre_eq_channel");
		get_pre_equalization_channel();
		reinit_subsystems.pre_equalization_channel=NO;
	}

	__srandom (bit_energy_dispersal_seed);
	bit_energy_dispersal_seed = default_configurations_telecom_system.bit_energy_dispersal_seed;
	for(int i=0;i<ldpc.N;i++)
	{
		data_container.bit_energy_dispersal_sequence[i]=__random()%2;
	}

	// Print active gain entry for this config (verbose only)
	if(g_verbose) {
		int nb = (narrowband_enabled == YES) ? 1 : 0;
		const char* mode = nb ? "NB" : "WB";
		if(M == MOD_MFSK)
		{
			tx_signal_type sig = (mfsk.nStreams == 1) ? TX_SIG_MFSK_1S : TX_SIG_MFSK_2S;
			printf("[TX-GAIN] %s MFSK %dS boost=%.4f\n", mode, mfsk.nStreams, tx_gain[sig][nb][nb]);
		}
		else
		{
			printf("[TX-GAIN] %s OFDM boost=%.4f\n", mode, tx_gain[TX_SIG_OFDM][nb][nb]);
		}
		printf("[TX-GAIN] %s ACK boost=%.4f  BREAK boost=%.4f\n",
			mode, tx_gain[TX_SIG_ACK][nb][nb], tx_gain[TX_SIG_BREAK][nb][nb]);
	}

	receive_stats.iterations_done=-1;
	receive_stats.delay=0;
	receive_stats.delay_of_last_decoded_message=-1;
	receive_stats.mfsk_search_raw=0;
	receive_stats.ofdm_search_raw=0;
	receive_stats.ofdm_batch_active=false;
	receive_stats.ofdm_drift_per_frame=0.0;
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
	// Check all canary guards before freeing — if any canary is corrupted,
	// the buffer it guards was overflowed during RX processing
	canary_check_all();
	canary_clear();

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
		CDELETE(pre_equalization_channel);
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

		int rwi = data_container.ring_write_index;
		memcpy(data_container.ready_to_process_passband_delayed_data, &data_container.passband_delayed_data[rwi], signal_period * sizeof(double));

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

		int rwi = data_container.ring_write_index;
		memcpy(data_container.ready_to_process_passband_delayed_data, &data_container.passband_delayed_data[rwi], signal_period * sizeof(double));

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

		int rwi = data_container.ring_write_index;
		memcpy(data_container.ready_to_process_passband_delayed_data, &data_container.passband_delayed_data[rwi], signal_period * sizeof(double));

		auto proc_start = std::chrono::steady_clock::now();
		st_receive_stats received_message_stats = receive_byte(data_container.ready_to_process_passband_delayed_data, out_data);
		auto proc_end = std::chrono::steady_clock::now();
		double proc_ms = std::chrono::duration<double, std::milli>(proc_end - proc_start).count();
		canary_check_all();

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
	// MFSK: sweep channel SNR from -25 to +5 dB in 1 dB steps
	// OFDM: sweep Es/N0 from -10 to +15 dB in 0.5 dB steps
	// MFSK: sweep channel SNR from -25 to +5 dB in 1 dB steps
	// OFDM: sweep Es/N0 from -10 to +15 dB in 0.5 dB steps
	int nPoints = (M == MOD_MFSK) ? 31 : 51;
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

	// NB mode: clamp OFDM configs to CONFIG_6 max (QPSK/QAM need more pilots than Nc=10 provides)
	if(narrowband_enabled == YES && is_ofdm_config(configuration) && configuration > NB_CONFIG_MAX)
	{
		printf("[PHY] NB mode: clamping config %d to NB max %d\n", configuration, NB_CONFIG_MAX);
		configuration = NB_CONFIG_MAX;
	}

	printf("[PHY] Loading configuration %d (was %d)\n", configuration, current_configuration);
	fflush(stdout);

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
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_11)
	{
		_modulation=MOD_8PSK;
		_ldpc_rate=8/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_12)
	{
		_modulation=MOD_QPSK;
		_ldpc_rate=14/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_13)
	{
		_modulation=MOD_8PSK;
		_ldpc_rate=12/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_14)
	{
		_modulation=MOD_8PSK;
		_ldpc_rate=14/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_15)
	{
		_modulation=MOD_16QAM;
		_ldpc_rate=14/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
	}
	else if(configuration==CONFIG_16)
	{
		_modulation=MOD_32QAM;
		_ldpc_rate=14/16.0;
		ofdm_preamble_configurator_Nsymb=4;
		ofdm_channel_estimator=LEAST_SQUARE;
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
		// Force full reinit when coming from CONFIG_NONE.
		// This covers first-time init (flags already YES from struct defaults)
		// and NB/WB switches (switch_narrowband_mode sets CONFIG_NONE, but
		// previous init left all flags NO — same config number + same modulation
		// wouldn't trigger any of the checks below, leaving Nc/bandwidth stale).
		reinit_subsystems = st_reinit_subsystems();
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
		if(configuration == ROBUST_0) { new_mfsk_M = narrowband_enabled ? 8 : 32; new_nStreams = 1; }
		else { new_mfsk_M = narrowband_enabled ? 4 : 16; new_nStreams = 2; } // ROBUST_1, ROBUST_2
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
	bool capture_mutex_held = false;  // Bug #42: track if we're holding the mutex

	if(reinit_subsystems.telecom_system==YES)
	{
		// Bug #42: Hold capture_prep_mutex across the entire deinit→init cycle.
		// The audio capture_prep thread reads Nofdm/buffer_Nsymb outside the
		// mutex, then re-checks inside. Without holding the mutex for the full
		// cycle, CPU store reordering or compiler optimizations can let the
		// capture thread see partially-initialized state (new Nofdm set by
		// set_size() but buffers not yet allocated), leading to heap corruption.
		// The mutex is held through deinit, parameter updates, and init, so the
		// capture thread always sees either the old consistent state or the new
		// consistent state — never an intermediate mix.
		if(reinit_subsystems.data_container==YES)
		{
			printf("[PHY-SWITCH] Taking capture_prep_mutex, zeroing Nofdm/buffer_Nsymb\n");
			fflush(stdout);
			MUTEX_LOCK(&capture_prep_mutex);
			capture_mutex_held = true;
			data_container.Nofdm = 0;
			data_container.buffer_Nsymb = 0;
			data_container.data_ready = 0;
		}

		printf("[PHY-SWITCH] deinit() start\n");
		fflush(stdout);
		this->deinit();
		printf("[PHY-SWITCH] deinit() done\n");
		fflush(stdout);
	}

	M=_modulation;
	ldpc.rate=_ldpc_rate;
	ofdm.preamble_configurator.Nsymb=ofdm_preamble_configurator_Nsymb;
	// NB MFSK: 8-symbol preamble for cross-correlation detection
	if(narrowband_enabled && M == MOD_MFSK)
		ofdm.preamble_configurator.Nsymb = 8;
	// NB estimator: blanket ZF for all NB configs.
	// ZF (per-pilot H=Y/P) is immune to inter-symbol phase jitter that makes
	// LS cross-pilot averaging destructive on VB-Cable/HF. With Nc=10 and only
	// 3-4 pilots per row, LS averaging can't reduce noise without destroying
	// phase coherence. Tested: LS gives mean_H=0.45 on VB-Cable CONFIG_15
	// while ZF gives mean_H=1.0. CPE_correction pre-estimator handles residual
	// freq offset. CONFIG_14+ NB is a design limitation of sparse pilot density.
	if(narrowband_enabled && ofdm_channel_estimator == LEAST_SQUARE)
	{
		ofdm_channel_estimator = ZERO_FORCE;
	}
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

	// nIdentical_sections derives from subcarrier spacing in configure().
	// WB (Nc>=50): every-4th → 4 identical sections (period Nfft/4)
	// NB (Nc<=10): every-2nd → 2 identical sections (period Nfft/2)
	// Must compute dynamically here — the old default (physical_config.cc:51 = 2)
	// overwrote the value set by configure() in init(), breaking Stage 4a.
	ofdm.preamble_configurator.nIdentical_sections = (ofdm.Nc >= 50) ? 4 : 2;
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

	// NB LS window: with CPE correction (pre-LS residual freq offset removal),
	// the full default window height can be used for NB too — CPE removes the
	// phase rotation that previously caused H cancellation in the LS window.

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
			if(current_configuration == ROBUST_0) {
				mfsk_M = narrowband_enabled ? 8 : 32;
				mfsk_nStreams = 1;
			} else {
				mfsk_M = narrowband_enabled ? 4 : 16;
				mfsk_nStreams = 2;
			}
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
		printf("[PHY-SWITCH] init() start (nb=%d M=%d config=%d)\n",
			narrowband_enabled, M, current_configuration);
		fflush(stdout);
		this->init();
		printf("[PHY-SWITCH] init() done (Nc=%d Nsymb=%d Nofdm=%d buffer_Nsymb=%d)\n",
			ofdm.Nc, ofdm.Nsymb, (int)data_container.Nofdm, (int)data_container.buffer_Nsymb);
		fflush(stdout);

		// Bug #42: Release the mutex after init() has fully set up the new
		// data_container buffers and parameters. The capture_prep thread will
		// now see a fully consistent new state.
		if(capture_mutex_held)
		{
			printf("[PHY-SWITCH] Releasing capture_prep_mutex\n");
			fflush(stdout);
			MUTEX_UNLOCK(&capture_prep_mutex);
			capture_mutex_held = false;
		}

		reinit_subsystems.telecom_system=NO;

		// Note: bandwidth-dependent parameters (bandwidth, carrier_frequency, FIR cutoffs)
		// are set directly by init() in the NB/WB recomputation block above.
		// No re-apply from default_configurations_telecom_system needed.
	}

	// Re-init MFSK now that ofdm.Nc is finalized (was AUTO_SELLECT during mfsk.init above)
	// This recalculates stream_offsets with the correct Nc value
	if(M == MOD_MFSK)
	{
		int mfsk_M, mfsk_nStreams;
		if(current_configuration == ROBUST_0) {
			mfsk_M = narrowband_enabled ? 8 : 32;
			mfsk_nStreams = 1;
		} else {
			mfsk_M = narrowband_enabled ? 4 : 16;
			mfsk_nStreams = 2;
		}
		mfsk.init(mfsk_M, ofdm.Nc, mfsk_nStreams);
	}

	// Generate MFSK cross-correlation template for preamble detection (NB+WB).
	// Round-trip through passband signal chain so template matches RX exactly:
	// symbol_mod → baseband_to_passband → passband_to_baseband(FIR) → decimate
	if(M == MOD_MFSK)
	{
		if(ofdm.mfsk_corr_template != NULL) { delete[] ofdm.mfsk_corr_template; ofdm.mfsk_corr_template = NULL; }

		// Generate preamble in frequency domain
		mfsk.generate_preamble(data_container.preamble_data, data_container.preamble_nSymb);

		// Modulate to baseband: zero_pad → IFFT → GI add (Nofdm samples per symbol)
		int template_nsymb = data_container.preamble_nSymb;
		int Nofdm = data_container.Nofdm;
		int bb_len = template_nsymb * Nofdm;
		std::complex<double>* bb_template = new std::complex<double>[bb_len];
		for(int i = 0; i < template_nsymb; i++)
		{
			ofdm.symbol_mod(&data_container.preamble_data[i * data_container.Nc],
			                &bb_template[i * Nofdm]);
		}

		// Round-trip: baseband → passband → FIR-filtered baseband
		// This ensures the template has the same spectral shaping as the RX signal
		int interp = frequency_interpolation_rate;
		int pb_len = bb_len * interp;
		double* pb_data = new double[pb_len];

		long unsigned saved_pss = ofdm.passband_start_sample;
		ofdm.passband_start_sample = 0;
		ofdm.baseband_to_passband(bb_template, bb_len, pb_data,
			sampling_frequency, carrier_frequency, carrier_amplitude, interp);
		ofdm.passband_start_sample = saved_pss;

		// Demodulate with FIR_rx_time_sync (same filter used in receive_byte)
		std::complex<double>* filtered = new std::complex<double>[pb_len];
		ofdm.passband_to_baseband(pb_data, pb_len, filtered,
			sampling_frequency, carrier_frequency, carrier_amplitude, 1, &ofdm.FIR_rx_time_sync);

		// Store decimated (baseband-rate) template — correlation steps by interp_rate
		ofdm.mfsk_corr_template_len = bb_len;
		ofdm.mfsk_corr_template_nsymb = template_nsymb;
		ofdm.mfsk_corr_template = CNEW(std::complex<double>, bb_len, "ofdm.mfsk_corr_template");
		for(int i = 0; i < bb_len; i++)
			ofdm.mfsk_corr_template[i] = filtered[i * interp];

		// Precompute total and per-symbol template energies for normalization
		ofdm.mfsk_corr_template_energy = 0.0;
		for(int k = 0; k < template_nsymb && k < 8; k++)
		{
			double sym_energy = 0.0;
			for(int n = 0; n < Nofdm; n++)
			{
				int idx = k * Nofdm + n;
				sym_energy +=
					ofdm.mfsk_corr_template[idx].real() * ofdm.mfsk_corr_template[idx].real() +
					ofdm.mfsk_corr_template[idx].imag() * ofdm.mfsk_corr_template[idx].imag();
			}
			ofdm.mfsk_corr_template_sym_energy[k] = sym_energy;
			ofdm.mfsk_corr_template_energy += sym_energy;
		}

		delete[] pb_data;
		delete[] filtered;
		delete[] bb_template;

		printf("[PHY] MFSK corr template: %d symbols, %d samples, energy=%.3f (per-sym corr, FIR round-tripped)\n",
			template_nsymb, bb_len, ofdm.mfsk_corr_template_energy);
		fflush(stdout);
	}
	else
	{
		if(ofdm.mfsk_corr_template != NULL) { delete[] ofdm.mfsk_corr_template; ofdm.mfsk_corr_template = NULL; }
		ofdm.mfsk_corr_template_len = 0;
		ofdm.mfsk_corr_template_energy = 0.0;
		ofdm.mfsk_corr_template_nsymb = 0;

#if 0 // Template generation disabled: using Schmidl-Cox autocorrelation
		// Generate OFDM matched-filter template for preamble detection.
		// Must replicate the full TX→RX chain so the template matches what
		// receive_byte actually sees:
		//   preamble × pre_eq → symbol_mod → boost → b2p → FIR_tx1 → FIR_tx2 → p2b(FIR_rx_time_sync)
		// Pre-equalization applies per-subcarrier complex rotations that completely
		// reshape the time-domain waveform. Without it, the template has ~0.02
		// correlation with the received signal (essentially random).
		if(ofdm.ofdm_corr_template != NULL) { delete[] ofdm.ofdm_corr_template; ofdm.ofdm_corr_template = NULL; }

		int template_nsymb = data_container.preamble_nSymb;
		int Nofdm = data_container.Nofdm;
		int bb_len = template_nsymb * Nofdm;
		std::complex<double>* bb_template = new std::complex<double>[bb_len];

		// Extract preamble subcarrier values WITH pre-equalization (same as transmit_byte).
		// pre_equalization_channel is computed earlier in load_configuration (line ~2396).
		std::complex<double> preamble_sc[256];
		for(int i = 0; i < template_nsymb; i++)
		{
			for(int k = 0; k < ofdm.Nc; k++)
				preamble_sc[k] = ofdm.ofdm_preamble[i * ofdm.Nc + k].value
					* pre_equalization_channel[k].value;
			ofdm.symbol_mod(preamble_sc, &bb_template[i * Nofdm]);
		}

		// === DIAG: pre_eq at template generation (remove after debug) ===
		printf("[TMPL-PREEQ] CONFIG_%d preamble_nSymb=%d pre_eq[0..4]=(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f)(%.4f,%.4f)\n",
			current_configuration, template_nsymb,
			pre_equalization_channel[0].value.real(), pre_equalization_channel[0].value.imag(),
			pre_equalization_channel[1].value.real(), pre_equalization_channel[1].value.imag(),
			pre_equalization_channel[2].value.real(), pre_equalization_channel[2].value.imag(),
			pre_equalization_channel[3].value.real(), pre_equalization_channel[3].value.imag(),
			pre_equalization_channel[4].value.real(), pre_equalization_channel[4].value.imag());
		fflush(stdout);

		// Apply power normalization + output power + preamble boost (same as transmit_bit lines 601-602).
		// sqrt(output_power_Watt) MUST be included so peak_clip applies at the same
		// absolute threshold as the TX path. CS is amplitude-invariant, so the
		// extra sqrt(output_power_Watt) factor doesn't affect the final metric,
		// but peak_clip is a nonlinear operation that depends on absolute amplitude.
		// Without this scaling, the template is clipped at a different PAPR level
		// than the TX signal → waveform mismatch → CS metric ~0.15 instead of ~1.0.
		double power_normalization = sqrt((double)(ofdm.Nfft * frequency_interpolation_rate));
		double preamble_boost = ofdm.preamble_configurator.boost;
		double ofdm_tx_gain = get_tx_gain(TX_SIG_OFDM);
		for(int i = 0; i < bb_len; i++)
			bb_template[i] = bb_template[i] / power_normalization * sqrt(output_power_Watt) * preamble_boost * ofdm_tx_gain;

		// Round-trip through full TX→RX passband chain:
		// baseband_to_passband → peak_clip → FIR_tx1 → FIR_tx2 → passband_to_baseband(FIR_rx_time_sync)
		int interp = frequency_interpolation_rate;
		int pb_len = bb_len * interp;
		double* pb_data = new double[pb_len];
		double* pb_fir1 = new double[pb_len];
		double* pb_fir2 = new double[pb_len];

		long unsigned saved_pss = ofdm.passband_start_sample;
		ofdm.passband_start_sample = 0;
		ofdm.baseband_to_passband(bb_template, bb_len, pb_data,
			sampling_frequency, carrier_frequency, carrier_amplitude, interp);
		ofdm.passband_start_sample = saved_pss;

		// Apply PAPR clipping to match TX path (transmit_bit line 616).
		// pre_equalization_channel boosts high-frequency preamble subcarriers
		// (above FIR_rx_data cutoff) to very large amplitudes. Without matching
		// peak_clip in the template, the clipped TX waveform diverges from the
		// unclipped template → CS metric drops from ~1.0 to ~0.15.
		ofdm.peak_clip(pb_data, pb_len, ofdm.preamble_papr_cut);

		// TX shaping filters (same as transmit_byte SINGLE_MESSAGE path)
		ofdm.FIR_tx1.apply(pb_data, pb_fir1, pb_len);
		ofdm.FIR_tx2.apply(pb_fir1, pb_fir2, pb_len);

		// Demodulate with FIR_rx_time_sync (same filter used in receive_byte line 767)
		std::complex<double>* filtered = new std::complex<double>[pb_len];
		ofdm.passband_to_baseband(pb_fir2, pb_len, filtered,
			sampling_frequency, carrier_frequency, carrier_amplitude, 1, &ofdm.FIR_rx_time_sync);

		// Store decimated (baseband-rate) template
		ofdm.ofdm_corr_template_len = bb_len;
		ofdm.ofdm_corr_template_nsymb = template_nsymb;
		ofdm.ofdm_corr_template = CNEW(std::complex<double>, bb_len, "ofdm.ofdm_corr_template");
		for(int i = 0; i < bb_len; i++)
			ofdm.ofdm_corr_template[i] = filtered[i * interp];

		// Precompute per-symbol and total template energies
		ofdm.ofdm_corr_template_energy = 0.0;
		for(int k = 0; k < template_nsymb && k < 16; k++)
		{
			double sym_energy = 0.0;
			for(int n = 0; n < Nofdm; n++)
			{
				int idx = k * Nofdm + n;
				sym_energy +=
					ofdm.ofdm_corr_template[idx].real() * ofdm.ofdm_corr_template[idx].real() +
					ofdm.ofdm_corr_template[idx].imag() * ofdm.ofdm_corr_template[idx].imag();
			}
			ofdm.ofdm_corr_template_sym_energy[k] = sym_energy;
			ofdm.ofdm_corr_template_energy += sym_energy;
		}

		delete[] pb_data;
		delete[] pb_fir1;
		delete[] pb_fir2;
		delete[] filtered;
		delete[] bb_template;

		printf("[PHY] OFDM corr template: %d symbols, %d samples, energy=%.3f (matched filter, FIR round-tripped)\n",
			template_nsymb, bb_len, ofdm.ofdm_corr_template_energy);
		printf("[TMPL-INIT] t[0]=(%.6f,%.6f) t[1]=(%.6f,%.6f) t[2]=(%.6f,%.6f)\n",
			ofdm.ofdm_corr_template[0].real(), ofdm.ofdm_corr_template[0].imag(),
			ofdm.ofdm_corr_template[1].real(), ofdm.ofdm_corr_template[1].imag(),
			ofdm.ofdm_corr_template[2].real(), ofdm.ofdm_corr_template[2].imag());
		printf("[TMPL-INIT] FIR_ts: nTaps=%d cut=%.1f trans=%.1f\n",
			ofdm.FIR_rx_time_sync.filter_nTaps,
			ofdm.FIR_rx_time_sync.lpf_filter_cut_frequency,
			ofdm.FIR_rx_time_sync.filter_transition_bandwidth);
		printf("[TMPL-INIT] carrier_freq=%.1f output_power=%.3f pre_eq[0]=(%.4f,%.4f) pre_eq[1]=(%.4f,%.4f)\n",
			carrier_frequency, output_power_Watt,
			pre_equalization_channel[0].value.real(), pre_equalization_channel[0].value.imag(),
			pre_equalization_channel[1].value.real(), pre_equalization_channel[1].value.imag());
		fflush(stdout);
	}
#endif
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
	receive_stats.ofdm_search_raw = 0;
	receive_stats.ofdm_batch_active = false;
	receive_stats.ofdm_drift_per_frame = 0.0;

	printf("[PHY] Config %d active: M=%.0f LDPC_rate=%.3f BW=%.0fHz Nc=%d Nsymb=%d nBits=%d\n",
		current_configuration, M, ldpc.rate, bandwidth,
		data_container.Nc, data_container.Nsymb, data_container.nBits);
	if(M == MOD_MFSK)
	{
		const double _max_Nc = 50.0;
		double _mfsk_boost = _max_Nc * pow(10.0, -2.0 / 20.0) / sqrt((double)data_container.Nc * mfsk.nStreams);
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

	// Universal ACK pattern: dedicated ack_mfsk with fixed M, nStreams=1 for ALL modes.
	// Config-independent: both sides always agree on ACK tone parameters,
	// so no config switching needed for ACK pattern TX/RX.
	{
		int ack_M = narrowband_enabled ? 8 : 16;  // M=8 fits in Nc=10, M=16 fits in Nc=50
		ack_mfsk.init(ack_M, data_container.Nc, 1);
		// NB: Sidelnikov sequences have intrinsic frequency diversity — disable hopping
		// so transmitted tones match the pre-computed sequence exactly.
		if (narrowband_enabled)
			ack_mfsk.tone_hop_step = 0;
	}

	ack_pattern_passband_samples = ack_mfsk.ack_pattern_nsymb * data_container.Nofdm * frequency_interpolation_rate;

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

	printf("[PHY] ACK pattern: %d symbols (M=%d, %s), %d passband samples (%.0f ms), "
		"match_threshold=%d/%d, detection_threshold=%.2f\n",
		ack_mfsk.ack_pattern_nsymb, ack_mfsk.M,
		narrowband_enabled ? "NB Sidelnikov" : "WB Welch-Costas",
		ack_pattern_passband_samples,
		1000.0 * ack_pattern_passband_samples / sampling_frequency,
		ack_mfsk.ack_match_threshold, ack_mfsk.ack_pattern_nsymb,
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

// SNR-to-config mapping for supershift (BER waterfall + 2 dB margin).
// Callers apply SUPERSHIFT_MARGIN_DB (3 dB) on top, so effective margin = 5 dB.
// BER waterfalls (100 frames, passband, EsN0):
//   C0:-14 C1:-11 C2:-10 C3:-9 C4:-8 C5:-7 C6:-6 C7:-5
//   C8:-4  C9:-2  C10:-1 C11:+1 C12:+2 C13:+4 C14:+7 C15:+9 C16:+12
char cl_telecom_system::get_configuration(double SNR)
{
	char configuration;

	if(SNR>11)
		configuration=CONFIG_15;
	else if(SNR>9)
		configuration=CONFIG_14;
	else if(SNR>6)
		configuration=CONFIG_13;
	else if(SNR>4)
		configuration=CONFIG_12;
	else if(SNR>3)
		configuration=CONFIG_11;
	else if(SNR>1)
		configuration=CONFIG_10;
	else if(SNR>0)
		configuration=CONFIG_9;
	else if(SNR>-2)
		configuration=CONFIG_8;
	else if(SNR>-3)
		configuration=CONFIG_7;
	else if(SNR>-4)
		configuration=CONFIG_6;
	else if(SNR>-5)
		configuration=CONFIG_5;
	else if(SNR>-6)
		configuration=CONFIG_4;
	else if(SNR>-7)
		configuration=CONFIG_3;
	else if(SNR>-8)
		configuration=CONFIG_2;
	else if(SNR>-9)
		configuration=CONFIG_1;
	else
		configuration=CONFIG_0;

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
