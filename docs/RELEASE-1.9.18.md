# Mercury 1.9.18: what changed since 1.9.17

1.9.18 replaces how a connected session moves data: an erasure-coded
"carousel" that the receiver drives, with a 32-tone MFSK floor below the OFDM
modes. Measured against 1.9.17 on the same radios and the same simulated
channels, it is faster everywhere, and the difference grows as the link gets
worse (tables below).

**It does not connect to 1.9.17 or any older version, in either direction.**
Plan the upgrade for every station you talk to at once: read
[UPGRADING-TO-1.9.18.md](UPGRADING-TO-1.9.18.md) first, which says what an
operator sees with mixed versions, how to upgrade, and what was tested.

The design is in [CAROUSEL-ARQ.md](CAROUSEL-ARQ.md), the rules in
[CAROUSEL-SPEC.md](CAROUSEL-SPEC.md), and the development record in
[CAROUSEL-REPORT.md](CAROUSEL-REPORT.md).

## Before you upgrade

- **Upgrade both ends: 1.9.18 and 1.9.x do not connect.** The connect frames
  now carry a protocol version. A 1.9.x station drops a 1.9.18 CALL, and
  1.9.18 does not answer a 1.9.x CALL. It logs it instead: "CALL for ... from
  another protocol version (Mercury 1.9.x?): not answered". There is no
  fallback to the old stop-and-wait data plane, which is gone. Development
  builds from mercuryv2 before this release do not connect to it either.
- **Old configuration keys are ignored, with a warning.** These `[arq]` keys
  set the removed stop-and-wait plane or the reply guards, which are now
  fixed: `no_progress_timeout_s`, `data_retry_slots`,
  `mode_hold_after_downgrade_s`, `ladder_up_successes`,
  `retry_downgrade_threshold`, `keepalive_interval_s`,
  `keepalive_miss_limit`, `peer_payload_hold_s`, `startup_max_s`,
  `channel_guard_ms` and `iss_post_ack_guard_ms`. A `mercury.ini` that sets
  them still loads, and each one is logged at startup:
  `[WRN] [engine] [arq] keepalive_interval_s in the configuration file is no
  longer used and is ignored; delete it`. Delete them. Saving the
  configuration from the UI drops them too.
