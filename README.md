# Mercury

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
- 17 OFDM robustness modes for the ARQ / Gearshift.
- 3 MFSK weak-signal modes (ROBUST_0, ROBUST_1, ROBUST_2) for decoding below the OFDM threshold.
- Non-coherent energy detection for MFSK modes (no channel estimation needed).
- Pattern-based ACK using Welch-Costas tone sequences for reduced ARQ overhead on slow modes.
- CRC16-MODBUS-RTU outer code.
- Coarse frequency synchronization for wider capture range.
- Base-36 callsign packing for small frame sizes.
- Cross-platform GUI with real-time constellation display, waterfall spectrum, and signal meters (ImGui + GLFW + OpenGL).


## Compilation And Installation

Mercury is implemented mainly in C++ (compiles in C++14 mode). Currently runs on Linux and Windows.
Compilation is tested on Linux (gcc / glibc toolchain) and Windows (Mingw64 posix toolchain). ALSA and PulseAudio libraries and development headers must be installed on Linux. On Windows, DirectSound and WASAPI are supported.
Other optional depencies are GNUPlot (for ploting constellations), GraphViz and Doxygen (for documentation). On
a Debian based system (eg. Debian, Ubuntu), the dependencies can be installed with:

```
apt-get install libasound2-dev libpulse-dev libglfw3-dev gnuplot-x11 graphviz
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

### GUI Build

Mercury can be built with a graphical user interface on both Linux and Windows. The GUI uses Dear ImGui (included in `third_party/imgui/`) with a GLFW + OpenGL3 backend. GLFW is bundled for Windows in `third_party/glfw/`; on Linux, install `libglfw3-dev`.

Using `build.sh` (recommended, works on both platforms via MSYS2/MinGW64 on Windows):
```
./build.sh release
```

Other build modes: `debug`, `o0`, `o1`, `o2`, `o3`, `asan`, `ubsan`.

Using `make` directly:
```
make clean
make -j4 GUI_ENABLED=1
```

To build without GUI (headless mode only):
```
make clean
make -j4 GUI_ENABLED=0
```

**Important**: Use a consistent compiler toolchain. Mixing object files from different GCC versions causes ABI incompatibility crashes. `build.sh` always does a clean build to avoid stale object file issues.

## Running

Mercury has different operating modes and parameters. Usage parameters:

```
Usage modes:
./mercury -m [mode] -s [modulation_config] -i [device] -o [device] -r [radio_type] -x [sound_system]
./mercury -m ARQ -s [modulation_config] -i [device] -o [device] -r [radio_type] -x [sound_system] -p [arq_tcp_base_port]
./mercury -h

Options:
 -c [cpu_nr]                Run on CPU [cpu_nr]. Use -1 to disable CPU selection (default).
 -m [mode]                  Available operating modes are: ARQ, TX_SHM, RX_SHM, TX_TEST, RX_TEST, TX_RAND, RX_RAND, PLOT_BASEBAND, PLOT_PASSBAND.
 -s [modulation_config]     Sets modulation configuration. Modes: 0 to 16 (OFDM), 100-102 (ROBUST MFSK). Use "-l" for listing all available modulations.
 -r [radio_type]            Available radio types are: stockhf, sbitx.
 -i [device]                Radio Capture device id (eg: "plughw:0,0").
 -o [device]                Radio Playback device id (eg: "plughw:0,0").
 -x [sound_system]          Sets the sound system API to use: alsa, pulse, dsound or wasapi. Default is alsa on Linux and wasapi on Windows.
 -p [arq_tcp_base_port]     Sets the ARQ TCP base port (control is base_port, data is base_port + 1). Default is 7002.
 -g                         Enables the adaptive modulation selection (gear-shifting).
 -t [timeout_ms]            Connection timeout in milliseconds (ARQ mode only). Default is 15000.
 -a [max_attempts]          Maximum connection attempts before giving up (ARQ mode only). Default is 15.
 -k [link_timeout_ms]       Link timeout in milliseconds (ARQ mode only). Default is 30000.
 -e                         Exit when client disconnects from control port (ARQ mode only).
 -R                         Enable Robust mode (MFSK for weak-signal hailing/low-speed data).
 -I [iterations]            LDPC decoder max iterations (5-50, default 50). Lower = less CPU.
 -T [tx_gain_db]            TX gain in dB (overrides GUI slider). E.g. -T -25.6 for -30 dBFS output.
 -G [rx_gain_db]            RX gain in dB (overrides GUI slider). E.g. -G 25.6 to boost weak input.
 -C                         Check audio configuration (stereo, sample rate) before starting.
 -f [offset_hz]             TX carrier offset in Hz for testing frequency sync.
 -v                         Verbose debug output (OFDM sync, RX timing, ACK detection).
 -n                         Disable GUI (headless mode). GUI is enabled by default.
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

