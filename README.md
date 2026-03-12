# Mercury — HERMES HF Modem

Mercury is part of the [HERMES project](https://www.rhizomatica.org/hermes/)
(High-Frequency Emergency and Rural Multimedia Exchange System) by
[Rhizomatica](https://www.rhizomatica.org/), funded by
[ARDC](https://www.ardc.net/) and others.

There are currently two versions:

- **[Mercury v1](https://github.com/Rhizomatica/mercury/tree/mercuryv1)** — the original Mercury modem written in C++.
- **Mercury v2** (this branch) — a complete rewrite in C with a new ARQ data link.

A Qt-based GUI is under development: [mercury-qt](https://github.com/Rhizomatica/mercury-qt)

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
- **Audio modem operation over multiple backends** (`alsa`, `pulse`, `dsound`, `wasapi`, `shm`) with split RX/TX modem orchestration.
- **Direct radio control** via HAMLIB or HERMES shared-memory interface for direct PTT keying.

```
Usage modes:
./mercury -m [mode_index] -i [device] -o [device] -x [sound_system] -p [arq_tcp_base_port] -b [broadcast_tcp_port] -f [freedv_verbosity] -k [rx_input_channel] [-G] [-u ui_ip] [-U ui_base_port] [-W]
./mercury [-h -l -z]

Options:
 -c [cpu_nr]                Run on CPU [cpu_nr]. Use -1 to disable CPU selection, which is the default.
 -m [mode_index]            Startup payload mode index shown in "-l" output. Used for broadcast and idle/disconnected ARQ decode. Default is 1 (DATAC3).
 -s [mode_index]            Legacy alias for -m.
 -f [freedv_verbosity]      FreeDV modem verbosity level (0..3). Default is 0.
 -k [rx_input_channel]      Capture input channel: left, right, or stereo. Default is left.
 -i [device]                Radio Capture device id (eg: "plughw:0,0").
 -o [device]                Radio Playback device id (eg: "plughw:0,0").
 -x [sound_system]          Sets the sound system or IO API to use: alsa, pulse, dsound, wasapi or shm. Default is alsa on Linux and dsound on Windows.
 -p [arq_tcp_base_port]     Sets the ARQ TCP base port (control is base_port, data is base_port + 1). Default is 8300.
 -b [broadcast_tcp_port]    Sets the broadcast TCP port. Default is 8100.
 -G                         Enable UI communication (UDP status/spectrum/command sockets for mercury-qt). Off by default.
 -u [ui_ip]                 Sets the UI IP address. Default is 127.0.0.1. Requires -G.
 -U [ui_base_port]          Sets the UI base port (TX is base_port, RX is base_port + 1, spectrum is base_port + 2). Default is 10000. Requires -G.
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
- With no `-R`, `-A`, or `-S`, Mercury does **not** key the radio directly; it leaves for the tcp client the radio keying task.
- `-R` selects a HAMLIB model ID, `-A` optionally points HAMLIB at a device path or `ip:port`, and `-K` prints the available HAMLIB models.
- `-S` selects the HERMES shared-memory controller interface, is mutually exclusive with `-A`, and is unavailable on Windows builds.

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

## Compilation

Edit config.mk with your C compiler and appropriate flags (defaults should be fine for most) and type:

```
make
```

## API documentation (Doxygen)

Online HTML docs: https://rhizomatica.github.io/mercury/

If you have `doxygen` installed, you can generate HTML documentation for the ARQ subsystem:

```
make doxygen
```

Output will be generated in `docs/html/` (open `docs/html/index.html` in a browser). To remove generated docs:

```
make doxygen-clean
```

## Logging and timing traces

- Default run (`./mercury`): logger runs at **INFO** level with timestamps (`[INF]/[WRN]/[ERR]`).
- Verbose run (`./mercury -v`): logger runs at **DEBUG** level and includes all detailed ARQ/modem traces (`[DBG]` and `[TMG]`).
- `./mercury -v -L /tmp/session.log` — write full DEBUG+TIMING log to file.
- `./mercury -v -L /tmp/session.log -J` — same, but in **JSONL** format for machine parsing with `jq`.
- TX state transitions are logged with timestamps at INFO level as:
  - `TX enabled (PTT ON)`
  - `TX disabled (PTT OFF)`

See [docs/ARQ.md](docs/ARQ.md) for full ARQ architecture, protocol reference, and OTA tuning guide.

## Physical Layer

Mercury v2 currently uses FreeDV modulator code developed by David Rowe. We plan to introduce other modulator modes present in Mercury v1.

## About

Mercury v2 is developed by Rhizomatica's HERMES team, namely:

- Rafael Diniz (ARQ, Broadcast, TCP interface, etc)
- Pedro Messetti (Testing framework, general improvements, etc)
- Matheus Thibau (Graphical User Interface)

This project is sponsored by ARDC.

## LICENSE

Please check LICENSE and LICENSE-freedv.
