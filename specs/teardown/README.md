# specs/teardown

How a carousel session ends, as `datalink_arq/arq_fsm.c` runs it:
`car_begin_disconnect`, `fsm_disconnecting`, the `RX_DISCONNECT` and
`RX_CALL` cases, and the carousel watchdogs (`ARQ_CAR_END_WAIT_MS`,
`ARQ_CAR_PEER_LOST_MS`).

    java -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC -workers 2 -noGenerateSpecTE \
        -metadir /tmp/tlc-teardown -config SessionTeardown.cfg SessionTeardown.tla

What it models:

- The caller keys its DISCONNECT when its application asks, and again up to
  `R` times while nothing answers; then it gives up.  While connected it
  keeps polling, and each poll gives the callee a slot.
- The callee keys nothing of its own: its DISCONNECT goes in its next slot,
  and in every slot after that while it is ending.  With no slot it gives up.
- Either end, hearing the peer's DISCONNECT, ends.  A connected end replies
  one guard later.  An ending end replies only if its own had not gone out
  (the crossed case).
- An ending end that hears its peer CALL again ends and takes the CALL.  A
  callee whose DISCONNECT is out, waiting only for the answer, takes a CALL
  for it from any callsign: a station calls with another MYCALL per
  application (`CALLS`).
- Silence ends a connected session.
- The caller may call again once it has ended (a deferred CONNECT), up to
  `NS` sessions.  Session ids come from `SIDS`, so consecutive sessions may
  share one, as `(time & 0x7F) | 1` allows.
- A DISCONNECT is taken on its session id alone, because its CRC is plain.
  Every other frame in a session is seeded with that session's nonce.
- The reply to a DISCONNECT is an ACK pattern (`send_disconnect_reply`):
  no session id, but keyed by the session's seed (`mfsk_pattern_set_session`),
  which the replying end keeps until the reply is out (`car_stop`).  Only an
  end whose own DISCONNECT is out takes one, as the answer, and only one of
  its own session.
- Time is abstract.  A timer fires only once nothing more is coming, since
  the timers are far longer than an exchange.  On a lossy channel a timer
  may fire whenever nothing is queued to it.  The channel keeps frames in
  order and may lose any of them.

Not modelled:

- The drain before a DISCONNECT (specs/carousel `DirDone`).
- When the DISCONNECT may key (specs/turns `DISC`).
- The handshake (specs/connect).

Properties:

- `NoKill`: no end leaves a session on another session's frame.
- `NoStale`, `NoForeign`: no end leaves on the pattern that answered an
  earlier session's DISCONNECT, nor on another station's.
- `Clean`: on a lossless channel no end ever times out, so every teardown
  is answered.
- `Mutual` (liveness): once one end has finished a session, eventually
  neither end is in it.
- `Answered`: a callee that is idle, or ending with its DISCONNECT out,
  never drops a CALL for it.

Results:

- `SessionTeardown.cfg`: the code's rules, lossless, 2 sessions, 2 caller
  callsigns.  No error, 2 069 distinct states (1 957 with DISCONNECT
  frames as replies).
- `SessionTeardown_lossy.cfg`: the same with any frame lost.  No error,
  52 041 distinct states (51 705).  `Clean` does not apply here.
- `Mut_ignorepattern.cfg`: the end waiting for the answer to its
  DISCONNECT does not take the pattern.  It violates `Clean`: it retries
  and times out.
- `ForeignPattern.cfg`: the code's rules, lossy, with another station's
  patterns heard (`FPAT`).  No error, 349 393 distinct states: keyed by the
  session, none is taken.  Safety only: unbounded foreign traffic keeps the
  queue full for ever, and `Mutual` fails on that, not on a rule.
- `Mut_unkeyed.cfg`: the same, with a pattern taken whatever session keyed
  it.  It violates `NoForeign`.  The code did this at first: the replying
  end cleared its seed before the reply went out, and in the harness the
  peer, still in the session, never detected it (3 runs of 3).
- `Mut_anypattern.cfg`: a pattern ends an end in any state, not only one
  whose DISCONNECT is out.  No error, the same 52 041 states: as with the
  session-id check below, frames in order and timers longer than an
  exchange mean no pattern reaches an end that is not waiting for one.
  In the code the rule keeps the carousel's own patterns (not modelled
  here) from ending a session.
- `Mut_crossedsilent.cfg`: an ending end that hears the peer's DISCONNECT
  before its own went out ends without a reply.  The code did this before
  the crossed-DISCONNECT fix (car24).  It violates `Clean`, because the peer
  retries until it times out.
- `Mut_samecall.cfg`: an ending callee takes a CALL only from the callsign
  of the session being torn down, as the code did until 2026-10-16.  It
  violates `Answered`.  On air, a UUCP session's teardown lost the
  callee's answer, and the gateway's NNCP, calling as PU2UIT-3 where UUCP
  had called as PU2UIT, was ignored for the callee's 180 s end wait.
- `Foreign.cfg`: another station's DISCONNECT with a matching 6-bit session
  id.  It violates `NoKill`.  This is a residual risk: 1 in 64 foreign
  DISCONNECTs heard ends the session.  A DISCONNECT seeded like the
  session's other frames would close it.

Two things the model showed:

- Between one pair of stations the session-id check is never what keeps
  sessions apart.  Frames arrive in order and the timers outlast an
  exchange, so no stale DISCONNECT reaches a later session, even one with
  the same id.  A mutant that dropped the check passed every property.  The
  check only filters other stations.
- After a teardown whose DISCONNECTs were all lost, the callee is still
  connected and drops the caller's redial: `fsm_connected` has no
  `RX_CALL` case.  It takes the CALL only once its silence watchdog has
  ended the session, up to `ARQ_CAR_PEER_LOST_MS` (240 s) after the
  caller's last frame.  That is a delay, not a safety violation, and
  `Mutual` holds.

Give each TLC run its own `-metadir`.
