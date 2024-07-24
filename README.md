# Mercury (Ver 0.2 June 2024)

Mercury is a configurable open-source software-defined modem.

#What's new in Mercury 0.2:

- New Least Square channel estimator with a configurable estimation window.
- Enhanced Time and Frequency synchronization for low SNR values.
- Enhanced TX and RX filtering with separate filters for time synchronization and data messages.
- Enhanced TX and RX mechanisms for higher robustness and computing efficiency.
- New Time and Frequency interleavers.
- New LDPC codes (1/16 to 14/16) optimized for multipath channel and outer CRC code.
- New energy dispersal for power amplification efficiency.
- Pre-equalization to combat filters and DSP imperfections.
- Peak to average power ratio (PAPR) and modulation error rate (MER) measurements.
- Two pilot distribution modes for different channels and bitrate requirements with further optimized parameters such as number of symbols, number of carriers, and synchronization symbols.
- Dynamic partial configuration of the physical layer for computing performance enhancement.
- Enhanced API of the physical layer to allow for byte or bit transfer.
- Separate Data and Acknowledge message robustness configuration.
- New Ladder-based Gearshift mode for the data link layer for low SNR.
- 17 new robustness modes for the ARQ/ Gearshift.
- Bug fixes and performance enhancements.

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

The data link later exchanges channel state information to negotiate the highest data rate possible for the downlink channel, and in case of a change, adapt to the new channel condition. 
This "Gearshift" is done via a set_config command that a commander can send at any time and a recovery mechanism in case of a worsening channel condition. The channel evaluation and gearshift operations are done at the beginning of each data block.

The gearshift algorithm is configurable and can be an SNR based that rely on a test message to evaluate the channel condition (downlink) and set the configuration accordingly, or a Ladder algorithm that starts at an initial selectable robustness configuration and switches to the next configuration in the case of crossing a configurable threshold or return to the more robust configuration in case of not achieving a configurable lower transmission success threshold.

The gearshift mechanism can enter a temporary configurable block period to avoid multiple unsuccessful up-gearshifts.

The robustness configurations are numbered (0 to 16) where CONFIG_0 is the most robust one.

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

- init_configuration: The initial data configuration to be used.
- ack_configuration: The configuration to be used for acknowledgment messages.
- gear_shift_on: Gearshit setting (Yes, No).
- gear_shift_algorithm: Gearshit algorithm (SNR-based, Ladder based)
- gear_shift_up_success_rate_limit_precentage: Threshold for up-gearshifting.
- gear_shift_down_success_rate_limit_precentage: Threshold for down-gearshifting.
- gear_shift_block_for_nBlocks_total: Number of transmission blocks to be done without a gearshift try in case of an up-gearshift failure.
- batch_size: Number of messages per data batch.
- nMessages: Number of messages per block.
- nResends: Number of trials for each data/control message.
- ack_batch_size:  Number of messages per acknowledgment batch.
- control_batch_size: Number of messages per control batch.
- ptt_on_delay_ms: time in milliseconds to wait after switching on the push to talk (ptt) before sending data.
- ptt_off_delay_ms: time in milliseconds to wait after switching off the push to talk (ptt) after sending data.
- switch_role_timeout_ms: time in milliseconds to wait before switching role with the responder in case of an empty data buffer.

The physical layer parameters can be defined in physical_layer/physical_config.cc

The physical layer has the following parameters to be configured (more fine-tuning parameters are in telecom_system.cc/telecom_system.h):

- init_configuration: The initial configuration to be used for transmission (Modulation, code rate, etc.)
- plot_folder: Folder to be used as a temporary for the plot function.
- plot_plot_active: Plot function activation (Yes, No).
- microphone_dev_name: Alsa capture device name ("plughw:1,0" for example).
- speaker_dev_name:  Alsa play device name ("plughw:1,0" for example).
- microphone_type: Capture channel.
- microphone_channels: Capture channels (Mono, Stereo, Left, Right).
- speaker_type: Play channel.
- speaker_channels: Play channels (Mono, Stereo, Left, Right).
- speaker_frames_to_leave_transmit_fct: Number of frames to be played before leaving the Alsa transmission function, to avoid Alsa underrun.

- bandwidth: the used bandwidth for TX and RX.
- time_sync_trials_max: number of time synchronization detection peaks to be considered.
- use_last_good_time_sync: whether to use the last time synchronization result or try to synchronize again in each frame.
- use_last_good_freq_offset: whether to use the last time synchronization result or try to synchronize again in each frame.
- frequency_interpolation_rate: sampling frequency interpolation rate to match the sound card sampling rate.
- carrier_frequency: the OFDM signal carrier frequency in TX and RX.
- output_power_Watt: the output signal power at TX.

