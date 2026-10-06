# Carousel turns: who may key, and when

Status: design, step 1 (spec and timed model). Nothing here is implemented
yet.  The model is `specs/turns/CarouselTurns.tla`.

## 1. Why

The carousel's blocks, erasure code, integrity check and session binding are
proved safe (`specs/carousel`, TLAPS): a collision can cost airtime, but
never data.  What still collides is the turn-taking.  Both stations key on
timers of their own when they hear nothing:

- the sender repeats an unanswered handover, nudges a silent receiver, and
  continues a floor stream on silence;
- the receiver re-polls after an empty window, and hands over after its turn
  quantum.

On a half-duplex HF channel, "I hear nothing" does not mean "the peer is
silent".  A fade, a lost header or a decoder bound to the wrong mode all hide
the peer.  The residual collisions in the sim, with NAV headers on, come down
to two such timers meeting.  Over 120 seeds both ways (CAR_TRACE_OVERLAP,
2026-10-06), they fall into three classes:

1. A station holds for the peer's turn and cannot decode the peer's repeated
   handovers.  Its 90 s silence nudge falls 0.9 s before a repeat, inside the
   NAV header's detection time.  This is 6 of 10 at fade −5 dB.
2. The receiver's poll is lost; its re-poll lands in the sender's handover
   repeat, unsensed in the fade.  This is 4 at fade −5 dB.
3. Both ends are below the control mode.  A lost pattern ACK leaves the
   receiver to hand over 2.1 s into the sender's floor continuation.  This is
   3 at fade −9 dB.

Every fix so far has added a timer rule that predicts the peer's timers, or
a sensing aid (the NAV header) that can itself be lost.  This design removes
the cause: only one station ever keys on a timer.

## 2. The rules

The **caller is the session's only timing master (M)**; the **callee (S)
only answers**.  The roles never change during a session, whichever way the
data flows.

**S keys only in a slot.**  S may key only in a slot anchored to the end of
an M keydown it decoded:
- the *response slot* starts Tg after that end;
- *continuation slots* follow, the i-th starting i·P(r) after the response
  slot, for i = 1..K, with P(r) = r + A.

Within this rule:
- S learns an M keydown's end only by decoding one of its frames.  It anchors
  to the end as it learned it, which is at most D late.  A decode later than
  D is still used as data but is not answered.
- Each S keydown is at most r long, the length the M keydown allowed.
- S keys at most once per slot, or not at all (no data, or nothing to
  continue).
- A newer decoded M keydown replaces the anchor.  S has no other timer that
  keys.

**M keys only where S cannot be.**  M may start a keydown of length L at s
only if [s − G, s + L + G) misses, for every recent M keydown k and every
slot i = 0..K of it, the interval in which S could be on the air:

    [E_k + Tg + i·P(r_k),  E_k + Tg + D + i·P(r_k) + r_k)

The exception is a slot M has heard S's keydown in, end to end.  S keys once
per slot, so that slot is over, and S's frames name the M keydown and slot
they answer.  E_k is the end of M's own keydown, which M knows exactly.  M
never relies on sensing or on hearing S for *when* it may key; hearing S only
changes *what* M sends, and lets it reuse a slot early.

That is the whole protocol for who may key and when.  Everything else (what
the keydowns carry, which rung, how much) is unchanged carousel.

## 3. Requirements on frames and modem

- **R1. Every frame states its position in its keydown** (frames left after
  it), and every mode's airtime is fixed.  A receiver that decodes any frame
  of a keydown therefore knows when the keydown ends.  The carousel's frame
  header already carries `left`, and control frames gain it.
- **R2. An M keydown states r**, the longest S keydown it allows.  This
  follows from the poll's rung and frame count, or is a control frame's
  airtime (DATAC16, or the MFSK control frame when S is control-deaf), or a
  pattern.
- **R3. An S keydown states which M keydown and which slot it answers**: the
  poll id and the slot index.
- **R4. The modem timestamps every decode** with the age of the frame's end
  (capture position against now).  S answers only if the age is at most D
  (A1, §5); otherwise the frame is data, not a timing reference.
- **R5. S keys at its slot time**, anchor + Tg + i·P(r), with bounded
  transmit latency.  That latency counts in D.
- **R6. M's "end of my keydown" is when its audio has left**, the TX-complete
  event after the drain and the tail.

## 4. The carousel on these rules

- **Connect.** CALL is M's keydown; ACCEPT is S's response.  This is
  unchanged.
