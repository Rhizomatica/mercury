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
- With its last word not yet confirmed, the caller holds its DISCONNECT
  while the drain delivers it, up to the drain deadline or the peer's
  silence.  The host hears DISCONNECTED when the session ends; a second
  DISCONNECT or a CONNECT meanwhile ends the drain at once.
- The callee keys nothing of its own: its DISCONNECT goes in its next slot,
  and in every slot after that while it is ending.  With no slot it gives up.
- Either end, hearing the peer's DISCONNECT, ends.  A connected end replies
  one guard later.  An ending end replies only if its own had not gone out
  (the crossed case).
- An ending end that hears its peer CALL again ends and takes the CALL.  A
  callee whose DISCONNECT is out, waiting only for the answer, takes a CALL
  for it from any callsign: a station calls with another MYCALL per
  application (`CALLS`).
- Silence ends a connected session.  A caller that has heard the callee's
  M_END and then hears nothing more, as long as the callee's own drain
  would last, ends it sooner, with one DISCONNECT.
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

- How the drain delivers (specs/carousel `DirDone`): here it only holds
  the caller's DISCONNECT until its last word is confirmed, or until it
  ends.
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
  last word the caller has not got.  A caller ending on the callee's
  silence after an M_END is exempt: that word is lost to the channel, as it
  was at the lost-peer watchdog, and the ghost `wordgone` records it.
- `NoCut`: the caller ends a session on silence only once the callee's
  application has hung up in it.
- `NoInherited`: the callee holds a DISCONNECT back only for the session
  whose application hung up.  No teardown step outlives its session.
- `Confirmed`: on a lossless channel the caller never leaves a session
  with its last word unacknowledged, unless the host had it dropped (a
  second DISCONNECT or a redial while the caller drains).
- `Released`: once the host has asked again while the caller drains, the
  caller no longer holds its DISCONNECT.

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

