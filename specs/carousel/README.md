# Verifying the carousel

Two instruments check the carousel data plane ([CAROUSEL-SPEC.md](../../docs/CAROUSEL-SPEC.md),
`datalink_arq/carousel.c`) for safety: a TLA+ model of its block layer,
checked exhaustively at small sizes, and an explorer that runs the real
`carousel.c` under every loss pattern and every carrier-sense failure up to a
bound.  The model covers every interleaving; the explorer covers the real
code with its timers.  Neither says anything about throughput.

## The TLA+ model: `CarouselBlocks.tla`

One direction of the block layer: the sender opens blocks, sends their
pieces and retires them on the receiver's word; the receiver collects pieces
by wire id, decodes, delivers in order, and answers by POLL (base and need per
window slot) or, at the floor, by a BREAK pattern.  Wire ids are mod `MOD`,
the window is `WIN`, `MOD = 2 * WIN` (16 and 8 in the code).  The rules
mirror `take_pieces`, `apply_need`, `car_on_pattern`, `deliver_in_order` and
`send_poll`; the comments name each.

Time, rungs and turns are left out: any enabled action may run, which covers
every schedule the timers can produce.  Each piece carries the true id of its
block as a ghost field, so a block rebuilt from another block's piece shows.

What the channel is allowed to do, and why:

- **Each direction is a FIFO that may lose frames.**  Nothing overtakes on
  air.  An unordered channel let a stale POLL arrive after a newer one, which
  cannot happen and produced false counterexamples.
- **Half duplex.**  A station that keys loses the frames still on the air
  towards it.  Without this rule the sender kept acting on polls it could
  not have heard.
- **Delivery is atomic with decoding.**  `deliver_in_order` runs inside
  `on_data`, so a poll's base is always the first block not yet decoded.
- **`send_poll` clears the BREAK flag** (`rx_break`), as the code does.

Properties, checked as invariants:

| Name | Meaning |
|---|---|
| `InOrder` | blocks reach the application in order, each once |
| `Integrity` | a delivered block was rebuilt only from its own pieces |
| `RetireSafe` | the sender drops a block only once the receiver holds it decoded |
| `NoPhantom` | nothing past the real blocks is delivered |

### Results

TLC (tla2tools 2026-10-05), 8 workers.  `CHECK_DEADLOCK FALSE`: a finished
transfer has no enabled action, and that is not an error.  `NotFinished`,
checked as an invariant, must fail: a complete transfer is reachable, so a
pass is not the model doing nothing.

The model as of the integrity work (a BREAK only stops a block, `CHECK` =
blocks carry a check), with repeating pieces; state counts from the last
run:

| Config | Channel | Sizes | Result |
|---|---|---|---|
| `Honest_Floor` | honest, floor (BREAK) | NB 4, K 2, sent ≤ 4, MOD 4, WIN 2, 3 in flight | pass, 2 254 974 states |
| `Honest_Above` | honest, polls only | same | pass, 3 654 644 states |
| `Honest_Floor_W3` | honest, floor | NB 7, K 2, sent ≤ 3, MOD 6, WIN 3, 2 in flight (ids wrap) | pass, 23 827 905 states |
| `Honest_Above_W3` | honest, polls only | same | pass, 63 416 012 states |
| `Spurious_Floor` | a BREAK nobody sent | NB 3, K 2, sent ≤ 3, MOD 4, WIN 2, 2 in flight | pass, 259 464 states |
| `Spurious_Floor_Big` | a BREAK nobody sent | NB 4, K 2, sent ≤ 4, MOD 4, WIN 2, 3 in flight | pass, 27 515 636 states |
| `Stale_Above` | a same-seed frame of an earlier session | NB 3, K 2, sent ≤ 3, MOD 4, WIN 2, 2 in flight | pass, 1 482 872 states |
| `Adversary_Floor` | both, at the floor | same | pass, 12 138 448 states |
| `Stale_Above_NoCheck` | a stale frame, blocks without a check | same | **`Integrity` violated** |

