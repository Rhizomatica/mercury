# Fuzzing Mercury's byte-input parsers

These libFuzzer targets exercise the parsers that consume untrusted bytes off the
air, off the TCP/KISS link, or from the WebSocket UI. They are **opt-in**: they are
not part of the blocking CI gates and require clang with libFuzzer.

## Build

    make -C tests/fuzz            # build all targets (needs clang)
    make -C tests/fuzz CC=clang-18  # if your clang is named differently

## Smoke test (build + short no-crash run over the seed corpus)

    make -C tests/fuzz check

This is what a reviewer runs to confirm the targets build and survive their seeds.
It is a gate on *no crash*, not a real campaign.

## Real fuzzing run

    cd tests/fuzz
    ./fuzz_kiss corpus/fuzz_kiss            # runs until a crash or Ctrl-C
    ./fuzz_kiss -max_total_time=300 corpus/fuzz_kiss   # 5-minute campaign

libFuzzer writes new interesting inputs into the corpus dir and, on a finding,
drops a `crash-<sha1>` / `leak-<sha1>` file in the current directory.

## Triage a finding

1. Copy the reproducer into the regression corpus so it is replayed forever:
   `cp crash-<sha1> regressions/fuzz_kiss/`
2. Re-run just that input to confirm and to get the sanitizer stack:
   `./fuzz_kiss regressions/fuzz_kiss/crash-<sha1>`
3. Fix the parser, then confirm the reproducer now passes:
   `./fuzz_kiss regressions/fuzz_kiss/crash-<sha1>` exits 0.

`make check` replays every file under `regressions/<target>/` too (they live
alongside the seed corpus), so a fixed bug cannot silently regress.

## Not yet fuzzed

The TNC control-command parser (`process_control_bytes` /
`execute_control_command` in `data_interfaces/tcp_interfaces.c`) is intentionally
not covered here: it routes every command through `arq_submit_tcp_cmd()` and reads
several external ring buffers, so a target needs a stub layer. See the "Roadmap"
section below.

The WebSocket JSON command parser (`parse_ws_command` in
`gui_interface/websocket/mercury_websocket.c`) is also deferred: the translation
unit includes mongoose.h (a ~1000-line single-file HTTP/WS amalgamation that
references TLS, filesystem, and network symbols at link time). Including the .c
whole-file to reach the two `static` helpers pulls in all those link dependencies.
A clean harness would require either linking the full mongoose amalgamation
(feasible but heavy, and mongoose is not part of the declared scope) or splitting
the two JSON helpers into a separate header, which would touch production code.
Both options are documented as follow-up work.

## Roadmap / future work

- **WebSocket JSON command parser** (`parse_ws_command` / `json_find_key`): extract
  the two JSON helpers to a separate `gui_interface/websocket/ws_json.c` so a fuzz
  target can reach them without dragging in the mongoose link surface. A
  `fuzz_ws_json.c` harness is already checked in; it will build once that split lands.
- **TNC control-command parser** (`process_control_bytes`): needs a stub layer for
  `arq_submit_tcp_cmd()`, the ARQ ring buffers, and socket state before it can be a
  clean target. Worth doing next - it parses attacker-influenced TCP text.
- **OSS-Fuzz onboarding**: these targets follow the standard libFuzzer entry-point
  shape (`LLVMFuzzerTestOneInput`), so an OSS-Fuzz `project.yaml` + `build.sh` that
  compiles `tests/fuzz/*.c` would give continuous cloud fuzzing for free. Good
  candidate given Mercury is open-source humanitarian comms infrastructure.
- **CI job**: a non-blocking scheduled workflow could run `make -C tests/fuzz check`
  (build + short smoke) on a timer. Deliberately not added in this PR to keep the
  blocking gates fast and gcc-only.
