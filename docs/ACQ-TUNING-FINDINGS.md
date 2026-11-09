# Acquisition tuning: the timing_mx_thresh knee and the UW budget

Below the fringe, DATAC16/15 lose frames to *acquisition* long before the
decoder fails: 99 -> 52 -> 7 acquired from −9 to −13 dB AWGN while frames that
do sync still decode 92-99% of the time (MODES.md, "Why DATAC15/16"). MODES.md
flags acquisition as "a future tuning candidate (`timing_mx_thresh`, UW length)
independent of the FEC design". This is that measurement.

First made 2026-10-31, **re-measured 2026-11-03** on v1.9.22 after the PR #455
review: the sweep's tail noise was receiver-dependent (rows drifted onto
different channels), the UW rows were seeded differently, the noise-only
control saw half the noise window, and the frequency offset was applied as
DSB mixing rather than a shift. All four are fixed (`utils/bench_common.h`),
and the tables below come from the fixed tool on the branch that carries the
#457 unsync fix — which the review showed was also biasing the aggressive
thresholds' false-sync counts, in the direction that makes them look better
than they are. AWGN only, as before: fading confounds acquisition loss with
payload fade loss, and the clean question belongs on the clean channel.

## Answer

**The threshold is the lever; the UW budget is not; the frequency offset is a
separate problem the threshold does not touch.** DATAC16 acquires ~1.5-2 dB
lower at `timing_mx_thresh` 0.40, and ~2-2.5 dB lower at 0.35, at a false-sync
cost of ~0% and 2-6% respectively — and the gain is real delivered frames.
At 0.30 the trade collapses: 30-40% of noise-only windows declare sync. The
UW budget buys nothing loose and costs real frames tight — with the pairing
fixed, the acquisition counts are *identical* across UW budgets (the proof
the pairs are now honest), while delivery degrades monotonically as the
budget tightens. The frequency offset is a separate problem, and the
sentence this document used to end with — that ±20 Hz destroys acquisition
at every threshold — was an instrument bug (see the correction below, and
[OFFSET-ACQUISITION-FINDINGS.md](OFFSET-ACQUISITION-FINDINGS.md)): a ±20 Hz
offset is nearly free, and what bounds the offset is the acquisition
search's ±55 Hz capture window.

## Method

New instrument: `utils/acq_tuning_sweep` (mode, trials, SNR range, noise
trials, knob lists), built on `utils/bench_common.h` with two new freedv
setters and two getters (`freedv_set/get_timing_mx_thresh`,
`freedv_set/get_bad_uw_errors`) that change acquisition behaviour only, not
the on-air frame format. The getters report the live values, so the tool
prints the stock knobs it actually measured with instead of restating
`ofdm_mode.c`.

- **Knob sweep with real pairing.** Payloads and in-burst noise come from a
  fixed-count stream; the tail after each burst is a pre-generated sequence
  with a fixed draw count per trial (drawing on the fly made the count depend
  on the receiver's `nin` sequence, which depends on the knob — the flaw the
  review found). Every knob combination is seeded identically: rows see the
  same payloads and noise, so row differences are the knobs. The stock row is
  always present, as the calibrated baseline.
- **A noise-only control column, over the same window.** For every SNR point
  the control feeds pure noise for burst + tail — the same window a signal
  trial occupies, clamped — and counts full RX_SYNC. Whatever a knob buys is
  only free if this stays near zero.
- **Calibration.** At stock settings the instrument reproduces the MODES.md
  acquisition table (7/26/52/84/99 acquired at −13..−8 on the pre-#457 code;
  on v1.9.22 the stock row measures 11/19/52/81/99 at −13..−9 — the
  unsync fix moved the low end slightly, the knee is unchanged).
- `nuwbits` is frame structure and was not touched; `acq_fft_en` was not
  touched (both search paths select the same argmax).

## DATAC16, the control mode (100 trials, 100 noise-only, AWGN, v1.9.22)

Acquired/delivered per 100; false = noise-only bursts reaching RX_SYNC per
100. Stock is row 1.

| thresh | −13 | −12 | −11 | −10 | −9 |
|---|---|---|---|---|---|
| **0.45 (stock)** | 11/3/0 | 19/16/0 | 52/46/0 | 81/78/0 | 99/95/0 |
| 0.40 | 32/9/0 | 51/34/0 | 79/67/0 | 98/91/0 | 100/95/0 |
| 0.35 | 77/20/6 | 92/45/3 | 99/77/2 | 100/95/3 | 100/97/3 |

