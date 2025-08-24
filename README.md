 Mercury

Mercury is a free software software-defined modem solution for the High-Frequency (HF) band.

## Features

- Least Square channel estimator with a configurable estimation window.
- Time and Frequency synchronization for low SNR values.
- TX and RX filtering with separate filters for time synchronization and data messages.
- Time and Frequency interleavers.
- LDPC codes (1/16 to 14/16) optimized for multipath channel
- Outer CRC coding.
- Energy dispersal for power amplification efficiency.
- Pre-equalization to compensate filters and DSP imperfections.
- Peak to average power ratio (PAPR) and modulation error rate (MER) measurements.
- Two pilot distribution modes for different channels and bitrate requirements with further optimized parameters such as number of symbols, number of carriers, and synchronization symbols.
- Dynamic partial configuration of the physical layer for computing performance enhancement.
- Enhanced API of the physical layer to allow for byte or bit transfer.
- Separate Data and Acknowledge message robustness configuration.
- Ladder-based Gearshift mode for the data link layer for low SNR.
- 17 robustness modes for the ARQ/ Gearshift.


## Compilation And Installation

Mercury is implemented mainly in C++ (compiles in C++14 mode). Currently runs on Linux and Windows.
Compilation is tested on Linux (gcc / glibc toolchain) and Windows (Mingw64 posix toolchain). ALSA and PulseAudio libraries and development headers must be installed on Linux. On Windows, DirectSound and WASAPI are supported.
Other optional depencies are GNUPlot (for ploting constellations), GraphViz and Doxygen (for documentation). On
a Debian based system (eg. Debian, Ubuntu), the dependencies can be installed with:

```
apt-get install libasound2-dev libpulse-dev gnuplot-x11 graphviz
```
To compile, use:

```
make
```

To install:

```
make install
```

To generate the Mercury documentation, run the following:

```
apt-get install doxygen
make doc
```

## Running

Mercury has different operating modes and parameters. Usage parameters:

```
Usage modes: 
./mercury -m [mode] -s [modulation_config] -i [device] -o [device] -r [radio_type] -x [sound_system]
./mercury -m ARQ -s [modulation_config] -i [device] -o [device] -r [radio_type] -x [sound_system] -p [arq_tcp_base_port]
./mercury -h

Options:
 -c [cpu_nr]                Run on CPU [cpu_br]. Defaults to CPU 3. Use -1 to disable CPU selection.
 -m [mode]                  Available operating modes are: ARQ, TX_SHM, RX_SHM, TX_TEST, RX_TEST, TX_RAND, RX_RAND, PLOT_BASEBAND, PLOT_PASSBAND.
 -s [modulation_config]     Sets modulation configuration for all modes including ARQ, except when gear-shift is enabled. Modes: 0 to 16. Use "-l" for listing all available modulations.
 -r [radio_type]            Available radio types are: stockhf, sbitx.
 -i [device]                Radio Capture device id (eg: "plughw:0,0").
 -o [device]                Radio Playback device id (eg: "plughw:0,0").
 -x [sound_system]          Sets the sound system API to use: alsa, pulse, dsound or wasapi. Default is alsa on Linux and dsound on Windows.
 -p [arq_tcp_base_port]     Sets the ARQ TCP base port (control is base_port, data is base_port + 1). Default is 7002.
 -g                         Enables the adaptive modulation selection (gear-shifting). Not working yet!.
 -l                         Lists all modulator/coding modes.
 -z                         Lists all available sound cards.
 -h                         Prints this help.
```

Mercury operating modes are:
- ARQ: Data-link layer and Automatic repeat request mode. Default control port is 7002 and default data port is 7003.
- TX_SHM: Transmits data read from shared memory interface (check folder examples).
- RX_SHM: Received data is written to shared memory interface.

