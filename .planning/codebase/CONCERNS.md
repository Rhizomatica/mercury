# Concerns — Mercury (HERMES HF Modem)

## Global Mutable State

- `volatile bool shutdown_` global flag used across all modules
- `arq_info arq_conn` is a global extern struct accessed from multiple threads
- `arq_fsm` is a global FSM handle with mutex protection, but other globals lack similar guards
- Several `extern` declarations scattered across `.c` files instead of centralized in headers

**Risk**: Race conditions in concurrent access to shared state. The `chan.h` and `queue.h` abstractions help, but some paths still access globals directly.

## Thread Safety

- Thread-per-connection TCP model creates threads that share state with the ARQ core
- Some TCP TNC status functions (`tnc_get_last_snr`, `tnc_get_last_bitrate_bps`) are documented as thread-safe, but the underlying data access patterns vary
- Signal handler (`handle_termination_signal`) calls `msleep` and `exit(0)` — potentially unsafe from signal context

## Memory Management

- `malloc`/`free` in `main.c` for device strings without NULL checks on allocation results
- `sprintf` used in a few places instead of safer `snprintf` (e.g., audio device defaults in `main.c`)
- No `valgrind` integration or memory sanitizer tooling visible
- Fixed-size buffers (`char radio_device[1024]`) with `strncpy` — generally safe but relies on correct size tracking

## Vendored Dependencies

- **FreeDV/codec2** (`modem/freedv/`): Large vendored codebase (~100+ files). Updates require manual synchronization with upstream.
- **ffaudio** (`audioio/ffaudio/`): Vendored audio library. Platform-specific backends may have bugs.
- **Mongoose** (`gui_interface/websocket/mongoose.c`): Single-file WebSocket library. Security updates require manual tracking.

**Risk**: Vendored libraries may fall behind upstream security patches or bug fixes.

## Platform Support Gaps

- CoreAudio (macOS) marked as `UNSUPPORTED` in code
- AAudio (Android) marked as `UNSUPPORTED` in code
- FreeBSD support present but may be undertested
- `FSK_LDPC` mode is marked as **experimental** — not recommended for production

## Testing Gaps

- No formal test framework for ARQ protocol logic
- No automated tests for TCP TNC command parsing
- No WebSocket/GUI interface tests
- Protocol correctness relies on manual over-the-air testing
- No CI/CD pipeline

**Risk**: Regressions in ARQ state machine or protocol handling could go undetected.

## Forced Exit in Signal Handler

```c
static void handle_termination_signal(int sig)
{
    shutdown_ = true;
    msleep(1500);
    exit(0); // forced exit — may skip cleanup
}
```

- `msleep` and `exit` in a signal handler is non-POSIX-safe
- Could leave shared memory segments or file descriptors in inconsistent state
- Should use `write` to a self-pipe or `sig_atomic_t` flag pattern instead

## Error Propagation

- Many initialization functions return 0/non-zero but callers don't always propagate errors consistently
- Some subsystem failures are non-fatal (e.g., UI init failure just logs a warning) — this is intentional but could mask real issues

## Documentation

- `docs/ARQ.md` (25KB) and `docs/TNC.md` (10KB) are substantial and well-maintained
- Doxygen comments are thorough in ARQ headers but sparse in other modules
- No architecture decision records (ADRs)

## Build System

- Pure Make with no dependency tracking beyond header includes — changing a shared header may require `make clean`
- No `make test` target
- Windows cross-compilation works but CI verification is not visible