The knee moves from ≈ −10.5 to ≈ −12.5 at 0.35: about **+2 dB of acquisition
and of delivered frames** (46 -> 77 at −11, 16 -> 45 at −12). 0.40 delivers
most of the acquisition gain (51 vs 19 at −12) at a false-sync rate of zero.
No regression at the top: −9 delivered 95-97 everywhere.

## DATAC15, the fringe payload rung (100 trials, 100 noise-only, AWGN, v1.9.22)

Re-measured on this mode before its half of the constant moves; same window
as DATAC16, stock UW 18/48 read from the modem. Calibration: the stock row
delivers 92% at −9 against the published AWGN 90% floor of ≈ −9.5 dB.

| thresh | −13 | −12 | −11 | −10 | −9 |
|---|---|---|---|---|---|
| **0.45 (stock)** | 5/0/0 | 29/0/0 | 59/29/0 | 91/82/0 | 95/92/0 |
| 0.40 | 25/0/0 | 62/2/0 | 85/43/0 | 99/85/0 | 100/97/1 |
| 0.35 | 63/0/5 | 94/5/1 | 98/45/5 | 100/87/2 | 100/96/4 |

The 0.40 verdict carries over: +1.5-2 dB of acquisition (29 -> 62 at −12,
59 -> 85 at −11) at ~0 false syncs, and delivered frames up where the mode
is not decode-limited (+14 points at −11, +3-5 at −10/−9). Two honest
nuances. First, DATAC15 is decode-limited below ≈ −11.5 (94 acquire at −12
with 0.35, 5 deliver), so its acquisition gain converts to fewer delivered
frames than DATAC16's — but the rung above the floor starts moving ~2 dB
earlier, which is what the ladder probes. Second, the collapse check on
this mode mirrors DATAC16 exactly (40/60 trials: 0.35 false 1-3/60, 0.30
false 19-25/60), so the knee is a property of the shared search geometry,
not of one mode's constant.

## The collapse below 0.35 (40 trials, 60 noise-only, v1.9.22)

| mode | thresh | false syncs per 60 |
|---|---|---|
| DATAC16 | 0.35 | 1-3 |
| DATAC16 | 0.30 | 18-24 |
| DATAC16 | 0.25 | 58-60 |

At 0.25 virtually every noise window declares sync. The knee is between 0.30
and 0.35, and it is sharper than the first (buggy) measurement suggested: the
#457 unsync fix stopped the receiver from wandering through stale audio,
which had been *suppressing* false syncs at aggressive thresholds — the
corrected numbers make 0.30 look worse than the first tables did, not better.
The threshold is the noise-rejection mechanism, and the UW does not cover for
it.

## The UW budget: buys nothing loose, costs real frames tight (100 trials)

| uw | −13 | −12 | −11 | −10 | −9 |
|---|---|---|---|---|---|
| 15/32 | 11/3 | 19/17 | 52/46 | 81/79 | 99/95 |
| **12/32 (stock)** | **11/3** | **19/16** | **52/46** | **81/78** | **99/95** |
| 8/32 | 11/1 | 19/13 | 52/39 | 81/71 | 99/93 |
| 6/32 | 11/1 | 19/6 | 52/23 | 81/59 | 99/83 |

With the pairing fixed, the acquisition column is *identical* across all four
budgets — the timing decision happens before the UW is checked, and the
identical counts are the proof the rows really are the same bursts. Delivery
falls monotonically as the budget tightens: −12 dB delivers 17/16 frames at
15/12 and only 6 at 6/32. A real sync at fringe SNR spends its UW bits.
`bad_uw_errors` stays where it is.

## Frequency offset: retracted, and re-measured as a negative result

> **CORRECTED 2026-11-04.** This section was wrong, and so was the sentence
> in the Answer above it. The offset rows below were measured with a
> defective instrument: `bench_freq_shift_real` produced a **half-amplitude**
> copy of the burst (-6.02 dB), so every offset row was 6 dB below its
> labelled SNR. Re-measured with the fixed instrument, **a ±20 Hz offset
> costs DATAC16 almost nothing**: 50/50 acquired and 50/50 delivered at
> -9 dB, the same as no offset. The conclusion "a ±20 Hz mistune destroys
> acquisition at every threshold" does not hold and is withdrawn. The
> offset question is answered properly in
> [OFFSET-ACQUISITION-FINDINGS.md](OFFSET-ACQUISITION-FINDINGS.md): the
> acquisition captures ±55 Hz, that window is the search's geometry rather
> than the threshold's, and the instrument now has a self-test
> (`utils/shift_selftest.c`). The rows below are kept as the record of what
> was measured and why it was wrong.

