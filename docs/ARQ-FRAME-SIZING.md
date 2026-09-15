# The retained frame decides which modes can ever carry it

Findings from the 0 dB bench cell on `mfsk-margin`, where a 1 KB transfer
stopped at exactly 276 bytes, 3 runs out of 3, and stayed stopped until the
no-progress timeout 206 seconds later.

## The mechanism

The data plane is stop-and-wait with a single retained frame. That frame is
**immutable**: it is never re-framed smaller, because the `seq <-> bytes`
mapping has to stay fixed for a duplicate to be idempotent on the peer. A
receiver that has already delivered `seq=4` must be able to drop a second copy
of `seq=4` without asking what is in it.

Immutability has a consequence that is easy to miss: **the width a frame is
read at decides, permanently, which modes can ever transmit it.** Slot sizes
are not close together —

| rung | mode | user bytes |
|---|---|---|
| 0 | MFSK | 90 |
| 1 | DATAC15 | 22 |
| 2 | DATAC4 | 46 |
| 3 | DATAC3 | 118 |
| 4 | DATAC1 | 502 |
| 5 | DATAC17 | 1172 |
| 6 | QAM16C2 | 1205 |

— so a frame read at DATAC1's full 502 bytes fits **nothing below DATAC1**.
`mode_that_fits()` then pins every retransmission to DATAC1 no matter what the
ladder does.

## What that looks like on the air

From the 0 dB run, sender side. The ladder walks all the way down:

```
+45.9s  Ladder step-up   to 4        <- probes DATAC1
+59.7s  Ladder step-down to 3 (retry)
+72.6s  Ladder step-down to 2 (retry)
+85.4s  Ladder step-down to 1 (retry)
+98.3s  Ladder step-down to 0 (retry)
```

and every keydown from the probe onward is the same length, to the millisecond:

```
+46.9 -> +51.7   4.817 s
+59.7 -> +64.6   4.817 s
+72.6 -> +77.4   4.817 s
+85.4 -> +90.2   4.817 s
+98.3 -> +103.1  4.817 s     <- ladder says MFSK (13.5 s); the air says DATAC1
```

DATAC1's frame duration is 4.81 s. The ladder was moving; the transmitter never
did. Every one of those step-downs was fiction.

Fiction is not harmless. Only one payload decoder runs at a time, so the
receiver's mirror — which follows the same delivery-driven rule to stay in step
without any on-wire negotiation — faithfully followed the sender *away* from the
one mode actually being transmitted:

```
receiver: +61.2 level 3   +76.2 level 2   +91.2 level 1   +106.2 level 0
```

From +106 s both ends sat at the MFSK floor, in perfect agreement, for 120
seconds — while the sender transmitted DATAC1 into a decoder listening for
MFSK. Neither end was faulty. They had simply stopped talking about the same
thing.

## The fix, and what it does not fix

Cap a **fresh** read at the slot of a rung that has already delivered
(`proven_level` in `send_data_burst`). The frame then always fits the rung the
ladder retreats to, so a failed probe costs one retry instead of the session.

The cap applies only while probing *above* the proven rung. Capping
unconditionally would be a bug of its own: slot sizes are not monotonic — MFSK
carries 90 bytes and DATAC15 carries 22 — so an unconditional `min()` shrinks a
floor frame to a fifth of its payload during exactly the deep fade the floor
exists for.

Measured:

All arms are `make clean` builds — an incremental build silently kept a stale
object here and produced a number that was wrong by 18%, so rebuild from
scratch before trusting any of this.

| | before | after |
|---|---|---|
| 0 dB bench, 1054 B payload | 276/1054, wedged, 3/3 | **912/1054** and still delivering when the harness deadline expired |
| clean bench, 1054 B payload | 81.18 s | 87.09 s (**~7% slower**) |
| sim fade cycle, 100 seeds (this branch's test, both arms) | 72/100 fail | 48/100 fail |

The 7% is the cost of the cap and it is real: one frame per rung climbed goes
out under-filled.  That buys immunity to a wedge that costs the *entire
session* when it fires, so the trade is worth making — but it is a trade, not
a free win.

### A start rung above the floor is not the way to win it back

Opening at DATAC15 instead of MFSK recovers the 7% and more — 81.18 s ->
71.57 s on the clean cell.  It also doubles the fringe: 102 B at SNR3k =
-12 dB goes 50.44 s -> 101.71 s, because every probe the channel cannot carry
costs a burst plus a full ACK timeout and at the fringe the ladder pays that
over and over.  Reach is unaffected (both arms deliver every byte).  Measured
and reverted; see ARQ_LADDER_START_LEVEL.

**The residual 48% is a different instance of the same defect.** (Both arms of
that row are this branch's test; neither is a trunk number — see the traps
section.) A frame read
at a rung that *has* delivered can still be stranded if the band collapses
afterwards. The cap cannot prevent that — the rung really had carried a frame.
On the sim's fade scenario that variant is unchanged (42/100 before, 43/100
after).

