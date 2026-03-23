# Stack — Mercury (HERMES HF Modem)

## Language & Standard

- **C (GNU C11)** — `std=gnu11` via `config.mk`
- **Python 3** — companion GUI (`mercury-qt/`) uses PySide6 + numpy
- No C++ in mercury core (v1 was C++; v2 is a complete rewrite in C)

## Build System

- **GNU Make** with `config.mk` shared configuration
- Cross-platform: Linux (primary), Windows (MinGW64 cross-compile), macOS, FreeBSD
- Platform auto-detection for aarch64 (Raspberry Pi 4/5 `-mcpu` tuning)
- `make windows` / `make windows-zip` for Windows cross-build
- Debian packaging support in `debian/`

## Core Dependencies

| Dependency | Purpose | Notes |
|------------|---------|-------|
| **FreeDV** (codec2) | OFDM/FSK modem, LDPC coding | Vendored in `modem/freedv/`, built as `libfreedvdata.a` |
| **ffaudio** | Multi-platform audio I/O | Vendored in `audioio/ffaudio/`, supports ALSA, PulseAudio, DirectSound, WASAPI, OSS, CoreAudio, AAudio, SHM |
| **HAMLIB** | Radio transceiver control (PTT) | Optional, detected via `pkg-config` or bundled Windows libs |
| **Mongoose** | WebSocket server for GUI | Vendored in `gui_interface/websocket/mongoose.c` |
| **pthreads** | Threading (POSIX) | Core threading model |

## System Libraries (Linux)

- `libpulse` — PulseAudio
- `libasound` — ALSA
- `libm` — Math
- `librt` — Real-time extensions (ring buffers)
- `libpthread` — Threading

## System Libraries (Windows)

- `ole32`, `dsound`, `dxguid` — DirectSound audio
- `ws2_32` — Winsock2 networking
- `libwinpthread` — Threading

## Companion Projects

| Project | Language | Purpose |
|---------|----------|---------|
| `mercury-qt` | Python 3 / PySide6 | Qt-based GUI, connects via WebSocket to mercury backend |

## Runtime Configuration

- All configuration via CLI flags (`getopt`)
- No config files — runtime state only
- Audio device selection, TCP ports, radio control all via command-line
- Version: `1.9.5` (defined in `main.c`)
- Git hash embedded at build time via `GIT_HASH` macro

## Compiler & Flags

- Default: `gcc` with `-Wall -O2 -std=gnu11 -pthread -D_GNU_SOURCE`
- aarch64: `-moutline-atomics` (LSE atomics on ARMv8.1+)
- Cross-compile: `x86_64-w64-mingw32-gcc` for Windows
