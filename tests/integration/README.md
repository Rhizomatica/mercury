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

## `-x rtp`: `TestMercuryARQTransferRTP` (Linux)

`mercury_rtp_linux_test.go` runs an ARQ transfer between two instances on
the RTP backend. `rtpsim_linux_test.go` plays both radio daemons on
loopback multicast: it sends each station a 20 ms RX packet on the radio's
clock, carrying what the other station transmits, and polices the TX
streams against the in-stream PTT contract. The transfer must complete;
every transmission must be keyed by a marker packet and ended by an empty
packet (none by the 200 ms dead-keyer), with TX in lockstep with RX.
Groups are 239.255.<random>.1-4, so concurrent runs don't collide.

## Channel simulation for `TestMercuryARQTransfer`

`TestMercuryARQTransfer` bridges two Mercury instances through a channel
simulator over the FIFO backend. Environment variables select the engine and
conditions (all optional; default is codec2 `ch` on an effectively clean link):

| Env var | Meaning |
|---|---|
| `MERCURY_CH_ENGINE` | `ch` (default, codec2 AWGN/MPP), `watterson` (in-tree ITU-R fading), or `pathsim` (AE4JY reference — requires `MERCURY_PATHSIM_BIN`) |
| `MERCURY_PATHSIM_BIN` | path to a streaming-mode `pathsim` binary (`MERCURY_CH_ENGINE=pathsim`) |
| `MERCURY_CH_NO` | AWGN noise density dBHz (`ch`/`watterson`; SNR3k ≈ −No − 14.82). For `pathsim`, carries its native `--snr` dB instead |
| `MERCURY_CH_FADING` | fading profile — `ch`: `mpg`/`mpp`/`mpd`; `watterson`: any name from `utils/watterson_test --list-presets`, e.g. `ccir-poor` or `itur-mm` (standard), or the legacy `good`/`moderate`/`poor`/`flutter`/`mpg`/`mpp`/`mpd` (Butterworth taps; the watterson `mpg`/`mpp`/`mpd` are not `ch`'s profiles); `pathsim`: a preset name, e.g. `ccir-poor`, `midlat-dist-nvis` |
| `MERCURY_CH_GAIN` | linear channel gain (default 1.0) |
| `MERCURY_CH_NO_FWD` / `_REV` | per-direction noise override (asymmetric link: A→B forward data vs B→A reverse polls and patterns); both transports (FIFO and `-x sock`) |
| `MERCURY_CH_FADING_FWD` / `_REV` | per-direction fading profile (`none` disables one direction) |
| `MERCURY_TEST_PAYLOAD_KB` | transfer size in KB (default ~102 bytes = 3 DATAC15 frames) |
| `MERCURY_TEST_LOGDIR` | directory to keep per-instance logs (default: temp, deleted) |
| `MERCURY_TEST_SEED` | the channel's noise/fading seed: unset = fixed, `random` = a new one per test, a number = rerun that seed (see below) |
| `MERCURY_TEST_SEED_LOG` | where the seed history goes (default `tests/integration/seed-history.log`) |
| `MERCURY_TEST_BIN_A` / `_B` | run station A or B on another `mercury` binary, to compare two builds of the same protocol version (a 1.9.x binary does not connect to this one) |

### Fixed and random seeds

The channel runs in one of two modes (`seed_test.go`):

- **Fixed** (`MERCURY_TEST_SEED` unset; `make integration-test`): every run
  gets the same noise and fading.  For benchmarks and A/B comparisons, where a
  change in the result must come from the code.  `ch` runs unseeded as it
  always has, so fixed-mode numbers compare with earlier ones; Watterson gets
  `MERCURY_WATTERSON_SEED`, or 1.
- **Random** (`MERCURY_TEST_SEED=random`; `make integration-test-random`):
  every test draws a new seed.  For finding issues a fixed channel never
  shows: the connect handshake repeats to the millisecond, so with a fixed
  seed a burst lands on the same stretch of the same noise every run, and a
  frame that decodes there passes every time however close to its cliff it
  sits.

Each direction gets its own seed derived from the run's.  Every test logs its
seed and the command that reruns it, repeats both on failure, and appends a
line to the seed history: time, git hash, test, mode, seed, PASS/FAIL, and the
`MERCURY_*` channel settings the rerun needs.  For example (a history line,
then the line a failure prints):

```
2026-10-21T14:02:11Z 73d94ef TestMercuryARQDisconnectReachesFloorPeer random 3141592653 FAIL MERCURY_CH_NO_FWD=-24.82 ...
channel seed: random 3141592653 -- reproduce with: MERCURY_CH_NO_FWD=-24.82 ... MERCURY_TEST_SEED=3141592653 go test -run '^TestMercuryARQDisconnectReachesFloorPeer$'
```

A seed reproduces the channel, not the wall clock: on the FIFO bridge a burst
still lands a few milliseconds differently each run, so a marginal failure
may need a few reruns of its seed; under `MERCURY_TEST_TRANSPORT=sock` the
connect path replays exactly.

Every Mercury the tests start gets a private, temporary state directory
(`XDG_STATE_HOME`, `LOCALAPPDATA`/`APPDATA`, and `HOME` on macOS; see
`main_test.go`), so test runs never read or write your real chat history.

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
