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

| | before | after |
|---|---|---|
| 0 dB bench, 1054 B payload | 276/1054, wedged, 3/3 | 552/1054 and still delivering at the harness deadline |
| sim fade cycle, full delivery, 100 seeds | 72/100 fail | 48/100 fail |

**The residual 48% is a different instance of the same defect.** A frame read
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

**The sim's fade test is a seed lottery.** `test_sim_fade_cliff_downgrades`
asserted full delivery and passes on its hardcoded seed 42 — while failing 72
of 100 other seeds on trunk. A green run there carried no information, and the
first change to shift the trajectory would have been blamed for a defect that
was already present. It now asserts prefix integrity (nothing duplicated or
reordered, which is what an unsafe re-framing fix would break) and takes
`SIMSEED` so the rate can be *measured* by sweeping rather than sampled by one
run.

**A dead bridge direction looks exactly like a connect bug.** In the integration
harness, `runChannelDir` returned on the first error, retiring that direction
for the rest of the test. In FIFO mode each peer's clock advances with the audio
it captures, so one dead direction freezes *both* processes: event loops
healthy, sockets responsive, every timer stopped. The symptom is a CALL sent
once and never retried while the peer sits in ACCEPTING — indistinguishable
from an FSM bug until you take a backtrace. It now retries.
