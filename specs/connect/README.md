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
  same ACCEPT; another session's CALL, or another station's, is ignored.  Out
  of ACCEPT retries it keeps the seed (`accept_fallback`) until it lapses.
- The callee connects on the caller's first frame only on the rung it named
  and with its nonce (the CRC is seeded).  A caller that gave up and hears an
  ACCEPT sends a DISCONNECT; a connected caller whose callee never answers
  gives up when the callee's kept seed lapses (`PeerLost`, one step).

- Encryption (`arq_crypto.h`, `crypto_negotiate`): a CONNECT's CALLs offer
  it or not; the callee answers the offer of the CALL it takes, once for
  the session -- every ACCEPT of it carries that answer, as it carries the
  nonce; the caller takes an ACCEPT's bit only with an offer of its own.  A
  required caller (`CReq`) calls only to offer and hangs up on an ACCEPT
  that did not take it; a required callee (`EReq`) answers only a CALL it
  takes.

Not modelled: the carrier (DATAC16 or MFSK: a frame is lost or not;
ConnectCarrier below has it), the session once up (its turns, rungs and
blocks are the other specs'), its teardown.

Properties:

- `NoDoom`: the caller never holds an ACCEPT the callee no longer stands by:
  same nonce (the CRC seed), start rung (and piece size) and bandwidth (rung
  cap, MFSK floor).
- `NoHalfConnect`: a connected callee means a connected caller, same session
  -- or a caller that has left it on its lost-peer watchdog.
- `NoDeafRedial`: a connected callee never leaves a CALL of its caller's
  unanswered.  The caller stops calling before its first frame, the one that
  connects the callee, so a CALL heard there means the caller has left.
- `Settles`: every attempt ends with both connected, or both idle.
- `Agree`: a connected callee and its caller both took the session to be
  encrypted, or both clear.  Otherwise one end waits for a handshake the
  other never sends, and the session fails closed.
- `ReqGate`: a required end that is connected is encrypted.

Results:

- `ConnectHandshake.cfg`: the code's rules.  No error, 9473 distinct
  states; `ConnectHandshake_big.cfg` (3 CALLs, 3 CONNECTs, 3 nonces): 34177.
  (1857 and 6421 before encryption; 1425 and 4813 before the caller's
  watchdog could fire first.)
- `ConnectHandshake_creq.cfg`, `ConnectHandshake_ereq.cfg`: the caller, or
  the callee, required.  No error: 3009 and 2913 distinct states.
- `Mut_reanswer.cfg`: a repeated CALL gets a fresh answer to its offer (the
  first port of #306 to 2.0 did this: the callee in its ACCEPT fallback
  answered a repeat anew).  Violates `Agree`: the caller re-CONNECTs with
  the same id, now offering, and decodes an ACCEPT of the old answer
  (clear) still in the air, while the callee takes the session encrypted.
- `Mut_anyaccept.cfg`: the caller takes an ACCEPT that took an offer its
  CONNECT did not make (one of an earlier CONNECT of the same id).
  Violates `Agree`: the callee encrypts, the caller goes clear.
- `Mut_ignorecall.cfg`: a connected callee ignores every CALL, as the code
  did before 2026-10-18.  Violates `NoDeafRedial`: the caller, its watchdog
  first, redials, and the callee answers only once its own watchdog fires
  (sim, nvis seed 28: 104 s later).
- `Mut_renonce.cfg`: a repeated CALL gets a fresh nonce.  Violates `NoDoom`.
- `Mut_remeasure.cfg`: a repeated CALL gets a re-measured start rung (as
  before spec R25).  Violates `NoDoom`.
- `Mut_bwleak.cfg`: another station's CALL sets the session bandwidth, as
  `arq.c` did for every CALL or ACCEPT addressed to us before the FSM took
  the bandwidth over (arch-2.0).  Violates `NoDoom`.  The model also showed
  that a repeated CALL must keep the session's bandwidth, as it keeps the
  nonce and rung: a caller that re-CONNECTs with another bandwidth and, by
  chance, the same session id may decode an older ACCEPT.

## ConnectCarrier

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
- The caller listens on the floor from its first repeat on.  A CALL that
  comes due while one of its decoders is on a frame arriving waits, and
  once it has, until `ARQ_CALL_DECODE_WAIT_MS` past the frame's end (the
  floor's decode allowance and a deferral step).  The energy busy detector
  holds no CALL.
- Any frame may be lost, and a decoder may miss an ACCEPT arriving (`Sense`).

Properties: `NoOverlap` (decoders always catch an ACCEPT: no two keydowns
at once); `Early` (the fast forward path and the floor delivering, the fast
return dead: connected on the second CALL's answer, no third CALL);
`Connects` (the floor delivering, decoders catching ACCEPTs or not:
connected by the horizon).

Results:

- `ConnectCarrier.cfg` (every path "any"): no error, 405606 distinct states.
- `ConnectCarrier_early.cfg`: no error, 23876.
- `ConnectCarrier_floor.cfg` (`Sense` "any"): no error, 355346.
- `F`, the fast CALLs before the floor (`ARQ_CALL_FAST_SLOTS`): 2 by
  default, `min(max(2, RETRIES/2), 4)` after a host's `RETRIES` (#442),
  so the `F` = 4 configs are the code's highest case.  Every
  config above has `F` = 2 (and the counts are unchanged).  The caller
  still listens on the floor from its second CALL whatever `F` is.
  - `F` = 4, `R` = 8 (`ConnectCarrier_f4*.cfg`): `NoOverlap` holds, 449131
    states (every path "any"); `Early`, 23876; MFSK-only callee
    (`_f4_emfsk`), `NoOverlap` and `FloorOnly`, 430194; MFSK-only caller
    (`_f4_cmfsk_floor`), `Connects`, 291289 (its CALLs are all on the
    floor, so `F` does not apply).
  - `F` = 10, `R` = 20 (`ConnectCarrier_f10*.cfg`, `RETRIES 20` without
    the cap; kept as the record of why the cap is there):
    `NoOverlap` holds, 2364589 states; `Early`, 42686; `_f10_emfsk`,
    2026816.
  - `Connects` needs a later horizon as `F` grows: with only the floor
    delivering, the first floor CALL comes after `F` fast ones, each 47
    steps (a CALL and its interval).  The earliest horizon it holds from:
    `F` = 2, 450 (112 s; `ConnectCarrier_floor.cfg` checks 520); `F` = 4,
    ~540 (134 s; `_f4_floor` and `_f4_emfsk_floor` check 560: 346036 and
    301462 states); `F` = 10, 820 (205 s; `_f10_floor` and
    `_f10_emfsk_floor` check 850: 683736 and 548691).  The same for an
    MFSK-only callee.  That is the cost of more fast CALLs on a path only
    the floor carries: ~23 s per two of them.
- `Mut_old.cfg`, the code before (no floor answer, no floor listening, no
  hold): violates `Early` (a third CALL, the floor one, 22 s later).
- `Mut_nohold.cfg`: the CALL goes when due.  Violates `NoOverlap`: it keys
  over the floor ACCEPT, which outlasts the 8 s call interval.
- `Mut_nodecwait.cfg`: the hold ends with the frame.  Violates `Early`: the
  CALL goes out before the ACCEPT decodes (sim, asym:-4:-9: the callee, now
  connected, took it for a redial).
- `Mut_noalt.cfg`: every repeat on the control mode answered on the floor.
  Violates `Connects`: a floor ACCEPT the caller's decoders miss is keyed
  over every time.
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

Give each TLC run its own `-metadir`.
