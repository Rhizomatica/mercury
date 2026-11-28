# specs/connect

The connect handshake, CALL / ACCEPT / the caller's first carousel frame, as
`datalink_arq/arq_fsm.c` runs it (`fsm_disconnected`, `fsm_listening`,
`fsm_calling`, `fsm_accepting`, `car_connect_callee`).

    java -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC -workers 4 \
        -metadir /tmp/tlc-connect -config ConnectHandshake.cfg ConnectHandshake.tla

What it models:

- The caller picks a session id and a bandwidth, CALLs up to `R` times and
  gives up; the application may CONNECT again (`S` times), with any id and
  bandwidth.  It takes an ACCEPT only for its session, and may decode any
  ACCEPT the callee keyed for it, late (one in flight as its next CALL goes
  out: spec R25).
- The callee answers a new session with a fresh nonce, a measured start
  rung and the CALL's bandwidth; every repeat of the session's CALL gets the
  same ACCEPT; another station's CALL is ignored.  The caller's CALL of
  another session (it CONNECTed again: a session id per CONNECT) means it
  has left the one being answered, and the callee answers the new one,
  fresh (`fsm_accepting`) -- except while its ACCEPT of the one it answers
  could still be answered (the caller's first frames may be on their way),
  and never one decoded too late: a stale frame is not a redial.  Out
  of ACCEPT retries it keeps the seed (`accept_fallback`) until it lapses.
- A CALL the caller keyed for a session it has left may surface at the
  callee late (a decoder backlog; on air, KG9NI <- K7EK, 2026-11-17
  21:19): `StaleCall`.  The callee, answering, ignores it; `Mut_staletake`
  answers it (the switch before the age check), which violates
  `NoDeadAnswer`.
- The callee connects on the caller's first frame only on the rung it named
  and with its nonce (the CRC is seeded).  A caller that gave up and hears an
  ACCEPT sends a DISCONNECT; a connected caller whose callee never answers
  gives up after its lost-peer watchdog (`PeerLost`).  The callee's kept
  seed lapses later, as long after its last ACCEPT (`EExpire`): meanwhile
  the caller may CONNECT again, and the callee answers from its fallback.
  (The two lapsed in one step before 2026-11-23; trace validation, below,
  found the code doing otherwise.)

- Encryption (`arq_crypto.h`, `crypto_negotiate`): a CONNECT's CALLs offer
  it or not, in the session id's low bit (`Offers`: odd ids offer; reserved
  and always clear until encryption lands); the callee answers the offer of the CALL it takes, once for
  the session -- every ACCEPT of it carries that answer, as it carries the
  nonce; the caller takes an ACCEPT's bit only with an offer of its own.  A
  required caller (`CReq`) calls only to offer and hangs up on an ACCEPT
  that did not take it; a required callee (`EReq`) answers only a CALL it
  takes.

