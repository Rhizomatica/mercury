# Structure — Mercury (HERMES HF Modem)

## Directory Layout

```
mercury/
├── main.c                    # Entry point — CLI parsing, subsystem init, main loop
├── Makefile                  # Top-level build (delegates to subdirs)
├── config.mk                # Shared compiler flags, platform detection
├── Doxyfile                  # Doxygen config for API docs
├── README.md                 # Project documentation
├── LICENSE                   # GPLv3
├── LICENSE-freedv            # FreeDV/codec2 license
├── mercury.1                 # Man page
│
├── modem/                    # Physical layer — FreeDV modem wrapper
│   ├── modem.c/h             # Modem init, TX/RX threads, spectrum data
│   ├── framer.c/h            # Frame type detection and routing
│   ├── freedv/               # Vendored FreeDV/codec2 library (~100+ files)
│   │   ├── freedv_api.c/h    # FreeDV public API
│   │   ├── ofdm.c/h          # OFDM modem core
│   │   ├── ldpc_codes.c/h    # LDPC forward error correction
│   │   └── ...               # codec2, FSK, filtering, LDPC matrices
│   └── Makefile
│
├── datalink_arq/             # ARQ data link layer
│   ├── arq.c/h               # ARQ public API, init, shutdown
│   ├── arq_fsm.c             # ARQ state machine handlers
│   ├── arq_protocol.c        # CALL/ACCEPT, ACK/NAK, keepalive, disconnect
│   ├── arq_channels.c/h      # Per-direction mode management
│   ├── arq_timing.c/h        # Retry timers, timeouts
│   ├── arq_modem.c/h         # Action queue for modem TX thread
│   ├── arq_events.h          # Message bus types and event definitions
│   ├── fsm.c/h               # Generic FSM dispatcher (thread-safe)
│   ├── arith.c/h             # Arithmetic utilities
│   └── Makefile
│
├── datalink_broadcast/       # Broadcast channel
│   ├── broadcast.c/h         # Broadcast init, frame alignment
│   ├── kiss.c/h              # KISS protocol framing
│   └── Makefile
│
├── data_interfaces/          # TCP TNC interface (VARA-compatible)
│   ├── tcp_interfaces.c/h    # Control/data socket servers, command parsing
│   ├── net.c/h               # Low-level socket helpers
│   └── Makefile
│
├── audioio/                  # Multi-platform audio I/O
│   ├── audioio.c/h           # Audio init, ring buffer bridge, device enum
│   ├── std.h                 # Audio subsystem constants
│   ├── ffaudio/              # Vendored ffaudio library
│   │   ├── ffaudio/          # Backend implementations (alsa, pulse, dsound, wasapi, oss, coreaudio, aaudio)
│   │   └── ffbase/           # Utility library (strings, ring buffers, etc.)
│   └── Makefile
│
├── common/                   # Shared utilities
│   ├── ring_buffer_posix.c/h # Lock-free circular buffer
│   ├── chan.c/h              # Typed message channels
│   ├── queue.c/h            # Generic queue
│   ├── hermes_log.c/h       # Async logging (multi-level, file, JSONL)
│   ├── os_interop.c/h       # Cross-platform helpers (msleep, etc.)
│   ├── shm_posix.c/h        # POSIX shared memory utilities
│   ├── crc6.c/h             # 6-bit CRC for compact frames
│   ├── defines_modem.h      # Global defines (buffer sizes, SHM names)
│   └── Makefile
│
├── gui_interface/            # WebSocket GUI backend
│   ├── ui_communication.c/h  # Status/spectrum publisher, command handler
│   ├── websocket/
│   │   ├── mercury_websocket.c/h  # WebSocket session management
│   │   └── mongoose.c/h     # Vendored Mongoose WebSocket library
│   └── Makefile
│
├── radio_io/                 # Radio control abstraction
│   ├── radio_io.c/h         # PTT keying, init, shutdown, restart
│   ├── sbitx_io.c/h         # HERMES SHM radio backend (Linux-only)
│   ├── shm_utils.c/h        # SHM helper functions
│   ├── rigctl_parse.c/h     # HAMLIB command parsing
│   └── Makefile
│
├── utils/                    # Test/diagnostic utilities
│   ├── broadcast_diag_rx.c   # Broadcast receive diagnostic
│   ├── broadcast_diag_tx.c   # Broadcast transmit diagnostic
│   ├── broadcast_test.c      # Broadcast test harness
│   ├── kiss.c/h              # KISS utility (separate from broadcast)
│   └── Makefile
│
├── adapters/                 # Empty — reserved for future adapter modules
├── scripts/                  # Empty — reserved for utility scripts
├── docs/                     # Documentation
│   ├── ARQ.md                # Full ARQ architecture & protocol reference
│   ├── TNC.md                # TNC TCP interface command reference
│   └── html/                 # Generated Doxygen HTML docs
│
├── debian/                   # Debian packaging files
│
└── mercury-qt/               # Companion GUI (separate git repo)
    ├── app.py                # Entry point (PySide6 application)
    ├── requirements.txt      # pyside6, numpy
    ├── core/                 # Core classes (components, connection)
    └── apps/mercury_qt/      # Mercury Qt application
```

## Naming Conventions

- **Files**: `snake_case.c` / `snake_case.h`
- **Directories**: `snake_case` (module boundaries)
- **Functions**: `module_verb_noun()` pattern (e.g., `arq_init()`, `ui_comm_init()`, `radio_io_key_on()`)
- **Types**: `snake_case_t` suffix (e.g., `arq_info`, `generic_modem_t`, `ui_ctx_t`)
- **Macros/Constants**: `UPPER_SNAKE_CASE` (e.g., `PACKET_ARQ_CONTROL`, `DEFAULT_ARQ_PORT`)
- **Enums**: `module_name_t` with `MODULE_VALUE_NAME` members

## Key Locations

| What | Where |
|------|-------|
| Entry point | `main.c` |
| Build config | `config.mk` |
| ARQ protocol | `datalink_arq/arq_protocol.c` |
| FSM states | `datalink_arq/arq_fsm.c` |
| Modem TX/RX | `modem/modem.c` |
| Frame routing | `modem/framer.c` |
| TCP commands | `data_interfaces/tcp_interfaces.c` |
| WebSocket GUI | `gui_interface/ui_communication.c` |
| Audio bridge | `audioio/audioio.c` |
| Logging | `common/hermes_log.c` |
| FreeDV API | `modem/freedv/freedv_api.c` |
| Version | `main.c` line 21 (`VERSION__`) |
