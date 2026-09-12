# NNCP bundles over the broadcast plane — bench trial

A real NNCP bundle carried end to end over Mercury's one-way broadcast plane,
between two Mercury modems with a channel simulator standing in for the path.

**This is a bench result, not an on-air one.** Everything below came from
`ch` (codec2's channel simulator), not from a radio. No NNCP bundle has yet
crossed a real HF link. Treat the numbers as an upper bound and a sanity
check on the mechanism, not as field performance.

Reproduce with `tests/integration/mercury_broadcast_nncp_test.go`:

```
MERCURY_BCAST_TRIAL=1 MERCURY_BCAST_MODE=10 \
  go test -run TestBroadcastNNCPBundleOverChannel -v ./...
```

## What is actually being tested

The bundle is produced by `nncp-bundle -tx` on one node and consumed by
`nncp-bundle -rx` + `nncp-toss` on a **second, independently keyed node**. A
pass therefore means NNCP itself accepted what came off the air — signatures
and all — and tossed the payload into the receiving node's incoming spool
byte-identical to the original. It is not a file comparison with NNCP-shaped
bytes; it is NNCP.

Mercury treats the bundle as opaque. Nothing in Mercury knows about NNCP, and
nothing needs to.

## Results

6000-byte payload → 7680-byte NNCP bundle. `SNR3k = -No - 14.82`. Every row
below ended with **NNCP accepting the bundle** and the payload delivered intact.

| mode | channel | SNR3k | cycles | wall | goodput |
|---|---|---|---:|---:|---:|
| QAM16C2 | AWGN | +17.2 | 14 | 26.0 s | 2360 bps |
| QAM16C2 | mpg (0.1 Hz) | +17.2 | 28 | 37.5 s | 1636 bps |
| QAM16C2 | mpg | +25.2 | 28 | 26.5 s | 2315 bps |
| QAM16C2 | mpp (1.0 Hz) | +25.2 | 28 | 30.0 s | 2045 bps |
| DATAC17 | mpg | +7.2 | 28 | 67.1 s | 916 bps |
| DATAC3 | AWGN | +0.2 | 190 | 361 s | 170 bps |
| DATAC3 | mpg | +0.2 | 190 | 515 s | 119 bps |
| DATAC3 | mpp | +0.2 | 190 | 423 s | 145 bps |

The 2360 bps AWGN figure for QAM16C2 agrees with the 2353 bps in
[BROADCAST-FILE.md](BROADCAST-FILE.md), measured separately with a plain file.

### Fading taxes the transfer, it does not cliff it

At each mode's AWGN floor, adding multipath costs roughly 40-45% more airtime
rather than failing:

- QAM16C2 at +17.2 dB: 26.0 s → 37.5 s (**+44%**)
- DATAC3 at +0.2 dB: 361 s → 515 s (**+43%**)

That is the fountain code doing its job. A frame lost to a fade is simply a
symbol the receiver did not collect; there is no retransmit to stall on and no
state to resynchronise, so the carousel just runs a little longer. An ARQ link
at the same SNR would be spending that airtime on retries and turnarounds.

About 8 dB of margin buys the tax back entirely: QAM16C2 under mpg at +25.2 dB
finished in 26.5 s, within noise of its AWGN time.

### Slow fading looked worse than fast fading

For DATAC3 the 0.1 Hz profile was *slower* than the 1.0 Hz one (515 s vs
423 s), which is the opposite of the usual "faster Doppler is harder" instinct.
The plausible reading is that slow fading produces long deep fades that take out
many consecutive frames, while faster fading decorrelates between frames and
gives the carousel diversity to work with.

**This is one run per point, so treat it as suggestive, not established.** `ch`
replays a single fixed fading realisation (seed 1), so these are one draw from
the ensemble, not an average over it. Confirming the effect needs repeats with
different realisations.

## Where it stops working

Walking the SNR down at a fixed 2x cycle overshoot until the bundle stops
arriving. "Last OK" and "first FAIL" bracket the transition; the step is 2 dB.

| mode | channel | last OK | first FAIL |
|---|---|---:|---:|
| QAM16C2 | AWGN | +13.2 | +11.2 |
| QAM16C2 | mpg | +15.2 | +13.2 |
| DATAC17 | AWGN | +3.2 | +1.2 |
| DATAC17 | mpg | +5.2 | +3.2 |

**Multipath costs about 2 dB**, and the same 2 dB on both modes — a much more
useful number than the percent-of-airtime figures above, because it is the
quantity you compare against a link budget.

### But that is not a decode floor, it is a carousel-length limit

The table above is "the SNR at which 2x overshoot stops being enough", which is
not the same thing as "the SNR at which the mode stops working". Taking the
QAM16C2/mpg point that failed at 2x and simply sending more:

| cycles | overshoot | result |
|---:|---:|---|
| 14 | 2x | FAIL |
| 28 | 4x | OK, 63.6 s |
| 56 | 8x | OK, 63.6 s |
| 112 | 16x | OK, 64.1 s |

Doubling the carousel recovered a point that had "failed". So under fading the
floor is a budget decision, not a hard limit: a fade takes out whole frames, and
more passes simply give the receiver more chances to catch the gaps between
fades.

**Overshoot is insurance, not latency.** 4x, 8x and 16x all delivered at the
same moment — the receiver finishes the instant it holds enough symbols, and
the sender goes on transmitting into the void afterwards. Extra cycles cost
airtime and channel occupancy, not time-to-delivery. Since broadcast has no
feedback and a short carousel delivers *nothing at all* rather than a partial
file, err generously.

### AWGN does have a hard floor

The same trick does **not** work on a clean channel:

| channel | SNR3k | overshoot | result |
|---|---:|---:|---|
| AWGN | +11.2 | 4x | FAIL |
| mpg | +9.2 | 16x | FAIL |
| mpg | +7.2 | 16x | FAIL |

This is the distinction worth carrying away. AWGN is stationary: below the
modem's decode threshold every frame fails equally, so repetition buys nothing
and no overshoot will save it — the answer there is a more robust mode. Fading
is time-varying, so between the 2x cliff and the true floor there is a region
where patience substitutes for SNR. Far enough down, fading runs out of good
periods too and even 16x fails.

So, operationally:

- **Marginal on a fading path** → send more cycles.
- **Marginal on a quiet, weak path** → change mode; cycles will not help.

## Caveats

- **Simulator, not radio.** See the top of this file.
- **n = 1 per row.** No repeats, no error bars. The cliff brackets in
  particular are single runs at a 2 dB step, so read them as "between these two
  values", not as a threshold measured to a decibel.
- **One fading realisation.** `ch` replays a fixed file; runs are reproducible
  but not independent samples.
- **The mpg and mpd fading files are generated locally**, not shipped: only the
  1.0 Hz (`--mpp`) file is in the tree. The 0.1 Hz and 2.0 Hz files used here
  were generated with the octave command that `requireChFading()` prints. Their
  layout matches the shipped file byte-for-byte in length.

## Choosing the cycle count

This is the number an operator gets wrong, and it is worth stating bluntly: a
too-small `-c` delivers **nothing at all**, not a partial file. There is no
feedback, so the sender cannot know it fell short.

The test computes it as

    symbols needed  ~= bundle_bytes / 41 + 2
    frames needed   ~= ceil(symbols / symbols_per_frame)
    cycles          =  2 x frames needed

For the 7680-byte bundle that is 189 symbols: 7 frames on QAM16C2 (29
symbols/frame) but 95 frames on DATAC3 (2 symbols/frame) — a factor of 13
between modes for the same object.

2x is enough with a few dB of margin in hand, and is what the trial harness
defaults to. It is **not** enough close to the cliff: at QAM16C2 under mpg at
+13.2 dB, 2x delivered nothing and 4x delivered in 63.6 s. See "Where it stops
working" — on a fading path, overshoot is the cheapest margin you have.

## Traps this trial hit

Recorded because each one produced confident, wrong results first:

1. **`ch` exits when a fading sample file is missing**, printing octave
   instructions. The bridge then carries no audio, and every mode fails at
   every SNR — indistinguishable from a channel too harsh to decode. An entire
   five-point sweep was read as "fading breaks broadcast" before the cause was
   found. `requireChFading()` now preflights this and skips with the command to
   generate the file.
2. **`bcast_file_tool send` returns in milliseconds.** It enqueues frames into
   Mercury's broadcast socket; the airtime that follows is seconds to minutes.
   A fixed arrival deadline measured from that return reported a transfer still
   in progress as a channel failure.
3. **Forcing one cycle count across modes starves the robust ones.** 28 cycles
   is ample at 29 symbols/frame and supplies 56 of 189 needed symbols at 2.
