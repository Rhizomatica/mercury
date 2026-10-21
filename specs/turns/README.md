# specs/turns

The turn rules of `docs/CAROUSEL-TURNS.md`, timed: who may key and when, so
that the two stations never transmit together, under any loss.

    java -cp /opt/TLA+Toolbox/tla2tools.jar tlc2.TLC -workers 6 \
        -metadir /tmp/tlc-turns -config CarouselTurns.cfg CarouselTurns.tla

- `CarouselTurns.cfg`: OFDM-like timing.  No error, 18.5 M distinct states.
- `CarouselTurns_mfsk.cfg`: a slow decode and long answers.  No error,
  27.1 M distinct states.
- `CarouselTurns_mfsk500.cfg`: the BW500 floor's 26.2 s frame, 6 units of
  4.5 s, with the same slow decode and spacing.  No error, 107.0 M distinct
  states (2 h 06 min, 6 workers).
- `Mut_*.cfg`: one rule broken each (`nogrid`, `late`, `long`, `blind`,
  `reuse`).  Each must report `Invariant NoOverlap is violated`.

Give each TLC run its own `-metadir`: runs sharing a `states/` directory
delete each other's state.

## One-frame polls (`ONE`)

With `ONE = TRUE`, M may key a poll for one frame: the idle ask, or a probe.
Such a poll has the response slot only, no continuation slots, at both ends,
and each end reads that from the poll itself (`poll_k` in
`datalink_arq/carousel.c`, from the poll's frame count).  With `ONE = FALSE`
the behaviour is exactly the base model's.

Why: with `K` continuation slots after every idle ask at the floor, an ask
that went unanswered held M's next ask off the air until its slots had
passed, and the slots of a few lost asks stacked.  In the sim a try took
9-35 s, so on a lossy link an idle session went 240 s without hearing its
peer, and the watchdog ended it (#407).

- `CarouselTurns_one.cfg`: one-frame polls, otherwise the base timing.  No
  error, 70.6 M distinct states (1 h 25 min, 3 workers).
- `Mut_onecont.cfg`: S continues after a one-frame poll, for which M keeps
  no continuation slots.  Violates `NoOverlap`: S keys in slot 1 while M is
  on the air.

## The slot-closing rules (`CLOSE`, `DISC`)

With `CLOSE = TRUE`, M may take a slot as over before its time is up, as
`m_close_slot`, `m_close_from`, `m_close_spent` and `pattern_in_time` in
`datalink_arq/carousel.c` do:

- S's keydown is one of three things:
  - a round, whose frames name the M keydown and the slot;
  - a status saying it has nothing, sent in a response slot;
  - a pattern, which names nothing, sent in a response slot.
- S continues in a later slot only after a round of its own on that anchor
  (`s_slot`: `sent_round`).
- When M hears a round, it closes that slot.  When it hears a status, it
  closes every slot of that keydown.  Either way it also closes every slot
  of every older keydown, because S re-anchors on each keydown it decodes.
- M detects a pattern up to `DP` after the pattern ends.  It takes the
  pattern as the answer to its latest keydown, closing slot 0 and older
  keydowns, only when it is off the air and the pattern is in time: no
  sooner than `Tg + PMIN` after that keydown, and no later than `DP` past its
  response slot.

With `DISC = TRUE`, M also keys the FSM's DISCONNECT.  It keys it clear of
S's slots (`car_disconnect_must_wait`) and logs it (`car_log_ctl`).  S
answers it `TgD` after decoding it, in a single slot, and the carousel
closes nothing on that answer.  With `CLOSE` and `DISC` both `FALSE`, the
behaviour is exactly the base model's.

- `CarouselTurns_close.cfg`: the code's rules.  Not exhausted: no error
  through 60.1 M distinct states at depth 50 (H = 24, 5 workers), with
  5.3 M still queued when the run was stopped for lack of memory.
- `Mut_spentany.cfg`: S continues after a status of nothing.  Violates
  `NoOverlap`, because M had closed those slots.
- `Mut_anytime.cfg`: M takes a pattern whenever it hears one.  Violates
  `NoOverlap`.  S's pattern for an older keydown is detected late, after
  M's next short keydown, and is taken as the answer to it.  M then keys
  over S's real answer.
- `Mut_closenewer.cfg`: an answer closes newer keydowns' slots too.
  Violates `NoOverlap`.
- `Mut_nologdisc.cfg`: M does not log its DISCONNECT.  Violates
  `NoOverlap`: S's reply meets M's next keydown.
- `FalsePattern.cfg`: M's pattern detector false-alarms in a response
  window.  Violates `NoOverlap`.  This is a residual risk: a false ACK/BREAK
  closes a slot S may still key in.  The detector's false-alarm rate is
  what bounds it (0 on the bench), and listen-before-talk catches what it
  can.

Not modelled: the wait in `s_slot` for an application answer.  S's start
may move by up to `min(d/2, APP_GRACE_MS)` past the opening of its slot.
With a decode `d` late, that start can fall up to
`min(d/2, APP_GRACE_MS) - TG_MS` beyond M's window: 100 ms on MFSK (1 s
against 0.9 s), and nothing on OFDM.  That is inside M's 700 ms guard.

When M asks (the idle poll's delay, carousel.c `m_decide_idle`: the
session's first ask at once, no backing off before S has been heard) is
covered as it is: M may key at any time its slot rules allow.

M_END (poll type 3, `car_wish_end`) is S's ordinary poll with its type
changed: it goes in the same slots, with the same length, so the slot
rules here cover it as they cover any poll of S's.  Which end may end the
session on it, and when, is `specs/teardown` (`NoUnasked`, `NoLostWord`).
