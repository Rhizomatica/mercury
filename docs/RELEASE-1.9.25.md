# Mercury 1.9.25: what changed since 1.9.24

1.9.25 transmits 4 dB louder, gives each bandwidth a ladder of its own on
faster modes, and fixes what field stations running BPQ32, Pat and Winlink
Express reported since 1.9.24.

> [!WARNING]
> **1.9.25 does not connect to 1.9.24 or any older version, in either
> direction.** The ladders' levels mean other modes now (below): left to
> connect, a 1.9.24 and a 1.9.25 station would lose every round on the
> levels they map apart. On a strong, steady link they connected and then
> delivered nothing at all. So they refuse each other and say so in the log.
> **Upgrade every station you talk to at once**, and the gateways first.

### Which versions talk to each other

(Questions: [the announcement on GitHub Discussions](https://github.com/Rhizomatica/mercury/discussions/637).)

Each group connects only within itself:

| Group | Versions | Connects to |
|---|---|---|
| 1 | 1.9.25 | 1.9.25 |
| 2 | 1.9.20, 1.9.21, 1.9.22, 1.9.23, 1.9.24 | one another |
| 3 | 1.9.19 | 1.9.19 |
| 4 | 1.9.18 | 1.9.18 |
| 5 | 1.9.17 and older | one another (the ARQ before the carousel) |

A 1.9.25 station logs a CALL or ACCEPT from group 2 ("from Mercury 1.9.24
or older ... the versions do not connect"); groups 3-5 use other connect
frames and are logged as "another protocol version".

Broadcast (KISS, port 8100) has no handshake: a 1.9.24 station still
decodes a 1.9.25 broadcast on DATAC1, DATAC3, DATAC4, DATAC13, DATAC14,
DATAC15 or DATAC16, and the other way round, but not on DATAC17 or QAM16C2,
whose frames changed.

## Before you upgrade

- **Mercury transmits 4 dB louder** (#634). Every mode used to leave
  Mercury peaking at −6 dBFS (the OFDM modes) or −6.5 dBFS (MFSK), so half
  of the sound card's range went unused. Now the peaks are at −2.0 and
  −2.5 dBFS, and nothing clips.
  - **On an sBitx** (through hermes-radio-daemon), whose power setting is
    reached only by a full-scale signal, that is about 2.5 times the power
    at the same setting. A field sBitx at 85 % sent 3.5 W, and its IC-7100
    peer missed its CALLs: on that link the same build at the old level
    failed both ways, and 1 dB above the new level (the first one tried)
    every session completed, its data 3.5 times faster towards the IC-7100.
  - **On a radio with ALC** (IC-7100 and most transceivers on a USB sound
    card), the ALC takes the extra drive; check that it stays inside its
    range. The short MFSK frames (CALLs, acks) reach it first: a
    constant-envelope signal has a higher average for the same peak.
  - **On a radio without ALC on a sound card** (uBitx and similar), more
    drive past the PA's linear range gives a weaker, wider signal, not a
    stronger one. Check the drive with a TUNE sweep, or lower it.
  - **`[audio] tx_gain_db = -4` keeps the 1.9.24 level.** `tx_gain_db` now
    trims around the new level. TUNE is unchanged: its level is absolute
    dBFS at the sound card.
  - The broadcast and monitor modes are 4 dB louder too.
- **1.9.24 and older stations are not connected** (connect minor 2). Their
  CALLs are logged ("CALL for ... from Mercury 1.9.24 or older ... the
  versions do not connect, not answered"), and so are their ACCEPTs when a
  1.9.25 station calls one. 1.9.24 still answers a 1.9.25 CALL, so the
  1.9.25 caller sees an ACCEPT it ignores, and gives up as if unanswered.
- **The ladders changed** (#599, #602, #618, #575):
  - BW2300: rung 1 is a new 2 kHz MFSK (MFSK2K) in place of DATAC15, and
    DATAC17 and QAM16C2 use 36 carriers.
  - BW500: a ladder of its own, up to the 9-carrier QAM16N2.
  - BW2750: two wider rungs at the top (DATAC17W, QAM16C2W).
  - A session connected on the 500 Hz MFSK (the deep floor) keeps it as its
    floor.
  - The rung thresholds are on the corrected S/N scale; a station climbs
    sooner where the old estimate read low.

## What's new

- **Faster at every S/N** (#599, #575, #596, #602, #621): on air over 24
  transfers at BW2300, 0.84 of the session time of the build before them
  (measured before the 2 kHz MFSK was added);
  in the simulator the 2 kHz MFSK takes rung 1 at 0.91 of DATAC15's time
  and 3.5 % CPU on a Pi 4 against 14.1 %. The 16-QAM rungs' preamble
  detector, at the threshold the other data modes use, ended a fading
  floor: 30 kB at CCIR Poor 20 dB, 22 % faster.
- **A deep floor** (#618): a caller that is not heard on the 1 kHz MFSK
  CALLs on the 500 Hz MFSK too. In the simulator, calls at −14 and −15 dB
  went from none connected to all 60.
- **`PENDING` as soon as a CALL starts arriving** (#619), not once it has
  decoded, so a scanning host stays on the channel: 7 of 9 scan phases
  connect within 120 s, against 5 of 9.
- **BPQ32 and Pat commands** (#561): `CLEANTXBUFFER`, `WINLINK SESSION` and
  `CWID ON|OFF` are accepted. CWID sends no CW ID. `VERSION` now answers
  a second line, `VERSION VARA HF v4.9.0`, after VarAC's unchanged
  `VARA version 4.9.0 registered` (#627, #591): Pat waits for a line
  starting with `VERSION`, and with `CWID ON` accepted its VARA HF start-up
  hung without it.
- **A `BUFFER` query always gets an answer** (#620), also when the count
  has not changed.
- **A playback device that stops taking audio is reopened** (#617), and
  reported in the GUI's audio health while it is down.
- **hermes_shm PTT on the HERMES trx_v1 uBitx controller** (#635, if
  merged), as well as the sBitx.

## Fixes

- **Sessions and teardown**: the caller acks the callee's last bytes before
  it hangs up (#601); its DISCONNECT retry is not held for a floor answer
  (#614); a DISCONNECT never retried goes twice to a marginal peer (#586);
  `ABORT` cuts the keydown on the air short (#592), is retried if the
  command bus was full (#616), and holds a scanning host until its one
  DISCONNECT is out (#560); an exit with a session up ends it on the air
  first (#609).
- **Connect**: a CALL whose listening gap has passed is not answered (#559);
  a scan hop drops the MFSK listeners' half-heard CALL (#613); a busy
  channel holds the caller for one keydown at most (#607).
- **Floor**: unanswered floor rounds mark the link deaf, so a lost "done"
  moves to MFSK (#615); `BUFFER` counts a floor block as in at its BREAK,
  never 0 early (#593).
- **TNC ports**: a host write during a call is held, not dropped, and the
  event loop never waits on the session lock (#590, #595); received data
  with no application reading is dropped, not waited on (#606); idle
  data-port bytes are dropped when their client leaves (#580); a web
  browser on the control port is told so and closed (#583); a host that
  resets its connection is logged as leaving (#608); an opt-in absorb of
  Winlink Express's P2P idle DISCONNECT (#581).
- **Radio and audio**: a network rig's Hamlib session is reopened after a
  timeout or a server restart (#571, #574); no read of `rig_caps.macro_name`
  from a mismatched Hamlib (#573); with PulseAudio, "default" lets it choose
  the device (#576).
- **Windows**: an upgrade keeps the station's `mercury.ini` (#594).

## Documentation

Each bandwidth's ladder in README and TNC.md (#626, #633); Winlink gateways
and several Trimode instances (#563, #611); the efficiency findings and
follow-ups (#552, #605, #624); a Linux build dependency (#557). CI runs the
unit tests on macOS arm64 (#551).

Thanks to Pedro Messetti, Nate Goodall, Tom-Neverwinter, andreahaku,
armysarge, tarantir and thagale.
