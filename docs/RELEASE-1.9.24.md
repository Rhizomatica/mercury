# Mercury 1.9.24: what changed since 1.9.23

1.9.24 is fixes from field reports by stations running BPQ32, LinBPQ, vARIM
and Pat (K7EK, KG9NI, N3JXB, NW8L, KA8RYU, VE4KLM): calls and hang-ups under
a scanning host, sessions that stalled when the control frames stopped
getting through, a rung dropped for frames that were never sent, and a
Windows sound card that changed its id.

**It connects to 1.9.23 in both directions**, tested, and to 1.9.20-1.9.22,
which use the same frames. No connect, poll or data frame changed.

## Before you upgrade

- **`DISCONNECTED` now comes when the session has closed** (#493), as VARA
  does, not at once after the host's `DISCONNECT`. The air-side teardown is
  unchanged; only the notice moves to its end. A scanning BPQ32 restarts
  its scan from `DISCONNECTED`, and the early notice let it cut teardowns
  short, leaving the other station polling a dead session for 240 s.
  - With no session up, `DISCONNECTED` is still sent at once.
  - `ABORT` still answers at once, and that answer is the session's only
    `DISCONNECTED`.
- **One host at a time on the TNC ports** (#546). While a host program is
  connected to the control port (8300), a second connection is refused and
  the connected host keeps the TNC; the data port works the same way.
  Before, the newcomer took over. Two BPQ32 ports set up for one Mercury
  then took it from each other for hours (KG9NI: 264 connects in one
  hour). A host that has really gone is still replaced at once, and a
  warning names both ends.
  - **A host program that hangs on the same machine keeps the port until
    it is killed**: its OS still answers the keepalive probes for it. Kill
    the old process before starting a new one. See [TNC.md](TNC.md).
- **A repeated fast CALL now waits for an ACCEPT on the MFSK floor** (#536).
  After a repeat on the control mode the next CALL is due 24 s later (37.5 s
  at BW500), not 8 s, because the callee may answer that repeat on the
  floor, and the next CALL keyed over 5 of 6 such answers on air. Where the
  fast CALLs never get through at all, that adds 16 s before the first
  floor CALL with the default two fast CALLs, and 48 s with BPQ32's
  `RETRIES 8` or more.
- **The sound cards are saved by name too** (#534): `[main]
  input_device_name` and `output_device_name`, written next to the ids when
  the GUI saves them. On Windows, a saved id that is no longer present
  (Windows gives a re-plugged USB sound card a new id) is replaced by the
  device with that exact name, if exactly one has it. A config without the
  names behaves as before.
- **The Android app needs Android 8 (API 26) or later.**

## What's new

- **Mercury is on Google Play** ([Mercury HF Modem](https://play.google.com/store/apps/details?id=org.rhizomatica.mercury)),
  since 1.9.23, as on the [App Store](https://apps.apple.com/app/mercury-hf-modem/id6822504870)
  for iPhone and iPad. Updates arrive through the store.
- **`ABORT` tells the peer once and lets go** (#514, #531). It used to run
  the whole DISCONNECT retry sequence: 53 s and three DISCONNECTs in
  N3JXB's log, to a peer that heard none of them, with the next call
  waiting 34 s behind it. An `ABORT` during a teardown already under way did
  nothing to it. Now:
  - one DISCONNECT goes out, in the peer's slot when one comes;
  - the callee keys it out of turn on a clear channel if no slot comes
    within about 10 s, before a scanning BPQ32 hops 15 s after its `ABORT`;
  - it is never keyed over a peer heard on the air; the caller waits such a
    keydown out, up to 30 s.
- **`PENDING` every 20 s while answering a call** (#543), so a scanning
  BPQ32 keeps the channel as long as Mercury is still answering. BPQ32
  holds its scan for six scan intervals per `PENDING`, and hopped 63-66 s
  into answers that were still going (K7EK, 12 November).
- **A `BITRATE` line at least every 20 s while calling** (#539). A LinBPQ
  BBS with a short session idle time dropped the call 61-62 s after the
  last line it got, before the first MFSK CALL had gone out (KG9NI, seven
  calls).
- **The callee lets go when the caller gives up while being answered**
  (#508). The caller's DISCONNECT to a stale ACCEPT was ignored, and a
  scanning BPQ32 stayed on the channel 47-104 s, missing every other caller.
  Now it ends the answer at once: `CANCELPENDING`, `DISCONNECTED`, back to
  listening.
- **A capture device that stays dead is reported, and found again by name**
  (#537, #544). K7EK's radio codec dropped for 47 minutes and came back
  under a new Windows id; Mercury reopened the old id 1380 times without
  saying anything. Now three stalls in a row with no samples (about 6 s,
  never counted while transmitting) log one error and set audio health to
  failed, which the GUI shows. The device is then looked for by name, and
  the recovery line says how long the audio was gone.
- **A shutdown while keyed tells the host `PTT OFF`** (#528). BPQ32 keys
  the radio from Mercury's `PTT ON`/`PTT OFF`, and the last `PTT OFF` was
  dropped at shutdown (K7EK).
- **macOS:** files and folders are picked in the system's open panel (#533),
  as the App Store's sandbox requires, and a forced exit works there
  (#521).
- **Sailfish OS:** taps work on the GUI (#512, #513), and text fields get
  the on-screen keyboard (#515).
- **Logs** (#525, #530): one line per Hamlib error (Windows' messages split
  lines), an engine that fails to start logs why, a host `DISCONNECT` is
  named only when there was one, and a broadcast client's leaving is not an
  error.
- **Simulator:** `CAR_CTL_SNR_OFFSET` makes the control frames read low, as
  on a selective channel, and applies it in the channel so their decoding
  follows (#523).

## Fixes

- **The control frames follow the data to the MFSK floor when they stop
  getting through** (#538, K7EK and KG9NI on 40 m, BW500). On an
  asymmetric path the caller decoded every MFSK frame of the callee's but a
  third of its DATAC16 frames, and the acknowledgement of the last message
  and both DISCONNECTs were among the lost ones: the session idled into the
  240 s watchdog. Now two unanswered asks in a row, with the callee still
  heard and a reading below -3 dB, move the callee's control to MFSK for
  60 s, or until a reading of -3 dB or more.
- **The rung estimate goes by the data frames' readings** (#545, NW8L on
  the IONOS MPD channel, #501). The narrow control frame read about 10 dB
  low there, while QAM16C2 delivered 90 %, and two low control readings
  kept the top rungs out of reach. Control readings no longer move the
  estimate while a data frame has been heard in the last 120 s. In the
  simulator with the control frames reading low, transfers are 8-25 %
  faster; with them reading right, the same within about 1 %.
- **A rung is no longer dropped for frames that were never sent** (#504,
  #511, #517, #526). A receiver that lost the tail of a round scored it as
  the size of an older poll: in NW8L's MPD run at 35 dB, one lost frame
  was scored as eight, QAM16C2 was marked dead while delivering 92 %, and
  the file went on DATAC17 at half the goodput (299 s against 240 s at
  25 dB). Rounds are now scored as what was asked about, what fits in the
  round's time, or what the sender declared.
- **A poll the peer never heard no longer holds the caller off the air**
  (#510). The caller waited out every continuation slot of two floor polls
  the callee never decoded: 97 s in NW8L's run, long enough that vARIM gave
  up on a reply delivered 25 s later. The caller now listens during each
  floor poll's first answer slot and frees the rest when nothing is on the
  air there.
- **The lost-peer watchdog waits out an answer already on the air** (#547).
  At the 240 s mark it ended a session 6 s before the callee's 27 s floor
  answer was through (NW8L, MPD at -10 dB). The caller now waits, at most,
  to the end of the answer slot of a keydown of its own that ended by the
  mark.
- **A callee that hears the caller below the control mode stays at the
  floor it asked for** (#548). When the round it asked for didn't come, it
  went back to a rung where nothing of the caller's decodes, and missed the
  caller's floor control until its own silence: in the simulator, the
  first floor round after a drop to -10 dB came 167 s after it, now 127 s.

## What was tested

| Test | Setup | Result |
|---|---|---|
| Protocol models | TLA+ (`specs/`), each with deliberately broken variants that must fail: every configuration on the release head | Every model that finished holds and every broken variant fails, as each spec's README records, including the new ones for #531, #536, #538 and #548. The largest state spaces (carousel liveness, the turns at BW500, the teardown with loss) take hours and were still running at release time, with no error so far |
| Simulator | `tests/sim`, 30 seeds per cell: cliff, fading, asymmetric, step and NVIS channels, one and two way, BW2300 and BW500, for each carousel change | No cell slower beyond noise; the gains above. #538's first version cost 14 % at BW500 and was reworked before it merged |
| Mixed versions | Two daemons in the Go harness, 1.9.23 against this release, each as station A and as station B: a transfer, a two-way session, both cross-bandwidth connects, and a DISCONNECT to a peer on the floor | All pass in both mixes |
| On air | Field reports with logs from K7EK, KG9NI, N3JXB, NW8L, KA8RYU and VE4KLM on 1.9.23; #493 and #531 on our bench with a host that scans as BPQ32 does | Each fix above reproduces the report it cites |
| Continuous integration | Every merged change | Unit tests (also on i386), two-daemon integration tests, ASan/UBSan, TSan. Builds for Linux amd64, arm64 and armhf, Windows x64, macOS arm64, iOS, Android, and the Fyne GUI |

## Known limits

- **Mercury puts its primary callsign on the air as the caller**, whatever
  source the host's `CONNECT` gave (BPQ32 forwarding as a secondary
  callsign). Open in #484.
- **A keydown already on the air at an `ABORT` runs to its end**: a keydown
  goes to the modem whole.
- **An idle link that drops below the control mode** takes about 150 s
  (`SILENT_MS`) to move the caller's control to the floor, about when
  vARIM's receiver gives up (NW8L). A shorter wait cost sessions on lossy
  idle links in the simulator; it needs another mechanism (#548).
- **After a fall-back to the floor that the callee never confirmed**, the
  caller can send up to six MFSK rounds blind (#529).
- **The caller waits out an empty response slot**, about 7 s after each
  keydown, before keying the host's next data (#495).
- **On Sailfish OS the GUI started from its icon has no CAT or PTT**: a
  radio's USB device needs root on the phone.
