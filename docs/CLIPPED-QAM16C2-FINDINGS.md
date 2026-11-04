# Clipping QAM16C2 for peak-limited transmitters

MODES.md documents QAM16C2 as running unclipped at PAPR ≈ 10 dB, "so on
peak-limited transmitters its effective +15 dB operating point is harder to
reach than the clipped QPSK modes' figures suggest — on the bench this shows
up directly as ~5 dB less average signal power at the same ch gain setting."
This is that statement turned into a measurement (2026-11-01, AWGN and the
Watterson bench): what does QAM16C2 cost a peak-limited radio, and does
enabling the clip machinery every other mode already has buy the loss back?

The question matters because the ladder's top rung is chosen by goodput, and
goodput is computed from delivery the receiver reports — but a peak-limited
transmitter never delivers the bench's average power on this mode in the
first place. The field crossover vs DATAC17 is not the bench crossover.

## Answer

**Yes.** QAM16C2 measures 11.4 dB PAPR against DATAC17's 7.0 — at the same
transmitter peak it radiates 4.4 dB less average power, so its whole delivery
curve sits 4.4 dB to the right of DATAC17's in radio terms. Clipping with
`clip_gain1` 2.0 / `clip_gain2` 1.0 cuts the PAPR to 6.67 dB — *below*
DATAC17's — and the delivery curves move accordingly: the AWGN knee falls
from radio setting +10 to +5, and under ccir-poor fading the top rung
crosses DATAC17's goodput ~3 dB earlier than stock, at roughly half the
radio peak it needs today. The damage side is real but smaller: the clipped
constellation needs ~1 dB more achieved SNR in AWGN, more under fading, and
drive 3.0 destroys the mode (its entire PAPR gain cancels). The optimum is
drive ~2, gain2 1.0 — and the change is TX-side only, so a clipped station
interoperates with a stock one without any wire change.

## Method

New instrument: `utils/qam16_papr_sweep` (mode, trials, SNR window, channel,
gain lists), plus one new freedv setter, `freedv_set_tx_clip_config`, that
applies the four-parameter TX peak config in-chain — through
`ofdm_hilbert_clipper`'s real drive/clip/BPF/restore path, which post-hoc
sample processing cannot reproduce. The preamble and postamble pass the same
live chain at emit time, so clipping shapes them too.

**The peak-limited-radio protocol.** Every variant's burst is normalised to
one common peak; the noise is set by the *radio setting*, not the variant:
each variant's higher or lower average power at that peak lands on its
achieved SNR, which the channel measures and the rows report. The axis is
expressed as the stock variant's SNR3k. Under fading this follows the house
convention (`SNR3k = -No + A`, the burst's level constant A calibrated from
one near-noiseless pass, as `mfsk_channel_sweep.sh` does).

- **Channels:** AWGN, and `ccir-poor` (CCIR 520 Poor: 2 ms, 1 Hz) — the
  Watterson preset matching the "MPP" of MODES.md. chanutil's legacy `mpp`
  Butterworth preset is a different model and was not used.
- **Variants:** stock; the gain grid at 20 trials (below); finalists at 100
  trials: stock, gain1 1.5 / 2.0 / 2.5, all at gain2 1.0. DATAC17 stock runs
  as the reference (mode rate 1410 bps, 1182 B / 6.93 s vs QAM16C2's
  3100 bps, 1215 B / 3.30 s).
- **Seeding:** payloads and noise re-seeded per variant, so row differences
  are the clip config, not the channel. Stock is always row one.
- **Cross-check:** DATAC17 on ccir-poor delivers 92/100 at +8 — their
  published MODES.md MPP figure is 89/100 at +8. DATAC17 in AWGN turns the
  corner at +3/+4 (43/99 of 100), against their "63/100 at +3, 100/100 at
  +5". The harness reproduces the bench it is calibrated against.

## PAPR (raw transmitter output, int16, measured not asserted)

