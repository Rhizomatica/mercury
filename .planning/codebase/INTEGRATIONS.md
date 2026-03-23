# Integrations — Mercury (HERMES HF Modem)

## TCP TNC Interface (VARA-compatible)

Mercury exposes a **VARA-style TCP TNC interface** for host applications:

- **Control socket**: `base_port` (default 8300) — text commands/status
- **Data socket**: `base_port + 1` (default 8301) — binary payload
- Commands: `MYCALL`, `LISTEN ON/OFF`, `CONNECT`, `DISCONNECT`, `BW`, `BUFFER`, `SN`, `BITRATE`, `CQFRAME`, `PUBLIC`, `RETRY`
- Status events: `CONNECTED`, `DISCONNECTED`, `PENDING`, `CANCELPENDING`, `BUFFER`, `SN`, `BITRATE`, `CQFRAME`
- Implemented in `data_interfaces/tcp_interfaces.c` and `data_interfaces/net.c`

## Broadcast TCP Interface

- **Broadcast port**: default 8100
- Separate TCP server for broadcast (one-way) data
- KISS framing in `datalink_broadcast/kiss.c`
- Used for store-and-forward email/file distribution

## WebSocket GUI Interface

- **WebSocket server** for `mercury-qt` GUI (default port 10000)
- Optional TLS (WSS) support
- Publishes: modem status (SNR, bitrate, callsigns, direction, sync), spectrum/waterfall data (~20fps), soundcard lists, radio lists
- Receives: UI commands (callsign changes, connect/disconnect, audio restart, radio config)
- Library: Mongoose (`gui_interface/websocket/mongoose.c`)
- Protocol: JSON messages over WebSocket frames
- Implemented in `gui_interface/ui_communication.c` and `gui_interface/websocket/mercury_websocket.c`

## Radio Control

Two mutually exclusive radio control backends:

### HAMLIB
- Industry-standard radio control library
- PTT keying, radio model enumeration
- Configured via `-R` (model ID) and `-A` (device path or ip:port)
- Implemented in `radio_io/rigctl_parse.c` (HAMLIB command parsing) and `radio_io/radio_io.c`

### HERMES Shared Memory (SHM)
- Direct shared-memory interface to sBitx and other HERMES radios
- Linux-only (`HAVE_HERMES_SHM` compile flag)
- PTT keying via shared memory segments
- Implemented in `radio_io/sbitx_io.c` and `radio_io/shm_utils.c`

## Audio I/O

Multi-backend audio via ffaudio library:

| Backend | Platform | Flag |
|---------|----------|------|
| ALSA | Linux | default on Linux |
| PulseAudio | Linux | `-x pulse` |
| DirectSound | Windows | default on Windows |
| WASAPI | Windows | `-x wasapi` |
| OSS | FreeBSD/Linux | `-x oss` |
| CoreAudio | macOS | `-x coreaudio` (unsupported marker) |
| AAudio | Android | `-x aaudio` (unsupported marker) |
| SHM | Linux (HERMES) | `-x shm` — shared memory audio bypass |

## FreeDV Modem

- Vendored codec2/FreeDV library in `modem/freedv/`
- Modes: DATAC0, DATAC1, DATAC3, DATAC4, DATAC13, DATAC14, FSK_LDPC
- OFDM modulation for data modes, FSK for experimental mode
- LDPC forward error correction
- Accessed via `freedv_api.h`

## No External Network Services

Mercury has **no internet-facing integrations** — all communication is over HF radio links and local TCP/WebSocket connections. This is by design for rural and emergency deployment.
