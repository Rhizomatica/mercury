# Mercury CI Pipeline

## What This Is

A comprehensive CI pipeline for Mercury, the HERMES HF modem — a C-based ARQ data link for reliable store-and-forward email and file transfer over HF radio. The pipeline covers build verification across platforms, static analysis, automated unit/integration testing, Debian packaging, and hardware-in-the-loop testing via real radio stations connected to sBitx radios with dummy loads and OTA links.

## Core Value

Every push and PR is verified against real hardware — catching regressions before they reach remote stations that are hard to recover.

## Requirements

### Validated

<!-- Shipped and confirmed valuable — inferred from existing codebase. -->

- ✓ ARQ data link with connect/accept handshake, ACK/retry, keepalive, disconnect — existing
- ✓ Adaptive payload gear-shifting (DATAC4/DATAC3/DATAC1) driven by SNR/backlog — existing
- ✓ Per-direction mode selection with independent negotiation — existing
- ✓ Broadcast data mode with KISS framing — existing
- ✓ VARA-style TCP TNC interface (control + data sockets) — existing
- ✓ Multi-platform audio via ffaudio (ALSA, PulseAudio, DirectSound, WASAPI, SHM) — existing
- ✓ Radio control via HAMLIB and HERMES shared memory — existing
- ✓ WebSocket GUI interface for mercury-qt — existing
- ✓ Cross-platform build (Linux, Windows, macOS, FreeBSD) — existing
- ✓ Basic test framework with SSH-based station control (`hermes-test-framework`) — existing

### Active

<!-- Current scope. Building toward these. -->

- [ ] GitHub Actions CI pipeline with build verification (Linux amd64, arm64, Windows cross-compile)
- [ ] Static analysis stage (cppcheck + selective `-Werror`, vendor warnings suppressed)
- [ ] ARQ protocol unit tests
- [ ] TCP TNC interface unit tests
- [ ] WebSocket command send/receive tests
- [ ] Broadcast test utils integration (existing tools, refined for CI)
- [ ] Debian `.deb` package build verification (amd64 + arm64)
- [ ] Dummy-load hardware tests via SSH (LAB1/LAB2 stations, every PR + daily)
- [ ] OTA hardware tests via SSH (AIR1 stations, each release)
- [ ] Test payloads: 0 bytes, <200B, 512B (exact frame), 2.5-5KB, image ≤10KB

### Out of Scope

- CD/deployment pipeline — deferred to later work
- FreeDV/codec2 vendored library testing — upstream's responsibility
- Mongoose WebSocket library testing — vendor testing not needed
- mercury-qt (GUI) CI — separate repository, separate pipeline
- Performance benchmarking — not in initial CI scope

## Context

- Mercury v2 is a complete C rewrite of the HERMES modem (v1 was C++)
- The project is hosted on GitHub at `Rhizomatica/mercury`
- `hermes-test-framework` exists with bash-based SSH test orchestration, currently triggered via cron + commit tags `[TEST AIR1/LAB1/LAB2]` — will be integrated into GitHub Actions
- 3 test station setups: AIR1 (OTA 500km, Nilsão↔Artur), LAB1 (Rafael home lab, dummy loads), LAB2 (Pedro home lab, dummy loads)
- All stations are Raspberry Pi arm64 running sBitx radios
- Stations are accessible via SSH through a VPN (10.70.96.x network) or local network
- Test transmission uses UUCP over Mercury ARQ link
- Currently no unit test framework in the codebase — tests need to be created from scratch
- No existing CI pipeline — GitHub Actions workflows need to be created

## Constraints

- **Platform**: Primary targets are Linux amd64 + arm64 (Raspberry Pi) and Windows cross-compile
- **Build System**: GNU Make (no CMake, no package manager) — CI must work with raw `make`
- **Hardware**: Dummy-load tests require SSH access to physical Raspberry Pi stations with sBitx radios
- **Network**: Test stations on VPN (10.70.96.x) — CI runner needs network route to stations
- **Vendor Warnings**: Warnings from `modem/freedv/` and `gui_interface/websocket/mongoose.c` must be suppressed (not our code to fix)

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| cppcheck for static analysis | Low false-positive rate, C-focused, free | — Pending |
| Suppress vendor warnings instead of fixing | FreeDV and Mongoose are external code we vendor | — Pending |
| Dummy-load tests on every PR, OTA only on release | Balance CI speed vs. real-world coverage | — Pending |
| No C++ test framework (pure C tests) | Mercury is pure C, keep the toolchain consistent | — Pending |
| GitHub Actions with SSH to self-hosted stations | Stations already accessible via SSH, avoids self-hosted runner complexity | — Pending |

## Evolution

This document evolves at phase transitions and milestone boundaries.

**After each phase transition** (via `/gsd-transition`):
1. Requirements invalidated? → Move to Out of Scope with reason
2. Requirements validated? → Move to Validated with phase reference
3. New requirements emerged? → Add to Active
4. Decisions to log? → Add to Key Decisions
5. "What This Is" still accurate? → Update if drifted

**After each milestone** (via `/gsd-complete-milestone`):
1. Full review of all sections
2. Core Value check — still the right priority?
3. Audit Out of Scope — reasons still valid?
4. Update Context with current state

---
*Last updated: 2026-03-23 after initialization*