| variant | PAPR dB | | variant | PAPR dB |
|---|---|---|---|---|
| stock | 11.41 | | (2.0, 0.8) | 8.31 |
| (1.5, 0.8) | 9.59 | | **(2.0, 1.0)** | **6.67** |
| (1.5, 1.0) | 8.36 | | (2.5, 0.8) | 7.42 |
| (2.5, 1.0) | 5.78 | | (3.0, 0.8) | 6.82 |
| (3.0, 1.0) | 5.25 | | DATAC17 (reference) | 6.98 |

The (2.0, 1.0) setting lands QAM16C2's PAPR just under DATAC17's: the top
rung becomes *no harder to transmit* than the QPSK rung below it.

## Finals: 100 trials, radio-setting axis (equal peak)

Delivery/100. Acq = full RX_SYNC.

**AWGN**

| radio | stock | (1.5,1.0) | (2.0,1.0) | (2.5,1.0) | DATAC17 |
|---|---|---|---|---|---|
| +4 | 0 | 0 | 0 | 0 | 99 |
| +5 | 0 | 9 | **100** | 8 | 100 |
| +6 | 0 | 100 | 100 | 99 | 100 |
| +7 | 0 | 100 | 100 | 100 | 100 |
| +8 | 60 | 100 | 100 | 100 | 100 |
| +9 | 99 | 100 | 100 | 100 | 100 |
| +10 | 100 | 100 | 100 | 100 | 100 |

**ccir-poor** (rows beyond +11 for stock: 69, 76, 87 at +12/+13/+14)

| radio | stock | (1.5,1.0) | (2.0,1.0) | (2.5,1.0) | DATAC17 |
|---|---|---|---|---|---|
| +5 | 0 | 2 | 6 | 0 | 31 |
| +6 | 0 | 15 | 18 | 1 | 51 |
| +7 | 0 | 26 | 33 | 8 | 80 |
| +8 | 5 | 45 | 50 | 16 | 92 |
| +9 | 16 | 67 | 68 | 25 | 95 |
| +10 | 29 | 78 | 82 | 45 | 97 |
| +11 | 48 | — | — | — | 98 |

Readings:

- **AWGN:** the 90 % knee falls from ≈ +9.3 (stock) to ≈ +5.5 ((2.0,1.0)) —
  ~4 dB of radio setting; the clipped variant's achieved-SNR damage is ~1 dB
  (it decodes fully at achieved +9.7 where stock needs +9).
- **ccir-poor:** at radio +10 the stock mode delivers 29 % and the clipped
  mode 82 %. At matched delivery levels the gain is ~4 dB (90 % point: stock
  ≈ +14.3, clipped ≈ +10.5); at the 50-60 % levels it is 5-6 dB.
- **Acquisition under fading improves too, and not by the decoder:** stock
  acquires only 7-77 % of bursts below +11 (it is acquisition-limited well
  before it is decode-limited); (2.0,1.0) acquires 43 % at radio +2 where
  stock manages 7 %. At the same transmitter peak the clipped waveform
  simply puts more energy into the preamble. Note the peak-limited radio was
  the *point* of the exercise; the unclipped mode's acquisition numbers on
  the centred bench did not show this because they were measured at equal
  average power.
- **Damage grows with drive and with fading.** Under ccir-poor, (2.5,1.0)
  delivers 45 % at radio +10 against (2.0,1.0)'s 82 % despite a further 0.9 dB
  of PAPR: the extra clip drive costs the constellation more than the extra
  average power buys. AWGN punishes this less (99/100 at +6).
- **Drive 3.0 is a cliff.** (3.0,1.0) collapses under ccir-poor — 0-4 of 20
  delivered even at achieved +24 dB (coarse grid, acquisition still 12-18/20:
  syncs fine, cannot decode). In AWGN it needs achieved ≈ +14.2 where stock
  needs ≈ +9. 16QAM through a 3x Hilbert clip is a different modulation.

## Goodput crossover vs DATAC17 (the ladder's decision, computed from these tables)

Rate × delivery at the same radio setting (QAM16C2 3100 bps vs DATAC17
1410 bps), ccir-poor:

