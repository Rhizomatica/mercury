# Mercury with a Hermes-Lite 2

A [Hermes-Lite 2](https://github.com/softerhardware/Hermes-Lite2/wiki) (HL2)
is a network SDR: it has no sound card and no CAT port, and streams I/Q
samples over Ethernet instead.  With `-x hl2` Mercury is the SDR host
itself.  It talks openHPSDR protocol 1 to the radio, does the
upper-sideband modulation and demodulation, tunes the radio and keys it.
There is no sound card, rigctld, CAT or PTT to set up.  It works the same
on every platform Mercury runs on: Linux (including the HERMES stations),
Windows, macOS, Android and iOS.

## Quick start

```sh
mercury -x hl2 -i auto -F 7074000
```

- `-i auto` (or no `-i`) takes the first HL2 that answers a broadcast on
  the local network.  `-i 192.168.0.86` (or a host name, or `host:port`)
  addresses one radio, which also works across subnets.
- `-F` is the dial frequency in Hz, upper sideband.  **Without a frequency
  the radio receives but never transmits**, so a forgotten setting cannot
  put a signal on a random frequency.
- `mercury -x hl2 -z` lists the radios that answer a broadcast.

In the GUI: **Soundcards → Audio Subsystem → Hermes-Lite 2 (network)**,
Apply, then pick the radio (or "the first one found") as the capture
device.  The Hermes-Lite 2 section below sets the frequency, RX gain, TX
drive and the PA.  These settings apply at once, also during a session, and
are saved.

## Settings

In `mercury.ini`:

```ini
[main]
sound_system = hl2
input_device = auto          ; or the radio's address

[hl2]
frequency_hz = 7074000       ; 0 = unset: receive only
rx_gain_db = 20              ; LNA, -12 .. +48 dB
tx_drive = 128               ; 0 .. 255
pa = true                    ; the 5 W PA; false = the ~10 mW output
```

- **TX drive**: the HL2 uses 16 steps of 0.5 dB (the top 4 bits).  At 0 it
  keys with no RF at all.  128 is about -3.5 dB, roughly 2 W.  Mercury's
  own TX gain (`[audio] tx_gain_db`) scales the signal before it reaches
  the radio, as with a sound card.
- **RX gain**: raise it until the noise floor shows, lower it if Mercury
  logs `ADC overload`.  Mercury runs its own AGC after the demodulator, so
  the gain only sets the radio's dynamic range, not the audio level.
- **Filters**: the N2ADR filter board's low-pass for the band, and the 3 MHz
  high-pass above 160 m, are switched from the frequency.

## What Mercury does on the wire

- **Pacing:** the radio's 48 kHz clock paces everything.  Each EP6 packet
  from the radio (126 RX samples, 2.625 ms) is answered by one EP2 packet
  (126 TX samples and two registers), so TX cannot drift from the radio's
  clock.  If the radio's stream pauses while Mercury is transmitting, TX
  carries on from the local clock until it comes back.
- **PTT:** MOX rides in every packet, set while the modem transmits or
  still has audio queued.
- **TX buffering:** the HL2 buffers 40 ms of TX samples before RF starts
  (its "TX latency" register; less is choppy).  Mercury's 100 ms PTT tail
  flushes that buffer before MOX drops.
- **During TX:** the radio keeps streaming RX while transmitting (duplex),
  and those samples are its own signal, so Mercury mutes its receiver for
  the transmission and 80 ms after it.  Its LNA drops to -12 dB while
  transmitting.
- **Sideband:** a signal above the dial frequency arrives at a negative
  frequency of (first word) + j(second word), as WDSP and every host built
  on it (piHPSDR, deskHPSDR, Thetis) assume, and as deskHPSDR's simulator
  generates; checked on a real HL2 with an AM broadcast carrier.  Mercury conjugates at the wire, so its own DSP works with
  upper sideband on positive frequencies (`audioio/hl2_wire.h`).
- **Keeping the link up:** lost UDP packets are filled with silence and
  counted.  If the stream stops for 2 s, Mercury asks the radio to start
  again.  If the radio is absent, or busy streaming to another program,
  Mercury reports it in the GUI and keeps looking.
- **Telemetry:** Mercury logs the forward and reflected power after each
  transmission, and warns about TX buffer underruns and ADC overloads.

## Networking

- **Same subnet:** broadcast discovery (`auto`) only reaches the local
  subnet.  On another subnet, give the radio's address.
- **Prefer a cable:** the stream is about 3 Mbit/s each way.  Phones and
  laptops on Wi-Fi may throttle the upload with power saving, which
  starves the radio's TX buffer.  Mercury marks its packets as voice
  (DSCP EF), but a cable is better.
- **One host at a time:** the HL2 talks to one host.  Close another SDR
  program (Thetis, Quisk, SparkSDR, piHPSDR) before starting Mercury.

## How it was tested

| Test | Result |
|---|---|
| Wire codec, unit test (`tests/audioio/test_hl2_wire.c`) | Every packet and register byte pinned, hand-derived from deskHPSDR and RADE_decode |
| SSB modem, unit test (`tests/audioio/test_hl2_dsp.c`) | Passband 200-2950 Hz flat within 0.2 dB; lower sideband, DC and the interpolation images > 60 dB down; AGC never clips a signal 60 dB above the noise |
| deskHPSDR's radio simulator (`hpsdrsim -hermeslite2`) | Its decode of every register is as intended; its 14.1 MHz test signal demodulates as a 1000 Hz tone when tuned 1 kHz below it, and is rejected when tuned 1 kHz above it |
| Two Mercury daemons, two emulated HL2s (`tests/integration/mercury_hl2_linux_test.go`) | Connect, 2 KB transfer, DISCONNECT on both ends; TX in exact lockstep with the radio clock, no TX buffer underruns |
| A real HL2 (gateware 73.2, N2ADR filter board), receive only | Discovery, streaming and the registers work. A local AM station at 880 kHz lands at +1998 Hz tuned to 878 kHz and at -2002 Hz tuned to 882 kHz: the sideband convention holds on hardware. Tuned to 879 kHz it demodulates as a 1000 Hz tone, and tuned to 881 kHz it is rejected. Transmitting has not been tried yet |

## Not yet

- **No CAT emulation:** applications that set the frequency themselves
  (Pat's rig control, VarAC) cannot reach the HL2.  Set it in Mercury.
- **No PureSignal** (TX predistortion), and one receiver only.