The ack the callee owes (2026-11-05): the caller's last word (B2F's FQ)
reaches the callee, whose application hangs up on it at once with nothing
of its own to send.  Its DISCONNECT went in the slot where the answer
acknowledging the FQ would have gone, so the caller never heard the FQ
arrive, and logged it as undelivered at the end of every clean B2F forward
(KG9NI-K7EK, 2026-11-04).  Now a callee owing that answer
(`car_owes_ack`) takes the M_END path: it answers with an M_END, and the
caller, with the ack and the hang-up, ends the session.  In the model:
`mw` (the caller's last word: owed, acked, confirmed), `MWord`, `SAck`, the
guard on `SAppDisc`, and `SDrained` waiting for the ack to be keyed.  An
M_END that the caller does not act on (the callee still holds a word of
its own) still acknowledges the caller's word, as the poll does in the
code.  `Mut_ackless.cfg` has the old rule.

The caller's drain (2026-11-05): the host hangs up, and the caller then holds its
DISCONNECT to deliver what it has not yet confirmed (`ARQ_TD_DRAINING`), at
the MFSK floor for up to 148 s.  On air (N3JXB -> VE4KLM, 1.9.22,
2026-11-04) the callee went silent after the ACCEPT: the operator's
DISCONNECT was held, the redial to K7EK queued behind it, a second
DISCONNECT cancelled the redial and re-armed the drain, and the old session
kept keying MFSK until the TNC was restarted.  Now a second DISCONNECT ends
the session at once, and a CONNECT ends the drain (the call is placed once
the teardown completes); both drop what is undelivered, as ABORT does.  The
drain also ends once the peer has been silent `ARQ_CAR_PEER_QUIET_MS`
(120 s): in the model, `MDrainEnd`, the drain's timer.  In the model:
`mdrain`, `MHold`, `MDrained`, `MDrainEnd`, `MAgain`, `MRedial`, the ghosts
`asked` and `forced`, and property `Released`.  `Mut_rearm.cfg` and
`Mut_redialwait.cfg` have the old rules.

When the host hears DISCONNECTED (2026-11-07): when the session has
closed, as VARA's command reference has it ("DISCONNECT: Disconnect the link,
once the TX buffer is empty"; "DISCONNECTED: Session closed by either end"),
not when it hangs up.  Until then arq.c said it at once, and BPQ32, which
restarts its scan 15 s after DISCONNECTED, moved the radio mid-teardown: its
LISTEN OFF ended the session before the peer had the DISCONNECT, and the peer
waited out its 240 s watchdog (on air, K7EK and KG9NI, 1.9.22).  BPQ32 keeps
the radio while it waits, and ABORTs after 30 s.  The model is unchanged: the
host may still ask again during the drain (`HostAgain`), told or not.

Silence after an M_END (2026-11-05): the callee's M_END says its
application has hung up, but the caller acts on it only holding everything
of the callee's.  On air (N3JXB -> KG9NI-1, 1.9.22) the M_END came with one
small frame of the callee's still to send, and then nothing more was
decoded: the caller polled for it until the 240 s lost-peer watchdog, which
dropped the session without a DISCONNECT.  Now the caller remembers the
M_END (`car_peer_ending`: `end_req` on the caller) and, once the callee has
been silent as long as the callee's own drain would last (the drain timeout
or the rung's budget, whichever is longer, at most 120 s), ends the session
with one DISCONNECT.  In the model: `mend` (set by any M_END of its session
the caller hears), `MEndSilent` (a timer), the ghosts `cut` and `wordgone`,
and property `NoCut`.  `Mut_endsilence.cfg` ends on silence without an
M_END.

Results:

- `SessionTeardown.cfg`: the code's rules, lossless, 2 sessions, 2 caller
  callsigns.  No error, 56 041 distinct states (41 421 before the
  silence after an M_END, 21 317 before the caller's drain, 10 645 before the callee's owed ack, 4 957 before the
  last word, 2 069 before M_END).
- `SessionTeardown_lossy.cfg`: the same with any frame lost.  No error,
  2 568 389 distinct states (1 373 461 before the silence after an M_END,
  599 881 before the caller's drain, 264 149
  before the owed ack, 124 141 before the last word, 52 041 before M_END).  `Clean` does not apply
  here.
- `Mut_ignorepattern.cfg`: the end waiting for the answer to its
  DISCONNECT does not take the pattern.  It violates `Clean`: it retries
  and times out.
- `ForeignPattern.cfg`: the code's rules, lossy, with another station's
  patterns heard (`FPAT`).  No error, 15 602 153 distinct states (8 403 333
  before the silence after an M_END, 3 621 957 before the caller's drain, 1 655 453 before the owed ack): keyed by the
  session, none is taken.  Safety only: unbounded foreign traffic keeps the
  queue full for ever, and `Mutual` fails on that, not on a rule.
- `Mut_unkeyed.cfg`: the same, with a pattern taken whatever session keyed
  it.  It violates `NoForeign`.  The code did this at first: the replying
  end cleared its seed before the reply went out, and in the harness the
  peer, still in the session, never detected it (3 runs of 3).
- `ForeignEnd.cfg`: the code's rules, lossy, with another station's M_END
  frames heard (`FEND`).  No error, 12 686 499 distinct states (6 850 643
  before the silence after an M_END, 2 956 247 before the caller's drain, 1 370 595 before the owed ack): seeded by its
  session, none is taken.  Safety only, as `ForeignPattern.cfg`.
- `Mut_unkeyedend.cfg`: the same, with an M_END taken whatever session sent
  it.  It violates `NoUnasked`.
- `Mut_endheld.cfg`: lossless, an M_END taken while the callee holds a last
  word the caller has not got (the rule as first merged).  It violates
  `NoLostWord`.
- `Mut_ackless.cfg`: lossless, the callee's application hangs up owing
  the ack of the caller's last word, and its DISCONNECT goes in that ack's
  slot (the rule before `car_owes_ack`).  It violates `Confirmed`: the
  caller replies to the DISCONNECT with its word unacknowledged.
- `Mut_rearm.cfg`: lossless, a second DISCONNECT while the caller drains
  re-arms the drain (the rule before 2026-11-05).  It violates
  `Released`.
- `Mut_redialwait.cfg`: lossless, a CONNECT while the caller drains waits
  behind the drain, the old session keying on (the rule before
  2026-11-05).  It violates `Released`.
- `Mut_endsilence.cfg`: lossy, the caller ends on silence without having
  heard an M_END.  It violates `NoCut`: a fade in a live session, the
  callee's application still in it, ends the session.
- `Mut_staleteardown.cfg`: lossless, a CALL taken keeps the DISCONNECT the
  last session held back to drain.  It violates `NoInherited`.
- `Mut_skipreply.cfg`: lossless, an end owing the reply to its peer's
  DISCONNECT goes idle without keying it.  It violates `Clean`: the peer
  retries and times out.
- `Mut_anypattern.cfg`: a pattern ends an end in any state, not only one
  whose DISCONNECT is out.  No error, the same 2 568 389 states: as with the
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
