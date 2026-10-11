# Upgrading to Mercury 1.9.18: an on-air change

> **1.9.18 does not talk to Mercury 1.9.17 or any older version over the
> air, in either direction.** A 1.9.18 station and a 1.9.x station never
> connect. Upgrade every station of a network together.

This page says what changes on the air, what an operator sees when versions
are mixed, how to upgrade, and what was tested before the release. What is
new is in [RELEASE-1.9.18.md](RELEASE-1.9.18.md).

## What changes on the air

Connected sessions move data a new way: the receiver drives an
erasure-coded "carousel" with a 32-tone MFSK floor below the OFDM modes
([CAROUSEL-ARQ.md](CAROUSEL-ARQ.md)). The CALL and ACCEPT that open a
session now carry a protocol version, and both ends must speak the same
one.

- A 1.9.x station drops a 1.9.18 CALL; it reads the new frame as an unknown
  bandwidth.
- A 1.9.18 station does not answer a 1.9.x CALL. It logs the CALL instead,
  so an operator can see why.
- There is no fallback to the old stop-and-wait data plane, which is gone.

Broadcast (KISS on port 8100, RaptorQ file broadcast) is unchanged since
1.9.17: its frames are the same.

## What an operator sees with mixed versions

Measured on two real Mercury daemons over the codec2 channel simulator,
2026-10-11. The 1.9.17 station ran tag `v1.9.17` (`b2ad7863`), the 1.9.18
station trunk `c0474bf`:

| | 1.9.17 calls 1.9.18 | 1.9.18 calls 1.9.17 |
|---|---|---|
| The calling station | sends 5 CALLs, then gives up after about 1 minute; its host gets `DISCONNECTED` | sends 5 CALLs (on DATAC16, then also on MFSK), then gives up after about 2 minutes; its host gets `DISCONNECTED` |
| The called station | never transmits, and stays listening | never transmits, and stays listening |
| The called station's log | `[WRN] [arq] CALL for <call> from another protocol version (Mercury 1.9.x?): not answered`, once per CALL it hears | **nothing**: it decodes the CALLs and drops them silently |

Neither side keys the radio after giving up, and nothing has to be
restarted. Only a 1.9.18 station can tell you that a 1.9.x station is
trying to reach it: watch its log for "another protocol version". A 1.9.x
station called by a 1.9.18 one looks as if nobody called.

## How to upgrade

1. **Upgrade every station of a network in the same window.**
   - On HERMES, upgrade the gateway and all its remote stations together.
   - A station left on 1.9.x cannot reach, or be reached by, the upgraded
     ones until it is upgraded too.
   - Development builds of mercuryv2 from before this release do not connect
     to it either.
2. **Clean the configuration file.** These `[arq]` keys no longer do
   anything:
   - `no_progress_timeout_s`, `data_retry_slots`,
     `mode_hold_after_downgrade_s`, `ladder_up_successes`,
     `retry_downgrade_threshold`;
   - `keepalive_interval_s`, `keepalive_miss_limit`, `peer_payload_hold_s`,
     `startup_max_s`;
   - `channel_guard_ms`, `iss_post_ack_guard_ms`.

   A `mercury.ini` that still sets them loads as before. Mercury logs one
   warning per key at every start, e.g. `[WRN] [engine] [arq]
   keepalive_interval_s in the configuration file is no longer used and is
   ignored; delete it`. Delete them to silence the warnings. Saving the
   configuration from the GUI drops them too.
3. **Give connects time.** A session can now open where DATAC16 cannot
   reach, through MFSK CALLs, and that takes a minute or two at the floor.
   - On HERMES, uuport and uucico allow 180 s (hermes-net #30, uucp
     1.07-39).
   - Other hosts (Winlink clients, BPQ) may need a longer connect timeout to
     use the deepest connects.
4. **Know what BW500 costs.** A `BW500` session uses the four rungs MFSK,
   DATAC15, DATAC4 and DATAC3; `BW2300` and `BW2750` use all seven. On a
   clean channel an 8 KB transfer took 304 s at BW500 against 49 s at
   BW2300 (below). Use BW500 only where the channel really is narrow.
5. **Building the GUI from source needs Go 1.26.9.** Debian 13's `golang-go`
   (1.24) fetches it on first use; see the README.

## What was tested before the release

All on 2026-10-11 unless noted. The Mercury binary did not change between
the on-air build (`285d4d5`) and the release candidate (`c0474bf`); the
commits in between touched only the Go GUI, documentation and CI.

| Test | Setup | Result |
|---|---|---|
| Mixed versions | Two daemons over `ch`; 1.9.17 calls 1.9.18, then the reverse | No connect either way, no stuck transmitter. 1.9.18 logs each 1.9.x CALL; 1.9.17 logs nothing (table above) |
| BW500 on the real modem | Two daemons over `ch`, clean channel, 8 KB, both stations set to the bandwidth | BW2300: negotiated 2300, 8 QAM16C2 frames, 49 s. BW500: negotiated 500, 72 DATAC3 frames and no DATAC1/DATAC17/QAM16C2, 304 s. Data intact both times |
| Old configuration files on air | Gateway (sBitx) ⇄ estacao2 (sBitx), 7.050 MHz into dummy loads, UUCP, both stations' `mercury.ini` still setting all 11 removed keys | Each start logged all 11 warnings, and nothing else changed. 20 % gateway power, 8 KB each way: whole call 234 s. 3 % power, 2 KB each way: 856 s. Every file intact |
| The protocol change on air | Same bench, interleaved runs, against the previous trunk (`dee652f`) with the same data plane | A tie: 267 / 275 s against 230 / 278 s at 20 %, and 735 / 888 s against 833 / 778 s at 3 %. Every connect on the first CALL, in about 8 s |
| 1.9.17 against 1.9.18 | Harness and on air, 2026-10-10 | 1.9.18 is faster everywhere and works where 1.9.17 fails; tables in [RELEASE-1.9.18.md](RELEASE-1.9.18.md) |
| Idle CPU, Raspberry Pi 4 | The gateway, a HERMES sBitx station, audio over RTP to the radio daemon; the Mercury process in idle LISTENING, 60 s per reading, interleaved | 1.9.17: 26.1 % and 27.5 % of one core. 1.9.18: 25.5 % and 28.4 %. Unchanged, although 1.9.18 also listens for MFSK CALLs |
| Simulator | The two-station simulator over 7 channel models, both ways, seeds 1-5 | The carousel is byte-identical to the previous trunk (70 runs). A BW500 session never keys a wideband rung, and the test fails if the cap is removed |
| Continuous integration | Every merged change | Unit tests, two-daemon integration tests, ASan/UBSan, TSan. Builds for Linux amd64, arm64, armhf, Windows x64, macOS arm64, and the Fyne GUI on Linux and Windows |
| macOS package | The release's DMG workflow, run by hand on `c0474bf` with Go 1.26.9 | Built, Developer ID signed, notarized, stapled, accepted by Gatekeeper |

Known limits are listed in [RELEASE-1.9.18.md](RELEASE-1.9.18.md).
