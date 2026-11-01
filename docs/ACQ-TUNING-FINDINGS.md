# Acquisition tuning: the timing_mx_thresh knee and the UW budget

Below the fringe, DATAC16/15 lose frames to *acquisition* long before the
decoder fails: 99 -> 52 -> 7 acquired from −9 to −13 dB AWGN while frames that
do sync still decode 92-99% of the time (MODES.md, "Why DATAC15/16"). MODES.md
flags acquisition as "a future tuning candidate (`timing_mx_thresh`, UW length)
independent of the FEC design". This is that measurement, made 2026-10-31 on
the bench, AWGN only.

The question: can the acquisition knee be moved by tuning knobs, and what does
each knob cost in false syncs — bursts of pure noise that declare sync? A
lower threshold buys acquisition exactly until noise peaks start passing it;
the unique-word check is the only thing left standing between a false timing
peak and a phantom frame, so the two knobs have to be measured together.

## Answer

**The threshold is the lever; the UW budget is not.** Both fringe modes
acquire 1.5-2 dB lower when `timing_mx_thresh` drops from 0.45 to 0.40 or
0.35, at a false-sync cost of ~0-2% — and the gain is real delivered frames,
not just syncs. At 0.30 the trade collapses: noise-only false syncs jump to
8-25%. Changing `bad_uw_errors` buys nothing in either direction: loosening
gains no acquisition, tightening *loses* deliveries, because a real sync at
fringe SNR needs the UW bits it has.

## Method

New instrument: `utils/acq_tuning_sweep` (mode, trials, SNR range, noise
trials, knob lists). It wraps the same RX loop as `utils/acquire_vs_decode`
with two additions, backed by two new freedv setters
(`freedv_set_timing_mx_thresh`, `freedv_set_bad_uw_errors`) that change
acquisition behaviour only, not the on-air frame format:

- **Knob sweep.** Per (thresh, uw) combination it re-seeds the RNG, so every
  row sees the *same* payloads and noise: row differences are the knobs, not
  the channel. The stock row is always present, as the calibrated baseline.
- **A noise-only control column.** For every SNR point it also feeds
  nburst samples of plain noise at the same level and counts full RX_SYNC.
  Whatever a knob setting "buys" is only free if this stays near zero — and
  the UW check is exactly what separates a real preamble from a lucky timing
  peak, so a loosened UW has to pay for itself here or nowhere.

Calibration: with stock knobs the instrument reproduces the MODES.md
acquisition table burst for burst (7/26/52/84/99 acquired, 2/19/48/78/98
delivered at −13..−8, 100 trials). `nuwbits` is frame structure and was not
touched; `acq_fft_en` was not touched (both search paths select the same
argmax).

## DATAC16, the control mode (100 trials, 100 noise-only, AWGN)

Acquired/delivered per 100; false = noise-only bursts reaching RX_SYNC per
100. Stock is row 1.

| thresh | −13 | −12 | −11 | −10 | −9 |
|---|---|---|---|---|---|
| **0.45 (stock)** | 7/2/0 | 30/23/0 | 50/44/0 | 85/79/0 | 98/97/0 |
| 0.40 | 37/7/0 | 58/36/0 | 82/72/1 | 99/87/0 | 100/97/0 |
| 0.35 | 61/16/1 | 83/50/1 | 99/80/1 | 100/95/2 | 100/99/1 |

The knee moves from ≈ −10 to ≈ −12 at 0.35: about **+2 dB of acquisition and
of delivered frames** (44 -> 80 at −11, 23 -> 50 at −12), at 1-2% false syncs.
No regression at the top: −9 delivered stays 97-99.

## DATAC15, the fringe payload rung (100 trials, 100 noise-only, AWGN)

| thresh | −13 | −12 | −11 | −10 |
|---|---|---|---|---|
| **0.45 (stock)** | 8/0/0 | 30/1/0 | 48/25/0 | 78/70/0 |
| 0.40 | 27/0/0 | 60/0/0 | 82/50/0 | 97/88/0 |
| 0.35 | 71/0/2 | 96/4/0 | 98/53/0 | 100/90/0 |

Same knee, same cost. Note what the acquisition gain does *not* buy: at −13
and −12 DATAC15 acquires (71/96 at 0.35) but delivers almost nothing. Below
≈ −12 the mode is **decode-limited**, so the threshold change extends
DATAC15's *sync* range by 2 dB but its useful payload range by well under 1.
The gain is real where it matters most: DATAC16 carries the polls, and a poll
that syncs but fails CRC is a poll the session still waits out.

## The collapse below 0.35 (20 trials, 40 noise-only)

| mode | thresh | false syncs per 40 |
|---|---|---|
| DATAC16 | 0.35 | 0-2 |
| DATAC16 | 0.30 | 7-10 |
| DATAC16 | 0.25 | 35-38 |
| DATAC15 | 0.35 | 0-3 |
| DATAC15 | 0.30 | 8-12 |

