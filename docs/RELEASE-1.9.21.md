# Mercury 1.9.21: what changed since 1.9.20

1.9.21 fixes sessions whose link falls below the control mode after they
started (Pat stalling at `FF>`), lets a station with an FSK-only radio
transmit, and adds a Hermes-Lite 2 as radio and sound card.

**It connects to 1.9.20 in both directions.**  Only a station that turns on
the new `mfsk_tx_only` option needs 1.9.21 at the other end too.

## Before you upgrade

- **1.9.20 and 1.9.21 connect to each other** as before.  1.9.19 and older
  still do not connect to either; see [RELEASE-1.9.20.md](RELEASE-1.9.20.md).
- **`[arq] mfsk_tx_only = true` only connects to 1.9.21 or later.**  A
  station with it set ignores a 1.9.20 CALL or ACCEPT; one without it (the
  default) is unaffected.

## What's new

- **MFSK-only transmit for FSK-only radios** (#424, #435).  `[arq]
  mfsk_tx_only = true` makes the station send everything on the MFSK floor
  (CALL and ACCEPT, control and payload), for radios that can only key FSK:
  the QRP Labs QDX, the QRPguys AFSK III and the like.
  - Receive is unchanged, so the far station still sends to it on any rung;
    the cost is throughput in one direction, not reach.
  - The connect frames say so (one bit of the 2.x minor revision, which
    1.9.20 reads and ignores), and the far station keeps that direction on
    the floor for the whole session.
  - Such a station answers a CALL only where its ACCEPT goes on the floor,
    and its first CALL waits the floor's interval, so the two ends do not
    key over each other while connecting.
  - Experimental: it has not yet been keyed on a real FSK-only radio.
- **A Hermes-Lite 2 on the network as radio and sound card** (`-x hl2`,
  #428): no sound card, CAT or PTT cable.  Mercury finds it (`-i auto`) or
  takes its address, tunes it (`-F`, or `[hl2] frequency_hz`), sets RX
  gain, TX drive and the PA, and keys it itself.  Without a frequency it
  receives and never transmits.  The GUI sets the same things, also during
  a session.  See [HERMES-LITE-2.md](HERMES-LITE-2.md).
- **A Mac App Store package** (`make fyne-ui-macos-appstore-pkg`, #436):
  the GUI sandboxed, for macOS 15.0 or later.  The website's `.dmg` is
  unchanged.
- **README: installing on Android and iOS, step by step** (#430), and the
  AltStore source for the notarized iOS build (#429, #433; see
  [ios/ALTSTORE.md](../ios/ALTSTORE.md)).

## Fixes

- **A session whose link falls below the control mode reaches the floor**
  (#431, #434).  A Winlink client whose path to the RMS faded below DATAC16
  after the banner kept asking on DATAC16 every 11 s, and Pat sat at `FF>`
  for minutes.  Three changes:
  - one MFSK frame's reading is enough to say the far station is heard
    below the control mode (smoothed, a single -9 dB frame after 0 dB still
    read as "above");
  - after two unanswered asks the asking end goes to the floor at 90 s, when
    the other end is already listening there, not at 150 s;
  - its data rounds go to the floor with its control frames.

  In the simulator, a drop from 0 to -9 dB at 10-40 s into that exchange:
  the client's 74 bytes took 213-432 s, now 123-172 s (BW500: 336-425 s,
  now 145-247 s).
- `make clean` no longer leaves a `.mercury_host` file behind (#426), which
  broke Debian source packages.

## What was tested

| Test | Setup | Result |
|---|---|---|
| Protocol models | TLA+ (`specs/`): floor silence gained "control, once on the floor, stays there" (#434); the connect model gained MFSK-only stations at either end (#435); each with deliberately broken variants that must fail | No error on the models; every mutant fails, including the 1.9.20 behaviour #434 replaced |
| Simulator | `tests/sim`, 120 seeds a cell, AWGN, cliffs, fading, NVIS, steps, BW500 and BW2300; the build before against this release on the same seeds | #434: 241 of 4020 runs moved, the step down to the floor 155 -> 100 s, NVIS 50 KB median +0.9 % (3 of 30 runs moved); no collisions, overruns or corruption.  #435 with the option off: every run identical |
| MFSK-only connect | Connect simulator, 20 seeds x 5 channels, the MFSK-only station calling and called | Calling: connected in 28 s, with no collisions.  Called: 30 s |
| Mixed versions | Two daemons, 1.9.20 against this release, each calling the other | They connect both ways |
| Hermes-Lite 2 | A real HL2 (gateware 73.2), receive only | Discovery, streaming and the radio's settings work; the sideband is right on the hardware |
| Continuous integration | Every merged change | Unit tests, two-daemon integration tests, ASan/UBSan, TSan; builds for Linux amd64, arm64, armhf, Windows x64, macOS arm64, and the Fyne GUI |

## Known limits

- The #431 fix is measured in the simulator only; it has not yet been
  checked on air with the reporting stations.
- `mfsk_tx_only` has not been keyed on a real FSK-only radio.
- The Hermes-Lite 2 has only been tried receiving.
- Everything under Known limits in [RELEASE-1.9.20.md](RELEASE-1.9.20.md)
  still applies.
