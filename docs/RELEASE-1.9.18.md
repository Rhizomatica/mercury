# Mercury 1.9.18: what changed since 1.9.17

1.9.18 replaces how a connected session moves data, and lets a session open and run about 5 dB deeper into the noise than before. Measured against 1.9.17 on the same radios and the same simulated channels, it is faster everywhere and the difference grows as the link gets worse (tables below). Everything else in this release is fixes and diagnostics around that change.

The design is in [CAROUSEL-ARQ.md](CAROUSEL-ARQ.md), the full development record in [CAROUSEL-REPORT.md](CAROUSEL-REPORT.md).

## Before you upgrade

- **Both stations need 1.9.18 for the new data plane.** A 1.9.18 station still talks to 1.9.17 and older in both directions, on the old stop-and-wait plane:

  | caller | callee | session |
  |---|---|---|
  | 1.9.18 | 1.9.18 | new (carousel) |
  | 1.9.18 | 1.9.17 or older | stop-and-wait, as before |
  | 1.9.17 or older | 1.9.18 | stop-and-wait, as before |

  Development builds of the carousel from before 2026-10-02 (pre-release only) do not mix with 1.9.18; update both ends.
- **Host timeouts.** A session can now open where DATAC16 cannot reach, through MFSK CALL/ACCEPT, which takes longer: about a minute or two at the floor. A host that hangs up an unconnected call early stops that. On HERMES stations uuport and uucico allow 180 s (hermes-net #30, uucp 1.07-39); other hosts (Winlink clients, BPQ) may need their connect timeout raised to use the deepest connects.
- **Idle CPU is higher.** A listening station now also listens for MFSK CALLs. On a Raspberry Pi 4 idle LISTENING went from 22-26 % of one core (1.9.17) to 60-83 % (1.9.18), measured on estacao8 over 60 s; the RX loop keeps up (no backlog warnings). During a session the extra decoder stops.
- **The old plane is still there.** `MERCURY_CAROUSEL=0` in the environment runs every session on stop-and-wait, as a station without the carousel does.

## What's new

- **The carousel data plane.** The receiver drives: it asks for "N frames in mode M", and chooses the mode every round from what actually arrived. Data is Reed-Solomon coded into blocks, so any K pieces rebuild a block and no single lost frame needs a resend. A lost round is asked for again within one window, and the mode steps down at once when rounds stop arriving -- no fixed retry timers per frame.
- **The MFSK floor.** Below DATAC15 the ladder continues to a 32-MFSK mode about 5 dB deeper, driven by 0.64 s pattern answers instead of DATAC16 polls. The full ladder: MFSK, DATAC15, DATAC4, DATAC3, DATAC1, DATAC17, QAM16C2.
- **Deep connect.** After two unanswered DATAC16 CALLs, the CALL alternates with MFSK, and the ACCEPT answers on the carrier the CALL came on.
- **Control at the floor.** Each end reports whether it hears the other below DATAC16; control then goes on MFSK, so a peer that only hears the floor still gets polls and DISCONNECTs.
- **Carrier sense.** A caught preamble holds the channel for its whole frame, and MFSK is sensed while a burst is still arriving.
- **Patterns between different radios.** Pattern detection now searches ±78 Hz (it held only ±15 Hz: an IC-7100 and an sBitx never heard each other's patterns).
- **Modes.** `mercury -l` reports each mode's payload bitrate and occupied bandwidth, and lists MFSK, which `-m` and `MODE` can select.
- **Tools.** `utils/onair_logs.py` analyses an on-air transfer from the two stations' journals (rounds, rungs, overlapping keydowns); `utils/pattern_probe` measures the pattern detector.

## Fixes

- MFSK bursts had rectangular symbol edges: out-of-band emission -27 dB, now about -71 dB (raised-cosine crossfade).
- A DISCONNECT waits for undelivered data as long as a few exchanges take on the mode in use (the fixed 30 s dropped UUCP's last reply at the floor); ABORT skips that wait.
- A station whose ACCEPTs went unheard no longer ignores broadcast frames until its next call.
- The UI's bytes transmitted / received count carousel sessions too.
- Unkey at once on shutdown; never drain playback at shutdown; a TNC listener whose client connected at once counts as started; a shutdown that hangs says which step.
- The MFSK SNR is calibrated against the channel simulator (within about 0.1 dB from +9 to -12 dB); per-mode SNR calibration is fitted as a line.

## Measured: 1.9.17 against 1.9.18

**Real modems through codec2's `ch`** (two daemons, 2 KB one way, whole test time including connect and disconnect):

| SNR (3 kHz) | 1.9.17 | 1.9.18 |
|---|---|---|
| +10 dB | 76 s | 37 s |
| +4 dB | 101 s | 52 s |
| 0 dB | 234 s | 134 s |
| -4 dB | 531 s | 141 s |
| -7 dB | failed: 1672 of 2048 B in 12 min | 556 s |
| -10 dB | failed: 1474 of 2048 B in 12 min | 661 s |
| +10 dB, fading (1 Hz Doppler, 2 ms) | 84 s | 48 s |
| +4 dB, fading (1 Hz Doppler, 2 ms) | 128 s | 52 s |

**On air** (gateway sBitx to estacao2 sBitx, 7.050 MHz into dummy loads, UUCP; the gateway's TX power sets the SNR; time to the file's arrival, every file intact):

| gateway power | load | 1.9.17 | 1.9.18 |
|---|---|---|---|
| 20 % | 10 KB | 321 / 326 s | 197 / 177 s |
| 3 % | 2 KB | 1098 / 1141 s | 673 / 728 s |
| 2 % | 2 KB | not delivered: never connected | not delivered: connected at 128 s, after the host had hung up (see below) |
| 20 %, both ways in one call | 8 KB each | 285 s / 452 s, call 494 s | 154 s / 269 s, call 318 s |

Measured 2026-10-03, runs interleaved, every delivered file intact (md5), no overlapping transmissions in any run. At 3 % the stations turned the channel around about 115 times each with 1.9.17 and 24-28 times with 1.9.18, and the receiving station was on the air 424-436 s against 85-96 s.

The 2 % run with 1.9.18 needed two MFSK CALLs to connect: Mercury connected at 128 s, but the gateway's uuport (still at the old 120 s timeout there) had hung up at 122 s -- the host-timeout point under "Before you upgrade". On the previous days 1.9.18 delivered 2 KB at 2 % in 670-721 s over five runs, connecting sooner.

## Known limits

- A callsign whose code fills the whole CALL slot (about 13-14 characters) cannot carry the marker; its sessions run stop-and-wait.
- In the simulator, deep fading (-5 dB, 0.5 Hz, both ways) still shows about one overlapping transmission per 95-minute session; it costs airtime, never data, and has not been seen on air. A fix is prototyped and parked (CAROUSEL-REPORT.md).
- Mixed-version sessions run the old plane, with its behaviour: a lost frame is retried at the same mode up to three times (about 40 s) before stepping down.
