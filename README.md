# Mercury — HERMES HF Modem

Mercury is part of the [HERMES project](https://www.rhizomatica.org/hermes/)
(High-Frequency Emergency and Rural Multimedia Exchange System) by
[Rhizomatica](https://www.rhizomatica.org/), funded by
[ARDC](https://www.ardc.net/) and others.

There are currently two versions:

- **Mercury v2** (this branch) — a complete rewrite in C with a new ARQ data link. **This is the recommended version.**
- **[Mercury v1](https://github.com/Rhizomatica/mercury/tree/mercuryv1)** — the original Mercury modem written in C++. Legacy; use only if you know what you are doing.

A Qt-based GUI is available: [mercury-qt](https://github.com/Rhizomatica/mercury-qt)

Mailing list: https://lists.riseup.net/www/info/hermes-general

## Mercury v2

Mercury v2 is a complete rewrite of the HERMES modem ARQ data link,
replacing the monolithic state machine with a modular reactor
architecture featuring per-direction mode selection, hybrid
SNR + delivery-feedback gear-shifting, split control/data channel
design (DATAC13 for signaling, DATAC4/DATAC3/DATAC1 for payload),
and a persistent FreeDV mode pool eliminating codec re-initialization
overhead. Built for reliable store-and-forward email and file transfer
over HF radio links in rural and emergency scenarios.

## What this software does

- **ARQ data link for P2P sessions** with connect/accept handshake, ACK/retry logic, keepalive, and controlled disconnect.
- **Adaptive payload "gear-shifting"** (DATAC4/DATAC3/DATAC1) driven by link quality and backlog, with DATAC13 used for control signaling.
- **Per-direction mode selection**: each path (A→B and B→A) negotiates its mode independently based on local SNR.
- **Broadcast data mode** in parallel to ARQ, with dedicated broadcast framing and TCP ingress port.
- **VARA-style TCP TNC interface** with separate control and data sockets (base port and base+1), including commands/status like `MYCALL`, `LISTEN`, `CONNECT`, `BUFFER`, `SN`, and `BITRATE`.
- **Audio modem operation over multiple backends** (`alsa`, `pulse`, `oss`, `coreaudio`, `aaudio`, `dsound`, `wasapi`, `shm`, `null`, `fifo`) with split RX/TX modem orchestration.
- **Direct radio control** via HAMLIB or HERMES shared-memory interface for direct PTT keying.

```
Usage modes:
./mercury -m [mode_index] -i [device] -o [device] -x [sound_system] -p [arq_tcp_base_port] -b [broadcast_tcp_port] -f [freedv_verbosity] -H [hamlib_log_level] -k [rx_input_channel] [-G] [-T] [-U ui_port] [-W] [-C config_file]
./mercury [-h -l -z]

Options:
 -c [cpu_nr]                Run on CPU [cpu_nr]. Use -1 to disable CPU selection, which is the default.
 -m [mode_index]            Startup payload mode index shown in "-l" output. Used for broadcast and idle/disconnected ARQ decode. Default is 1 (DATAC3).
 -s [mode_index]            Legacy alias for -m.
 -f [freedv_verbosity]      FreeDV modem verbosity level (0..3). Default is 0.
 -H [hamlib_log_level]      Hamlib radio log level (0..6). Default is 0.
 -k [rx_input_channel]      Capture input channel: left, right, or stereo. Default is left.
 -i [device]                Radio Capture device id (eg: "plughw:0,0").
 -o [device]                Radio Playback device id (eg: "plughw:0,0").
 -x [sound_system]          Sets the sound system or IO API to use: alsa, pulse, oss, coreaudio, aaudio, dsound, wasapi, shm, null or fifo. Default is alsa on Linux, dsound on Windows.
                             null and fifo are developer/test backends; fifo uses raw s32le PCM at 8 kHz via -i/-o paths.
 -p [arq_tcp_base_port]     Sets the ARQ TCP base port (control is base_port, data is base_port + 1). Default is 8300.
 -b [broadcast_tcp_port]    Sets the broadcast TCP port. Default is 8100.
 -G                         Enable UI communication (WebSocket status/spectrum/command interface for mercury-qt). Off by default.
 -T                         Use WSS (WebSocket Secure/TLS) for UI communication. Requires -G. Default uses plain WS (no TLS).
 -U [ui_port]               Sets the UI port (WebSocket port). Default is 10000. Requires -G.
 -W                         Disable waterfall/spectrum data sent to the UI (saves CPU). Requires -G.
 -l                         Lists all modulator/coding modes.
 -z                         Lists all available sound cards.
 -v                         Verbose mode. Prints more information during execution.
 -L [path]                  Write log to file (TIMING level and above).
 -J                         Write log file in JSONL format (requires -L).
 -R [radio_model]           Sets HAMLIB radio model.
 -A [radio_address]         Sets HAMLIB radio device file or ip:port address.
 -S                         Use HERMES shared memory radio control (Linux-only; do not use with -R and -A).
 -K                         List HAMLIB supported radio models.
 -C [path]                  Path to INI configuration file (default: mercury.ini in the current directory).
 -t                         Test TX mode.
 -r                         Test RX mode.
 -h                         Prints this help.
```

Mode behavior notes:
- `-m` / `-s` affects **broadcast** and **test** modes only.
- During an active ARQ link, control frames use DATAC13 and ARQ payload starts in DATAC4 (then may adapt to DATAC3/DATAC1).
- VARA `BW500` blocks DATAC1; `BW2300` and `BW2750` both allow the full Mercury
  payload-mode ladder.
- `CALL` advertises the local BW token and `ACCEPT` returns the negotiated
  session token. If either side uses `BW500`, the link stays narrow; `BW2750`
  is preserved in `CONNECTED ... BW` only when both peers advertise it.
- `FSK_LDPC` is currently **experimental** (mainly for lab/test usage), may have longer decode/sync latency depending on setup, and is not recommended for production links yet.

Radio control notes:
- With no `-R`, `-A`, or `-S`, Mercury does **not** key the radio directly; it leaves the radio keying task to the TCP client.
- `-R` selects a HAMLIB model ID, `-A` optionally points HAMLIB at a device path or `ip:port`, and `-K` prints the available HAMLIB models.
- `-S` selects the HERMES shared-memory controller interface, is mutually exclusive with `-A`, and is unavailable on Windows builds.

## Getting Started with Mercury

### Compile and Install from Git repository on Linux

1. Install the dependencies:
    ```bash
    sudo apt-get update && sudo apt-get install build-essential pkg-config libasound2-dev libpulse-dev libhamlib-dev make git
    ```

2. Clone Mercury GitHub repository:
    ```bash
    git clone https://github.com/Rhizomatica/mercury.git
    ```

3. Change directory to `mercury/`:
    ```bash
    cd mercury
    ```

4. Build mercury (edit `config.mk` first if you need a custom compiler or flags; defaults are fine for most):
    ```bash
    make
    ```

5. Install system-wide:
   ```bash
   sudo make install
   ```

   The FreeDV codec is vendored in-tree — no external FreeDV or codec2 packages are needed.

### Install via Debian package on Linux

For now just Debian 13 (Trixie) packages are built, for both arm64 (works on both RaspberryPi OS and Debian) and amd64.

1. Install the repository certificate:
    ```bash
    wget --no-check-certificate -qO- https://debian.hermes.radio/hermes/hermes.key | gpg --dearmor -o - | sudo tee /etc/apt/trusted.gpg.d/hermes.gpg > /dev/null
    ```

2. Add mercury to the sources list (change it for your current architecture):

    **ARM64:**

    ```bash
    echo 'deb [arch=arm64] http://debian.hermes.radio/hermes trixie main' | sudo tee /etc/apt/sources.list.d/hermes.list
    ```
    **AMD64:**
    ```bash
    echo 'deb [arch=amd64] http://debian.hermes.radio/hermes trixie main' | sudo tee /etc/apt/sources.list.d/hermes.list
    ```

3. Update the Debian packages:
    ```bash
    sudo apt-get update
    ```

4. Install mercury:
    ```bash
    sudo apt-get install mercury
    ```

**Note:** Installation via Debian package requires Debian 13 (Trixie)

### Install ZIP package on Windows

1. Navigate to the releases page on the official GitHub repository: https://github.com/Rhizomatica/mercury/releases
2. Download the ZIP package for the latest version
3. Go to Downloads folder (or the folder where you downloaded the Mercury ZIP) and extract the files
4. Click on the `mercury` executable file (``mercury.exe``) to run Mercury HF modem

## Configuration File

Mercury reads an INI-format configuration file at startup. The default path is `mercury.ini` in the current working directory; use `-C` to specify an alternative path. Command-line arguments take priority over values from the file.

See the included [mercury.ini.example](mercury.ini.example) for all available settings and their default values — copy it to `mercury.ini` and edit as needed.

## Documentation

Online HTML docs: https://rhizomatica.github.io/mercury/

## Logging and timing traces

- Default run (`./mercury`): logger runs at **INFO** level with timestamps (`[INF]/[WRN]/[ERR]`).
- Verbose run (`./mercury -v`): logger runs at **DEBUG** level and includes all detailed ARQ/modem traces (`[DBG]` and `[TMG]`).
- `./mercury -v -L /tmp/session.log` — write full DEBUG+TIMING log to file.
- `./mercury -v -L /tmp/session.log -J` — same, but in **JSONL** format for machine parsing with `jq`.
- TX state transitions are logged with timestamps at INFO level as:
  - `TX enabled (PTT ON)`
  - `TX disabled (PTT OFF)`

See [docs/ARQ.md](docs/ARQ.md) for full ARQ architecture, protocol reference, and OTA tuning guide.

## Reticulum

Mercury can carry [Reticulum](https://github.com/markqvist/reticulum) mesh
networking over HF via its KISS-over-TCP broadcast port (verified) or as a
point-to-point ARQ backbone. See [docs/RETICULUM.md](docs/RETICULUM.md) for
the integration architectures and configuration.

## Physical Layer

Mercury v2 currently uses FreeDV modulator code developed by David Rowe. We plan to introduce other modulator modes present in Mercury v1.

## Graphical Interfaces

Mercury v2 has two interfaces:
- **Mercury-qt** (desktop): https://github.com/Rhizomatica/mercury-qt
- **Web-based**: located in `docs/app/` in this repository, and accessible via https://rhizomatica.github.io/mercury/app/

Also, community interfaces also exist:
- **Mercury-tk**: https://github.com/odorajbotoj/mercury-tk/

## About

Mercury v2 is developed by Rhizomatica's HERMES team, namely:

- Rafael Diniz (ARQ, Broadcast, TCP interface, etc)
- Pedro Messetti (Testing framework, general improvements, etc)
- Matheus Thibau (Graphical User Interface)

This project is sponsored by ARDC.

## LICENSE

Mercury is free software, licensed under the **GNU General Public License,
version 3 or (at your option) any later version** (GPL-3.0-or-later). See the
LICENSE file and the per-file headers.

Mercury bundles third-party components which carry their own licenses when
taken separately:

- `modem/freedv` — a subset of FreeDV / codec2, **LGPL-2.1** (see LICENSE-freedv)
- `common/iniparser` — **MIT** (see common/iniparser/LICENSE)
- `audioio/ffaudio` and `ffbase` — **Unlicense** / public domain (see audioio/ffaudio/UNLICENSE)
- Windows binary releases link against Hamlib — **LGPL-2.1** (see radio_io/hamlib-w64/COPYING.LIB.txt)

The combined work (the Mercury binary) is distributed under the terms of the
GPL-3.0-or-later. The LGPL/MIT/Unlicense terms apply to those components only
when they are used separately from Mercury.
