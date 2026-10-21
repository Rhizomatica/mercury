# Mercury 1.9.20: what changed since 1.9.19

1.9.20 is the architecture we called "2.0". The speed ladder starts where
the SNR says and falls back fast, connect and teardown are quicker, a 500 Hz
session has its own MFSK floor, and high-Doppler channels, which delivered
nothing before, now deliver on the MFSK floor.

**It does not connect to 1.9.19 or any older version, in either direction.**
Upgrade every station you talk to at once.

## Before you upgrade

- **Upgrade both ends: 1.9.20 and 1.9.19 do not connect.** The connect
  frames have a new packet type (CALL2), and the versions refuse each other
  cleanly:
  - a 1.9.20 station logs a 1.9.19 (or older) CALL as "CALL for ... from
    another protocol version (Mercury 1.9.x?): not answered", and never
    transmits;
  - a 1.9.19 station logs a 1.9.20 CALL as "Unknown frame type received",
    and never transmits either;
  - the caller gives up after its CALLs, and nothing stays keyed.

  Measured with two daemons over the channel simulator, 1.9.19 against
  this release, each calling the other: 5 CALLs each way in 110 s, every
  one heard and refused; the called station never keyed, and the caller
  ended back in DISCONNECTED.
- **1.9.x is still out.** Everything in
  [RELEASE-1.9.19.md](RELEASE-1.9.19.md) and
  [UPGRADING-TO-1.9.18.md](UPGRADING-TO-1.9.18.md) about older versions
  still applies.
- **Nothing else changes for an operator.** The configuration file, the TNC
  commands and the broadcast plane are as in 1.9.19, plus the new options
  below.

## What's new