The first version of this document claimed the lower threshold recovers most
of the acquisition lost to ±20 Hz of offset, on the strength of a sweep that
mixed the real burst with cos(2πft) — which does not shift a signal; it makes
two half-amplitude images at fc±f that beat each other. Re-measured with a
true analytic shift (`bench_freq_shift_real`), both signs, 50 trials:

| thresh | −12 | −11 | −10 | −9 |
|---|---|---|---|---|
| 0.45 | 0/50 | 0/50 | 0/50 | 0-1/50 |
| 0.40 | 0-1/50 | 1/50 | 1/50 | 2-3/50 |
| 0.35 | 3-5/50 | 4-6/50 | 7-12/50 | 9-11/50 |

acquired per 50; delivered ≈ 0 everywhere. A ±20 Hz mistune destroys
DATAC16 acquisition at fringe SNR at *every* threshold measured, both signs,
and no knob setting repairs it. That problem lives in the acquisition search
itself (preamble length, fine-stage range), not in the acceptance threshold —
a separate question, recorded here so it is not re-derived from the artefact.
(It is consistent with the OTA record: the pattern detector needed widening
for exactly this class of radio-to-radio spread.)

## Verdict

1. **Move `timing_mx_thresh` 0.45 -> 0.40 for datac15 and datac16** — about
   +1.5-2 dB of acquisition and delivered frames at ~0% noise-only false
   syncs, no high-SNR regression, measured on v1.9.22 with the corrected
   instrument and the #457 fix in place. 0.35 buys ~0.5 dB more acquisition
   for a 2-6% false-sync rate on the control channel; the OTA bench can
   price that trade, but the default should not carry it.
2. Leave `bad_uw_errors` and `nuwbits` alone — loose is a no-op, tight
   costs deliveries.
3. Frequency-offset acquisition is a separate problem the threshold does not
   touch. (Corrected 2026-11-04: the investigation has been done, and the
   threshold was never the issue — the search captures ±55 Hz, and ±20 Hz
   costs almost nothing. See OFFSET-ACQUISITION-FINDINGS.md.)
4. datac13/datac14 keep 0.45: same geometry, not measured here, no longer on
   the ARQ ladder.
5. **Idle CPU measured on x86** (`utils/idle_cpu_probe`, 600 s of noise per
   setting, DATAC16): idle listen cost is flat ~3% of a core at every
   threshold — 0.45: 3.1%, 0.40: 3.4%, 0.35: 2.6% — because 0.40 and stock
   record **zero false syncs in 600 s of audio** (0.35: 6/h, 0.30: 276/h,
   0.25: 2142/h, each sync a wasted decode attempt). The setting proposed
   here costs nothing measurable while listening; the decode storm belongs
   to the thresholds the collapse table already rejects. The same command
   run on the Pi 4 bench is the number that matters; per audio-second
   reporting makes the two directly comparable.

## Caveats

- AWGN only. Fading confounds acquisition loss with payload loss; the fading
  answer is OTA territory.
- **The offset rows in this document were superseded on 2026-11-04** by a
  corrected instrument; see the correction notice in the offset section and
  OFFSET-ACQUISITION-FINDINGS.md.
- 100 trials on the finals; the collapse grid (40/60) is directional. Cell
  sizes move by more than run noise everywhere the verdict reads on them.
- Simulation only, and the false-sync cost column is a bench proxy for what
  the ARQ state machines experience; the session-level cost is a question
  their OTA bench answers.

## Reproducing

```sh
make -C utils acq_tuning_sweep
# finals:
utils/acq_tuning_sweep DATAC16 100 -13 -9 100 0.45,0.40,0.35 12
# UW budgets (note the identical acquisition columns -- the pairing check):
utils/acq_tuning_sweep DATAC16 100 -13 -9 100 0.45 15,8,6
# the collapse:
utils/acq_tuning_sweep DATAC16 40 -13 -10 60 0.45,0.35,0.30,0.25 12
# offset, both signs (analytic shift):
utils/acq_tuning_sweep DATAC16 50 -12 -9 50 0.45,0.40,0.35 12 20
utils/acq_tuning_sweep DATAC16 50 -12 -9 50 0.45,0.40,0.35 12 -20
```
