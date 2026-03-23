# Architecture — Mercury (HERMES HF Modem)

## Pattern

**Modular reactor architecture** — event-driven finite state machine (FSM) with message bus, multi-threaded worker model, and shared ring buffers for audio I/O.

## High-Level Component Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                       main.c (entry)                        │
│  CLI parsing → subsystem init → main loop → graceful exit   │
└──────┬──────────┬──────────┬──────────┬──────────┬──────────┘
       │          │          │          │          │
  ┌────▼───┐ ┌───▼────┐ ┌───▼────┐ ┌───▼───┐ ┌───▼──────┐
  │ modem/ │ │datalink│ │datalink│ │ data_ │ │   gui_   │
  │        │ │ _arq/  │ │_bcast/ │ │interf.│ │interface/│
  │FreeDV  │ │ARQ FSM │ │KISS    │ │TCP TNC│ │WebSocket │
  │TX/RX   │ │gear    │ │bcast   │ │VARA   │ │mercury-qt│
  └────┬───┘ └───┬────┘ └───┬────┘ └───┬───┘ └───┬──────┘
       │         │          │          │          │
  ┌────▼─────────▼──────────▼──────────▼──────────▼──────┐
  │                    common/                            │
  │  ring_buffer, chan, queue, os_interop, hermes_log,    │
  │  crc6, shm_posix, defines_modem                      │
  └──────────────────────┬───────────────────────────────┘
                         │
  ┌──────────┐    ┌──────▼─────┐
  │ audioio/ │    │ radio_io/  │
  │ ffaudio  │    │ HAMLIB/SHM │
  │ capture/ │    │ PTT keying │
  │ playback │    └────────────┘
  └──────────┘
```

## Layers

### 1. Entry Point (`main.c`)

- CLI option parsing via `getopt`
- Subsystem initialization in dependency order: audio → radio → modem → ARQ → broadcast → TCP → UI
- Global shutdown flag (`volatile bool shutdown_`) + signal handlers (SIGINT, SIGTERM)
- Graceful shutdown in reverse order

### 2. Modem Layer (`modem/`)

- FreeDV codec initialization and management (`generic_modem_t`)
- TX thread: dequeues ARQ actions → modulates audio → pushes to playback ring buffer
- RX thread: reads capture ring buffer → demodulates → dispatches frames to ARQ/broadcast
- Framer (`modem/framer.c`): frame type detection and routing (ARQ control, ARQ data, broadcast, CALL/ACCEPT, CQ)
- Spectrum data extraction for UI waterfall display

### 3. ARQ Data Link (`datalink_arq/`)

- **FSM** (`fsm.c`, `arq_fsm.c`): thread-safe state machine for link lifecycle (IDLE → CALLING → CONNECTED → DISCONNECTING)
- **Protocol** (`arq_protocol.c`): CALL/ACCEPT handshake, ACK/NAK, keepalive, controlled disconnect
- **Channels** (`arq_channels.c`): per-direction mode management, bandwidth negotiation
- **Timing** (`arq_timing.c`): retry timers, timeout management
- **Modem interface** (`arq_modem.c`): action queue (TX control, TX payload, mode switch) consumed by modem TX thread
- **Gear-shifting**: adaptive mode selection (DATAC4→DATAC3→DATAC1) based on SNR + delivery feedback
- **Message bus** (`arq_events.h`): typed union messages between TCP bridge, modem, and ARQ core

### 4. Broadcast Data Link (`datalink_broadcast/`)

- One-way broadcast framing alongside ARQ
- KISS protocol framing (`kiss.c`)
- Frame size tables per FreeDV mode

### 5. TCP/TNC Interface (`data_interfaces/`)

- VARA-compatible control/data socket pair
- Command parsing and status emission
- Bridge between TCP clients and ARQ subsystem
- Thread-per-connection model

### 6. GUI Interface (`gui_interface/`)

- WebSocket server (Mongoose) for `mercury-qt`
- Status publisher thread: polls modem/ARQ state → JSON → WebSocket (500ms interval)
- Spectrum publisher thread: FFT data → WebSocket (~20fps, 50ms interval)
- Command handler: receives UI commands → dispatches to appropriate subsystem
- Optional TLS support

### 7. Radio I/O (`radio_io/`)

- Abstraction layer for PTT keying
- HAMLIB backend (pluggable radio models)
- HERMES SHM backend (direct shared memory for sBitx radios)
- Hot-restart capability (`radio_io_restart`)

### 8. Audio I/O (`audioio/`)

- ffaudio wrapper for multi-platform audio capture/playback
- Shared ring buffers (`capture_buffer`, `playback_buffer`) connect audio threads to modem
- Device enumeration and selection
- Hot-restart capability (`audioio_restart`)

### 9. Common (`common/`)

- `ring_buffer_posix.c` — lock-free circular buffers for audio
- `chan.c` — typed message channels (thread-safe)
- `queue.c` — generic queue implementation
- `hermes_log.c` — async logging with levels (DEBUG/TIMING/INFO/WARN/ERROR), file output, JSONL format
- `os_interop.c` — cross-platform helpers
- `crc6.c` — 6-bit CRC for compact frames
- `shm_posix.c` — POSIX shared memory utilities

## Data Flow

```
Host App ←TCP→ data_interfaces ←msg bus→ ARQ core ←action queue→ modem ←ring buffers→ audioio ←audio API→ Sound Card ←analog→ Radio
                                           ↕                       ↕
                                      FSM events              FreeDV codec
```

**RX path:** Sound Card → audioio capture → ring buffer → modem RX thread → FreeDV decode → framer → ARQ/broadcast → TCP data socket → Host App

**TX path:** Host App → TCP data socket → ARQ queue → ARQ protocol → action queue → modem TX thread → FreeDV encode → ring buffer → audioio playback → Sound Card

## Threading Model

| Thread | Module | Purpose |
| -------- | -------- | --------- |
| Main | `main.c` | Init, shutdown, sleep loop |
| Modem TX | `modem/` | Dequeue ARQ actions, modulate, play |
| Modem RX | `modem/` | Capture audio, demodulate, dispatch |
| ARQ Worker | `datalink_arq/` | Process message bus events |
| ARQ Tick | `datalink_arq/` | 1Hz maintenance (timers, keepalive) |
| Audio Capture | `audioio/` | Read from sound card |
| Audio Playback | `audioio/` | Write to sound card |
| TCP Control | `data_interfaces/` | ARQ control socket server |
| TCP Data | `data_interfaces/` | ARQ data socket server |
| TCP Broadcast | `data_interfaces/` | Broadcast socket server |
| UI Publisher | `gui_interface/` | WebSocket status broadcast |
| Spectrum Publisher | `gui_interface/` | WebSocket waterfall data |