- **Host timeouts.** A session can now open where DATAC16 cannot reach,
  through MFSK CALL/ACCEPT. That takes longer, a minute or two at the floor,
  so a host that hangs up an unconnected call early prevents it. On HERMES
  stations uuport and uucico allow 180 s (hermes-net #30, uucp 1.07-39).
  Other hosts (Winlink clients, BPQ) may need their connect timeout raised to
  use the deepest connects.
- **Idle CPU is unchanged.** A listening station also listens for MFSK
  CALLs, with a third decoder. Idle LISTENING on a Raspberry Pi 4 (the HERMES
  gateway, sBitx, audio over RTP, 60 s per reading, interleaved,
  2026-10-11): 1.9.17 26.1 % and 27.5 % of one core, 1.9.18 25.5 % and
  28.4 %.
- **Building the GUI from source needs Go 1.26.9** (`fyne-ui`,
  `mercury-client`). Debian 13's `golang-go` 1.24 fetches it on first use
  (`GOTOOLCHAIN`); see the README.
- **Small interface changes.**
  - `MERCURY_CAROUSEL` no longer exists.
  - The UI's `peer_snr` always reads unknown: the carousel reports the rung
    the peer hears instead (`peer_hears_mode`, "hears DATAC3").
  - The `[TMG] disconnect` log line has lost its `retries=` field.
  - `BITRATE (code)` is a mode code: 1 DATAC1, 3 DATAC3, 4 DATAC4,
    15 DATAC15, 17 DATAC17, 25 QAM16C2, 100 MFSK.

## What's new

- **The carousel data plane.** The receiver drives the session.
  - It asks for "N frames on rung M" and picks the rung each round from what
    actually arrived.
  - The application's bytes are Reed-Solomon coded into blocks, so any K
    pieces rebuild a block, and no lost frame is ever resent as such.
  - The stream is delivered in order as pieces arrive, ahead of the block
    check.
- **Seven rungs, from an MFSK floor.** Each direction runs on MFSK, DATAC15,
  DATAC4, DATAC3, DATAC1, DATAC17 or QAM16C2. It starts on the rung the
  connect's SNR supports and moves on measured goodput: a rung that delivers
  every frame earns doubling rounds, and a round nobody answered counts as
  one loss, not the whole round.
- **The session bandwidth caps the rungs.** A `BW500` session uses MFSK
  through DATAC3. `BW2300` and `BW2750` use every rung; nothing is wider than
  2.1 kHz yet. Both stations derive the cap from the bandwidth negotiated at
  connect.
- **Deep connect.** After two unanswered DATAC16 CALLs, the CALL alternates
  with MFSK, and the ACCEPT answers on the carrier the CALL came on.
- **Control at the floor.** Each end reports whether it hears the other below
  DATAC16. Control then goes on MFSK, or as 0.64 s tone patterns, so a peer
  that hears only the floor still gets its polls and DISCONNECTs.
- **One timing master per session.** The caller times every transmission, and
  the callee transmits only in slots the caller opened for it. The two
  stations never transmit at once, without relying on hearing each other
  (simulator: no overlapping transmissions in 960 two-way runs on
  deep-fading channels).
- **Integrity.**
  - Every block carries a check keyed by the session, and a block that fails
    it ends the session instead of being delivered.
  - The session's CRC seed includes a nonce from the callee, so a late frame
    of an earlier session is not taken for this one's.
  - The ACK/BREAK patterns are bound to the session, so another pair's
    patterns are not read as ours.
  - Below 0 dB, each transmission opens with a 0.64 s header that tells the
    peer how long it will last.
- **Request/response in one exchange.** The caller's poll rides inside its
  data, and an acknowledgement waits up to 1 s for the application's answer,
  so a reply goes back in the same turn (UUCP, chat).
- **More data per MFSK frame.** A session that starts on the floor cuts its
  data into 23-byte pieces: four fit an MFSK frame where three 24-byte pieces
  did (28 % more). On air that was 16 % faster at 3 % gateway power.
- **SNR above 12 dB.** The estimate no longer saturates there, and a 16-QAM
  estimator serves QAM16C2, so sessions climb to and stay on the top rungs.
- **Patterns between different radios.** Pattern detection searches ±78 Hz.
  It held only ±15 Hz, and an IC-7100 and an sBitx never heard each other's
  patterns.
- **Monitor mode.** `MONITOR ON` / `MONITOR OFF` on the TNC control port
  reports every frame heard on any mode (CALL/ACCEPT, CQ, DISCONNECT,
  broadcast) without taking part in the link (see [TNC.md](TNC.md)). It does
  not decode carousel sessions, whose frames carry a per-session CRC seed.
  It runs nine extra decoders, about four times the idle CPU, which suits a
  desktop rather than a Pi.
- **UI.** The status names MFSK, and reports each direction's mode only when
  data goes that way, and what the peer hears us on. Chat messages carry
  timestamps, and a right-click clears a chat pane.
- **Modes.** `mercury -l` reports each mode's payload bitrate and occupied
  bandwidth, and lists MFSK (index 11), which `-m` and `MODE` can select. See
  [MODES.md](MODES.md) for where each mode is used.
- **Tools and verification.**
  - `utils/onair_logs.py` analyses an on-air transfer from the two stations'
    journals.
  - `utils/pattern_probe` measures the pattern detector.
  - The carousel has a rule-by-rule specification, TLA+ models of its block
    layer and of who may transmit when, and an explorer that runs it under
    every loss pattern up to a bound.

## Fixes

- The Mercury client's chat pane shows a received line that ends with a bare
  CR, as many HF chat programs send them. Before, such a line was run
  together with the next one, or not shown at all if no LF followed. Text
  with no line ending is shown after 30 s of quiet, or when the session ends
  (#340).
- A DISCONNECT waits for undelivered data as long as a few exchanges take on
  the rung in use. The fixed 30 s dropped UUCP's last reply at the floor.
  ABORT skips that wait.
- A station whose ACCEPTs went unheard no longer ignores broadcast frames
  until its next call.
- Shutdown:
  - The radio is unkeyed at once.
  - Playback is never drained.
  - A TNC listener whose client connected at once counts as started.
  - A shutdown that hangs says which step it is stuck on.
- Per-mode SNR calibration is fitted as a line, and MFSK's is calibrated
  against the channel simulator to within about 0.1 dB from +9 to −12 dB.

## Measured: 1.9.17 against 1.9.18

These measurements were taken on trunk `dee652f` (2026-10-10). The data plane
has not changed since: the simulator gives byte-identical results on 70 runs,
and an on-air A/B on 2026-10-11 tied (below).

**Real modems through codec2's `ch`** (two daemons, 2074 bytes one way, the
whole test including connect and disconnect):

| SNR (3 kHz) | 1.9.17 | 1.9.18 |
|---|---|---|
| +10 dB | 76 s | 34 s |
| +4 dB | 102 s | 53 s |
| 0 dB | 268 s | 146 s |
| −4 dB | 531 s | 151-245 s (see Known limits) |
| −7 dB | failed: 1672 of 2074 B in 12 min | 458 s |
| −10 dB | failed: 1452 of 2074 B in 12 min | 417 s |
| +10 dB, fading (1 Hz Doppler, 2 ms) | 77 s | 43 s |
| +4 dB, fading (1 Hz Doppler, 2 ms) | 128 s | 52-60 s |

**On air.** Gateway sBitx to estacao2 sBitx, 7.050 MHz into dummy loads,
UUCP, with the gateway's TX power setting the SNR. Each run is one call that
carries a file each way; the time is the whole call. Every file arrived
intact:

| gateway power, load | 1.9.17 | 1.9.18 |
|---|---|---|
| 20 %, 8 KB each way | 485 s, 509 s | 312 s, 238 s |
| 3 %, 2 KB each way | 1310 s, 1266 s | 832 s, 755 s |

**The protocol-version change** (this release's connect frames, against
`dee652f` with the same carousel, 2026-10-11, interleaved runs, the same
build on both stations in each run):

| gateway power, load | before | 1.9.18 |
|---|---|---|
| 20 %, 8 KB each way | 230 s, 278 s | 267 s, 275 s |
| 3 %, 2 KB each way | 833 s, 778 s | 735 s, 888 s |

That is a tie within the bench's spread. Every connect completed on the first
CALL, in about 8 s.

## Known limits

- At a steady −4 dB (simulator and harness), a session sometimes steps down
  from DATAC4 to the MFSK floor even after a perfect DATAC4 round. The two
  rungs' estimated goodput ties there, and an SNR reading of −3.1 against
  −2.8 dB tips the choice. The transfer still completes, but in 245 s instead
  of 151 s. It costs speed, never data.
- On a clean channel that loses frames at random (simulator, 25 % loss, both
  ways at once), sessions take about 11 % longer than with the first
  carousel. After a lost poll the caller waits out the slot it opened before
  asking again. That wait is what keeps the two stations from ever
  transmitting at once (CAROUSEL-REPORT.md, Open).
- Monitor mode does not decode carousel sessions.
- BW2750 has the same rungs as BW2300 until a mode wider than 2.1 kHz is
  added.