Before it (trunk 2c07bf4: a BREAK retired the block, and blocks had no
check), two configurations failed, in 9 steps each:

- **Spurious BREAK, `RetireSafe` (risks R5, R6).**  The sender streams
  block 0 at the floor and sends both of its pieces, which are still in
  flight.  A BREAK arrives that the receiver never sent: a detector false
  alarm, or another station's pattern, since patterns carry no session.
  `car_on_pattern` retired block 0 because it went out at least K times.  The
  receiver had not decoded it, and nobody would send it again.  Fixed: a
  BREAK only stops the block, and only a poll retires one.
- **Stale frame, `Integrity` (R17, and a CRC16 false pass, R1).**  A frame
  of an earlier session with the same seed (session ids have 64 values)
  carries wire id 0.  The receiver took it as a piece of the new block 0,
  decoded block 0 from that piece and one real one, and delivered it.  Fixed:
  the block's check fails and the session ends (`Stale_Above_NoCheck` keeps
  the old behaviour); the callee's nonce in the seed makes the stale frame
  itself a 1-in-65536 event.

One abstraction matters here: the model delivers a block when it decodes,
where the code streams its data pieces as they arrive.  So in the code a bad
block's leading bytes can reach the application before its check fails; the
session then ends (fail closed), which the explorer checks below.

### Liveness

Safety says nothing bad happens; `Completes` says the transfer ends: every
block reaches the application, or the session fails closed.  It is checked
under `LiveSpec` (`Live_*.cfg`), which adds what the code does and what a
lossy channel can fairly be asked:

- `TURNS`: a half-duplex turn.  The sender keys a round of at least one
  piece, then listens until the answer comes or its timer runs out.  With
  any interleaving the sender kept keying over the poll in flight, forever.
- The sender's pieces repeat (the code's indices wrap mod 256).
- Fairness: the sender keeps sending, the receiver keeps answering, and polls
  infinitely often (the code's floor cadence guarantees one every 4 patterns,
  12 deep).  Each piece *of each block* and each poll, offered again and
  again, is eventually delivered.  Per piece index alone was not enough: TLC
  found a channel that delivered only the duplicates the receiver already
  held.  A frame on the air ends, received or lost.
- `INJMAX`: at most this many spurious BREAKs or stale frames.  No protocol
  progresses against forgeries without end: a BREAK always at the head of the
  return channel keeps the poll behind it from ever being heard.  The safety
  configurations leave them unbounded.

| Config | Result |
|---|---|
| `Live_Honest_Floor` | pass (2.8 M states, 29 min); a rerun on 2026-10-06 did not finish its temporal check in 24 h (3.1 M states) and was stopped |
| `Live_Spurious_Floor` (2 spurious BREAKs) | pass (516 k states, 2 h) |
| `Live_Adversary_Floor` (spurious BREAKs and stale frames, 3 blocks) | pass (2.58 M states, 14.2 h) |
| `Live_Honest_Above_3` (above the floor, at `Live_Adversary_Floor`'s sizes) | pass (78 k states, 9 min) |

`Live_Adversary_Floor` also covers an honest channel at its sizes: its
injections are bounded and may be none, so every honest behaviour is one of
its fair behaviours.  `Live_Honest_Above` at 4 blocks and channel 3 has not
been run to the end.

The counterexamples on the way were the model's, not the code's: the
unbounded adversary, a timeout that waited for the channel to empty, a sender
ending its turn without sending.  Each is recorded where the model was fixed.

### Running it

```
java -XX:+UseParallelGC -cp tla2tools.jar tlc2.TLC -workers 8 \
     -metadir /some/private/dir -config Honest_Floor.cfg CarouselBlocks.tla
```

`tla2tools.jar` is the TLA+ tools release from
https://github.com/tlaplus/tlaplus/releases (these results: the Toolbox's,
2026-10-05).  Give each run its own `-metadir`: two runs sharing `states/`
destroy each other's state.  A violated property prints the trace state by
state.

