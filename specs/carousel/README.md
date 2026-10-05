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

TLC (tla2tools 2026.10.04), 8 workers.  `CHECK_DEADLOCK FALSE`: a finished transfer has no
enabled action, and that is not an error.

| Config | Channel | Sizes | Result |
|---|---|---|---|
| `Honest_Floor` | honest, floor (BREAK) | NB 4, K 2, sent ≤ 4, MOD 4, WIN 2, 3 in flight | pass, 57 258 states, depth 48 |
| `Honest_Above` | honest, polls only | same | pass, 341 673 states, depth 40 |
| `Honest_Floor_W3` | honest, floor | NB 7, K 2, sent ≤ 3, MOD 6, WIN 3, 2 in flight (ids wrap) | pass, 112 410 states, depth 66 |
| `Honest_Above_W3` | honest, polls only | same | pass, 4 529 446 states, depth 56 |
| `Spurious_Floor` | a BREAK nobody sent | NB 3, K 2, sent ≤ 3, MOD 4, WIN 2, 2 in flight | **`RetireSafe` violated**, depth 9 |
| `Stale_Above` | a same-seed frame of an earlier session | same, polls only | **`Integrity` violated**, depth 9 |

On an honest channel the block layer is safe at these sizes, with the window
wrapping.  The two counterexamples are the spec's risks R5 and R17, found by
the model rather than only argued:

- **Spurious BREAK (R5, R6).**  The sender streams block 0 at the floor and
  sends both of its pieces, which are still in flight.  A BREAK arrives that the
  receiver never sent: a detector false alarm, or another station's
  pattern, since patterns carry no session.  `car_on_pattern` retires block
  0 because it went out at least K times.  The receiver has not decoded it,
  and nobody will send it again.
- **Stale frame (R17, and a CRC16 false pass, R1).**  A frame of an earlier
  session with the same seed (session ids have 64 values) carries wire id 0.
  The receiver takes it as a piece of the new block 0, decodes block 0 from
  that piece and one real one, and delivers it.

Both are fixed by authenticating what the receiver acts on (step 2 of the
carousel work); the fixed model must then pass both configs.

### Running it

```
java -XX:+UseParallelGC -cp tla2tools.jar tlc2.TLC -workers 8 -cleanup \
     -config Honest_Floor.cfg CarouselBlocks.tla
```

`tla2tools.jar` is the TLA+ tools release from
https://github.com/tlaplus/tlaplus/releases.  A violated invariant prints the
trace state by state.

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

A failing set is printed, and `test_car_explore replay` / `csreplay` reruns
it (`CAR_TRACE=1` traces it).  With no arguments, which is how `make test`
runs it, it does both at k ≤ 3: 19 215 runs, about 1.3 s.  `all k` and
`cs k` go further.  `CAR_EXPLORE_BYTES=4000 make -B test_car_explore` builds a
longer transfer, and `CAR_EXPLORE_CHANS` sets the channels.

### Results (trunk 2c07bf4)

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

What the explorer does not cover: SNR-dependent decoding (each frame either
arrives or is lost), fading collisions between two keydowns that both sense
correctly but too late, and more than k events.  Those are the job of the
seeded matrix (`carousel_bench`) and of the step-3 work on control signals.
