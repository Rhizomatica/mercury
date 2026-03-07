# Mercury

Mercury is a free software software-defined modem for the High-Frequency (HF) band. It provides reliable data transfer over HF radio channels using OFDM and MFSK modulation with LDPC forward error correction, adaptive modulation, end-to-end encryption, and automatic repeat request (ARQ).

## Features

### Physical Layer — 20 Modes from 14 bps to 5665 bps

**OFDM modes** (CONFIG_0 through CONFIG_16): 17 configurations spanning BPSK 1/16 through 32QAM 14/16. Coherent demodulation with LDPC SPA decoding (up to 50 iterations), CRC16 outer code, and adaptive pilot-aided channel estimation.

**MFSK weak-signal modes** (ROBUST_0, ROBUST_1, ROBUST_2): Non-coherent energy detection for decoding below the OFDM threshold. ROBUST_0 operates at -13 dB Es/N0 — well into the noise floor. Gray-coded tone mapping with soft LLR output feeds into the same LDPC decoder as OFDM.

**Adaptive gearshift**: Automatic mode selection from ROBUST_0 through CONFIG_16 based on measured SNR. Turboshift uses SNR-based supershift for fast initial climb, then ladder gearshift for fine-grained adaptation. A verification probe at the top config ensures the highest mode actually works before data transfer begins.

### Narrowband Mode (500 Hz)

Operates in a 469 Hz bandwidth (Nc=10 subcarriers) alongside the standard 2344 Hz wideband mode (Nc=50). All 20 modes are available in narrowband. Bandwidth negotiation is automatic: NB stations connect via MFSK hailing and optionally upgrade to WB when both sides support it.

- CLI: `-M nb` (narrowband only), `-M auto` (NB hail, WB upgrade)
- ZF channel estimator (immune to narrowband phase rotation artifacts)
- NB ceiling at CONFIG_14; CONFIG_15/16 available but marginal in 469 Hz

### Batch Compression

Entire data batches are compressed before splitting across frames, improving throughput by up to 1.9x on text. Two algorithms compete per batch — best ratio wins:

- **PPMd8** (order 6, 2 MB model): Adaptive statistical compression, strong on text
- **zstd** (level 3): Fast dictionary compression, effective on structured/binary data
- **Streaming context**: PPMd model carries across batches; zstd uses a 32 KB raw-data prefix. Self-correcting on failure (CRC16 detects model desync).

Compression is negotiated via capabilities (`CAP_COMPRESSION`) and auto-arms on B2F detection or force-enabled with `-F on`.

### End-to-End Encryption

Optional authenticated encryption for all data transfers:

- **Key exchange**: X25519 ECDH (Curve25519) with HKDF-Blake2b key derivation
- **Symmetric cipher**: ChaCha20-Poly1305 AEAD (16-byte auth tags)
- **Pre-shared key** (optional): `-K <hex>` adds a PSK mixed into key derivation for quantum-resistant forward secrecy
- **Key confirmation**: PSK mismatch detection prevents silent decryption failure
- Direction-bound nonces (separate TX/RX keys) prevent reflection attacks
- CLI: `-E strict` (require encryption), `-E fast` (classical-first negotiation)
- Cryptographic library: Monocypher 4.0.2 (BSD-2, vendored)

### ARQ and Data Link

