# Mercury

Mercury is a configurable open-source software-defined modem: (https://github.com/Rhizomatica/mercury).

# System Components and Configuration

The mercury software-defined modem is composed of two main layers, the physical layer, and the data link layer.

The physical layer is the part responsible for data protection via the use of Low-Density Parity-Check (LDPC) codes and information mapping on the electromagnetic spectrum via modulation using Orthogonal Frequency-Division Multiplexing (OFDM) and other digital signal processing methods.

The physical layer provides a connection-less transmission. In other words, the transmitter doesn't have any information regarding the receiver status, the channel status, or packets received/lost. This task is left to the data link layer.

 The physical layer can be interfaced from the upper end and the lower end.  The lower end interfaces with the hardware via the sound card using the Alsa driver. The speaker and microphone can be connected to an off-the-shelf radio transceiver to transmit the signal over an electromagnetic channel.

The upper and the physical layer interface with the datalink layer via three main connection points, send(), receive(), and load_configuration(). In addition, function load_config(SNR) allows the physical layer to inform the data link layer of the appropriate configuration for a specific Signal to Noise Ratio (SNR).

ARQ

The data link can be in one of two roles, a commander or a responder. The commander is the side responsible for controlling the message flux.
The data link layer provides a connection based using automatic repeat requests (ARQ) via retransmission and acknowledgment mechanisms. This is to guarantee the reception of the information even in cases of message loss. In the case of an ack message lost, a request to resend the last ack message can be sent.

The data link layer packs the messages in batches to avoid a fast TX/RX switch. Nevertheless, the batch size is configurable and can be set to one. Several batches form a block. Writing to the TX data buffer and reading from the RX data buffer are done in blocks. The block size is configurable as well.

Gearshift

The data link later exchanges channel state information to negotiate the highest data rate possible for the downlink channel, and in case of a change, adapt to the new channel condition. This "Gearshift" is done via a set_config command that a commander can send at any time and a recovery mechanism in case of a worsening channel condition. The channel evaluation and gearshift operations are done at the beginning of each data block.

The data link layer connects to a possible application layer via two TCP/IP connections, a data dump buffer, and a control connection with an application-level interface (API).

API

A base API was implemented to provide the basic functionality in a way that would be familiar to users of commercially available modems.
The API can easily be updated and new commands can be added to provide further control to the application layer.

The base API is composed of the following commands:

1. MYCALL mycall\r

Sets the AQR call sign to the value of the string "my_call".

Reply: OK

2. LISTEN ON\r

Sets the ARQ to a responder role and waits for an incoming connection.

Reply: OK

3. CONNECT my_call destination_call\r

Sets the ARQ to a commander role and initiates the connection sequence to "destination_call".

Reply: OK

4. DISCONNECT\r

Terminates the current connection.

Reply: OK

5. BW2300\r

Sets the bandwidth to 2300 Hz.

Reply: OK

6. BW2500\r

Sets the bandwidth to 2500 Hz.

Reply: OK

7. BUFFER TX\r

Reads the number of data bytes in the TX buffer yet to be sent.

Reply: BUFFER bytes

In the case of an unrecognized command, a reply: NOK is provided.



The physical layer features an OFDM modulator/demodulator with an LDPC error correction code encoder/decoder with an embedded Additive white Gaussian noise (AWGN) channel simulator. 


# Configuration


The data link layer parameters can be defined in data_link_layer/datalink_config.cc

The data link layer has the following parameters to be configured:

- fifo_buffer_tx_size: The first-in-first-out transmission buffer size (data to be transmitted).
- fifo_buffer_rx_size: The FIFO reception buffer size.
- fifo_buffer_backup_size: Backup FIFO buffer for configuration switch.
- link_timeout: Maximum time without any activity on the link (TX or RX) before link drops (ms).
- tcp_socket_control_port: Control TCP/IP port.
- tcp_socket_control_timeout_ms: Control TCP/IP timeout (ms).
- tcp_socket_data_port: Data TCP/IP port.
- tcp_socket_data_timeout_ms: Data TCP/IP timeout (ms).
- gear_shift_on: Gearshit setting (Yes, No).
- current_configuration: The configuration used if Gearshit was disabled.
- batch_size: Number of messages per data batch.
- nMessages: Number of messages per block.
- nBytes_header: Size of messages header (byte).
- nResends: Number of trials for each data/control message.
- ack_batch_size:  Number of messages per acknowledgment batch.
- control_batch_size: Number of messages per control batch.
- ptt_on_delay_ms: time in milliseconds to wait after switching the push to talk (ptt) before sending data.

The physical layer parameters can be defined in physical_layer/physical_config.cc

The physical layer has the following parameters to be configured (more fine-tuning parameters are in telecom_system.cc/telecom_system.h):

- test_tx_AWGN_EsN0_calibration: Calibration value (dB) to compensate for cable losses (and other losses).
- test_tx_AWGN_EsN0: AWGN noise level to be transmitted with the test signal.
- tcp_socket_test_port: Test TCP/IP port.
- tcp_socket_test_timeout_ms: Test TCP/IP timeout (ms).
- current_configuration: Current configuration to be used for transmission (Modulation, code rate, etc.)
- plot_folder: Folder to be used as a temporary for the plot function.
- plot_plot_active: Plot function activation (Yes, No).
- microphone_dev_name: Alsa capture device name ("plughw:1,0" for example).
- speaker_dev_name:  Alsa play device name ("plughw:1,0" for example).
- microphone_type: Capture channel type (Mono, Stereo).
- microphone_channels: Number of capture channels.
- speaker_type: Play channel type (Mono, Stereo).
- speaker_channels: Number of play channels.
- speaker_frames_to_leave_transmit_fct: Number of frames to be played before leaving the Alsa transmission function, to avoid Alsa underrun.
- M: the used modulation (BPSK, QPSK, 8QAM, 16QAM, 32QAM, 64QAM).
- bit_interleaver_block_size: the interleaving block size to combat pulse noise.
- bandwidth: the used bandwidth for TX and RX.
- time_sync_trials_max: number of time synchronization detection peaks to be considered.
- lock_time_sync: whether to use the last time synchronization result or try to synchronize again in each frame.
- frequency_interpolation_rate: sampling frequency interpolation rate to match the sound card sampling rate.
- carrier_frequency: the OFDM signal carrier frequency in TX and RX.
- output_power_Watt: the output signal power at TX.
- RX FIR_filter_window: the Finite Impulse Response (FIR) window at RX.
- RX FIR_filter_transition_bandwidth: the FIR transition width at RX.
- RX FIR_filter_cut_frequency the FIR cut frequency at RX.
- TX 1 FIR_filter_window: the Finite Impulse Response (FIR) window at TX.
- TX 1 FIR_filter_transition_bandwidth: the FIR transition width at TX.
- TX 1 FIR_filter_cut_frequency the FIR cut frequency at TX.
- TX 2 FIR_filter_window: the Finite Impulse Response (FIR) window at TX.
- TX 2 FIR_filter_transition_bandwidth: the FIR transition width at TX.
- TX 2 FIR_filter_cut_frequency the FIR cut frequency at TX.
- ofdm_preamble_configurator_Nsymb: number of preamble symbols.
- ofdm_preamble_configurator_nIdentical_sections: number of identical parts per preamble symbol.
- ofdm_preamble_configurator_modulation: preamble modulation;
- ofdm_preamble_configurator_boost: preamble boost.
- ofdm_preamble_configurator_seed: preamble pseudo-random bits seed.
- ofdm_preamble_papr_cut: preamble Peak to Average Power Ratio (PAPR) cut limit (before filtering)
- ofdm_data_papr_cut: data Peak to Average Power Ratio (PAPR) cut limit (before filtering)

The OFDM has the following main parameters (more fine-tuning parameters are in ofdm.cc/ofdm.h):

- Nfft: the Fast Fourier transfer size.
- Nc: the number of used sub-channels.
- Dx: frequency distance between pilots.
- Dy: time distance between pilots.
- Nsymb: number of symbols per OFDM frame.
- gi: Guard interval to remove the multipath effect.
- pilot_boost: to define pilot carriers' power in relation to data carriers' power.
- time_sync_Nsymb: to define the number of symbols to be used in time synchronization.
- freq_offset_ignore_limit: the minimum value of frequency offset to be compensated.
- channel estimation method


The LDPC has the following main parameters:
- decoding algorithm: the decoding algorithm can be either the Sum-Product algorithm (SPA) or the Gradient Bit-Flipping (GBF)
- standard and framesize: the LDPC code deploys specially designed LDPC matrices of the size 1600 bits with three different code rates
- rate: the code rate that defines the protection level vs data rate (2/16, 8/16, 14/16).
- GBF_eta: the GBF LDPC decoder correction rate.
- nIteration_max: the number of maximum decoding iterations.

# Operation Mode

Mercury operates in one of six different modes:
- ARQ_MODE: Data-link layer and Automatic repeat request active.
- BER_PLOT_baseband: Baseband Bit Erro Rate (BER) simulation mode over an AWGN channel with/without plotting.
- BER_PLOT_passband: Passeband BER simulation mode over an AWGN channel with/without plotting.
- TX_TEST: random data transmission test
- RX_TEST: random data reception test with/without plotting.


# Prerequisite

* GNU PLOT

In case of switching  telecom_system.plot.plot_active to YES, the gnuplot is required to be installed on the machine.

To install GNU plot run the following command as root: 'apt-get install gnuplot-x11'.

* ALSA Sound

To use the sound card via ALSA Sound library, the library should be installed and linked to the code at compilation.

To install ALSA Sound run the following command as root: 'apt-get install libasound2-dev'.

* Documentation

To generate the documentation, the Doxygen and GraphViz packets should be installed.

To install Doxygen run the following command as root: 'apt-get install doxygen'.

To install GraphViz run the following command as root: 'apt-get install graphviz'.




# Compile And Install

To compile the Mercury code, run the following:

* make

To generate the Mercury documentation, run the following:

* make doc




## include

Contains the header files of Mercury.

## final notes:

The current version of Mercury was sucessfuly tested using Sbitx radios using Raspberry pi 4 microcomputers.
Tx power= 20 Watts, distance 70 km, Rx SNR= -3 dB using TX_test, Rx_test Config0.


### Authors

The code was written by Fadi Jerji fadi.jerji@ <gmail.com, rhizomatica.org, caisresearch.com, ieee.org>
