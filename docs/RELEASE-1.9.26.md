# Mercury 1.9.26: what changed since 1.9.25

1.9.26 connects to 1.9.25, in either direction: no connect, poll or data
frame changed. It fixes what field stations running BPQ32 gateways, JNOS
and Pat reported in the week after 1.9.25. The peer is now told when a
session ends badly. A late connect is no longer lost to a scanning host.
Short messages are faster where a rung had failed or the start was misread.
And ACK patterns are heard from a peer whose radio is off frequency.

### Which versions talk to each other

| Group | Versions | Connects to |
|---|---|---|
| 1 | 1.9.25, 1.9.26 | one another |
| 2 | 1.9.20 to 1.9.24 | one another |
| 3 | 1.9.19 | 1.9.19 |
| 4 | 1.9.18 | 1.9.18 |
| 5 | 1.9.17 and older | one another (the ARQ before the carousel) |

1.9.24 and older are still refused, and logged, as in
[1.9.25](RELEASE-1.9.25.md). Broadcast (KISS, port 8100) is unchanged.

## Before you upgrade

- **`DRIVELEVEL` now lowers the transmit level** (#598). 1.9.25 answered it
  `WRONG` and changed nothing. Now `DRIVELEVEL <dB>`, from −30 to 0, is
  an attenuation on top of `tx_gain_db` for the data frames, and below the
  `TUNE` level for the carrier. It never raises the level, and it is not
  saved: every start is at 0 dB. **If your host program sends `DRIVELEVEL`,
  check your output power after the upgrade.**
- **The TNC command parser reads the whole line** (#598). A command needs a
  word boundary after its name: `LISTENING ON` or `ABORTX` is no longer
  taken for `LISTEN` or `ABORT`, and extra words after `CONNECT`, `TUNE`,
  `MODE` and similar are `WRONG`. The same goes for a callsign longer than
  15 characters, which used to be cut short. `CONNECT A B VIA C` is
  `WRONG`: Mercury does not relay, and before, it called B directly. A
  `CONNECT` from a `MYCALL` whose callsign is too long for a CALL frame
  (over 14 characters) is `WRONG`; before, it answered `OK` and sent
  nothing. `GATEWAY <anything>` answers `OK` and changes nothing.
- **A scanning host hears `CANCELPENDING` later** (#657). When a callee's
  ACCEPTs run out, it keeps the host on `PENDING` until 90 s after its last
  ACCEPT left the air, then sends `CANCELPENDING`. 1.9.25 sent it straight
  away, and a BPQ32 scan hopped away from a caller that had connected.
- **After a lost peer, `DISCONNECTED` comes once our one DISCONNECT is
  out** (#648), not at once, so a scanning host doesn't retune while we
  still transmit.

## What's new

- **`[arq] call_mfsk500`** (#654, default `true`: no change). Set to
  `false`, a BW2300 or BW2750 caller sends no 500 Hz MFSK CALLs. Each of
  those lasts 27 s, which no scanning gateway stays on one frequency for.
  It alternates the 1 kHz MFSK floor and DATAC16, as 1.9.24 did. This is for
  stations that mostly call Winlink RMS or BPQ32 scan lists. It costs
  about 3 dB of reach to a station that listens on one frequency. The
  called station needs nothing.
- **BPQ32's idle timer is kept off a stalled upload** (#645). A station
  receiving an upload has nothing to confirm, so `BUFFER` stays at 0. BPQ32
  then closed the CMS session about 130 s after the last delivered byte.
  While the peer's data is still on its way and nothing has passed for 50 s,
  Mercury now sends `BUFFER 1` and at once the true count. An idle session
  gets no pulse.
- **Logs**: the connect line names the carrier the handshake used, MFSK
  included (#643). A line says where the peer's MFSK arrives when it is
  off frequency (#661). The frame-in-flight figure at a scanner's
  `LISTEN OFF` is measured again; it always read 0.0 s (#647). A build from
  GitHub's Download ZIP knows its commit (#660).

## Fixes

- **The peer is told when a session ends badly.**
  - A station whose lost-peer watchdog gives up cuts its keydown, then
    sends one DISCONNECT, never retried (#646, #648). On a one-way link
    the peer still hears it and doesn't sit out its own 240 s.
  - `ABORT` no longer cuts a keydown that is our DISCONNECT, or our answer
    to the peer's, so the peer is told (#665). PTT can stay on for the rest
    of that one frame: 3.7 s on DATAC16, 13.5 s on the floor at BW2300.
  - The DISCONNECT that answers a late ACCEPT, after the host gave up a
    call, waits the 700 ms reply guard, so the callee is back in receive to
    hear it (#673).
- **A late connect is kept.** As well as #657 above, a late connect from
  the ACCEPT fallback keeps the session's bandwidth: a BW500 caller and a
  BW2300 callee no longer choose rungs for different bandwidths (#658).
- **Short messages.**
  - A poll for a small answer asks only for the frames it needs, so a lost
    poll no longer holds the caller through an oversized slot (#641).
  - A small message leaves a failing rung (#642).
  - No probe goes up from a rung that has delivered nothing (#656).
  - A lost poll doesn't mark the rung it asked for as failed for the rest
    of the session (#666): on air, 8 KB took 218 s on DATAC1 against
    76 s.
  - A start misread low at the connect follows the next readings up
    (#668): on air, a −2.6 dB reading on a 12-17 dB link held UUCP on the
    2 kHz MFSK for 80 s.
- **ACK patterns.**
  - The pattern detectors look for the peer where its MFSK frames
    arrived (#661). A peer off frequency by more than about 90 Hz, whose
    frames still decode, has its ACKs and BREAKs heard too.
  - A pattern keyed exactly on time is no longer thrown away as early by
    a station whose sound card plays out late (#663).
- **Radio**: reopening a network rig's Hamlib session after rigctld
  restarts no longer races rigctld closing the old connection (#655).
- **GUI**: the TX gain slider shows the engine's gain, also after a
  restart, and sends it once per drag instead of on every step, each of
  which rewrote `mercury.ini` (#659, #664).
- **macOS**: the DMG's binaries and app target macOS 15.0, not the macOS
  of the machine that built them, so they start on macOS 15 (#650).
  The app reports Mercury's version, not 0.0.1 (#652).
- **Build**: the top-level Makefile rebuilds the modem, host and iOS
  objects when a header changes (#672).

## Known issue: JNOS's 60 s CONNECT timeout

JNOS gives up a `CONNECT` after 60 s. A connect that needs the MFSK floor
takes about 71 s, so it never completes from JNOS. Mercury's side of the
cleanup is #673 above. The fix that lets those calls connect is a longer
JNOS timeout (120 s or more).

## Documentation

The TLA+ models as of November 2026, in `docs/presentations`. The README's
1.9.25 compatibility note and Sailfish build version (#644). CI's
integration suite no longer fails at random on the rigctld test, a hold
test's deadline or its overall timeout (#655).

Thanks to Pedro Messetti and Tom-Neverwinter, and to the stations whose
logs found these: K7EK, KG9NI, N3JXB and VE4KLM.
