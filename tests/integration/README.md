# Mercury integration test notes

This directory contains small Go startup/control integration tests. Mercury currently has
Unity-based C unit tests under `tests/`; these Go tests are intentionally
separate because they start the real `mercury` binary.

Run them from the repository root with:

```sh
make integration-test
```

or directly:

```sh
cd tests/integration && go test -v ./...
```

## Current state

- `make test` delegates to `tests/Makefile`, which builds and runs the current
  unit-test binaries.
- Runtime audio is handled through `audioio/` and ffaudio device backends
  such as ALSA, PulseAudio, WASAPI, DirectSound, CoreAudio, OSS, and AAudio.
  Developer/test backends now include `null` and `fifo`.
- Mercury also accepts `-x shm`. In that mode `main.c` skips the sound-card
  capture/playback threads, and `modem/modem.c` connects to shared-memory ring
  buffers named by `SIGNAL_INPUT` and `SIGNAL_OUTPUT`.
- The `fifo` backend supports raw s32le PCM at 8 kHz via `-i`/`-o` named pipes,
  suitable for two-process integration tests (see `mercury_backtoback_test.go`).
- Codec2's channel simulator source is already present at `modem/freedv/ch.c`,
  but the local FreeDV Makefile does not build a `ch` executable;
  it can be built manually with `gcc -I. -o ch ch.c -L. -lfreedvdata -lm`.

## Current test

`mercury_control_test.go` uses only the Go standard library. It locates or builds
the root Mercury binary, starts one process with `-x null`, then one process
with `-x fifo`, each with unique TCP ports, then waits for the ARQ/TNC control
port.

If the control port opens, the test connects and sends CR-terminated TNC
commands:

- `MYCALL TESTA`
- `LISTEN ON`
- `BUFFER`

It asserts that Mercury responds and remains running, then asks the process to
stop. Stdout and stderr are captured to temporary log files and printed on
failure.

The FIFO backend exposes raw signed 32-bit little-endian PCM at 8 kHz:

- `-i <path>` is Mercury RX input, read by Mercury into `capture_buffer`.
- `-o <path>` is Mercury TX output, written from `playback_buffer`.

The test fails if the control port does not open. It does not use ALSA,
PulseAudio, PipeWire, SHM setup, or real audio devices.

If neither a root Mercury binary nor `make` is available, the test is skipped
with a prerequisite message. `MERCURY_BIN=/path/to/mercury` can be used to point
the harness at an already-built binary.

## Channel simulation for `TestMercuryARQTransfer`

`TestMercuryARQTransfer` bridges two Mercury instances through a channel
simulator over the FIFO backend. Environment variables select the engine and
conditions (all optional; default is codec2 `ch` on an effectively clean link):

| Env var | Meaning |
|---|---|
| `MERCURY_CH_ENGINE` | `ch` (default, codec2 AWGN/MPP), `watterson` (in-tree ITU-R fading), or `pathsim` (AE4JY reference — requires `MERCURY_PATHSIM_BIN`) |
| `MERCURY_PATHSIM_BIN` | path to a streaming-mode `pathsim` binary (`MERCURY_CH_ENGINE=pathsim`) |
| `MERCURY_CH_NO` | AWGN noise density dBHz (`ch`/`watterson`; SNR3k ≈ −No − 14.82). For `pathsim`, carries its native `--snr` dB instead |
| `MERCURY_CH_FADING` | fading profile — `ch`: `mpg`/`mpp`/`mpd`; `watterson`: `good`/`moderate`/`poor`/`flutter`; `pathsim`: a preset name, e.g. `ccir-poor`, `midlat-dist-nvis` |
| `MERCURY_CH_GAIN` | linear channel gain (default 1.0) |
| `MERCURY_CH_NO_FWD` / `_REV` | per-direction noise override (asymmetric link: A→B forward data vs B→A reverse ACKs) |
| `MERCURY_CH_FADING_FWD` / `_REV` | per-direction fading profile (`none` disables one direction) |
| `MERCURY_TEST_PAYLOAD_KB` | transfer size in KB (default ~102 bytes = 3 DATAC15 frames) |
| `MERCURY_TEST_LOGDIR` | directory to keep per-instance logs (default: temp, deleted) |

Example — one 1 KB transfer over an NVIS-disturbed pathsim channel:
```
MERCURY_BIN=./mercury MERCURY_CH_ENGINE=pathsim \
MERCURY_PATHSIM_BIN=/path/to/pathsim MERCURY_CH_FADING=midlat-dist-nvis \
MERCURY_CH_NO=10 MERCURY_TEST_PAYLOAD_KB=1 \
go test -run 'TestMercuryARQTransfer$' -v
```

The Watterson engine (`utils/watterson_test`) is calibrated to `ch`
(SNR3k = −No − 14.82) and cross-validated against `pathsim`. **`pathsim`
streaming is validated for offline/continuous input; live bursty bridging is
still WIP** — see the S1 decision doc. For the fully deterministic (no wall
clock, no audio) FSM sweep, use the two-FSM sim in `../sim` instead.

## Future two-process harness

Start with a Go harness here that can:

1. Build or locate `./mercury`.
2. Allocate per-test, per-instance FIFO audio paths.
3. Start Mercury A and Mercury B with distinct TCP/UI/radio settings and
   software audio only.
4. Run a small in-process channel bridge that reads A TX audio and writes B RX
   audio, and vice versa.
5. Assert process startup, port readiness, clean shutdown, and basic data-link
   observability before attempting payload exchange assertions.

The next harness step can start two `-x fifo` instances and bridge A's TX FIFO
to B's RX FIFO, and B's TX FIFO to A's RX FIFO. That is the perfect-channel
path to prove before adding channel impairments.

Do not vendor or wire `ch.c` as part of the first harness PR. The first useful
test can use a no-impairment bridge; once process orchestration is stable, add
an optional channel-simulator process or library wrapper.
