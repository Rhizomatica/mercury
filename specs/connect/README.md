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

Not modelled: the carrier (DATAC16 or MFSK: a frame is lost or not), the
session once up (its turns, rungs and blocks are the other specs'), its
teardown.

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

Results:

- `ConnectHandshake.cfg`: the code's rules.  No error, 1857 distinct
  states; `ConnectHandshake_big.cfg` (3 CALLs, 3 CONNECTs, 3 nonces): 6421.
  (1425 and 4813 before the caller's watchdog could fire first.)
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

Give each TLC run its own `-metadir`.