- **Callee → caller data.** M polls ("n frames at rung lv"), and S's round is
  its response.
  - At the floor, S continues in slots 1..K while it hears nothing.  That
    keeps the floor's "silence means keep going" and its speed.
  - M answers between S's keydowns with a pattern ACK/BREAK or a poll, in the
    gaps the slots leave (A ≥ D + 2G + M's answer).
  - A poll S does not hear leaves S on the old slots, and M keeps clear of
    them (they stay in M's log).
- **Caller → callee data.** M sends its round; S's response is the status it
  sends today as a receiver (base/need, the rung and size it wants next) or a
  pattern.  M follows that status in its next round.  The receiver of each
  direction still chooses the rung.
- **Both ways.** One M keydown can carry M's round and a poll for S's round.
  S's response then carries its status and its round.  M's scheduling policy
  decides how to alternate directions; it replaces the turn quantum and the
  handover.
- **Disconnect.** M sends DISCONNECT and S answers.  S asks for it by a flag
  in a response.
- **Silence.** S never keys without an anchor, so a lost session makes it go
  quiet, never talk blindly.  M gives up after its own timeout.

**What goes away:**
- the handover and its repeats;
- the sender's silence nudge and peer-turn hold;
- the receiver's re-polls and its waits on the sender's timers
  (`clear_of_peer_nudge`, `clear_of_peer_handover_repeat`, floor hold,
  preamble hold);
- in-session dependence on carrier sense and on NAV headers.

NAV may stay to warn third-party stations.

## 5. Assumptions

- **A1. Decode latency.** Every decode of an M keydown happens within D of
  its end, including the error of the end computed from R1.  D also covers
  S's transmit latency (R5).  Each rung needs its own D (MFSK decodes later).
- **A2. Clocks.** The two clocks run at the same rate within the guard G over
  one slot pattern: K·P(r) is at most a few minutes, and 50 ppm over 5 min is
  15 ms.
- **A3. Durations.** M's computed slot bounds use the true airtimes of S's
  possible keydowns (R1, R2).

None of these involve sensing, a fade, or the other station's timers.

## 6. The model

`specs/turns/CarouselTurns.tla` runs in discrete time with two half-duplex
stations.  The channel loses any M keydown, S decodes any time up to D after
the end, and S's keydowns are any length up to r, or absent.  M keys anything
at any time FreeFor allows.  It may learn that a slot is over at the earliest
possible moment, as S's keydown ends.

- **NoOverlap:** the two are never on the air together.
- **MNotShutOut:** off the air, M can always key its shortest keydown within
  one whole slot pattern.

| config | constants | result |
|---|---|---|
| `CarouselTurns.cfg` | Tg=1 D=2 G=1 K=2 A=5, M lengths {1,3}, r {1,3}, H=40 | no error, 18.5 M distinct states (644 s) |
| `CarouselTurns_mfsk.cfg` | D=3 A=6, r {2,4}, H=48 (slow decode, long answers) | no error, 27.1 M distinct states (1216 s) |

Each mutant breaks one rule.  Each must fail, which shows the model can see a
collision:

| mutant | broken rule | expected |
|---|---|---|
| `nogrid` | M minds only the response slot | NoOverlap violated (3098 states) |
| `late` | S answers a decode D+1 late | NoOverlap violated (1089 states) |
| `long` | S keys longer than allowed | NoOverlap violated (1966 states) |
| `blind` | S also keys on a silence timer, as today | NoOverlap violated (425 states) |
| `reuse` | S keys twice in a slot M took as over | NoOverlap violated (625 states) |

## 7. Results

TLC (`/opt/TLA+Toolbox/tla2tools.jar`, 2026-10-05 build), 2026-10-06:
- both configurations: no overlap, and M never shut out, in every reachable
  state;
- every mutant: a counterexample within seconds.  So each rule in §2 is
  needed: dropping any one of them lets the stations collide.
- `blind` is today's design in miniature: a callee keying on a silence timer
  of its own.

What the model does not cover, and step 2 must:
- the content of keydowns, and whether data progresses (the carousel's block
  layer, already proved);
- M's choice of what to send in a gap;
- the values of K and A for speed.

Liveness of the timing itself is the `MNotShutOut` invariant: M always gets
a gap.

## 8. Next (steps 2 and 3)

1. Implement behind `car_io_t`: M's slot log and FreeFor, and S's anchor and
   slots.  Remove the handover and the blind timers.  Blocks, rungs,
   integrity and modes are unchanged.
2. Sim A/B against today's carousel over 120 seeds: overlaps (must be 0 with
   any sensing failure, so the explorer's sensing-failure sets must pass) and
   time.  Then car_explore, the real-modem harness, and on air.
3. Choose K and A for performance (the floor's speed); they do not affect
   safety.