## What would close it

Frame identity that survives re-framing. Today the receiver dedups by sequence
number, so shrinking a frame changes what `seq=N` means and the two ends'
stream positions diverge — the sender would resend bytes the receiver already
delivered. Identify payload by **stream offset** instead and re-framing becomes
idempotent by construction: the receiver delivers bytes at their offset and
drops anything below its high-water mark, whatever frame it arrived in.

That is a wire change (the header carries a 1-byte `tx_seq` today), which pre-2.0
permits. It is also most of what multi-frame bursts need, so the two are worth
doing together rather than separately.

## Two traps this cost time in

**A budget with no margin looks exactly like a seed lottery — measure before
blaming it.** An earlier revision of this document claimed
`test_sim_fade_cliff_downgrades` "fails 72 of 100 seeds on trunk". That was
wrong, and the error is worth recording: the baseline arm was *this branch's*
rewritten test on *this branch's* rewritten FSM, and the number was then
labelled as trunk's.

Swept properly — trunk at `f79a28e`, only the seed and the time budget
parameterised, 100 seeds:

| arm | budget | assertion | pass |
|---|---|---|---|
| trunk, as it ships | 2 h | full delivery | **100 / 100** |
| trunk, budget cut to 1 h | 1 h | full delivery | 12 / 100 |
| this branch, as it stands | 1 h | prefix integrity | 59 / 100 |

Trunk is not a lottery; it passes every seed. The lottery is a property of the
*one-hour* budget, which is exactly what trunk's own comment says it found and
fixed by doubling it. This branch shortens the budget again — with a stated
reason, since the band is restored to 12 dB and the remainder drains at a fast
rung instead of the floor's ~22 B per ~11 s — and weakens the assertion from
full delivery to prefix integrity.

Its 41 failures split by cause, and the split is the point:

| failure | count |
|---|---|
| "did not recover to a fast mode after the fade" | 31 |
| "did not climb on the good band" | 10 |
| **prefix integrity (correctness)** | **0** |

No seed produced duplicated or reordered bytes, so the property this test was
rewritten to protect — that re-framing can never corrupt the delivered stream —
holds on every draw. What remains seed-sensitive are the two ladder assertions
(`max_level >= 3`), fitted the same way the old delivery assertion was, and due
the same treatment: margin, or a swept threshold instead of a fitted one.

**A dead bridge direction looks exactly like a connect bug.** In the integration
harness, `runChannelDir` returned on the first error, retiring that direction
for the rest of the test. In FIFO mode each peer's clock advances with the audio
it captures, so one dead direction freezes *both* processes: event loops
healthy, sockets responsive, every timer stopped. The symptom is a CALL sent
once and never retried while the peer sits in ACCEPTING — indistinguishable
from an FSM bug until you take a backtrace. It now retries.


## Where the time actually goes, and why the obvious speedups fail