Mercury also has some modes for development / channel analysis:
- PLOT_BASEBAND: Baseband Bit Error Rate (BER) simulation mode over an AWGN channel with/without plotting.
- PLOT_PASSBAND: Passeband BER simulation mode over an AWGN channel with/without plotting.
- TX_RAND: Transmission test using random data as source
- RX_RAND: Data reception test (supports plotting constelation)
- TX_TEST: Data transmission test (a moving byte set to 1, all the rest zeros)
- RX_TEST: Data reception test

For using the shared memory interface, Mercury should be started in mode RX_SHM in receive side, and TX_SHM at transmit site. 

Example (stock hf radio, like an ICOM IC-7100) for transmitter side:
```
./mercury -m TX_SHM -s 1 -r stockhf -i "plughw:0,0" -o "plughw:0,0"
```

Example of Mercury in the receive side (sBitx v3 radio):
```
./mercury -m RX_SHM -s 1 -r sbitx -i "plughw:0,0" -o "plughw:0,0"
```

ARQ mode is under active development and can be used, for example, in a stock HF radio, as:

```
./mercury -m ARQ -r stockhf -i "plughw:0,0" -o "plughw:0,0"
```

For receiving broadcat data in test mode, for example, in a sBitx radio, using mode 0, use:

```
./mercury -m RX_TEST -s 0 -r sbitx -i "plughw:0,0" -o "plughw:0,0"
```

For transmitting such test data broadcast data, in stock HF radio (like an ICOM IC-7100), using mode 0, use (and key the radio using rigctl):

```
./mercury -m TX_TEST -s 0 -r stockhf -i "plughw:0,0" -o "plughw:0,0"
```

On Windows it is recommended to use the default device, by not setting explicitly. Example using the DirectSound driver (other option is wasapi):

```
./mercury -m TX_TEST -s 0 -r stockhf -x dsound
```

For enabling tx (keying the radio) in an ICOM IC-7100, for example, use:

```
rigctl -r /dev/ttyUSB0 -m 3070 T 1
```

For unkeying:

```
rigctl -r /dev/ttyUSB0 -m 3070 T 0
```

For the sBitx radio, use the HERMES software stack, available at https://github.com/Rhizomatica/hermes-net (use trx_v2-userland implementation).

## Supported clients

Mercury alone is not very useful. A client is needed to receive and transmit information using Mercury. The folder "examples" has a transmitter and receiver example
to use with the TX_SHM and RX_SHM modes. A more complete client called HERMES-BROADCAST to be used for data broadcast which uses RaptorQ codes is available here: https://github.com/Rhizomatica/hermes-broadcast .

For a simple ARQ client which supports hamlib, take a look at: https://github.com/Rhizomatica/mercury-connector

Any VARA client should be compatible with Mercury. Compatibility support is not complete. If you
find a VARA client which does not communicate, please report. Base TCP port is 7002 (7002 control and 7003 data).

For a more complete ARQ client which integrates Mercury to UUCP, look at: https://github.com/Rhizomatica/hermes-net/tree/main/uucpd


## Supported Modulation Modes

The modulation modes can be listed with "./mercury -l", and are (without outer-code overhead):

```
CONFIG_0 (84.841629 bps)
CONFIG_1 (169.683258 bps)
CONFIG_2 (254.524887 bps)
CONFIG_3 (339.366516 bps)
CONFIG_4 (424.208145 bps)
CONFIG_5 (509.049774 bps)
CONFIG_6 (678.733032 bps)
CONFIG_7 (787.815126 bps)
CONFIG_8 (945.378151 bps)
CONFIG_9 (1260.504202 bps)
CONFIG_10 (1390.866873 bps)
CONFIG_11 (1855.263158 bps)
CONFIG_12 (2287.581699 bps)
CONFIG_13 (2521.008403 bps)
CONFIG_14 (3428.921569 bps)
CONFIG_15 (4411.764706 bps)
CONFIG_16 (5735.294118 bps)
```

## Discussion

Join HERMES mailing list:
https://lists.riseup.net/www/info/hermes-general


## About

The code was originally written by Fadi Jerji for Rhizomatica's HERMES project. Currently the project is maintained by Rhizomatica's HERMES team.

This project is sponsored by ARDC.