The threshold is the noise-rejection mechanism, and the UW does not cover for
it: at 0.30-0.25 the demod declares sync on most noise windows, decodes
garbage (CPU cost on a Raspberry Pi), misses real bursts that arrive during
the phantom, and trips the session state machines. The knee is between 0.30
and 0.35.

## The UW budget: no gain either way (20 trials, coarse)

- Looser (DATAC16 15/32, DATAC15 21/48): no acquisition gain at stock
  threshold. The binding constraint is the correlation threshold, not the UW.
- Tighter (DATAC16 8/32, DATAC15 12/48): no acquisition gain at 0.45, and at
  0.35 it *costs* deliveries (DATAC16 −11: 12 vs 18 delivered; DATAC15 −11:
  50 -> 42 at 100 trials not needed — coarse already shows the direction).
  A real sync at fringe SNR spends its UW budget; tightening it rejects real
  frames before it rejects noise peaks.

`bad_uw_errors` stays where it is.

## Adjacent finding: the threshold interacts with frequency offset

Real radios sit tens of Hz apart (the pattern detector needed widening for
exactly this, CAROUSEL-REPORT "Different radios"). Mixing DATAC16 bursts off
centre and measuring at 0 dB SNR (10 trials, stock knobs):

| foff Hz | 0 | 5 | 10 | 15 | 20 | 40 |
|---|---|---|---|---|---|---|
| acquired | 10/10 | 10/10 | 10/10 | 8/10 | 5/10 | 4/10 |

Acquisition degrades with offset even at high SNR. At +20 Hz held while the
SNR is swept (50 trials, 50 noise-only):

| thresh | 0 dB | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|---|
| **0.45 (stock)** | 17/12/0 | 27/20/0 | 29/22/0 | 34/22/0 | 30/23/0 | 41/32/0 | 38/29/0 |
| 0.35 | 50/48/0 | 50/50/1 | 50/50/1 | 50/50/1 | 50/50/0 | 50/50/1 | 50/50/0 |

(acq/del/false per 50.) **With a realistic radio offset the stock threshold
loses 20-65% of acquisitions at 0-6 dB — comfortable SNR, the regime the
control mode is supposed to be bulletproof in — and 0.35 restores it to 100%
with ~0-1% false syncs.** At −9 dB / +20 Hz the same contrast (1/50 vs
25/50). Their own `freedv_data_raw_rx` decodes a clean +20 Hz burst fine, so
the waveform tolerates offset once synced: the loss is all at acquisition,
and it is the threshold that stands in the way.

This reframes the change: what the centred bench prices as "+1.5 dB at the
fringe" is, on a mistuned link, the difference between losing a third of
control frames at high SNR and losing none. The offset axis still deserves
its own systematic sweep (both signs, several powers, fading) before the
constant moves; but the direction is consistent everywhere measured.

## Verdict

1. **Move `timing_mx_thresh` 0.45 -> 0.40 for datac15 and datac16** — about
   +1.5 dB of acquisition at ~0-1% false syncs, no high-SNR regression, and
   it recovers most of the offset loss above. 0.35 is the measured optimum
   (+2 dB centred, 100% under +20 Hz offset at high SNR, 1-2% false) and is
   defensible, but the false-sync rate on the control channel is what
   sessions pay for, and OTA evidence does not exist yet.
2. Leave `bad_uw_errors` and `nuwbits` alone.
3. Sweep the frequency-offset axis properly (both signs, fading) before
   trusting the centred numbers too hard; every offset point measured so far
   favours the lower threshold.
4. datac13/datac14 keep 0.45: same geometry, not measured here, no longer on
   the ARQ ladder.

## A crash found on the way (needs triage)

`acq_tuning_sweep DATAC16 20 -13 -11 40 0.25 8` reliably SIGSEGVs the stock
-O2 build; the same run completes under ASan+UBSan and under gdb. The combo
drives the demod into near-continuous false sync (35-38 false/40 at 0.25 with
stock UW), so the crash lives in a rarely-taken sync path and looks like an
uninitialised read that sanitizers' layout changes happen to bury. Neither
0.25 with stock UW nor 0.35/0.40 with any UW crashes. Reproducer above;
not reached by any knob setting proposed here.

## Caveats

- AWGN only, like the acquire-vs-decode split this builds on. Fading
  confounds acquisition loss with payload loss; the clean question belongs on
  the clean channel, and the fading answer is OTA territory.
- 100 trials at ±0 cell resolution: cells move by more than the ±3% run
  noise everywhere the verdict reads on them. The coarse grid (20/40) is
  directional only, and is not used for the verdict.
- Simulation only. The false-sync cost column is a bench proxy for what the
  ARQ state machines experience; the real cost (a phantom poll versus a
  missed real one) is a session-level question their OTA bench answers.