- **The ladder starts high and falls back fast** (#406).
  - Every poll carries the SNR at which its sender hears the other station,
    in dB, so both ends know both directions.
  - A session starts on the rung the connect's SNR allows, and a fresh
    reading takes it straight to its rung instead of one rung at a time.
    In the simulator, after a step from -12 to +10 dB the session reaches
    DATAC1 in 20 s (40 s before); after a step to +20 dB, QAM16C2 in 20 s
    (64 s).
  - After a drop below the control mode the session reaches the MFSK floor
    in about 96 s (about 15 minutes before): 120 of 120 simulated 50 KB
    transfers complete, against 92.
- **Connect and teardown** (#406, #405).
  - A DISCONNECT is answered by a short acknowledgement tone. On air the
    caller's teardown took 5.9 s, against 8.6 s.
  - A callee whose application has hung up says so in its last poll, and the
    caller ends at once.
  - The last bytes are delivered on a hang-up: a reply written just before
    DISCONNECT is no longer lost (it was in 2 of 4 tries).
  - A callee that hears the same CALL twice on DATAC16 answers the repeat on
    the MFSK floor (#420): its DATAC16 answer did not get back.  On air
    (an sBitx calling an IC-7100) none of the DATAC16 answers arrived and
    every MFSK one did.  In the simulator, on such a one-sided link, the
    connect takes 30 s instead of 51.5 s.  The caller holds its next CALL
    while one of its decoders hears that answer arriving.
- **A 500 Hz MFSK floor for BW500 sessions** (#406): 32 tones 15.6 Hz apart,
  inside a 500 Hz filter. Wide sessions keep the 1 kHz floor. In the
  simulator a BW500 session now completes at -14 dB; at -12 dB it is slower
  than before.
- **High-Doppler channels** (#406). Under 10-30 Hz of Doppler spread no OFDM
  mode decodes, whatever the SNR. A connect heard only on MFSK now starts
  the session on the MFSK floor, and the receiver tries a single OFDM frame
  now and then, so a session that went to MFSK on a good channel leaves it.
- **Idle sessions stay up on lossy links** (#407, #413). An unanswered idle
  poll is asked again within seconds, and a poll for one frame keeps no
  continuation slots. In the simulator, with a fifth of all frames lost,
  5 of 100 idle sessions ended in 30 minutes, against 82.
- **`-d` (trace)** logs the raw bytes of every decoded connect frame, and
  `-v` logs who is calling whom on the channel (#409).
- **A default station callsign** can be set in `mercury.ini`, for when no
  client has set one.
- **Mercury as a library**: `libmercury_host.a` links Mercury into
  hermes-radio-daemon (#400). The host API (`host/mercury_host.h`, #414)
  drives chat, the listen mode and a broadcast carousel in-process, reports
  what keeps the modem busy, and hands the host every broadcast frame and
  file it hears. An ARQ session stops a running carousel.
- **On Windows, `mercury.exe` run without `-L` logs to
  `logs\mercury_engine.log`** beside itself, the file the GUI writes (#411).
  Before, it logged only to its console window.
- **Standard channels in the Watterson simulator**: the ITU-R and CCIR
  presets (#379).

## Technology previews: Android and iOS

Both are the Mercury GUI with the modem inside, for trying out, not yet for
relying on.

- **Android**: `Mercury-1.9.20-android-arm64.apk`, to sideload (64-bit ARM).
  - The radio's USB sound card is picked in Soundcards, by name ("USB Audio
    CODEC (USB)"); capture takes the signal without the phone's voice
    processing.
  - CAT and PTT over the radio's own USB cable: Radio, method Hamlib,
    **"Direct USB, no driver (experimental)..."**.  Android gives an app no
    serial port, so the bundled Hamlib drives the radio's USB-serial chip
    itself (Silicon Labs CP210x: Icom IC-7100 and IC-7300, Yaesu FT-710,
    many cables; Hamlib/Hamlib#2272).
  - The .apk is signed with Mercury's release key: a test build installed
    before has to be uninstalled first.
- **iOS** (iPhone and iPad): through AltStore PAL (EU, Brazil, Japan) once
  Apple has notarized it; not available yet.
- **The same USB path on Linux** without the chip's kernel driver, or
  without access to `/dev/ttyUSB*` (the Jolla Phone): give Hamlib's device
  as `usb:VVVV:PPPP[:SERIAL][@INTERFACE]` (#421), with a Hamlib that has
  Hamlib/Hamlib#2272.

## Fixes

- **Pat's `write: buffer timeout` on large Winlink uploads** (#397, #412).
  Pat gives up when no `BUFFER` comes for a minute, and on a slow, lossy
  link a block can take longer than that to be confirmed. In a session with
  data waiting, Mercury now says `BUFFER` again once 50 s have passed
  without one.
- **The TNC control port drops no line** (#412). A queue that dropped
  `BUFFER`, `SN` and `MONITOR` lines when full, and `PTT`, `CONNECTED` and
  `DISCONNECTED` after 50 ms, is now a ring buffer that makes the writer
  wait. A client that reads nothing for 5 s gets its port restarted.
- A received broadcast chat message is stored even when no broadcast
  client is attached (#414).
- Idle sessions whose peer has gone end within 4 minutes; spent poll slots
  are freed (#385, #384).
- Small messages go on the shortest rung that fits them (#386).
- Both ends agree on the rung after a lost acknowledgement (#387).
- A payload decoder switched to its mode drops a sync it still holds (#389).
- A `-L` log file that cannot be opened is reported, also to the GUI.

## What was tested

| Test | Setup | Result |
|---|---|---|
| Protocol models | TLA+ (`specs/`): connect, rung agreement, poll rungs, floor silence, turns, teardown, the carousel's blocks; each with deliberately broken variants that must fail | No error on the models; every mutant fails |
| Standard channels | Two daemons over the Watterson simulator, every ITU-R and CCIR preset at 15, 6 and 0 dB, 4 KB, 2 seeds a cell; the build before against this release on the same seeds | 60/84 before, 80/84 now. The gain is the high-Doppler presets (ccir-flutter, itur-ld, itur-hm: 0/18 before, 18/18 now; itur-hd 0/6 before, 2/6 now, at about 4 bytes/s). Every other preset passed with both builds |
| On air | Gateway ⇄ estacao2 (both sBitx, 7.050 MHz into dummy loads, 20 %), the build before against this release, alternated | Keyboard chat about 30 % faster; a 5 KB file and UUCP both ways equal within the on-air spread; everything delivered |
| Simulator | `tests/sim`, 120 seeds a cell, AWGN, cliffs, fading, NVIS, steps, BW500 and BW2300 | Equal or better than the build before in every cell but the BW500 fringe at -12 dB (above) |
| Idle sessions | Two FSMs, 100 idle sessions of 30 minutes, 10 dB with 10/20/30 % of frames lost, and fading at 0 dB (#413) | Sessions dropped: 33/82/98 and 64 before, 0/5/31 and 3 now |
| Android | The .apk on a Jolla Phone (Sailfish OS 5.2, Android 15 App Support); the USB-serial path with Hamlib's rigctl and Mercury on the same phone's Linux side (IC-7100), and on Debian 12 (FT-710) | Starts, receives and decodes; CAT reads, PTT keys and releases. CAT and the USB sound card inside the Android app are not yet tried on a phone with a radio |
| Continuous integration | Every merged change | Unit tests, two-daemon integration tests, ASan/UBSan, TSan; builds for Linux amd64, arm64, armhf, Windows x64, macOS arm64, and the Fyne GUI. The integration tests also run on a fresh random channel seed each time, and every seed is logged so a failure can be replayed (#415) |

## Known limits

- A callee that starts listening late makes the connect go to MFSK on a good
  channel; the session then needs a minute or so to find OFDM again.
- At 30 Hz of Doppler (itur-hd) it is just usable.
- BW500 at -12 dB is slower than before.
- Optional encryption (#410) is not in this release.
- Android: the app must stay in the foreground (no background service yet);
  only CP210x USB-serial chips; a device picked in Soundcards may have to
  be picked again after the radio is plugged in again.  Tested on Android
  15 only: one function the app uses arrived in Android 11, so older
  versions may not start it.
