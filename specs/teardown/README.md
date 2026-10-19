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
- `NoUnasked`: the caller ends a session on an M_END only of that session.
- `NoLostWord`: the caller never ends on an M_END while the callee holds a
  last word the caller has not got.
- `NoInherited`: the callee holds a DISCONNECT back only for the session
  whose application hung up.  No teardown step outlives its session.

M_END (2026-10-18): a callee whose application hangs up while its
DISCONNECT waits for its data to drain says so in its answers (poll type 3,
`car_wish_end`).  The caller, connected and with everything delivered both
ways, then ends the session as if its own application had asked
(`fsm_connected_carousel`).  Not yet drained, it ignores the M_END, and the
callee's own DISCONNECT ends the session later, as before.  In the model:
`SWish`, `SEndPoll` and `SDrained`, and M taking an `end` frame of its
session (`TakesEnd`).

"Everything delivered" is judged by the caller from the M_END poll itself
(`peer_ended`): the poll names the callee's latest block and whether bytes
wait unopened, and the caller knows which blocks it holds.  At first it
was `car_drained`, whose view of the callee's data is `status_seen`, which a
round with nothing heard also sets.  On air (2026-10-18) the callee wrote
its reply and hung up at once, and the caller ended on the M_END without
the reply.  In the model the callee may hang up holding a last word
(`word`), which reaches the caller while both are in the session
(`SDeliver`); `Mut_endheld.cfg` has the old rule.

Teardown steps (2026-10-19): the code kept the steps in seven flags, and
three outlived the state they belong to: a DISCONNECT held to drain, from a
session the peer left by calling again, ended the next session at once; a
notice owed when the host dialed inside the reply guard made a later
LISTEN ON a no-op; and another transmission's TX_COMPLETE ended a teardown
whose reply had not gone out.  They are one field now
(`arq_session_t.teardown`), reset on every state change.  The model always
had it so: a CALL taken resets `wish`, and `reply` ends only with `Reply`.
`Mut_staleteardown.cfg` and `Mut_skipreply.cfg` have the old code's rules.

Results:

- `SessionTeardown.cfg`: the code's rules, lossless, 2 sessions, 2 caller
  callsigns.  No error, 10 645 distinct states (4 957 before the last
  word, 2 069 before M_END).
- `SessionTeardown_lossy.cfg`: the same with any frame lost.  No error,
  264 149 distinct states (124 141 before the last word, 52 041 before
  M_END).  `Clean` does not apply
  here.
- `Mut_ignorepattern.cfg`: the end waiting for the answer to its
  DISCONNECT does not take the pattern.  It violates `Clean`: it retries
  and times out.
- `ForeignPattern.cfg`: the code's rules, lossy, with another station's
  patterns heard (`FPAT`).  No error, 1 655 453 distinct states: keyed by the
  session, none is taken.  Safety only: unbounded foreign traffic keeps the
  queue full for ever, and `Mutual` fails on that, not on a rule.
- `Mut_unkeyed.cfg`: the same, with a pattern taken whatever session keyed
  it.  It violates `NoForeign`.  The code did this at first: the replying
  end cleared its seed before the reply went out, and in the harness the
  peer, still in the session, never detected it (3 runs of 3).
- `ForeignEnd.cfg`: the code's rules, lossy, with another station's M_END
  frames heard (`FEND`).  No error, 1 370 595 distinct states: seeded by its
  session, none is taken.  Safety only, as `ForeignPattern.cfg`.
- `Mut_unkeyedend.cfg`: the same, with an M_END taken whatever session sent
  it.  It violates `NoUnasked`.
- `Mut_endheld.cfg`: lossless, an M_END taken while the callee holds a last
  word the caller has not got (the rule as first merged).  It violates
  `NoLostWord`.
- `Mut_staleteardown.cfg`: lossless, a CALL taken keeps the DISCONNECT the
  last session held back to drain.  It violates `NoInherited`.
- `Mut_skipreply.cfg`: lossless, an end owing the reply to its peer's
  DISCONNECT goes idle without keying it.  It violates `Clean`: the peer
  retries and times out.
- `Mut_anypattern.cfg`: a pattern ends an end in any state, not only one
  whose DISCONNECT is out.  No error, the same 264 149 states: as with the
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
  connected and dropped the caller's redial: `fsm_connected` had no
  `RX_CALL` case, and took the CALL only once its silence watchdog had
  ended the session, up to `ARQ_CAR_PEER_LOST_MS` (240 s) after the
  caller's last frame.  A delay, not a safety violation (`Mutual` holds).
  Since 2026-10-18 a connected station takes its peer's CALL
  (specs/connect, `NoDeafRedial`).

Give each TLC run its own `-metadir`.