- **Pattern-based ACK**: Welch-Costas tone sequences replace LDPC-encoded ACK frames on slow modes, reducing turnaround from seconds to ~725 ms
- **HAIL beacon**: "I am Mercury" MFSK pattern for connectionless discovery. Directed HAIL adds a 4-tone CRC suffix derived from target callsign to prevent multi-station collisions.
- **SSID support**: Callsign-SSID addressing (e.g., `N0CALL-5`) for multi-station environments
- **B2F unroll/reroll**: Transparent stripping of Winlink LZHUF compression on TX (Mercury's PPMd/zstd compresses the raw payload better), deterministic re-encoding on RX
- **Frame completeness gating**: Detects when OFDM frames extend beyond captured audio and recaptures remaining symbols rather than dropping the block

### Passive Monitor Mode

Third-party stations can monitor ongoing sessions without participating in the ARQ protocol. The monitor decodes both sides of a conversation in real-time using a parallel OFDM decoder with independent LDPC abort flags. Useful for FCC compliance monitoring and debugging.

- CLI: `--monitor` (passive decode, no TX)
- Outputs decoded text to stdout or GUI monitor panel

### GUI

Cross-platform graphical interface (Dear ImGui + GLFW + OpenGL3):

- Real-time constellation diagram and waterfall spectrum
- Signal level meters, SNR display, compression ratio indicator
- Sound card selection, callsign/SSID configuration, network port settings
- Gearshift controls, encryption status, bandwidth mode selector
- Run headless with `-n`

### Performance

- **PocketFFT** (BSD): Replaces hand-rolled Cooley-Tukey for all demodulation FFTs
- **SIMD dispatch**: Runtime detection of AVX2+FMA / SSE4.2 / SSE2 for FFT butterfly operations
- **3-stage preamble detection**: Schmidl-Cox autocorrelation + matched-filter refinement + batch prediction from prior frame timing
- **RX mute guard**: Prevents TX echo feedback on shared audio devices (virtual cables, full-duplex radios)

## Mode Reference

All modes sorted by wideband PHY rate. NB PHY is approximately WB/5.

```
Mode          Modulation    LDPC    WB PHY    NB PHY    Es/N0
                            Rate     (bps)     (bps)    Waterfall
-----------   -----------   ----    ------    ------    ---------
ROBUST_0      32-MFSK       1/16       14        28    -13.0 dB
ROBUST_1      16-MFSK x2    1/16       22        37    -11.0 dB
CONFIG_0      BPSK          1/16       71        14    -10.0 dB
ROBUST_2      16-MFSK x2     1/4       87       149     -8.0 dB
CONFIG_1      BPSK          2/16      156        31     -7.5 dB
CONFIG_2      BPSK          3/16      241        48     -6.0 dB
CONFIG_3      BPSK          4/16      326        65     -4.5 dB
CONFIG_4      BPSK          5/16      411        82     -3.5 dB
CONFIG_5      BPSK          6/16      496        99     -2.5 dB
CONFIG_6      BPSK          8/16      665       133     -1.5 dB
CONFIG_7      QPSK          5/16      763       153     -0.5 dB
CONFIG_8      QPSK          6/16      920       184     +0.5 dB
CONFIG_9      QPSK          8/16     1235       247     +1.5 dB
CONFIG_10     8PSK          6/16     1354       271     +3.0 dB
CONFIG_11     8PSK          8/16     1818       364     +4.0 dB
CONFIG_12     QPSK         14/16     2261       452     +6.5 dB
CONFIG_13     8PSK         12/16     2471       494     +7.5 dB
CONFIG_14     8PSK         14/16     3390       678     +9.0 dB
CONFIG_15     16QAM        14/16     4361       872    +12.5 dB
CONFIG_16     32QAM        14/16     5665      1133    +13.5 dB
```

With streaming compression on English text, effective throughput reaches ~1.9x PHY rate (e.g., CONFIG_10 WB: 1354 PHY -> ~2573 effective bps).

## Getting Mercury

### Pre-built Binaries

**Windows:** Ready-to-run executables are available on the [GitHub Releases page](https://github.com/Rhizomatica/mercury/releases).

**Debian / Raspberry Pi OS:** A package repository is available for amd64 and arm64 (Debian 13 Trixie / Raspberry Pi OS). To install:

```
# Install the repository certificate
wget --no-check-certificate -qO- https://debian.hermes.radio/hermes/hermes.key | gpg --dearmor -o - | sudo tee /etc/apt/trusted.gpg.d/hermes.gpg > /dev/null

# For arm64 (Raspberry Pi, sBitx radio, etc.)
echo 'deb [arch=arm64] http://debian.hermes.radio/hermes trixie main' | sudo tee -a /etc/apt/sources.list.d/hermes.list

# For amd64 (laptop, desktop, etc.)
echo 'deb [arch=amd64] http://debian.hermes.radio/hermes trixie main' | sudo tee -a /etc/apt/sources.list.d/hermes.list

sudo apt update
sudo apt install mercury
```

## Building

Mercury is implemented in C++ (C++14). Builds on Linux (gcc/glibc) and Windows (MinGW64 POSIX toolchain).

### Dependencies

**Linux:**
```
apt-get install libasound2-dev libpulse-dev libglfw3-dev gnuplot-x11 graphviz
```

**Windows:** MinGW64 POSIX toolchain via MSYS2. GLFW is bundled in `third_party/glfw/`.

### Compile

Using `build.sh` (recommended, works on both platforms):
```
./build.sh release    # optimized build with GUI
./build.sh o3         # max optimization
./build.sh debug      # debug symbols, no optimization
```

Other build modes: `o0`, `o1`, `o2`, `asan`, `ubsan`.

Using `make` directly:
```
make clean && make -j4 GUI_ENABLED=1    # with GUI
make clean && make -j4 GUI_ENABLED=0    # headless only
```

To install:
```
make install
```

**Important**: Use a consistent compiler toolchain. Mixing object files from different GCC versions causes ABI crashes. `build.sh` always does a clean build.

### Documentation

```
apt-get install doxygen
make doc
```

## Usage

```
mercury -m [mode] [options]

Operating modes (-m):
  ARQ               Automatic Repeat Request (primary mode)
  MONITOR           Passive monitor — decode without transmitting
  TX_SHM / RX_SHM   Shared memory interface (see examples/)
  PLOT_BASEBAND     Baseband BER simulation (AWGN)
  PLOT_PASSBAND     Passband BER simulation (AWGN)
  TX_TEST / RX_TEST Test pattern transmission/reception
  TX_RAND / RX_RAND Random data transmission/reception

Device and audio:
  -i [device]       Audio capture device (e.g., "plughw:0,0" or device name from -z)
  -o [device]       Audio playback device
  -x [api]          Sound system: alsa, pulse, dsound, wasapi (default: alsa/wasapi)
  -A [channel]      Audio channel index override (enables multichannel mode)
  -r [radio]        Radio type: stockhf, sbitx
  -c [cpu_nr]       Pin to CPU core (-1 = auto, default)
  -C                Check audio config (stereo, sample rate) before starting
  -z                List available sound devices

Modulation and bandwidth:
  -s [config]       Modulation: 0-16 (OFDM), 100-102 (ROBUST MFSK). Use -l to list.
  -g                Enable adaptive gearshift
  -R                Enable ROBUST mode (MFSK weak-signal hailing)
  -M [auto|nb]      Bandwidth: auto (NB hail + WB upgrade) or nb (500 Hz only)
  -N                Force narrowband mode (500 Hz, 10 subcarriers)
  -W                Force wideband mode (2344 Hz, 50 subcarriers)
  -I [5-50]         LDPC decoder max iterations (default: 50)
  -l                List all modulation/coding modes

ARQ tuning:
  -p [port]         TCP base port (control=port, data=port+1). Default: 7002
  -t [ms]           Connection timeout (default: 15000)
  -a [attempts]     Max connection attempts (default: 15)
  -k [ms]           Link timeout (default: 30000)
  -e                Exit on client disconnect

Compression and encryption:
  -F [on|off]       Force compression on/off (default: auto-detect B2F)
  -E [strict|fast]  Encryption: strict (require PQ KX) or fast (classical-first)
  -K [hex]          Pre-shared key for encryption (hex, up to 64 bytes)

Gain and calibration:
  -T [dB]           TX gain override (e.g., -T -25.6)
  -G [dB]           RX gain override (e.g., -G 25.6)
  -B [gain]         NB MFSK boost override
  -Q [n]            NB probe max (0=disable, default 2)

OFDM tuning:
  --gi [ms]         Guard interval in ms (1.0-8.0, default 3.0). Both sides must match.

Testing and debug:
  -Z [snr_dB]       Inject AWGN noise at specified SNR
  -f [hz]           TX carrier offset for frequency sync testing
  -P [nBits]        Punctured LDPC BER test with specified ctrl_nBits
  -v                Verbose debug output
  --stdout          Output decoded plaintext to stdout (monitor mode)
  --log [file]      Redirect output to log file (for HF field debugging)

General:
  -n                Disable GUI (headless mode)
  -h                Print this help
```

### Examples

ARQ with gearshift on a stock HF radio:
```
mercury -m ARQ -g -r stockhf -i "plughw:0,0" -o "plughw:0,0"
```

Narrowband ARQ on Windows with virtual cable:
```
mercury -m ARQ -g -R -M nb -x wasapi -i "CABLE Output (VB-Audio Virtual Cable)" -o "CABLE Input (VB-Audio Virtual Cable)"
```

Encrypted session with pre-shared key:
```
mercury -m ARQ -g -R -E strict -K 0123456789abcdef0123456789abcdef -x wasapi
```

BER simulation for CONFIG_10:
```
mercury -m PLOT_PASSBAND -s 10
```

List sound devices:
```
mercury -z
```

## Testing and Benchmarking

The `tools/` directory contains test and benchmark scripts. These require Python 3 and a virtual audio cable (e.g., VB-Audio Virtual Cable on Windows).

**Benchmark suite** (`tools/mercury_benchmark.py`): SNR sweep, stress test, and adaptive gearshift test. See `tools/BENCHMARK_GUIDE.md`.

```
pip install numpy sounddevice matplotlib
python tools/mercury_benchmark.py sweep --configs 100,0,8,16 --measure-duration 60
python tools/mercury_benchmark.py stress --num-bursts 5
python tools/mercury_benchmark.py adaptive --measure-duration 60
```

**Bisect benchmark** (`tools/bisect_benchmark.py`): Automated throughput regression testing across git commits for NB and WB configs.

**Loopback test** (`tools/robust_loopback_test.py`): Quick ARQ sanity check for ROBUST modes.

```
python tools/robust_loopback_test.py 100    # ROBUST_0
python tools/robust_loopback_test.py 101    # ROBUST_1
python tools/robust_loopback_test.py 102    # ROBUST_2
```

## Supported Clients

Mercury's ARQ mode uses a VARA-compatible TCP protocol (control on base port, data on base+1). Compatible clients include:

- **Built-in GUI**: Real-time monitoring and configuration (enabled by default, `-n` for headless)
- **mercury-connector**: Simple ARQ client with hamlib support — https://github.com/Rhizomatica/mercury-connector
- **HERMES-BROADCAST**: Data broadcast using RaptorQ codes — https://github.com/Rhizomatica/hermes-broadcast
- **hermes-net UUCP**: UUCP integration — https://github.com/Rhizomatica/hermes-net/tree/main/uucpd
- **VARA clients**: Any VARA-compatible client should work. Base TCP port is 7002.

For the sBitx radio, use the HERMES software stack: https://github.com/Rhizomatica/hermes-net (trx_v2-userland).

## Discussion

Join the HERMES mailing list: https://lists.riseup.net/www/info/hermes-general

## About

Mercury was initially written by Fadi Jerji for Rhizomatica's HERMES project, and currently is developed by Kameron Markham and Rhizomatica's HERMES team.

This project is sponsored by ARDC.

## License

GNU Affero General Public License v3.0
