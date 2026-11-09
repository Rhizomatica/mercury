# Mercury 1.9.23: what changed since 1.9.22

1.9.23 is mostly fixes from field reports by stations running BPQ32, LinBPQ,
JNOS and QTermTCP: scanning hosts, sessions that would not end, and calls
that went on for minutes. It also caps the log file, and the GUI's chat
window now drives its own engine directly instead of through the TNC ports.

**It connects to 1.9.22, 1.9.21 and 1.9.20 in both directions.** No connect,
poll or data frame changed. The one addition on the air is a broadcast frame
type for the HERMES beacon, which older releases ignore.

## Before you upgrade

- **1.9.20 to 1.9.23 all connect to each other.** 1.9.19 and older still
  connect to none of them; see [RELEASE-1.9.20.md](RELEASE-1.9.20.md).
- **BPQ32 and LinBPQ: `RETRIES=20` is now applied** (#479). Mercury used to
  answer that form `WRONG`, so those stations ran on the default retries.
  They now get 4 fast DATAC16 CALLs and up to 20 retries, but a call stops
  keying new CALLs after 300 s (`[arq] call_max_s`, #476). Set
  `call_max_s = 0` for no cap.
- **The log file is capped at 20 MB** (`[main] log_max_mb`, #478). Past it,
  the file moves to `<file>.1` and a new one starts. Set `log_max_mb = 0` to
  keep one growing file, as before.
- **A `RETRIES` or `CALLINT` value that isn't a whole number is now
  `WRONG`** (`RETRIES 10x` used to be taken as 10). Every `WRONG` reply is
  now logged as a warning, with the line as received.
- **vARIM 1.13 and later need no `max_keydown_s`.** They allow 35 s keyed,
  longer than any Mercury transmission (30 s at most). With an older vARIM,
  keep `[arq] max_keydown_s = 16` on every station that runs it.

## What's new

- **The GUI's chat window drives its own engine directly** (#486, #491). With
  the engine in the same program, the Mercury Client window no longer
  connects to it over the TNC ports (8300, 8301, 8100), so they stay free for
  uucp, VarAC or another TCP client. One that connects takes the TNC from
  the window, as it would from another TCP client, and the window says so.
  The main window's TCP indicator reads **In app** while the window holds
  the TNC. A remote engine is still reached over TCP.
- **`[arq] call_max_s`** (#476), default 300 s, 0 for no cap, at most
  3600. Once a call has run that long, no new CALL is keyed; the CALL
  already on the air keeps its ACCEPT window, so a late answer still
  connects.
  - **Why:** BPQ32 opens every connect with `RETRIES 20`, about 8 minutes of
    CALLs. In K7EK's log for 3-6 November, 18 unanswered calls each keyed
    for the full 8 minutes, while 20 of the 21 calls that connected did so
    within 300 s (the slowest at 321 s).
- **`PENDING` again on each repeated CALL while answering** (#477). BPQ32
  holds its scanner for six scan intervals per `PENDING`, then hops, and
  the hop cancelled connects still being answered: 5 in K7EK's log, 72-85 s
  after the first `PENDING`. A CALL heard again from the station being
  answered now renews the hold. Other VARA hosts get one more `PENDING` per
  repeated CALL.
- **`[main] log_max_mb`** (#478). Each log file has a line naming the other.
  Rotation loses no lines. Under a scanning BPQ32, K7EK's log had grown to
  170 556 lines.
- **The log names the build** (#494): `Mercury 1.9.23 (git <hash>)` right
  after the logger starts.
- **The busy detector can be switched while running** (#490). The TNC
  commands are `BUSYDETECT ON|OFF` and `BUSYRESEED ON|OFF`; bare, each
  reports its state.
  - `BUSYRESEED` (ini `[channel] busy_reseed_on_hop`, default off) takes a
    new noise floor at each scan hop. It trades false BUSY on a noisier
    channel for a missed BUSY when the hop lands on a signal wider than about
    1.8 kHz already on the air (BW2300 DATAC, VARA 2300).
  - Switching the detector off while it reports BUSY sends `BUSY OFF`.
  - Each BUSY and CLEAR verdict is logged with what made it (#489).
  - The detector is still off by default.
- **Station messages** (#480), packet type 0x07, for the HERMES position
  beacon. A host sends one on DATAC16, MFSK or MFSK500, which every listening
  Mercury decodes, and every station hands up what it hears, whatever its
  listen mode. Older releases drop the type.
- **Acquisition of DATAC15 and DATAC16 is about 1.5-2 dB more sensitive**
  (#455): their acquisition threshold went from 0.45 to 0.40. On AWGN,
  DATAC16 at -12 dB acquires 51 of 100 frames instead of 19, with no false
  syncs. CPU on a Raspberry Pi 4 is the same. On air, 16 runs on our bench
  were a tie. The study is in
  [ACQ-TUNING-FINDINGS.md](ACQ-TUNING-FINDINGS.md); a study of QAM16C2 under
  peak-limited radios, not yet enabled, is in
  [CLIPPED-QAM16C2-FINDINGS.md](CLIPPED-QAM16C2-FINDINGS.md) (#464).
- **The carousel logs what an application's data waits for** when it can't
  go at once (#499), such as the peer's answer slot. On air, N3JXB's
  commands took 2-11 s to key, and the log didn't say why.
- **Sailfish OS:** the RPM spec lives in this repository (#461), and the GUI
  builds natively on the phone (#463). The HERMES Sailfish repository now
  carries `mercury` and `mercury-ui`; see the README.
- **Android:** audio restarts when the microphone permission is granted late
  (#462). **GUI:** Help > Submit issue opens the issues list (#466).
- **README:** installing on iOS from the App Store (#465).

## Fixes

- **A callee that hangs up owing the caller an ack now ends cleanly**
  (#468). At the end of 4 of 5 clean B2F forwards between K7EK and KG9NI,
  the caller logged `Peer ended the session: 3 bytes of ours
  undelivered`: the callee's DISCONNECT answered the caller's last data
  instead of an ack. The callee now acknowledges first. It costs one more
  control transmission: in the simulator, 2-5 s more at the end of a session
  on good channels, the same at the floor.
- **A DISCONNECT on a dead link ends promptly, and a redial gets through**
  (#471, N3JXB). On the MFSK floor the rig could go on keying for minutes:
  - a second DISCONNECT re-armed the drain;
  - a CONNECT waited behind it unseen;
  - the drain ran its whole 148 s budget even when the peer had gone silent;
  - the DISCONNECT waited for slots a silent peer wasn't using.

  Now a second DISCONNECT or a CONNECT ends the drain at once, as `ABORT`
  does, and a drain ends once the peer has been silent 120 s.
- **The session ends soon after the callee hangs up and goes silent**
  (#472). The caller polled a silent callee for 4 minutes, until the
  lost-peer watchdog. Now it ends with one DISCONNECT once the callee has
  been silent as long as its own drain would last, at most 120 s.
- **A session cut by a scanner's `LISTEN OFF` is logged, and no longer
  reports the next incoming session backwards** (#473). The release itself is
  unchanged: no drain and no DISCONNECT frame after a `LISTEN OFF`.
- **`CONNECTED` names the two stations in the right order** (#483). A
  `CONNECT` that met an incoming CALL could be reported as `CONNECTED <peer>
  <us>`, and BPQ32 doesn't route such a session. The side now comes from the
  state machine. An outgoing call also no longer ranks its retries with the
  last incoming CALL's dialed callsign (#485).
- **A rung the SNR rules out is no longer re-probed** (#475, VE4KLM-K7EK on
  30 m). A failed DATAC3 was polled again at -4.2 dB, below its gate; the
  first time, the next frame decoded 71 s later.
- **A receiver whose link goes dark drops a rung sooner** (#497, NW8L). Two
  rounds in a row with nothing at all from the peer now make a rung dead at
  two losses. Before, NW8L's receiver went on polling DATAC3 after a +5 to -5
  dB drop for 230 s, and the session was lost 7 s after it reached the floor.
  In the simulator, the longest silence fell from 130-196 s to 111-149 s.
- **A scanner's hop drops every partly received CALL** (#492, #498). An MFSK
  or DATAC16 CALL that straddled the hop could decode on the next channel
  and be answered there, holding BPQ32 on the wrong channel for about 2
  minutes: 6 of K7EK's 10 failed incoming calls on 7 November. The caller's
  next CALL is answered instead.
- **A DISCONNECT no longer keys over the peer's answer to a long round**
  (#500, NW8L). The DISCONNECT waits for the channel in 0.5 s steps, at most
  10 s, and the steps while the station was still keyed counted toward that.
  A 27 s MFSK floor round used up the 10 s, and the DISCONNECT went out
  22 ms after the round, over the peer's ACK: both were lost, and the
  teardown took 46 s and two retries. The 10 s now counts from the end of
  the station's own transmission.
- **The busy detector stays muted until a session's teardown is over**
  (#487), so it no longer delays a DISCONNECT with a false BUSY (11 s in one
  K7EK session).
- **CI:** the i386 and TSan jobs run again on the ubuntu-26.04 runner image
  (#488), and the GUI builds and tests without the embedded engine too (#467).

## What was tested

| Test | Setup | Result |
|---|---|---|
| Protocol models | TLA+ (`specs/`), each with deliberately broken variants that must fail. Teardown: #468's owed ack, #471's second DISCONNECT and redial, #472's silence after M_END. Rungs: #497's rule, with a rung that dies mid-session (`Kill`) and the step-down after K unanswered asks (K = 4 and 2) | `SessionTeardown` lossless and lossy: no error. All 13 teardown mutants fail, including the code before each fix (`Mut_ackless`, `Mut_rearm`, `Mut_redialwait`, `Mut_endsilence`). All 23 rung configurations give their documented results, and the mutant without the step-down (`Mut_nodrop`) fails `Delivered` |
| Simulator | `tests/sim`, AWGN, cliffs, fading, NVIS, steps, on fixed seeds | #497: the longest run of unanswered polls 9-12 -> 5-8, NVIS two-way 56 -> 58 of 100 done. #468: the end of a session 2-5 s longer on good channels, the same at the floor. #499: output identical to the build before |
| Acquisition | `utils/acq_tuning_sweep`, AWGN, 100 trials per point | #455: DATAC16 at -12 dB 19 -> 51 of 100 acquired, no false syncs |
| Mixed versions | Two daemons in the Go harness, 1.9.22 against this release, each as station A and as station B: a transfer, a two-way session, both cross-bandwidth connects, and a DISCONNECT to a peer on the floor | All pass in both mixes. Across bandwidths, connects in 51 and 77 s either way; transfers connect in 8 s |
| GUI chat window | The window's in-process TNC against a CLI Mercury over FIFO audio, under Go's race detector | Connect in 8.5 s, chat both ways, 12 KB through the 8 KB receive ring, eviction by a TCP client, and a clean shutdown with the TNC held. No data race |
| On air | Field reports with logs: K7EK, KG9NI, N3JXB, VE4KLM and NW8L, on 1.9.22 | Each fix above reproduces the report it cites. #455: 16 runs between our two sBitx stations, a tie |
| Continuous integration | Every merged change | Unit tests (also on i386), two-daemon integration tests, ASan/UBSan, TSan. Builds for Linux amd64, arm64 and armhf, Windows x64, macOS arm64, Android, and the Fyne GUI with and without the engine |

## Known limits

- **`DISCONNECTED` still comes at once after a host's `DISCONNECT`**, as in
  1.9.22, so a scanning BPQ32 can restart its scan and cut the on-air
  teardown short. Sending it at the real end of the session, as VARA does,
  is a draft (#493) waiting for an on-air check with BPQ32.
- **Not yet checked on air with a scanning BPQ32:** #477's repeated
  `PENDING` and #498's DATAC16 flush.
- **Mercury puts its primary callsign on the air as the caller**, whatever
  source the host's `CONNECT` gave (BPQ32 forwarding as a secondary
  callsign). Open in #484.
- **On Sailfish OS the GUI started from its icon has no CAT or PTT:** a
  radio's USB device needs root on the phone. Neither package has been used
  with a radio there yet.
- **The busy re-seed** (`BUSYRESEED`) misses a wideband signal already on a
  channel the hop lands on, until it stops. It is off by default.