Measured on the clean bench cell, 5 KB and 8 KB payloads (sender-side PTT
edges, both peers' logs):

| | 5 KB | 8 KB |
|---|---|---|
| keydowns | 13 | 16 |
| airtime | 66.7 s (67%) | 77.9 s (67%) |
| **turnaround** | **32.6 s (33%)** | **39.2 s (33%)** |
| goodput | 51.7 B/s (414 bps) | 70.0 B/s (560 bps) |

The turnaround is rung-independent — 2.47 s, every burst, whatever mode it
was — and decomposes as:

```
A PTT-off -> B PTT-on   0.46 s   (channel guard + decode)
B ACK keydown           1.05 s   (100 ms silence + 640 ms pattern + 200 ms silence)
B PTT-off -> A PTT-on   0.68 s   (post-ACK guard)
```

Every keydown carries 300 ms of deliberate silence (100 ms head, 200 ms tail,
the same for data bursts and pattern ACKs). Across 23 keydowns that is 7% of a
5 KB transfer. Trimming it is tempting and NOT safe to do from the simulator:
the tail exists so the audio backend flushes the last samples before PTT
drops, and that behaviour has not been measured on real hardware.

**The ceiling is one ACK per data frame.** Stop-and-wait pays 2.47 s per
frame regardless of how fast the mode is, so at the top rung a 3.70 s
QAM16C2 burst spends 40% of its cycle not transmitting. Only more data per
keydown changes that:

| frames per keydown | cycle | goodput at QAM16C2 |
|---|---|---|
| 1 (today) | 6.17 s | 195 B/s |
| 2 | 9.87 s | 244 B/s (+25%) |
| 4 | 17.27 s | 279 B/s (+43%) |

### Rejected: dropping the two "dominated" rungs

Per-rung goodput with the measured turnaround says rungs 1 and 2 are
pointless — DATAC15 moves 22 user bytes per 6.87 s cycle (3.2 B/s) and DATAC4
46 per 8.27 s (5.6 B/s), against the MFSK floor's 90 per 15.97 s (5.6 B/s).
Same or worse, and both are less robust than MFSK. A five-rung ladder
(MFSK, DATAC3, DATAC1, DATAC17, QAM16C2) is **13.9% faster** on the clean
cell: 104.81 s -> 90.20 s for 5 KB.

It is **3.2x slower at the fringe**: 102 B at SNR3k = -12 dB goes
50.45 s -> 162.09 s.

Those rungs are not there for throughput. They are intermediate steps that can
actually *succeed*: at -12 dB the ladder can sit on DATAC15, whereas with the
gap widened every climb attempt probes DATAC3, fails, and burns a full ACK
timeout. Goodput-per-rung is the wrong metric because it ignores the
probability that a probe lands — the ladder's granularity is protective, and
the cost of that protection is paid only on links fast enough not to care.

This is the third speedup measured and rejected for the same reason (see also
the start rung and the two-rung ramp): **anything that makes the ladder
climb more aggressively wins on a channel that never needed the help and
loses on the one that does.**


### Rejected: halving the Welch-Costas ACK

The ACK keydown is the largest single piece of the turnaround (1.05 s of
2.47 s), and 640 ms of it is the pattern itself: for the 32-MFSK geometry an
8-tone Welch-Costas array sent **twice**, accepted at 8 matched symbols.
Sending it once would cut ~13% off the turnaround, ~4% end to end.

`utils/ackpat_sweep` drives the shipped `mfsk_pattern_tx`/`mfsk_pattern_detect`
(not a reimplementation) against AWGN.  SNR here is burst-band signal RMS
against noise RMS over the same samples — **not** SNR3k, so compare these
numbers only with each other:

```
   SNR    detect   false-accept   detect(one repetition, pessimistic)
   -6.0   100.0%      0.0%         100.0%
  -10.0   100.0%      0.0%          78.0%
  -12.0   100.0%      0.0%          29.5%
  -14.0    87.0%      0.0%           2.5%
  -16.0    23.5%      0.0%           0.0%
  -18.0     1.5%      0.0%           0.0%
```

Two things stand out.

**False accepts are zero everywhere**, down to -30 dB where the pattern is
buried. The 8-of-16 threshold is not being paid for in false-alarm immunity;
there is none to reclaim.

**The second repetition buys 4-6 dB.** The one-repetition column is a
pessimistic bound — it replaces the second half with noise while the detector
still demands 8 matches, so all 8 signal symbols must land, which is stricter
than a purpose-built 8-symbol pattern with a relaxed threshold would be. Allow
that design 1-2 dB back and halving still costs about 3 dB, which is what
halving the integration time is worth in theory.

That is the wrong 3 dB to spend. The ACK is the reverse path, and it has to
outlive the data mode it acknowledges: a link that carries data forward but
drops ACKs stalls exactly as dead as one that carries nothing, and it stalls
in the confusing way, with the sender retransmitting into a receiver that
already has the frame. This project has been bitten by precisely that
asymmetry before (see the in-session ACK on DATAC18, which killed 5 kB uucp
transfers around 2 kB).

So: 4% end to end, paid for out of the margin that keeps the reverse path
alive at the SNRs the MFSK rung exists for. Rejected — and this is the fourth
speedup to fail the same test.

If it is ever revisited, the measurement that would justify it is the ACK's
cliff against the *data* cliff in the same units: the pattern only needs to be
robust enough to outlive the slowest rung, and the sweep above cannot say how
much of the gap it currently has because it does not put the two on one axis.