- RX time sync FIR_filter_window: the Finite Impulse Response (FIR) window at RX for the time sync part of the signal.
- RX time sync FIR_filter_transition_bandwidth: the FIR transition width at RX for the time sync part of the signal.
- RX time sync FIR_filter_cut_frequency: the FIR cut frequency at RX for the time sync part of the signal.
- RX time sync FIR_filter_type the FIR type (LPF, HPF, etc..) for the time sync part of the signal.

- RX data FIR_filter_window: the Finite Impulse Response (FIR) window at RX for the data part of the signal.
- RX data FIR_filter_transition_bandwidth: the FIR transition width at RX for the data part of the signal.
- RX data FIR_filter_cut_frequency the FIR cut frequency at RX for the data part of the signal.
- RX data FIR_filter_type the FIR type (LPF, HPF, etc..) for the data part of the signal.

- TX 1 FIR_filter_window: the Finite Impulse Response (FIR) window at TX.
- TX 1 FIR_filter_transition_bandwidth: the FIR transition width at TX.
- TX 1 FIR_filter_cut_frequency the FIR cut frequency at TX.
- TX 1 FIR_filter_type the FIR type (LPF, HPF, etc..).

- TX 2 FIR_filter_window: the Finite Impulse Response (FIR) window at TX.
- TX 2 FIR_filter_transition_bandwidth: the FIR transition width at TX.
- TX 2 FIR_filter_cut_frequency the FIR cut frequency at TX.
- TX 2 FIR_filter_type the FIR type (LPF, HPF, etc..).

- ofdm_preamble_configurator_Nsymb: number of preamble symbols.
- ofdm_preamble_configurator_nIdentical_sections: number of identical parts per preamble symbol.
- ofdm_preamble_configurator_modulation: preamble modulation;
- ofdm_preamble_configurator_boost: preamble boost.
- ofdm_preamble_configurator_seed: preamble pseudo-random bits seed.
- ofdm_preamble_papr_cut: preamble Peak to Average Power Ratio (PAPR) cut limit (before filtering)
- ofdm_data_papr_cut: data Peak to Average Power Ratio (PAPR) cut limit (before filtering)
- ofdm_time_sync_Nsymb: Number of OFDM symbols when the GI is used for time synchronization.

The OFDM has the following main parameters (more fine-tuning parameters are in ofdm.cc/ofdm.h):

- Nfft: the Fast Fourier transfer size.
- Nc: the number of used sub-channels.
- Dx: frequency distance between pilots.
- Dy: time distance between pilots.
- Nsymb: number of symbols per OFDM frame.
- gi: Guard interval to remove the multipath effect.
- pilot_boost: to define pilot carriers' power in relation to data carriers' power.
- freq_offset_ignore_limit: the minimum value of sampling frequency and carrier frequency offset to be compensated.

- ofdm_pilot_configurator_first_row: Set the first row to data or pilot.
- ofdm_pilot_configurator_last_row: Set the last row to data or pilot.
- ofdm_pilot_configurator_first_col: Set the first column to data or pilot.
- ofdm_pilot_configurator_second_col: Set the second column to data or pilot.
- ofdm_pilot_configurator_last_col: Set the last column to data or pilot.
- ofdm_pilot_configurator_seed: The seed used in the pseudorandom data generator of the pilot sequence.
- ofdm_pilot_density: Pilot density (HIGH_DENSITY or LOW_DENSITY).
- ofdm_start_shift: Number of non-used subcarriers starting at 0 Hz.
- ofdm_channel_estimator: The channel estimation Method (Zero-Force or Least-Square).
- ofdm_channel_estimator_amplitude_restoration: Activate amplitude restoration (Yes, No). (only with M-PSK modulation)
- ofdm_LS_window_width: The least-Square window width.
- ofdm_LS_window_hight: The least-Square window height.
- bit_energy_dispersal_seed: The seed used in the pseudorandom data generator of the bit energy dispersal.


The LDPC has the following main parameters:
- decoding algorithm: the decoding algorithm can be either the Sum-Product algorithm (SPA) or the Gradient Bit-Flipping (GBF)
- standard and framesize: the LDPC code deploys specially designed LDPC matrices of the size 1600 bits with three different code rates
- rate: the code rate that defines the protection level vs data rate (1/16, 2/16, 3/16, 4/16, 5/16, 6/16 8/16, 14/16).
- GBF_eta: the GBF LDPC decoder correction rate.
- nIteration_max: the number of maximum decoding iterations.

The outer code has the following options:
- outer_code: (CRC16_MODBUS_RTU or NONE)

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

To compile Mercury code:

* make

To install:

* make install

To generate the Mercury documentation, run the following:

* make doc


## include

Contains the header files of Mercury.


### Authors

The code was written by Fadi Jerji at Rhizomatica.
