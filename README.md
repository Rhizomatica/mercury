# Mercury

Mercury is a configurable open-source software-defined modem: (https://github.com/Rhizomatica/mercury).


# System Components and Configuration

The software-defined modem features an Orthogonal Frequency-Division Multiplexing (OFDM) modulator/demodulator with a Low-Density Parity-Check (LDPC) error correction code encoder/decoder with an embedded Additive white Gaussian noise (AWGN) channel simulator. 

 The general system has the following parameters to be configured (more fine-tuning parameters are in telecom_system.cc/telecom_system.h):

- M: the used modulation (BPSK, QPSK, 8QAM, 16QAM, 32QAM, 64QAM).
- bit_interleaver_block_size: the interleaving block size to combat pulse noise.
- bandwidth: the used bandwidth for TX and RX.
- time_sync_trials_max: number of time synchronization detection peaks to be considered.
- lock_time_sync: whether to use the last time synchronization result or try to synchronize again in each frame.
- frequency_interpolation_rate: sampling frequency interpolation rate to match the sound card sampling rate.
- carrier_frequency: the OFDM signal carrier frequency in TX and RX.
- output_power_Watt: the output signal power at TX.
- filter_window: the Finite Impulse Response (FIR) window at RX.
- filter_transition_bandwidth: the FIR transition width at RX.
- filter_cut_frequency the FIR cut frequency at RX.

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


The LDPC has the following main parameters:

- standard and framesize: the LDPC code deploys specially designed LDPC matrices of the size 1600 bits with three different code rates
- rate: the code rate that defines the protection level vs data rate (2/16, 8/16, 14/16).
- GBF_eta: the Gradient Bit-Flipping (GBF) LDPC decoder correction rate.
- nIteration_max: the number of maximum decoding iterations.

Others:

Further components of the system have their configuration such as the ALSA sound library and the TCP/IP socket.



# Operation Mode

Mercury operates in one of six different modes:

- BER_PLOT_baseband: Baseband Bit Erro Rate (BER) simulation mode over an AWGN channel with/without plotting.
- BER_PLOT_passband: Passeband BER simulation mode over an AWGN channel with/without plotting.
- TX_TEST: random data transmission test
- RX_TEST: random data reception test with/without plotting.
- TX_TCP: TCP/IP data transmission
- RX_TCP: TCP/IP data reception with/without plotting.


# Prerequisite

* GNU PLOT

In case of switching  telecom_system.plot.plot_active to YES, the gnuplot is required to be installed on the machine.

To install GNU plot run the following command as root: 'apt-get install gnuplot-x11'.

* ALSA Sound

To use the sound card via ALSA Sound library, the library should be installed and linked to the code at compilation.

To install ALSA Sound run the following command as root: 'apt-get install libasound2-dev'.



# Compile And Install

To compile the Mercury code, run the following:

* make




## include

Contains the header files of Mercury.


### Authors

The code was written by Fadi Jerji fadi.jerji@ <gmail.com, rhizomatica.org, caisresearch.com, ieee.org>

