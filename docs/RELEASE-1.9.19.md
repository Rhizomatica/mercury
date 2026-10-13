# Mercury 1.9.19: what changed since 1.9.18

1.9.19 moves the MFSK floor to the centre of the passband, where every other
mode already is. 1.9.18 sent its 32 tones at 1531-2500 Hz; 1.9.19 sends them at
1015.6-1984.4 Hz, centred on 1500 Hz like the codec2 modes. One passband
setting on the radio now suits every rung. The move was reported by an
operator on 1.9.18, who saw the MFSK signal start at 1.5 kHz instead of being
centred there.

**It does not connect to 1.9.18 or any older version, in either direction.**
Upgrade every station you talk to at once.

## Before you upgrade

- **Upgrade both ends: 1.9.19 and 1.9.18 do not connect.** The two would
  otherwise connect on DATAC16 and then never hear each other once the session
  fell to the MFSK floor. So the connect frames now carry protocol version 2,
  and the versions refuse each other cleanly, as 1.9.18 refuses 1.9.x:
  - a 1.9.19 station logs a 1.9.18 (or older) CALL as "CALL for ... from
    another protocol version (Mercury 1.9.18 or older?): not answered", and
    never transmits;
  - a 1.9.18 station logs a 1.9.19 CALL as "... (Mercury 1.9.x?): not
    answered", and never transmits;
  - the caller gives up after its CALLs, and nothing stays keyed.
  Measured with two real daemons over the channel simulator, 1.9.18
  (`9df34e9`) against this release, both directions.
- **1.9.x is still out.** Everything in
  [UPGRADING-TO-1.9.18.md](UPGRADING-TO-1.9.18.md) about 1.9.17 and older still
  applies.
- **Nothing else changes for an operator.** The configuration file, the TNC
  commands, the broadcast plane and its frames are as in 1.9.18.

## What's new

- **MFSK centred on 1500 Hz** (#375). The carrier moved from 2000 Hz to
  1484.375 Hz. The transmit mixer mirrors the spectrum, so the tones sit half a
  tone spacing above the carrier. All four MFSK mixers (transmit, receive, the
  pattern detector and the streaming pattern detector) now read one phasor
  table. The 2000 Hz carrier was a quarter of the sample rate, which the
  receive mixers used to skip trigonometry; the table keeps that saving. Listening
  CPU on a Raspberry Pi 4 is unchanged (below).
- **JNOS 2.0 over Mercury** ([JNOS.md](JNOS.md), #377). JNOS drives Mercury
  through its VARA driver. On the bench, BBS-to-BBS sessions and telnet from
  one station to the other station's Linux worked over the air, in both
  directions. The telnet bytes travel inside the Mercury session: there is no
  IP on the air. Stock JNOS always asks for BW500, which since 1.9.18 keeps a
  session at DATAC3 or below. Our JNOS fork adds `vara bw <interface> 2300`
  (Rhizomatica/jnos2 `4850ff3`), and the patch has been offered upstream.

## Fixes

- **`mercury -r` (the receive test) decoded no MFSK, hung on Ctrl-C, and lost
  its output** (#376).
  - It dropped every frame decoded while the decoder's sync flag was clear, and
    MFSK clears that flag at the very moment it decodes a frame.
  - It read audio with a blocking call that nothing woke once Ctrl-C had
    stopped the audio thread, so it hung in every mode.
  - Redirected to a file, its output was buffered and lost when it was killed.
  It now decodes MFSK (3 of 3 frames on air), exits within 1-2 s of Ctrl-C, and
  writes each frame as it decodes. Normal sessions and the broadcast plane never
  used this code.

## What was tested

| Test | Setup | Result |
|---|---|---|
| Mixed versions | Two daemons over `ch`; 1.9.18 calls this release, then the reverse | No connect either way; each end logs the other's CALLs as another protocol version and never keys |
| MFSK on the simulator | Two daemons over `ch` at about −10 dB SNR | The data went on 13.5 s MFSK keydowns; 102/102 bytes |
| MFSK on air | Gateway ⇄ estacao2 (both sBitx, 7.050 MHz into dummy loads), gateway at 2 % power, 1 KB over UUCP | Delivered intact (md5) in 533 s. estacao2 decoded 26 MFSK frames, and the MFSK acknowledgement patterns worked both ways at the new position |
| Transmit level | Gateway at 20 % | 2.5-2.8 W at SWR 1.0, as for 1.9.18 (2.1-3.0 W): moving the tones does not change the drive |
| Idle CPU, Raspberry Pi 4 | The gateway in idle LISTENING, 60 s per reading, interleaved | 1.9.18: 27.8 % and 28.3 % of one core. This release: 30.1 % and 28.2 %. Within the usual spread |
| On-air sessions on the release code | Both stations on `6d6998b` all evening: JNOS BBS-to-BBS, telnet to the other Linux both ways, at BW500 and BW2300 | All worked; at 2300 the sessions climbed to DATAC1 and QAM16C2 |
| `mercury -r` | Pipes on a PC, and on air | MFSK 4/4 and DATAC3 6/6 frames over pipes; MFSK 3/3 on air; exits on Ctrl-C |
| Continuous integration | Every merged change | Unit tests, two-daemon integration tests, ASan/UBSan, TSan; builds for Linux amd64, arm64, armhf, Windows x64, macOS arm64, and the Fyne GUI |

Known limits are as in [RELEASE-1.9.18.md](RELEASE-1.9.18.md).
