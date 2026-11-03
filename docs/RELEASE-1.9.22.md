# Mercury 1.9.22: what changed since 1.9.21

1.9.22 lets stations on different bandwidths connect on the MFSK floor. It
also lets a station keep its transmissions short for vARIM, and fixes a modem
bug that could crash the receiver or leave it searching old audio. The
connect and the fallback to the floor are now tunable.

**It connects to 1.9.21 and 1.9.20 in both directions.** Nothing on the wire
changed.

## Before you upgrade

- **1.9.20, 1.9.21 and 1.9.22 all connect to each other.** 1.9.19 and older
  still connect to none of them; see [RELEASE-1.9.20.md](RELEASE-1.9.20.md).
- **Upgrade the station that gets called** to get the faster connects
  across bandwidths (#451): the second MFSK listener runs on the callee. A
  1.9.21 callee still connects, but later, on a DATAC16 CALL.
- **vARIM users: set `[arq] max_keydown_s = 16` on every station that runs
  vARIM** (#445). See "What's new".
- **Log lines now start with the date** as well as the time
  (`2026-11-02 23:15:57.123`). Scripts that parse Mercury's text log may
  need updating. `utils/onair_logs.py` already handles it.

## What's new

- **A waiting station hears MFSK calls on both bandwidths** (#451, #442).
  Before, if a call had to use the slow MFSK floor (a weak path, or a
  caller that only transmits MFSK), it never reached a station waiting on
  the other bandwidth:
  - a station at BW500 didn't hear the 1 kHz MFSK call of a BW2300 or BW2750
    caller;
  - a station at BW2300 or BW2750 didn't hear the 500 Hz call of a BW500
    caller.

  While listening for calls, each station now decodes both. The session
  then runs at the narrower bandwidth.
  - **On air:** gateway at BW2750, station at BW500, with an MFSK-only
    caller. 0 of 4 connected before, 5 of 5 after, in 27.6-54.3 s.
  - **Cost:** the second decoder takes 1.3-2.3 % of a Raspberry Pi 4 core
    while the station waits for a call.
  - **A CPU fix found on the way:** a receiver fed exact digital silence
    used to spin at 84-114 % of a Pi 4 core; now about 29 %.
- **`[arq] max_keydown_s`** (#449, #445). A station can hold every one of
  its transmissions to a set length, 5-30 s (default 0, the protocol's
  30 s). It's meant for TCP clients that give up when PTT stays on too long:
  - vARIM sends `ABORT` after about 20 s keyed, and a full carousel round is
    25-30 s, so set 16 on every station that runs vARIM.
  - The limit counts the whole transmission, including a poll sent before a
    round. The station also asks its peer for rounds that short.
  - A peer without the setting can still transmit for up to 30 s, so set it
    at both ends.
  - **Cost** in the simulator: 0-6 % of throughput on fast channels, 13-30 %
    on slow ones and at BW500.
- **`[arq] silence_fallback_s`** (#444, #442). How long a session hears
  nothing from the peer before it moves to the MFSK floor: 150 s, as before
  (range 110-600).
  - **Lower:** faster when a channel drops out, but slower on multipath,
    where it moves to a floor it didn't need. In the simulator, 110 s is
    1-2 % faster on a channel that steps down and 11 % slower on NVIS.
  - **Higher:** the reverse.
  - Not measured: the gain under interference, because the simulator has
    no QRM.
- **The fast CALLs follow `RETRIES`** (#443, #442).
  - **What changes:** a host that sends `RETRIES n` gets n/2 CALLs on the
    fast DATAC16 mode (at least 2, at most 4) before the first MFSK CALL.
    Without `RETRIES`, it's 2, as before.
  - **Why:** short DATAC16 CALLs are more likely to be heard by a station
    that is scanning, and any listener decodes them whatever its bandwidth.
    The cap keeps the first MFSK CALL, which a weak path needs, at about
    47 s.
- **Log timestamps carry the date, and can be in UTC** (`[main] log_utc =
  true`, which adds a `Z`). Local time stays the default (#442, #450).
- **A session's teardown is logged** at INFO level: when the host's
  `DISCONNECT` is deferred until delivery, when teardown starts, and when
  the peer's `DISCONNECT` arrives (#442).
- **README: installing on Sailfish OS, step by step** (#456), from the
  HERMES repository (technology preview; not yet used with a radio there).

## Fixes

- **The modem could read outside its receive buffer, or search old audio,
  after an unsync** (#457). Found by the crash in the acquisition sweep of
  #455.
  - **The bug:** a postamble sync moves the decoder's search position back to
    the start of its packet. An unsync then cleared the buffer but left the
    position there. The decoder went on searching audio about 4 s old, and
    the next postamble sync moved the position before the start of the
    buffer: an out-of-bounds read, and on DATAC15 a crash.
  - **Where Mercury unsyncs a decoder:** on a rebind, and on the payload
    decoder after every control frame it decodes (#223).
  - **The fix:** an unsync now resets the position, as the decoder already
    did itself on every other return to search.
  - Upstream codec2 has the same code.
- **An unanswered probe round now falls back to the last good rung when the
  turn passes back** (#448, #452).
  - **Before:** a round sent behind a poll on a rung the far station hadn't
    confirmed got no answer, and the next round went out on the same rung.
    The far station had already gone back to the rung it last heard.
  - **Seen on air** between K7EK and VE4KLM on 10.143 MHz: an 11 s DATAC4
    round keyed at a peer listening on MFSK, about 30-40 s lost.
- **The daemon build for hermes-radio-daemon links again** (#453). It was
  missing two Android-only functions.
- **The GUI's "Embedded client" checkbox is gone.** The chat client is
  always available, as before the checkbox existed. The interlock that
  stops it taking the TNC ports from a connected client stays.
- **CI:**
  - macOS builds run on the macos-15 runner image (#454), because macos-14
    was retired on 2026-11-02;
  - CI also builds the Fyne GUI for Linux arm64.

## What was tested

| Test | Setup | Result |
|---|---|---|
| Protocol models | TLA+ (`specs/`), each with deliberately broken variants that must fail. Rungs: #448's fall-back with a probe round lost on its own (`WastedF`, `FallbacksAgree`). Connect: #443's fast CALL count as a constant, up to 10. Floor silence: #444's settings over the whole range, with the two ends set differently | No error. Every mutant fails, including the code before #448 (`Mut_norecvfb`, which needs 2 wasted rounds where the fix needs 1) |
| Simulator | `tests/sim`, AWGN, cliffs, fading, NVIS, steps. Each build compared with the one before on the same seeds | #448: fires in 4 of 360 paired runs, same means. #444 at its default and #449 off: every run identical. #443: a cap of 4 is the best for floor-only paths and for scanning stations |
| Receiver unsync | `tests/modem/test_ofdm_acq.c`: a sync on a postamble alone, an unsync, then the next burst. The #455 sweep under AddressSanitizer | Before #457: the next burst lost in 4 of 6 cases, and a crash on DATAC15. After: 6 of 6, no sanitizer error. The MODES.md acquisition table is unchanged |
| Cross-bandwidth connect | Two daemons in the Go harness, BW2750 calling BW500 and BW500 calling BW2300, down to about -13 dB | Before #451: no connect in 2 of the 4 noisy cases. After: all connect, in 51-78 s |
| Mixed versions | Two daemons in the Go harness, 1.9.21 against this release, each as station A and as station B: a transfer, a two-way session, and both cross-bandwidth connects | A transfer and a two-way session pass in both mixes. Across bandwidths every pair connects: with 1.9.22 called, in 51 and 77 s; with 1.9.21 called, in 69 and 96 s, on a later DATAC16 CALL, as 1.9.21 does on its own |
| On air | Gateway and estacao2 (both sBitx, about 25 dB), 8 KB UUCP transfers each way, hash-checked, builds alternated | #448 against 1.9.21 (29 Oct) and #457 against the build before it (2 Nov): ties, every file intact. #451 (1 Nov): connects across bandwidths 0 of 4 before, 5 of 5 after |
| CPU | Raspberry Pi 4, `-x fifo`, noise and exact silence | #451's second listener: 1.3-2.3 % of a core. Exact silence: 84-114 % before, about 29 % after |
| Continuous integration | Every merged change | Unit tests, two-daemon integration tests, ASan/UBSan, TSan. Builds for Linux amd64, arm64 and armhf, Windows x64, macOS arm64, Android, and the Fyne GUI |

## Known limits

- **`max_keydown_s` cannot shorten the BW500 floor.** Its single MFSK frame
  is 27 s and is never split, so vARIM may still abort a BW500 session that
  falls to its floor. The setting hasn't yet been used with vARIM on air.
- **#451 was checked on air only with an MFSK-only caller.** The bench link
  is strong enough that normal calls connect on DATAC16 and never reach
  MFSK. Its cost on a Pi Zero 2 W is only estimated (about 4-7 % of a core).
- **Not measured: how often the #457 bug cost a burst on air.** The resync
  it affects isn't logged.
- **`silence_fallback_s`'s gain under interference is unmeasured.**
