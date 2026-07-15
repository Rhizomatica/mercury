# Extending the low-SNR floor: OFDM acquisition vs. an FSK weak-signal mode

**Date:** 2026-07-15 · **Question:** can we push the robust-mode operating floor
below ~−7 dB — via (A) more robust OFDM preamble/acquisition, or (B) a
non-coherent FSK weak-signal mode? Follow-on to docs/LDPC-V1-VS-V2-FEASIBILITY.md
(which showed the floor is not the LDPC code).

## TL;DR — recommendation
- **Do NOT invest in OFDM acquisition tuning (Track A).** Measured: on the
  realistic fading channel the dominant fringe loss is **decode**, not
  acquisition; and the acquisition **threshold is not a usable lever** (0.45 is
  already optimal — lowering it floods false alarms for zero decode gain).
- **Do pursue an FSK weak-signal bottom rung (Track B).** codec2's in-tree
  `FREEDV_MODE_FSK_LDPC` — even as plain 2-FSK — is **measurably more robust
  than DATAC15** at the fringe (~2× delivery, ~2 dB lower floor) at ~half the
  goodput. This is the real lever; v1's 32-MFSK (Rhizomatica-owned) would extend
  it further and is the natural follow-up.
- **Keep leaning on HARQ** (already landed) for the decode-under-fading loss.

## Method
Deterministic, isolated: `freedv_data_raw_tx --testframes 100 --bursts 100`
→ `ch --No <n> [--mpp]` → `freedv_data_raw_rx --testframes`. SNR3k = −No − 14.82
(calibrated). Acquisition counted separately from decode via a throwaway sync
0→1 transition counter (reverted after measuring); false alarms measured by
feeding pure noise (no signal). Runs are on this dev host; absolute SNR is
codec2-sim, the deltas are what matter.

## Track A — OFDM acquisition (DATAC15)

**A1 — acquire vs. decode split** (delivered/100):

| channel | SNR3k | acquired | decoded | acquired-but-not-decoded |
|---|---|---|---|---|
| AWGN | −8.8 | 100 | 98 | 2 |
| AWGN | −10.8 | 84 | 74 | 10 |
| AWGN | −12.8 | 30 | 2 | 28 |
| **MPP** | −6.8 | 97 | **63** | **34** |
| **MPP** | −8.8 | 85 | 47 | 38 |
| **MPP** | −10.8 | 68 | 22 | 46 |

On the realistic **MPP fading** channel the dominant fringe loss is **decode**
(97% acquire but only 63% decode at −6.8 dB). Acquisition becomes the bigger
loss only on **AWGN below ~−11 dB** — not the operating channel. So the ceiling
for any acquisition improvement is just the acquire-fail minority.

**A2 — `timing_mx_thresh` sweep** (DATAC15, currently 0.45). Lowering it does
NOT help and adds false alarms:

| TMX | decoded @ −10.8 MPP | false acq. on pure noise |
|---|---|---|
| 0.45 | 22 | **0** |
| 0.35 | 17 | 2 |
| 0.30 | 17 | 27 |
| 0.25 | 16 | 255 |

Acq-count exploded (68→417 spurious syncs/100 bursts as TMX 0.45→0.25) while
decoded frames *fell* slightly. The fringe acquisition limit is
signal-correlation strength, not the gate — 0.45 is correct (zero false alarms).

**A3 — longer/boosted preamble: not pursued.** Given A1 (decode dominates on
fading) and A2 (threshold isn't the lever), a longer preamble would recover only
the acquire-fail minority, at an airtime cost — low value versus Track B. Deferred
unless A1's AWGN-deep-fringe case ever becomes the operating point.

## Track B — FSK weak-signal mode (the lever)

codec2 `FREEDV_MODE_FSK_LDPC` (default **2-FSK**, Rs=100 Hz, 200 Hz shift, 30
payload bytes/frame, already in the mode list) vs DATAC15, MPP fading,
delivered %:

| SNR3k | FSK_LDPC (2-FSK) | DATAC15 (rate 1/3 OFDM) |
|---|---|---|
| −6.8 | **90 %** | 63 % |
| −8.8 | **66 %** | 47 % |
| −10.8 | **40 %** | 22 % |
| −12.8 | **10 %** | 1 % |
| −14.8 | 0 % | 0 % |

Non-coherent FSK delivers ~2× the frames of DATAC15 at the fringe and its floor
extends ~2 dB lower, at roughly half the goodput (~34 vs 68 bps) — the right
robustness/throughput trade for a **deep-fringe bottom rung**. This is only
2-FSK; v1's 32-MFSK (−13 dB Es/N0 claimed) would go further.

## Recommendation & next steps
1. **Add `FSK_LDPC` as the bottom rung of the ARQ payload/control ladder**
   (`datalink_arq` OLLA gear-shift), below DATAC15/16 — engaged only at the
   deep fringe. Integration cost: mode-pool entry, per-direction selection,
   non-coherent turnaround (multiple timing trials — v1 used 5). Gate on Pedro
   OTA before landing.
2. **Keep HARQ** as the primary answer to decode-under-fading (the measured
   dominant fringe loss).
3. **Later:** evaluate porting v1's 32-MFSK for maximum reach if 2-FSK proves
   insufficient in the field.
4. **Drop** OFDM acquisition tuning — measured not to be the constraint.

## Caveats
- codec2-sim absolute SNR; MPP is codec2's fading preset (not full Watterson) —
  deltas between modes on the same channel are the trustworthy output.
- FSK_LDPC goodput/turnaround in the live ARQ ladder (vs isolated raw-tool
  bursts) needs a loopsim/OTA check before committing to the integration.
