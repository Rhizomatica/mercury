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

#include <iostream>
#include <complex>
#include "telecom_system.h"
#include <fstream>
#include <math.h>
#include <iostream>
#include <complex>
#include <iomanip>



int main(int argc, char *argv[])
{
	srand(time(0));

	cl_telecom_system telecom_system;

	telecom_system.operation_mode=RX_TEST;

	telecom_system.M=MOD_64QAM;
	telecom_system.psk.set_predefined_constellation(telecom_system.M);
	telecom_system.awgn_channel.set_seed(rand());
	telecom_system.test_tx_AWGN_EsN0_calibration=1;
	telecom_system.test_tx_AWGN_EsN0=50;

	telecom_system.ofdm.Nc=AUTO_SELLECT;
	telecom_system.ofdm.Nfft=512;
	telecom_system.ofdm.gi=1.0/16.0;
	telecom_system.ofdm.Nsymb=AUTO_SELLECT;
	telecom_system.ofdm.pilot_configurator.Dx=AUTO_SELLECT;
	telecom_system.ofdm.pilot_configurator.Dy=AUTO_SELLECT;
	telecom_system.ofdm.pilot_configurator.first_row=DATA;
	telecom_system.ofdm.pilot_configurator.last_row=DATA;
	telecom_system.ofdm.pilot_configurator.first_col=DATA;
	telecom_system.ofdm.pilot_configurator.second_col=DATA;
	telecom_system.ofdm.pilot_configurator.last_col=AUTO_SELLECT;
	telecom_system.ofdm.pilot_configurator.pilot_boost=1.33;
	telecom_system.ofdm.pilot_configurator.first_row_zeros=YES;
	telecom_system.ofdm.print_time_sync_status=NO;
	telecom_system.ofdm.freq_offset_ignore_limit=0.1;

	telecom_system.ldpc.standard=HERMES;
	telecom_system.ldpc.framesize=HERMES_NORMAL;
	telecom_system.ldpc.rate=14/16.0;
	telecom_system.ldpc.decoding_algorithm=GBF;
	telecom_system.ldpc.GBF_eta=0.5;
	telecom_system.ldpc.nIteration_max=100;
	telecom_system.ldpc.print_nIteration=NO;

	telecom_system.bandwidth=2300;
	telecom_system.time_sync_trials_max=1;
	telecom_system.lock_time_sync=YES;
	telecom_system.frequency_interpolation_rate=1;
	telecom_system.carrier_frequency=1450;
	telecom_system.output_power_Watt=1.6;
	telecom_system.filter_window=HANNING;
	telecom_system.filter_transition_bandwidth=1000;
	telecom_system.filter_cut_frequency=6000;

	telecom_system.init();

	telecom_system.bit_interleaver_block_size=telecom_system.data_container.nBits/10;
	telecom_system.ofdm.time_sync_Nsymb=telecom_system.ofdm.Nsymb;

	telecom_system.tcp_socket.port=6002;
	telecom_system.tcp_socket.timeout_ms=10000;
	telecom_system.plot.folder="/mnt/ramDisk/";
	telecom_system.plot.plot_active=NO;




	if(telecom_system.operation_mode==RX_TEST || telecom_system.operation_mode==RX_TCP)
	{
		telecom_system.data_container.sound_device_ptr=(void*)&telecom_system.microphone;
		telecom_system.microphone.type=CAPTURE;
		telecom_system.microphone.dev_name="plughw:1,0";
		telecom_system.microphone.baudrate=telecom_system.sampling_frequency;
		telecom_system.microphone.nbuffer_Samples=2 * telecom_system.ofdm.Nfft*(1+telecom_system.ofdm.gi)*telecom_system.frequency_interpolation_rate*telecom_system.ofdm.Nsymb;
		telecom_system.microphone.channels=LEFT;
		telecom_system.microphone.frames_per_period=telecom_system.ofdm.Nfft*(1+telecom_system.ofdm.gi)*telecom_system.frequency_interpolation_rate;
		telecom_system.microphone.data_container_ptr=&telecom_system.data_container;
		telecom_system.microphone.init();

		if(telecom_system.operation_mode==RX_TEST)
		{
			telecom_system.plot.open("PLOT");
			telecom_system.plot.reset("PLOT");

			std::complex <double> data_fft[telecom_system.ofdm.pilot_configurator.nData];

			int tmp[N_MAX];
			int counter=0;
			int constellation_plot_counter=0;
			int constellation_plot_nFrames=1;
			float contellation[telecom_system.ofdm.pilot_configurator.nData*constellation_plot_nFrames][2]={0};
			std::cout << std::fixed;
			std::cout << std::setprecision(1);
			while(1)
			{
				if(telecom_system.data_container.data_ready==1)
				{
					telecom_system.data_container.data_ready=0;
					counter++;

					if(telecom_system.data_container.frames_to_read==0)
					{
						st_receive_stats test=telecom_system.receive((const double*)telecom_system.data_container.passband_delayed_data,tmp);
						shift_left(telecom_system.data_container.passband_delayed_data, telecom_system.data_container.Nofdm*telecom_system.data_container.interpolation_rate*telecom_system.data_container.buffer_Nsymb, telecom_system.data_container.Nofdm*telecom_system.data_container.interpolation_rate);

						for(int i=0;i<telecom_system.ofdm.pilot_configurator.nData;i++)
						{
							data_fft[i]=telecom_system.data_container.ofdm_deframed_data[i];
						}
						if (test.message_decoded==YES)
						{
							telecom_system.data_container.frames_to_read=telecom_system.data_container.Nsymb;
							std::cout<<"decoded in "<<test.iterations_done<<" data=";
							for(int i=0;i<10;i++)
							{
								std::cout<<tmp[i]<<",";
							}
							std::cout<<"delay="<<test.delay;
							std::cout<<" 1st symb loc="<<test.first_symbol_location;
							std::cout<<" freq_offset="<<telecom_system.receive_stats.freq_offset;
							std::cout<<" SNR="<<telecom_system.receive_stats.SNR<<" dB";
							std::cout<<" Signal Strength="<<telecom_system.receive_stats.signal_stregth_dbm<<" dBm ";
							std::cout<<std::endl;
						}
						else
						{
							std::cout<<"Syncing.. ";
							std::cout<<"delay="<<test.delay;
							std::cout<<" 1st symb loc="<<test.first_symbol_location;
							std::cout<<" freq_offset="<<telecom_system.receive_stats.freq_offset;
							std::cout<<" Signal Strength="<<telecom_system.receive_stats.signal_stregth_dbm<<" dBm ";
							std::cout<<std::endl;
						}

						for(int i=0;i<telecom_system.ofdm.pilot_configurator.nData;i++)
						{
							contellation[constellation_plot_counter*telecom_system.ofdm.pilot_configurator.nData+i][0]=data_fft[i].real();
							contellation[constellation_plot_counter*telecom_system.ofdm.pilot_configurator.nData+i][1]=data_fft[i].imag();
						}

						constellation_plot_counter++;

						if(constellation_plot_counter==constellation_plot_nFrames)
						{
							constellation_plot_counter=0;
							telecom_system.plot.plot_constellation(&contellation[0][0],telecom_system.ofdm.pilot_configurator.nData*constellation_plot_nFrames);
						}

						counter=0;
					}

				}
				else
				{
					usleep(50000);
				}
			}
			telecom_system.plot.close();
		}
		else if(telecom_system.operation_mode==RX_TCP)
		{
			int received_message[N_MAX];
			std::cout << std::fixed;
			std::cout << std::setprecision(1);
			long timeout_counter_ms=telecom_system.tcp_socket.timeout_ms;
			st_receive_stats received_message_stats;
			if(telecom_system.tcp_socket.init()==SUCCESS)
			{
				while(1)
				{
					if(telecom_system.data_container.data_ready==1)
					{
						telecom_system.data_container.data_ready=0;

						if(telecom_system.data_container.frames_to_read==0)
						{
							received_message_stats=telecom_system.receive((const double*)telecom_system.data_container.passband_delayed_data,received_message);
							shift_left(telecom_system.data_container.passband_delayed_data, telecom_system.data_container.Nofdm*telecom_system.data_container.interpolation_rate*telecom_system.data_container.buffer_Nsymb, telecom_system.data_container.Nofdm*telecom_system.data_container.interpolation_rate);


							if (received_message_stats.message_decoded==YES)
							{
								telecom_system.data_container.frames_to_read=telecom_system.data_container.Nsymb;

								if (telecom_system.tcp_socket.get_status()==TCP_STATUS_ACCEPTED)
								{

									for(int i=0;i<telecom_system.data_container.nBits;i++)
									{
										telecom_system.tcp_socket.message->buffer[i]=received_message[i];
									}
									telecom_system.tcp_socket.message->length=telecom_system.data_container.nBits;
									telecom_system.tcp_socket.transmit();
								}

								std::cout<<"decoded in "<<received_message_stats.iterations_done<<" data=";
								for(int i=0;i<10;i++)
								{
									std::cout<<received_message[i]<<",";
								}
								std::cout<<"delay="<<received_message_stats.delay;
								std::cout<<" 1st symb loc="<<received_message_stats.first_symbol_location;
								std::cout<<" freq_offset="<<telecom_system.receive_stats.freq_offset;
								std::cout<<" SNR="<<telecom_system.receive_stats.SNR<<" dB";
								std::cout<<" Signal Strength="<<telecom_system.receive_stats.signal_stregth_dbm<<" dBm ";

								std::cout<<std::endl;
							}
						}
					}
					if (telecom_system.tcp_socket.get_status()==TCP_STATUS_ACCEPTED)
					{
						if(telecom_system.tcp_socket.receive()>0)
						{
							timeout_counter_ms=telecom_system.tcp_socket.timeout_ms;
						}
						else
						{
							timeout_counter_ms--;
							usleep(1000);
							if(timeout_counter_ms<1)
							{
								telecom_system.tcp_socket.check_incomming_connection();
								if (telecom_system.tcp_socket.get_status()==TCP_STATUS_ACCEPTED)
								{
									timeout_counter_ms=telecom_system.tcp_socket.timeout_ms;
								}
								else
								{
									usleep(10000);
								}
							}
						}

					}
					else
					{
						telecom_system.tcp_socket.check_incomming_connection();
						if (telecom_system.tcp_socket.get_status()==TCP_STATUS_ACCEPTED)
						{
							timeout_counter_ms=telecom_system.tcp_socket.timeout_ms;
						}
						else
						{
							usleep(10000);
						}
					}
				}
			}
		}
	}
	else if (telecom_system.operation_mode==TX_TEST || telecom_system.operation_mode==TX_TCP)
	{
		telecom_system.speaker.type=PLAY;
		telecom_system.speaker.dev_name="plughw:1,0";
		telecom_system.speaker.baudrate=telecom_system.sampling_frequency;
		telecom_system.speaker.nbuffer_Samples=2 * telecom_system.ofdm.Nfft*(1+telecom_system.ofdm.gi)*telecom_system.frequency_interpolation_rate*telecom_system.ofdm.Nsymb;
		telecom_system.speaker.frames_per_period=telecom_system.ofdm.Nfft*(1+telecom_system.ofdm.gi)*telecom_system.frequency_interpolation_rate;
		telecom_system.speaker.channels=MONO;
		telecom_system.speaker.frames_to_leave_transmit_fct=800;
		telecom_system.speaker.init();
		float sigma=1.0/sqrt(pow(10,((telecom_system.test_tx_AWGN_EsN0+telecom_system.test_tx_AWGN_EsN0_calibration)/10)));

		if (telecom_system.operation_mode==TX_TEST)
		{
			while(1)
			{
				for(int i=0;i<telecom_system.data_container.nBits-telecom_system.ldpc.P;i++)
				{
					telecom_system.data_container.data[i]=rand()%2;
				}
				telecom_system.transmit(telecom_system.data_container.data,telecom_system.data_container.passband_data);
				telecom_system.awgn_channel.apply_with_delay(telecom_system.data_container.passband_data,telecom_system.data_container.passband_data,sigma,telecom_system.data_container.Nofdm*telecom_system.data_container.Nsymb*telecom_system.data_container.interpolation_rate,0);
				telecom_system.speaker.transfere(telecom_system.data_container.passband_data,telecom_system.data_container.Nofdm*telecom_system.data_container.interpolation_rate*telecom_system.ofdm.Nsymb);
			}
		}
		else if (telecom_system.operation_mode==TX_TCP)
		{
			long int timeout_counter_ms=telecom_system.tcp_socket.timeout_ms;
			if(telecom_system.tcp_socket.init()==SUCCESS)
			{
				while(1)
				{
					if (telecom_system.tcp_socket.get_status()==TCP_STATUS_ACCEPTED)
					{
						if(telecom_system.tcp_socket.receive()>0)
						{
							timeout_counter_ms=telecom_system.tcp_socket.timeout_ms;
							for(int i=0;i<telecom_system.tcp_socket.message->length;i++)
							{
								telecom_system.data_container.data[i]=telecom_system.tcp_socket.message->buffer[i];
							}

							for(int i=telecom_system.tcp_socket.message->length;i<telecom_system.data_container.nBits-telecom_system.ldpc.P;i++)
							{
								telecom_system.data_container.data[i]=rand()%2;
							}

							telecom_system.transmit(telecom_system.data_container.data,telecom_system.data_container.passband_data);
							telecom_system.speaker.transfere(telecom_system.data_container.passband_data,telecom_system.data_container.Nofdm*telecom_system.data_container.interpolation_rate*telecom_system.ofdm.Nsymb);// SYNC TODO:Remove
							telecom_system.speaker.transfere(telecom_system.data_container.passband_data,telecom_system.data_container.Nofdm*telecom_system.data_container.interpolation_rate*telecom_system.ofdm.Nsymb);


							//prepare the reply
							telecom_system.tcp_socket.message->buffer[0]='O';
							telecom_system.tcp_socket.message->buffer[1]='K';
							telecom_system.tcp_socket.message->length=2;
							telecom_system.tcp_socket.transmit();
						}
						else
						{
							timeout_counter_ms--;
							usleep(1000);
							if(timeout_counter_ms<1)
							{
								telecom_system.tcp_socket.check_incomming_connection();
								if (telecom_system.tcp_socket.get_status()==TCP_STATUS_ACCEPTED)
								{
									timeout_counter_ms=telecom_system.tcp_socket.timeout_ms;
								}
								else
								{
									usleep(10000);
								}
							}
						}

					}
					else
					{
						telecom_system.tcp_socket.check_incomming_connection();
						if (telecom_system.tcp_socket.get_status()==TCP_STATUS_ACCEPTED)
						{
							timeout_counter_ms=telecom_system.tcp_socket.timeout_ms;
						}
						else
						{
							usleep(10000);
						}
					}
				}
			}
		}
	}
	else if (telecom_system.operation_mode==BER_PLOT_baseband || telecom_system.operation_mode==BER_PLOT_passband)
	{

		telecom_system.plot.open("BER");
		telecom_system.	plot.reset("BER");
		int nPoints=25;
		float data_plot[nPoints][2];
		float data_plot_theo[nPoints][2];
		telecom_system.output_power_Watt=1;

		for(int ind=0;ind<nPoints;ind++)
		{
			float EsN0=(float)ind/1.0;

			data_plot[ind][0]=EsN0;
			if(telecom_system.operation_mode==BER_PLOT_baseband)
			{
				data_plot[ind][1]=telecom_system.baseband_test_EsN0(EsN0,100).BER;
			}
			if(telecom_system.operation_mode==BER_PLOT_passband)
			{
				data_plot[ind][1]=telecom_system.passband_test_EsN0(EsN0,100).BER;
			}

			data_plot_theo[ind][0]=EsN0 ;

			if(telecom_system.M==MOD_BPSK)
			{
				data_plot_theo[ind][1]=0.5*erfc(sqrt(pow(10,EsN0/10)));
			}
			else
			{
				data_plot_theo[ind][1]=(2.0/log2(telecom_system.M))*(1.0-1.0/sqrt(telecom_system.M))*erfc(sqrt(((3.0* log2(telecom_system.M))/(2.0*(telecom_system.M-1))) *pow(10,EsN0/10)/log2(telecom_system.M)));
			}
			std::cout<<EsN0<<";"<<data_plot[ind][1]<<std::endl;
		}

		telecom_system.plot.plot("BER Simulation",&data_plot[0][0],nPoints,"BER theoretical",&data_plot_theo[0][0],nPoints);
		telecom_system.plot.close();

	}


	return 0;
}