## A proof for every size: `CarouselProof.tla`, `CarouselProofs.tla`

TLC checks the model at small sizes.  `CarouselProofs.tla` proves the block
layer safe with TLAPS for the code's window -- WIN = 8, ids mod 16 -- and any
number of blocks, any K, and FIFO channels of any length that lose any frames:

```
THEOREM SafetyHolds == Spec => []Safety      \* RetireSafe /\ Integrity
```

823 obligations, all proved (tlapm bfa9468, Z3 and Zenon):
`tlapm --threads 8 CarouselProofs.tla`.

`CarouselProof.tla` is the block layer written for proof: sets and functions
over block ids instead of sequences filtered with `SelectSeq`, ghost fields
on frames in flight, and no BREAK.  The sender may send a piece of *any*
open block at any time, which covers whatever a BREAK -- genuine, spurious,
another station's -- makes it choose.  K does not enter: a block may decode
on any piece.

The inductive invariant (`Inv`), in words:
- the sender's open blocks span at most WIN ids below `nxt`; every id below
  `nxt` is open or retired;
- the receiver's base is the first block not decoded;
- a data frame in flight was sent from an open block (`snxt` = `nxt` then,
  in FIFO order), so its block lies within WIN of the base either side and
  its id mod 16 resolves to it -- or reads as behind the base;
- a poll in flight carries a faithful snapshot (`abase`, `sd`): its base is
  at most the receiver's, everything it reports decoded is decoded; older
  polls come first with smaller snapshots; and its base is open, or not yet
  opened -- so every open block is below it plus WIN.

The last point is where FIFO order is needed: an older poll can only retire
blocks it saw decoded, so it never retires a later poll's base, and the
sender's window stays anchored to every poll in flight.  The window lemmas
(`ResolveIn`, `ResolveBehind`) are where WIN = 8 enters: with a symbolic WIN,
`%` is non-linear for the SMT solver.

`ProofCheck.tla` runs TLC over the proof model with `Inv` as the invariant
(13 556 states at WIN 2): it found the invariant's gaps before any proof was
attempted.  Dropping the span check from `Open` breaks it (TLC) and the proof
(2 obligations fail).

