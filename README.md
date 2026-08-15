# Mercury — HERMES HF Modem

Mercury is part of the [HERMES project](https://www.rhizomatica.org/hermes/)
(High-Frequency Emergency and Rural Multimedia Exchange System) by
[Rhizomatica](https://www.rhizomatica.org/), funded by
[ARDC](https://www.ardc.net/) and others.

Mercury is a software modem for sending email, files and messages over HF
radio — the kind of link you have when there is no internet, no phone network
and no power grid worth speaking of.

**Start here:** [Getting Started](#getting-started-with-mercury) ·
[Graphical interfaces](#graphical-interfaces) · [Configuration](#configuration-file) ·
[Docs](https://rhizomatica.github.io/mercury/) ·
[Mailing list](https://lists.riseup.net/www/info/hermes-general)

There are currently two versions:

- **Mercury v2** (this branch) — a complete rewrite in C with a new ARQ data link. **This is the recommended version.**
- **[Mercury v1](https://github.com/Rhizomatica/mercury/tree/mercuryv1)** — the original Mercury modem written in C++. Legacy; use only if you know what you are doing.

## What this software does

Mercury turns a single-sideband radio into a data link. It handles the
waveform, the error correction, the retransmissions and the radio keying, and
presents the result to your application as an ordinary TCP connection.

- **ARQ data link for point-to-point sessions** — connect/accept handshake, ACK and retry logic, and controlled disconnect, so a file either arrives intact or you are told it did not.
- **Adaptive speed** — the link starts robust and climbs through six payload modes as conditions allow (DATAC15 → DATAC4 → DATAC3 → DATAC1 → DATAC17 → QAM16C2), stepping back down when the channel fades. Control frames always ride the most robust mode, DATAC16, so signalling survives when payload cannot.
- **Broadcast mode** alongside ARQ, with its own framing and TCP ingress port — used for one-to-many traffic and for [Reticulum](#reticulum) mesh networking.
- **VARA-style TCP TNC interface** on two sockets (control on the base port, data on base+1), speaking `MYCALL`, `LISTEN`, `CONNECT`, `BUFFER`, `SN`, `BITRATE` and `TUNE` (a 1 kHz ATU tuning carrier with a hard 60 s unkey timer). Existing VARA-aware software can generally talk to Mercury unchanged.
- **Runs on the audio hardware you have** — `alsa`, `pulse`, `oss`, `coreaudio`, `aaudio`, `dsound`, `wasapi`, plus `shm`, `null` and `fifo` for embedded and test use.
- **Keys the radio for you** via Hamlib CAT or the HERMES shared-memory interface — or stays out of the way and lets your client do it.

Mercury runs on Linux, Windows and macOS, including Raspberry Pi.

## Command-line reference

Most settings live in `mercury.ini` (see [Configuration File](#configuration-file));
command-line flags override it. Run `./mercury -h` for this list at any time.

<details>
<summary><b>All command-line options</b> (click to expand)</summary>

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

</details>

Mode behavior notes:
- `-m` / `-s` affects **broadcast** and **test** modes only.
- During an active ARQ link, control frames always use DATAC16, and the payload
  starts at the robust end of the ladder (DATAC15), climbing through DATAC4,
  DATAC3, DATAC1, DATAC17 and QAM16C2 as the channel allows.
- VARA `BW500` keeps the link narrow; `BW2300` and `BW2750` allow the full
  Mercury payload-mode ladder.
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

### Build targets

| Target | Description |
|--------|-------------|
| `make` / `make all` | Build the standalone CLI `mercury` binary |
| `make libmercury_core.a` | Compile all Mercury C objects into a host-arch static library (used by `fyne-ui`) |
| `make fyne-ui` | Build the single-binary native GUI (`mercury-ui`), embedding the engine via CGo. Requires `make libmercury_core.a` first. **Needs Go** ([setup](#gui-build-prerequisites-go--fyne)). |
| `make libmercury_core_w64.a` | Cross-compile Mercury C objects for Windows x64 (requires `gcc-mingw-w64-x86-64`) |
| `make fyne-ui-windows` | Cross-compile the single-binary GUI to `mercury-ui.exe`. Depends on `libmercury_core_w64.a`. **Needs Go** ([setup](#gui-build-prerequisites-go--fyne)). |
| `make windows-installer` | Full Windows installer pipeline: cross-compile engine + GUI, copy DLLs, patch config. Output goes to `windows-installer/`. Requires MinGW + Go. |
| `make windows-zip` | Build standalone CLI + GUI Windows binaries and zip them for distribution |
| `make fyne-ui-macos` | Package `Mercury.app` for the host architecture (run on macOS). **Needs Go + the `fyne` CLI** ([setup](#gui-build-prerequisites-go--fyne)). |
| `make fyne-ui-macos-dmg` | Wrap the host-arch `Mercury.app` in a drag-to-install `Mercury.dmg` at the repo top level. **Needs Go + the `fyne` CLI** ([setup](#gui-build-prerequisites-go--fyne)). |
| `make fyne-ui-macos-universal-dmg` | **Distribution build:** universal (x86_64 + arm64) `Mercury.app` wrapped in `Mercury.dmg` at the repo top level. This is the artifact for the website. **Needs Go + the `fyne` CLI** ([setup](#gui-build-prerequisites-go--fyne)). |

### Build the Fyne GUI (Linux)

```bash
sudo apt install golang-go libhamlib-dev libpulse-dev libasound2-dev
make fyne-ui
./mercury-ui
```

### Cross-compile the Windows installer (from Linux)

```bash
sudo apt install golang-go gcc-mingw-w64-x86-64 mingw-w64-x86-64-dev
make windows-installer
# → windows-installer/ contains mercury-ui.exe + DLLs + mercury.ini
# Use Inno Setup Compiler on Windows to build the .exe installer from installer.iss
```

   The FreeDV codec is vendored in-tree — no external FreeDV or codec2 packages are needed.

### GUI build prerequisites (Go + fyne)

Every `fyne-ui*` target builds a Go program, so **Go is required** — the CLI
`mercury` binary needs none of this. The macOS packaging targets additionally
need the `fyne` command-line tool:

```bash
# Go: https://go.dev/dl/  (or `brew install go` on macOS, `apt install golang` on Debian)
go install fyne.io/tools/cmd/fyne@latest

# `go install` drops binaries in $(go env GOPATH)/bin, which is NOT on PATH by
# default.  Without this you get: error: 'fyne' not found
export PATH="$PATH:$(go env GOPATH)/bin"     # add to ~/.zshrc or ~/.bash_profile
```

Check both are visible before building:

```bash
go version && fyne version
```

### Build the macOS app for distribution (universal .dmg)

Run on macOS. This produces the self-contained, universal (Intel + Apple
Silicon) `Mercury.dmg` used for the website — hamlib and libusb are vendored as
static universal libraries in-tree, so no Homebrew dependencies are needed at
build or run time.

```bash
# one-time tools:
brew install go
go install fyne.io/tools/cmd/fyne@latest
export PATH="$PATH:$HOME/go/bin"        # so `fyne` is on PATH (add to ~/.bash_profile)

make fyne-ui-macos-universal-dmg
# → ./Mercury.dmg  (universal: x86_64 + arm64)
```

The finished `Mercury.dmg` lands at the repository top level, ready to upload.
It is unsigned, so on first launch Gatekeeper warns — right-click the app →
**Open** to run it. For a quick host-architecture-only build use
`make fyne-ui-macos-dmg` (also emits `./Mercury.dmg`).

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

### Install on Windows

**Via installer (recommended):** Download the `Mercury_HF_Modem_Setup.exe` from the releases page, run it, and the single-binary GUI (`mercury-ui.exe`) will be installed. The GUI launches the Mercury engine in-process — no separate backend needed. Desktop and Start Menu shortcuts are created automatically.

**Via ZIP:** Download the ZIP package from the releases page, extract it, and run `mercury-ui.exe` for the GUI or `mercury.exe` for the headless CLI.

### Install on macOS

Download `Mercury.dmg` from the website, open it, and drag **Mercury** into
**Applications**. The build is universal (runs natively on Intel and Apple
Silicon) and self-contained (no Homebrew needed). It is unsigned, so on first
launch right-click **Mercury** → **Open** to get past Gatekeeper.

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

Mercury builds on the FreeDV / codec2 OFDM data modes developed by David Rowe
and contributors, and **extends them with waveforms and link-layer techniques
developed by Rhizomatica** for the conditions HERMES actually operates in:
long NVIS and regional paths, marginal signal levels, and fading.

Four of the modes Mercury uses are ours, not upstream:

| Mode | Bandwidth | Payload | What it is for |
|---|---|---|---|
| **DATAC15** | 200 Hz | 30 B | Fringe data. Rate-1/3 LDPC, 3 carriers — reaches below the floor of any stock data mode |
| **DATAC16** | 200 Hz | 14 B | Control and ACK at the fringe; the mode the whole session depends on |
| **DATAC17** | 2100 Hz | 1180 B | Intermediate SNR, roughly 2× the goodput of DATAC1 |
| **QAM16C2** | 2100 Hz | 1213 B | Good channels, roughly 2.9× DATAC1 |

Beyond the waveforms, the link layer adds:

- **HARQ with Chase combining** — a failed frame is not thrown away. Repeats are soft-combined with the original, so a frame that fails twice can still decode from the pair. At the fading cliff, where a single-shot link delivers essentially nothing, this recovers around half the frames ([measured](docs/HARQ-FINDINGS.md): 0/130 → 61/130 at −5.8 dB).
- **Outer-loop link adaptation (OLLA)** — closes the loop on observed delivery rather than SNR estimates alone, which stops the mode ladder oscillating on a fading path.
- **Per-mode SNR calibration**, so gear-shifting decisions are made on numbers that mean the same thing across modes.

Every mode figure above is measured on a calibrated bench (Watterson fading
channel, 100-burst trials) that reproduces the published upstream numbers
before it is trusted for ours. See [docs/MODES.md](docs/MODES.md) for the full
tables, methodology, and the negative results.

Work continues on porting the remaining Mercury v1 modulators, including a
32-tone MFSK mode for the deepest fringe conditions.

## Graphical Interfaces

Mercury has three **fully supported** interfaces. All three are maintained by
the HERMES team and all three are first-class — pick whichever suits how you
operate.

**Built-in GUI (Go / Fyne)** — *the simplest way to run Mercury.*
The engine and the interface are one binary: no separate modem process to
start, no ports to wire up. Waterfall and spectrum, telemetry, radio and audio
settings, and a **Mercury Client** chat window for ARQ and broadcast messaging
built straight in. Ships as an installer on Windows, a universal `.dmg` on
macOS, and `make fyne-ui` on Linux.

**Web interface** — runs in a browser, nothing to install, and works against a
Mercury running on another machine (a Raspberry Pi at the antenna, say).
Included in `docs/app/`, or use it directly at
https://rhizomatica.github.io/mercury/app/

**Mercury-qt** — the native Qt desktop client:
https://github.com/Rhizomatica/mercury-qt

The web and Qt interfaces talk to the engine over its WebSocket interface
(`-G`); the built-in GUI links the engine directly, so it needs no flags.

Community interfaces also exist:
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