ARQ mode with gearshift can be used, for example, in a stock HF radio, as:

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

On Windows, WASAPI is the default and recommended audio driver. To use the default audio device:

```
./mercury -m TX_TEST -s 0 -r stockhf -x wasapi
```

To set a specific audio device, use `-i` and `-o` with the device name. Use `-z` to list available devices:

```
./mercury -z
./mercury -m ARQ -r stockhf -x wasapi -i "CABLE Output (VB-Audio Virtual Cable)" -o "CABLE Input (VB-Audio Virtual Cable)"
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

## Supported Clients

Mercury includes a built-in GUI for monitoring and configuration. When built with `GUI_ENABLED=1` (the default), Mercury displays a real-time constellation diagram, waterfall spectrum, signal level meters, and modem status indicators. The GUI also provides dialogs for sound card selection, callsign, network ports, and gearshift settings. To run headless (without GUI), use the `-n` flag.

For ARQ mode, a client application connects to Mercury over TCP to send and receive data. The folder "examples" has a transmitter and receiver example for the TX_SHM and RX_SHM modes. A more complete client called HERMES-BROADCAST for data broadcast using RaptorQ codes is available here: https://github.com/Rhizomatica/hermes-broadcast .

For a simple ARQ client which supports hamlib, take a look at: https://github.com/Rhizomatica/mercury-connector

Any VARA client should be compatible with Mercury. Compatibility support is not complete. If you
find a VARA client which does not communicate, please report. Base TCP port is 7002 (7002 control and 7003 data).

For a more complete ARQ client which integrates Mercury to UUCP, look at: https://github.com/Rhizomatica/hermes-net/tree/main/uucpd


## Supported Modulation Modes

The modulation modes can be listed with "./mercury -l", and are (without outer-code overhead):

```
ROBUST_0 (~14 bps)   - 32-MFSK, LDPC 1/16, 1 stream
ROBUST_1 (~22 bps)   - 16-MFSK, LDPC 1/16, 2 streams
ROBUST_2 (~87 bps)   - 16-MFSK, LDPC 1/4, 2 streams
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

ROBUST modes use MFSK (non-coherent) modulation and can decode at significantly lower SNR than the OFDM modes. With gearshift enabled, Mercury starts at ROBUST_0 and works up through ROBUST_1, ROBUST_2, then into CONFIG_0 through CONFIG_16 as channel conditions improve.

## Testing and Benchmarking

The `tools/` directory contains test and benchmark scripts. These require Python 3 and a virtual audio cable (eg. VB-Audio Virtual Cable on Windows).

**Benchmark suite** (`tools/mercury_benchmark.py`) — SNR sweep, stress test, and adaptive gearshift test. Generates VARA-style throughput charts (bytes/min vs SNR). See `tools/BENCHMARK_GUIDE.md` for full documentation.

```
pip install numpy sounddevice matplotlib
python tools/mercury_benchmark.py sweep --configs 100,0,8,16 --measure-duration 60
python tools/mercury_benchmark.py stress --num-bursts 5
python tools/mercury_benchmark.py adaptive --measure-duration 60
```

**Loopback test** (`tools/robust_loopback_test.py`) — quick ARQ sanity check for ROBUST modes. Starts commander + responder on a virtual cable and monitors for successful data exchange.

```
python tools/robust_loopback_test.py 100    # ROBUST_0
python tools/robust_loopback_test.py 101    # ROBUST_1
python tools/robust_loopback_test.py 102    # ROBUST_2
```

## Discussion

Join HERMES mailing list:
https://lists.riseup.net/www/info/hermes-general


## About

The code was originally written by Fadi Jerji for Rhizomatica's HERMES project. Currently the project is maintained by Rhizomatica's HERMES team.

This project is sponsored by ARDC.