| radio | stock QAM16C2 | (2.0,1.0) | DATAC17 | winner |
|---|---|---|---|---|
| +7 | 0 | 1023 | 1128 | DATAC17 (close) |
| +8 | 155 | 1550 | 1297 | **clipped QAM16C2** |
| +9 | 496 | 2108 | 1340 | **clipped QAM16C2** |
| +11 | 1488 | — | 1382 | stock QAM16C2, barely |

The stock mode only overtakes DATAC17 around +10.7; the clipped mode from
≈ +7.5. **Under fading, the top rung becomes the right rung ~3 dB earlier in
radio terms.** In AWGN the effect is larger: the clipped mode wins from +5
where the stock mode needs +9-10.

## SNR estimate stability

The modem SNR estimate on synced trials reads −4.5 ± 0.1 across stock and
every clipped variant on both channels — clipping does not further bias the
estimator the ladder reads. (Its absolute offset, ~10 dB low against true
SNR3k at these points, is the pre-calibration state documented in
OTA-PHASE-A-SNR-CALIB.md and handled by the ladder's per-mode constants; not
re-litigated here.)

## Verdict

1. **TX clipping at `clip_gain1` 2.0 / `clip_gain2` 1.0 is the recommended
   setting** for QAM16C2 on peak-limited radios: PAPR 11.4 → 6.7 dB, AWGN
   knee −4.5 dB of radio setting, fading goodput crossover vs DATAC17 ~3 dB
   earlier, and the same transmitter burden as the rung below.
2. **TX-side only.** The receiver is untouched; a clipped station
   interoperates with a stock one. No wire format, frame size, mode table or
   ladder change is required — which is why this document is findings-only
   and the one-line `ofdm_mode.c` change is deliberately *not* in this
   branch: mode-config changes ride the OTA bench, like everything else that
   transmits.
3. Do not clip harder than drive ≈ 2.0-2.5. (2.5, 1.0) trades fading
   robustness for PAPR and loses; 3.0 is unusable on this mode.

## Caveats

- The finals tables were re-confirmed on the corrected instrument
  (`utils/bench_common.h`, 2026-11-03) on v1.9.22 including the #457 unsync
  fix, during the PR #455 review: the ccir-poor stock and (2.0, 1.0) rows
  reproduced within run noise
  (stock 6/17/30/47/69/86 at +8..+14; (2.0,1.0) 18/30/50/65/81/90 at
  +6..+12) and the tables stand as printed.
- No frequency offset was run; the fading preset is ccir-poor — the Watterson
  preset matching the MPP parameters of MODES.md (2 ms, 1 Hz). Fading is where clip damage shows, so a second regime (e.g. itur-mdv
  NVIS or flutter) is the natural extension; the coarse AWGN/ccir-poor pair
  agrees in direction everywhere they overlap.
- 100 trials per point on the finals, 20 on the coarse grid; the coarse grid
  located the neighbourhood and the finalists confirm it, but the (1.75,
  2.25) fine structure between drives was not resolved.
- Simulation only. What a real transmitter's AGC, audio chain and final do
  to a clipped OFDM waveform is exactly what the OTA bench exists to answer;
  an independent over-the-air check is being arranged alongside this branch.
- The stock PAPR was measured on the bench waveform including preamble and
  postamble; MODES.md's "~10 dB" is consistent with this (11.4 measured, of
  which the analytic-envelope detail differs slightly from how upstream
  quoted it).

## Reproducing

```sh
make -C utils qam16_papr_sweep
# finals, AWGN (stock + the three finalists):
utils/qam16_papr_sweep qam16c2 100 3 11 awgn 1.5,2.0,2.5 1.0 1
# finals, ccir-poor (= MODES.md MPP):
utils/qam16_papr_sweep qam16c2 100 2 14 ccir-poor 1.5,2.0,2.5 1.0 1
# DATAC17 reference:
utils/qam16_papr_sweep datac17 100 1 9 awgn
utils/qam16_papr_sweep datac17 100 3 11 ccir-poor
# coarse grid (all nine variants):
utils/qam16_papr_sweep qam16c2 20 2 18 awgn 1.5,2.0,2.5,3.0 0.8,1.0 2
```