Not modelled: the carrier (DATAC16 or MFSK: a frame is lost or not;
ConnectCarrier below has it), the session once up (its turns, rungs and
blocks are the other specs'), its teardown.  A CALL is heard as it is
keyed; a stale one surfaces only as `StaleCall`, at the callee, and only
while it is answering.  The caller's next CALL, heard on time, brings the
callee back, as with a connected callee (`fsm_connected` ends its session
on such a CALL the same way).

Properties:

- `NoDoom`: the caller never holds an ACCEPT the callee no longer stands by:
  same nonce (the CRC seed), start rung (and piece size) and bandwidth (rung
  cap, MFSK floor).
- `NoHalfConnect`: a connected callee means a connected caller, same session
  -- or a caller that has left it on its lost-peer watchdog.
- `NoDeafRedial`: a callee connected in, or answering, a session its caller
  has left never leaves the caller's CALL unanswered.  The caller stops
  calling before its first frame, the one that connects the callee, so a
  CALL heard there means the caller has left; and it calls only for its
  current session, so a CALL of another session means it has left the one
  being answered.
- `NoDeadAnswer`: a callee never answers a session other than the one its
  connected caller is in.  A stale CALL, answered, drops the live session's
  seed; the caller's frames fail it and the session ends on its lost-peer
  watchdog.  `Settles` cannot see that (a lost-peer failure still ends with
  both idle), so the stale-CALL mutant is checked against this.
- `Settles`: every attempt ends with both connected, or both idle.
- `Agree`: a connected callee and its caller both took the session to be
  encrypted, or both clear.  Otherwise one end waits for a handshake the
  other never sends, and the session fails closed.
- `ReqGate`: a required end that is connected is encrypted.
- `SameLadder`: a connected caller maps the ladder's levels to the same modes
  as its callee (`Ladder`: minors 0 and 1 share one; 2 and 3 each have
  their own).  The wire carries a rung's level, not its mode.  Minor 2
  (1.9.25) changed what the levels mean (BW2300 rung 1 the 2 kHz MFSK, the
  BW500 and BW2750 ladders, the deep floor), and minor 3 put PSK3 at rung
  2.  Each station takes a CALL or ACCEPT only from its lowest minor up
  (`CMin`, `EMin`: `ARQ_CONNECT_MINOR_MIN`,
  `arq_protocol_connect_compatible`); 1.9.20-1.9.24 read any minor (0) and
  1.9.25 any from 2, so an older station still answers a newer CALL, and
  the newer caller ignores that ACCEPT.

Results:

- `ConnectHandshake.cfg`: the code's rules.  No error, 12017 distinct
  states; `ConnectHandshake_big.cfg` (3 CALLs, 3 CONNECTs, 3 nonces): 89929.
  Four session ids, two for each offer (10609 and 52649 with two ids and
  the offer apart from the id; 9473 and 34177 before the stale CALL; 1857 and 6421 before encryption;
  1425 and 4813 before the caller's watchdog could fire first.)
- `ConnectHandshake_creq.cfg`, `ConnectHandshake_ereq.cfg`: the caller, or
  the callee, required.  No error: 4833 and 4113 distinct states (4833 and
  3313 with two ids; 3009 and 2913 before the stale CALL).
  (Every configuration above is of two minor-3 stations; the counts are
  unchanged by the minors.)
- `ConnectHandshake_oldE.cfg` (a minor-3 caller, a 1.9.25 callee),
  `ConnectHandshake_old24.cfg` (a minor-3 caller, a 1.9.24 callee) and
  `ConnectHandshake_oldC.cfg` (a 1.9.25 caller, a minor-3 callee): no
  error, `Settles` included, 6497, 6497 and 161 distinct states (5569 and
  97 with two ids; 5105 and 65 before the stale CALL).  They never connect: the old callee answers and
  the caller gives up, or the new callee never answers.
- `Mut_mixed.cfg`: a minor-3 station takes any minor.  Violates
  `SameLadder`: the minor-3 caller connects on the 1.9.25 callee's ACCEPT.
- `Mut_reanswer.cfg`: a repeated CALL gets a fresh answer to its offer (the
  first port of #306 to 2.0 did this: the callee in its ACCEPT fallback
  answered a repeat anew).  Violates `Agree`: the callee's answer changes
  between two CALLs of the session (its key, or its client's ENCRYPT), and
  the caller decodes an ACCEPT of the old answer still in the air.
- `Mut_anyaccept.cfg`: the caller takes an ACCEPT that took an offer its
  CONNECT did not make (one of an earlier CONNECT of the same id).  No
  error since the offer is the id's low bit: an ACCEPT of the id answered
  the same offer.  With the offer apart from the id it violated `Agree`
  (the callee encrypts, the caller goes clear); the encryption code keeps
  the check.
- `Mut_ignorecall.cfg`: a connected callee ignores every CALL, as the code
  did before 2026-10-18.  Violates `NoDeafRedial`: the caller, its watchdog
  first, redials, and the callee answers only once its own watchdog fires
  (sim, nvis seed 28: 104 s later).
- `Mut_ignorenewer.cfg`: a callee answering a session ignores its caller's
  CALL of another session, as the code did before 2026-11-19.  Violates
  `NoDeafRedial`: the caller re-CONNECTs, and its new session's CALLs go
  unanswered until the callee's ACCEPTs for the old one run out (on air:
  K7EK answering N3JXB, 2026-11-18 06:06, 3 min; KG9NI answering an old
  CALL of K7EK's decoded late, 2026-11-17 21:19, the attempt failed).
- `Mut_staletake.cfg`: a callee answering answers a stale CALL, one of a
  session the caller keyed earlier and left (the switch before the age
  check).  Violates `NoDeadAnswer`: the caller has taken the live
  session's ACCEPT, and the callee drops it for the dead one, whose seed
  fails the caller's frames.  `NoHalfConnect` and `Settles` hold -- the
  session still ends with both idle, on the caller's lost-peer watchdog --
  which is why the model needs `NoDeadAnswer` for this mutant.
- `Mut_renonce.cfg`: a repeated CALL gets a fresh nonce.  Violates `NoDoom`.
- `Mut_remeasure.cfg`: a repeated CALL gets a re-measured start rung (as
  before spec R25).  Violates `NoDoom`.
- `Mut_bwleak.cfg`: another station's CALL sets the session bandwidth, as
  `arq.c` did for every CALL or ACCEPT addressed to us before the FSM took
  the bandwidth over (arch-2.0).  Violates `NoDoom`.  The model also showed
  that a repeated CALL must keep the session's bandwidth, as it keeps the
  nonce and rung: a caller that re-CONNECTs with another bandwidth and, by
  chance, the same session id may decode an older ACCEPT.
- `Mut_fallbackbw.cfg`: the callee's ACCEPT fallback loses the session
  bandwidth its ACCEPTs named, as the code did before #658:
  `cb_notify_cancelpending` zeroed `session_bw` when the retries ran out, so
  a late connect from the fallback ran at the callee's own bandwidth (a
  narrower caller's: other rungs and floor).  Violates `NoDoom`.  The model
  had always kept it (`EFallback` left `eBw` alone); the code had drifted.

## Trace validation: is the code one of the spec's behaviours?

    make -C tests connect_trace
    TMPDIR=... specs/connect/trace_check.sh 1 100              # each seed its own loss
    TMPDIR=... specs/connect/trace_check.sh 101 160 0.75 3 4   # 75 % loss, 3 CONNECTs, 4 CALLs

The specs above say what the code should do; nothing so far checked that
the code does it.  `tests/sim/connect_trace` runs the two real FSMs of the
sim (tests/sim: the caller CONNECTs once or twice, every frame lost with a
probability), reads both sessions after every dispatch, and writes each
change of what `ConnectHandshake` sees as module `TraceData`: both ends'
states (the callee's fallback is LISTENING with `accept_fallback`), session
ids and nonces, numbered in order of appearance.  `TraceHandshake.tla` takes
every step of the spec that goes to the trace's next row or keeps the one it
is on (a step the trace cannot see: a CALL lost, an ACCEPT keyed again), and
leaves the rest of the spec's state -- CALLs left, the ACCEPTs in the air,
rungs, bandwidths, encryption -- to TLC.  The run is a behaviour of the
spec iff TLC reaches the last row (`TraceNotDone` violated); otherwise TLC
prints the furthest row matched, and the row after it is where code and
spec part.

Results:

- The first run rejected seed 7 (47 % loss, 2 CONNECTs) after row 5 of 10:
  the caller's watchdog fired while the callee was still in its fallback
  with the session's seed, the caller CONNECTed again, and the callee
  answered from there.  The spec had both lapse in one step (`PeerLost`), so
  it never took that path -- nor its case where the new CONNECT picks the
  same id and gets the old ACCEPT.  Split (`PeerLost`, `EExpire`): every
  configuration above passes with the counts above (the states were all
  reachable already, by a caller giving up while calling; the transition
  was not), and every mutant still fails.
- Since, with the start rungs read too (the caller's first round,
  `car_tx_level`, and the rung the callee's ACCEPT names, `accept_level`),
  each seed picking its SNR (-2..20 dB) and half duplex or not: seeds 1-150
  (their own loss, 0-69 %, 1-2 CONNECTs, 2-4 CALLs) all accepted; seeds
  151-250 at 75 % loss, 3 CONNECTs of 4 CALLs, all accepted, up to 19 rows.

Not read yet: bandwidths and encryption (the sim has no crypto callback),
and the stale CALL (the sim does not delay a decode).

## ConnectCarrier

The floor is one carrier here: the callee decodes the caller's floor CALLs
whatever their bandwidths (as `ConnectHandshake` takes any CALL, its
bandwidth folded into `Bws`).  The code matches that since the MFSK call
listener decodes both floors (#442); before, a BW500 listener missed a wider
caller's MFSK CALLs, and only its DATAC16 ones reached it.

    java -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC -workers 4 -deadlock \
        -metadir /tmp/tlc-carrier -config ConnectCarrier.cfg ConnectCarrier.tla

The carriers and timing ConnectHandshake leaves out, in 0.25 s steps
(`arq_call_carrier`, `call_answer_carrier`, `accept_carrier`,
`call_must_wait`):

- The callee answers a repeated CALL on the control mode on the floor,
  unless its last ACCEPT went there; the first answer, and an MFSK CALL's,
  go on the CALL's carrier.  On air (2026-10-21, an sBitx calling an
  IC-7100) 0 of 3 DATAC16 ACCEPTs arrived and every MFSK one did, and the
  caller's MFSK CALL cost 22 s more.
- The caller listens on the floor from its first repeat on, and waits for
  an ACCEPT there: the next CALL is due one call interval of the carrier
  the ACCEPT may come on after the last ended (`call_interval_s`), the
  floor's (24 s) after a repeat on the control mode, not the control mode's
  8 s.  On air (KG9NI calling K7EK, 2026-11-10) 5 of K7EK's 6 floor ACCEPTs
  were keyed over by the next CALL, 3.7-4.9 s of each.  A CALL that
  comes due while one of its decoders is on a frame arriving waits, and
  once it has, until `ARQ_CALL_DECODE_WAIT_MS` past the frame's end (the
  floor's decode allowance and a deferral step).  The energy busy detector
  holds no CALL.
- Any frame may be lost, and a decoder may miss an ACCEPT arriving (`Sense`).
- The callee keys an ACCEPT only for a CALL decoded within `D(carrier)` of
  its end (1 s on the control mode, 4.5 s on the floor: the carousel's
  decode bound, `car_decode_bound_ms`) and while it is not keyed
  (`call_too_late`).  A later CALL is not answered: it still holds the
  scanner and restarts the ACCEPT retries (`notify_pending_again`), and the
  caller's next CALL is answered.  The model's callee decodes within D by
  construction (`CEnd`), so that is the code meeting the model's assumption;
  before `call_too_late` the code answered a CALL decoded at any age.
- Not modelled: a CALL decoded while the callee is keyed.  `eDec` holds one
  pending decode, and a CALL that overlaps the callee's own keydown is lost
  (half duplex), so a second CALL decoded during the callee's ACCEPT (the
  on-air case: a decoder behind delivering an older CALL) can't be
  expressed.  The code's keyed check covers it; the unit tests
  (`test_accepting_call_decoded_while_keyed_is_not_answered`) pin it.

Properties: `NoOverlap` (no two keydowns at once; with decoders always
catching an ACCEPT, and since `call_interval_s` with `Sense` "any" too); `Early` (the fast forward path and the floor delivering, the fast
return dead: connected on the second CALL's answer, no third CALL);
`Connects` (the floor delivering, decoders catching ACCEPTs or not:
connected by the horizon).

Results:

- `ConnectCarrier.cfg` (every path "any"): no error, 149404 distinct states
  (405606 before `call_interval_s`).
- `ConnectCarrier_sense.cfg` (every path "any", `Sense` "any"): `NoOverlap`
  holds, 297879: a CALL never keys over an ACCEPT, caught by a decoder or
  not.
- `ConnectCarrier_early.cfg`: no error, 23222 (23876).
- `ConnectCarrier_floor.cfg` (`Sense` "any"): no error, 244623 (355346).
- `Mut_fastiv.cfg` (`ConnectCarrier_sense.cfg` with a repeat on fast
  waiting the fast interval, the code before `call_interval_s`): violates
  `NoOverlap`, the CALL keyed over the floor ACCEPT a decoder missed.
- `F`, the fast CALLs before the floor (`ARQ_CALL_FAST_SLOTS`): 2 by
  default, `min(max(2, RETRIES/2), 4)` after a host's `RETRIES` (#442),
  so the `F` = 4 configs are the code's highest case.  Every
  config above has `F` = 2 (and the counts are unchanged).  The caller
  still listens on the floor from its second CALL whatever `F` is.
  - `F` = 4, `R` = 8 (`ConnectCarrier_f4*.cfg`): `NoOverlap` holds, 84324
    states (every path "any"; 449131 before `call_interval_s`); `Early`,
    23222; MFSK-only callee (`_f4_emfsk`), `NoOverlap` and `FloorOnly`,
    67577; MFSK-only caller (`_f4_cmfsk_floor`), `Connects`, 291289 (its
    CALLs are all on the floor, so `F` does not apply).
  - `F` = 10, `R` = 20 (`ConnectCarrier_f10*.cfg`, `RETRIES 20` without
    the cap; kept as the record of why the cap is there):
    `NoOverlap` holds, 217943 states; `Early`, 42032; `_f10_emfsk`,
    178655.
  - `Connects` needs a later horizon as `F` grows: with only the floor
    delivering, the first floor CALL comes after `F` fast ones, each 47
    steps (a CALL and its interval), 111 from the first repeat on (its
    interval is the floor's: `call_interval_s`).  The earliest horizon it
    holds from: `F` = 2, ~454 (114 s; 450 before `call_interval_s`;
    `ConnectCarrier_floor.cfg` checks 520); `F` = 4, ~532 (133 s; ~540
    before; `_f4_floor` and `_f4_emfsk_floor` check 560: 201759 and 160779
    states); `F` = 10, ~1200 (300 s; 820 before; `_f10_floor` and
    `_f10_emfsk_floor` now check 1250: 964335 and 773661).  The same for
    an MFSK-only callee.  That is the cost of more fast CALLs on a path
    only the floor carries: ~28 s per fast repeat now, so the cap at 4.
- `Mut_lateanswer.cfg` (`ConnectCarrier.cfg` with the callee also
  answering a CALL decoded up to `IvFast` past D, as before
  `call_too_late`): violates `NoOverlap`.  The first CALL ends at 3.75 s
  and decodes 6 s late; the ACCEPT is keyed at 10.5 s, and the caller's
  repeat, due at 11.75 s, goes before its decoder is on that ACCEPT
  (`SA`, 1.5 s in) and keys over it.  1880 states.
- `Mut_old.cfg`, the code before (no floor answer, no floor listening, no
  hold): violates `Early` (a third CALL, the floor one, 22 s later).
- `Mut_nohold.cfg`: the CALL goes when due.  Violated `NoOverlap` before
  `call_interval_s`: it keyed over the floor ACCEPT, which outlasted the
  8 s call interval.  Now no error (149404): the interval waits that
  ACCEPT out.  The hold stays in the code for what the model leaves out:
  an ACCEPT later than its allowances, and other stations' frames.
- `Mut_nodecwait.cfg`: the hold ends with the frame.  Violated `Early`
  before `call_interval_s` (the CALL went out before the ACCEPT decoded;
  sim, asym:-4:-9: the callee, now connected, took it for a redial).  Now
  no error (23222), for the same reason.
- `Mut_noalt.cfg`: every repeat on the control mode answered on the floor.
  Violated `Connects` before `call_interval_s`: a floor ACCEPT the caller's
  decoders missed was keyed over every time.  Now no error (244623): none
  is keyed over.  `accept_carrier` still alternates.
- `Mut_nolisten.cfg`: the caller does not listen on the floor.  Violates
  `Early` (and `NoOverlap`, below).
- `MfskTx`, an end that transmits only MFSK (`arq:mfsk_tx_only`, #424):
  `ConnectCarrier_emfsk.cfg`, `_cmfsk.cfg`, `_cemfsk.cfg` (callee, caller,
  both; every path "any"): `NoOverlap` and `FloorOnly` hold, 415745, 309216
  and 309216 distinct states.  `_floor` (`Sense` "any"): `Connects` holds,
  330069, 291289, 291289.  `_early`: connected on the second CALL's answer,
  23766, 145768, 145768.  The callee is silent where its ACCEPT would go on
  fast, so a first CALL on DATAC16 gets no answer and the repeat gets one on
  the floor.  `ConnectCarrier_emfsk_listenall.cfg`, #424 as opened (that
  ACCEPT on the floor, every caller listening there from its first CALL):
  also no error, 26390 states, at the cost of every station's MFSK
  listener while it calls.
- `Mut_loud.cfg`: the MFSK-only callee answers a first CALL on the floor
  without the caller listening there.  Violates `NoOverlap`: the caller's
  repeat keys over the 13.5 s ACCEPT.
- `Mut_firstiv.cfg`: an MFSK-only caller's next CALL comes due before its
  first ends (#424 as opened set the first deadline from the fast interval).
  Violates `NoOverlap`.  In the connect sim (SIM_HD=1 SIM_CS=1500) the
  connect took 79 s with 40 collisions in 20 seeds; with the floor's
  interval 28 s and none.
- A decoder of the caller's is on an ACCEPT only on a carrier it listens on
  (since `MfskTx`); `Mut_nolisten.cfg` now violates `NoOverlap` first.
- At 0.5 s steps the rounding ate the last floor CALL's 0.75 s of slack and
  `Connects` failed on a tie with the caller giving up, in the old code too:
  the reason for 0.25 s.  The floor call interval is safe from 21 s up (the
  code's is 24 s).
- `Deep`, a caller wider than 500 Hz (the deep floor): a third carrier,
  `"deep"` (the 500 Hz MFSK, `LD` 108 = 27 s, call interval `IvDeep` 150 =
  37.5 s, the floor's decode allowance), joins the rotation after the fast
  CALLs: the floor, then deep twice (in the connect sim the third slot on
  fast added nothing, and a second deep CALL connected more under fading).  The caller listens on one MFSK carrier
  at a time, the one its last CALL's ACCEPT may come on (`cListen`, now a
  carrier: the payload decoder is one); the callee answers a deep CALL on
  deep, and a fast repeat on the floor unless its last ACCEPT went there.
  Every config above has `Deep` FALSE (a 500 Hz caller, or the rotation
  before) and gives the counts above: the model with `Deep` FALSE is the
  one before, and also a caller wider than 500 Hz with
  `arq:call_mfsk500 = false` (#654).  With `Deep` TRUE:
  - `ConnectCarrier_deep.cfg` (every path "any", `H` 700 so the rotation
    reaches both deep CALLs): no error, 294649 states; `_deep_sense`
    (`Sense` "any"), `NoOverlap` holds, 588005; `_deep_f4` (`F` = 4, `R` =
    8, `H` 1100), 623918; `_deep_emfsk` (an MFSK-only callee: `FloorOnly`
    allows either MFSK carrier), 276863.
  - `_deep_early`: `Early` holds, 23222 (the deep CALL comes after the
    second); `_deep_floor`: `Connects`, 244623 (connected on the floor CALL
    before the deep one comes due).
  - `ConnectCarrier_deeponly.cfg` (only the deep paths deliver, `Sense`
    "any", `H` 600): `Connects` holds, 51801: the fringe only the 500 Hz
    MFSK reaches.  The earliest horizon it holds from is between 560 and 580
    (140-145 s): the deep CALL is the fourth, keyed at 77 s.  `_deeponly_f4`
    (`H` 820, 205 s), 50583; `_deeponly_emfsk`, 51801.
  - An MFSK-only caller (`MfskTx` "C") with `Deep`: no fast CALLs, the
    floor and deep twice from the first.  `ConnectCarrier_deep_cmfsk.cfg`
    (every path "any"): `NoOverlap` and `FloorOnly` (any MFSK carrier) hold,
    385439 states; `_deeponly_cmfsk`: `Connects` too, 165715.  Without
    `Deep` its CALLs are on the floor, as before (`_cmfsk*` unchanged).
  - `Mut_deepfloor.cfg` (the caller keeps listening on the floor after a
    deep CALL): violates `Connects`.  `Mut_deepiv.cfg` (a deep CALL waits
    the floor's interval, `Sense` "any"): violates `NoOverlap`, the next CALL
    keyed over the 27 s ACCEPT a decoder missed.

Give each TLC run its own `-metadir`.  The state counts here are TLC
2026.10.04 (`tlc2.TLC -h` prints the version); another version can count a
few hundred states differently with the same results.

## The notice that holds a scanning host while a CALL arrives (`arrival_hold`, #527)

Not modelled here: it adds no state to the connect.  It is modelled, with the
ACCEPT fallback's hold and every other notice a callee gives its host about an
incoming call, in `specs/notices` (`CalleeNotices`).  LISTENING says `PENDING` when
a CALL decoder (the control plane's, or the 1 kHz MFSK listener's) has locked
onto a frame, again every `ARQ_PENDING_HOLD_MS` while the lock lasts, and
`CANCELPENDING` `ARQ_ARRIVAL_GRACE_MS` after it is gone when the FSM has not
taken a CALL out of it.  Where the FSM does (a CALL for this station decoded),
the CALL's own `PENDING` (`cb_notify_pending`) follows in the same pass and the
hold lets go without a `CANCELPENDING`: the session's notices take over, so the
host sees one unbroken `PENDING` from the lock to `CONNECTED`, `CANCELPENDING`
or `DISCONNECTED`.  The hold is a function of the decoder's lock, the
connection state and the clock (statics in `arq.c`, nothing in the session),
and is silent while a session exists or the radio is keyed.  Measured in the Go
harness with a scanning host (4 s dwell, 0.5 s deaf, 2 channels, nine phases of
the scan cycle): connects at 7 of 9 phases within 120 s, against 5 of 9 before.
The two it misses, phases 4 and 5, are those where the caller's CALL repeats
land outside the dwell each time (its cadence has locked to the scan cycle).

