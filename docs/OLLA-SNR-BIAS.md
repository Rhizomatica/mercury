# The mode ladder is steered by a broken instrument

OLLA picks the payload mode from the peer's reported SNR.  That number comes
from `freedv_get_modem_stats()`, read in `modem.c` with no per-mode correction
and passed to the peer in the ACK header.

It is not a measurement of the channel.  It is a measurement of the channel
*and the mode currently being decoded*, and the second term is larger than the
first.

## Measured

`utils/acquire_vs_decode` now prints the modem's own estimate beside the SNR3k
`chanutil` actually delivered.  MPG, 20 trials a point:

| true SNR3k | DATAC16 says | DATAC17 says | QAM16C2 says |
|---|---|---|---|
| +5.5 | — | — | +11.68 |
| +7.5 | — | — | +12.04 |
| +9.6 | +8.98 | +9.72 | +11.91 |
| +11.3 | +10.46 | +10.71 | — |
| +14.3 | +12.35 | +11.96 | — |

DATAC16 and DATAC17 track the channel: their error moves smoothly from about
−2.3 dB at the top to +0.8 dB lower down, and a change in the link shows up as
a change in the number.

**QAM16C2 does not track it at all.**  Its estimate sits at 11.7–12.0 dB while
the true SNR moves over 4 dB beneath it.  The error is +2.3 dB at +9.6 and
+6.2 dB at +5.5 — not an offset that could be calibrated away with a constant,
but a saturation: the estimate stops carrying information about the channel.

## Why this costs throughput

Two consequences, and both were observed rather than reasoned about.

**A step up the ladder looks like a change in the link.**  On the integration
harness with the channel set to *no noise at all*, the same station reports
17.8 dB while decoding DATAC16 and 12.6–14.3 dB while decoding QAM16C2.
Nothing about the channel changed; the ladder moved.  OLLA sees a 4–5 dB drop
and reacts to it.  This is sufficient on its own to produce the gear-shift
oscillation in `docs/SPEED-REGRESSION-FINDINGS.md`, with no help from fading.

**Ladder thresholds sit inside the estimator's noise.**  Within one mode the
estimate jitters about ±1.1 dB run to run on that same noiseless channel
(16.65–18.85 dB observed on the ISS).  A 1 dB difference in the reported peer
SNR is enough to straddle the QAM16C2 threshold: two builds of Mercury that
differed only in their ACK path landed on opposite rungs, one on QAM16C2
(1213 B in 3.70 s) and one on DATAC17 (1180 B in 7.40 s).  That is 3.6 s on
every data burst, decided by rounding.  It made an unrelated A/B bimodal until
it was traced, and it is the single largest effect seen on that bench.

## What this does NOT say

The bias figures above are from one channel (MPG) at one trial count, and the
harness observations are from a noiseless bench.  They establish that the
estimate is unfit to steer the ladder; they do not yet give the correction.

QAM16C2's saturation in particular cannot be fixed by a constant offset.  The
options worth measuring, in the order they look promising:

1. Do not steer on the peer's SNR while the peer is decoding QAM16C2 — hold the
   last reading from a mode that tracks, and let frame-error rate drive the
   decision instead.  OLLA already has the FER machinery.
2. Per-mode correction for the modes that DO track (DATAC16 −0.4, DATAC17
   varying with SNR), which is a smaller and better-behaved fix.
3. Widen the ladder hysteresis so decisions cannot be flipped by ±1.1 dB of
   estimator jitter.  This treats the symptom, but cheaply.

## Reproducing

```
cd utils && make acquire_vs_decode
./acquire_vs_decode QAM16C2 20 -44 -38 mpg
./acquire_vs_decode DATAC16 20 -44 -38 mpg
```

The `bias` column is `modem_est - SNR3k(meas)`.  `No` counts DOWN from noisy to
clean, so the first row is the noisiest point.