The Isabelle backend of this tlapm build does not start (its heaps record the
build machine's paths), so nothing here uses it: the one step that wanted it,
the least undecoded block, is avoided by letting the base slide to *a* first
undecoded block (one always exists).

## The explorer: `tests/sim/car_explore.c`

It runs the real `carousel.c` in the carousel simulator (`tests/sim/carousel_sim.c`,
virtual clock) on a short transfer, one way and both ways, on four channels:
a fast rung (`cliff:20`), the middle (`cliff:3`), the DATAC15/MFSK boundary
(`cliff:-5`) and the MFSK floor (`cliff:-9`).  First it runs the session
clean, then once for every set of at most k forced events, counted in keying
order across both stations:

- **`test_car_explore all k`: lost frames.**  A run must deliver everything
  intact, never key over the peer, and never key a frame the modem would
  refuse.  Sensing works, so an overlap is a protocol fault.
- **`test_car_explore cs k`: keydowns the peer does not sense.**  Data must still
  arrive whole.  Keying over the unsensed keydown is expected, and can happen
  more than once when that keydown is long.  An overlap with a keydown that
  could be sensed counts as a cascade and fails.
- **`test_car_explore brk k`: spurious BREAKs.**  The keydown is lost whole
  and a BREAK nobody sent answers it, 1.2 s after it ends.  The data must
  arrive whole.
- **`test_car_explore bad k`: corrupt frames that passed their CRC.**  One
  byte flipped (`CAR_CORRUPT_POS`, default the middle).  The session must
  complete intact or fail closed (`car_failed`); never deliver wrong data and
  carry on, never stall.

A failing set is printed, and `test_car_explore replay` / `csreplay` /
`brkreplay` / `badreplay` reruns it (`CAR_TRACE=1` traces it).  With no
arguments, which is how `make test` runs it, it does losses and sensing
failures at k ≤ 3, single spurious BREAKs and single corrupt frames at four
byte positions: 19 765 runs, about 1.5 s.
`CAR_EXPLORE_BYTES=4000 make -B test_car_explore` builds a longer transfer,
and `CAR_EXPLORE_CHANS` sets the channels.

### Results

Trunk 2c07bf4, before the integrity work:

| Run | Transfer | Runs | Failures |
|---|---|---|---|
| lost frames, k ≤ 3 | 600 B | 14 858 | 0 |
| lost frames, k ≤ 4 | 600 B | 118 769 | **1** (below) |
| lost frames, k ≤ 2 | 4000 B | 24 966 | 0 |
| lost frames, k ≤ 3 | 4000 B | 9 093 700 | 0 |
| unsensed keydowns, k ≤ 4 | 600 B | 22 183 | 0, no cascade |
| unsensed keydowns, k ≤ 2 | 4000 B | 29 718 | 0, no cascade |

In every sensing-failure run the data arrived whole.  The overlaps all come
at the floor, both ways (`cliff:-9 bidir`).  There, a long unsensed round,
such as a 41 s handover round, has the peer's blind floor continuations
start under it.  The session recovers once the round ends.

**The one failure: two blind timers fire together** (`cliff:-5`, both ways,
frames 5, 9, 10, 11 lost: `CAR_TRACE=1 CAR_EXPLORE_CHAN=cliff:-5
test_car_explore replay 1 5 9 10 11`).

1. B's handover poll is lost.  B's round still arrives, because A's payload
   decoder is already bound to its rung.
2. A's next poll is lost, then A's handover keydown: the poll and the data
   frame of its round.
3. B, which has heard nothing for 90 s, repeats its round at 164.7 s
   (`SENDER_SILENCE_MS`).
4. A's handover repeat timer fires 0.3 s later.  That is shorter than the
   time to sense a carrier, so the two keydowns overlap, three frames are
   lost, and the session goes on intact.

This is the risk R10 of the spec in a concrete form: blind timers on the two
ends are not ordered against each other.  Step 3 of the carousel work
(staggering blind keydowns by role) answers it, and this set becomes its
regression case.

After the integrity work (carousel-integrity):

| Run | Transfer | Runs | Failures |
|---|---|---|---|
| everything in `make test` | 600 B | 19 765 | 0 (203 corrupt runs failed closed) |
| spurious BREAK, k = 1 | 4000 B | 486 | 0 (trunk: **45 of 480**, sessions stalled with data undelivered) |
| spurious BREAKs, k ≤ 2 | 600 B | 697 | 1: two senders (R23, below) |
| corrupt frame, k = 1, every byte position 0-130 | 600 B | 117 per position | 0 |
| corrupt frames, k ≤ 2 | 600 B | 1 508 | 0 (1 109 failed closed) |
| corrupt frame, k = 1 | 4000 B | 846 | 0 (467 failed closed) |

**Two senders after a spurious pattern** (R23; `test_car_explore brkreplay
1 cliff:-5 3 4`).  B's poll is lost, then B's handover keydown, and a BREAK
nobody sent reaches B just after it.  B takes it as A's answer to its
handover and streams; A never heard the handover and keeps sending too.
Each sends on a rung the other's payload decoder is not bound to, nobody
polls, and the session runs until the 240 s lost-peer watchdog ends it (not
in this simulator).  Nothing is delivered wrong.  It needs two events in a
precise order, and it is for step 3 with the other control-signal work.

What the explorer does not cover: SNR-dependent decoding (each frame either
arrives or is lost), fading collisions between two keydowns that both sense
correctly but too late, and more than k events.  Those are the job of the
seeded matrix (`carousel_bench`) and of the step-3 work on control signals.
