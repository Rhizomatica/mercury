# Reviving Mercury v1's low-rate LDPC codes — feasibility & robustness study

**Date:** 2026-07-15 · **Question:** would porting Mercury v1's low-rate custom
LDPC codes (rates down to 1/16) into v2 meaningfully improve robustness at the
very low SNR of marginal NVIS HF links — enough to justify the work?

## TL;DR — recommendation: **No** (not as a bare LDPC port)

Three independent technical findings converge, the core one **measured**:

1. **Lower rate buys ~no channel-SNR robustness in v1's codes.** In an isolated
   LDPC A/B (same decoder), v1's rate 1/16, 3/16 and 5/16 codes all hit their
   FER≈0.1 floor at essentially the **same channel SNR** (~−3.3 dB Es/N0 AWGN,
   ~−2.3 dB interleaved Rayleigh). A 5× rate cut (5/16→1/16) buys ≤1 dB and 0 dB
   at the 90 %-delivery point — because v1's short (N=1600) IRA codes get weaker
   as rate drops (avg column degree 3.38→2.77→2.23; degree-2 accumulator
   dominance). Lower rate = same floor, up to 5× less goodput = **strictly worse**.
2. **v2's floor is not the code — it's acquisition.** v2's robust modes
   (DATAC15/16, rate 1/3) are acquisition-limited at ~−7 dB SNR3k MPP
   (≈−9.5 dB AWGN 90 %); `docs/MODES.md`: *"decode failures are near zero once
   acquired."* A stronger/lower-rate LDPC in the same waveform can't extend the
   floor — below the acquisition wall the preamble/UW is never detected, so the
   code never sees a frame.
3. **v1's deep-fringe capability came from MFSK, not the LDPC.** v1's ROBUST_0
   reaches **−13 dB Es/N0** via **32-MFSK non-coherent energy detection**, with
   the rate-1/16 LDPC as the *shared* FEC across all modes. The lever is the
   waveform (non-coherent MFSK works below OFDM's coherent acquisition
   threshold), not the code.

## Background

| | Mercury v1 (`mercuryv1` branch) | Mercury v2 (current) |
|---|---|---|
| Language / modem | C++, custom OFDM + MFSK | C, codec2/FreeDV OFDM |
| LDPC | hand-rolled IRA, N=1600, rates **1/16…14/16** | codec2 HRA/H codes, floor **~1/3** (DATAC15) |
| Storage | quasi-cyclic adjacency (`QCmatrixC/V`) | sparse `H_rows`/`H_cols` (IRA/accumulator) |

v2 has no code below ~rate 1/3; v1 goes to 1/16. The v2 README states an intent
to "introduce other modulator modes present in Mercury v1" — this study asks
whether the *LDPC codes* are the part worth reviving.

## Method (isolated LDPC A/B)

Throwaway analysis tooling (not committed to the modem): a self-contained numpy
harness (`utils` scratch) builds each code's **full parity-check matrix H** from
v1's `QCmatrixC` adjacency (verified against `QCmatrixV` as its transpose), then
runs the **all-zero-codeword** method — valid for any linear code on a symmetric
channel — over BPSK with normalized min-sum belief-propagation (α=0.75, 50 iter),
counting FER vs **Es/N0** (channel SNR, the axis that matters for HF; Eb/N0
normalizes out the rate and hides the point). The **same decoder** is used for
every code, so the dB *delta* between rates is the trustworthy output.

**Decoder validated** against a known reference — a regular (3,6) rate-1/2 LDPC,
N=2000 — whose FER waterfall lands at +1.5…+2.0 dB Eb/N0, exactly where theory
predicts. (Absolute Es/N0 here is isolated-LDPC min-sum and is **not** directly
comparable to v2's OFDM `SNR3k` figures — different measurement bases; the valid
conclusions are the *relative* ones.)

## Findings — data

**AWGN, FER vs Es/N0 (dB), 500 frames, same decoder:**

| code | rate | col-deg | −1 | −2 | −3 | −4 | −5 | −6 |
|---|---|---|---|---|---|---|---|---|
| 1_16 | 0.0625 | 2.23 | 0.00 | 0.01 | 0.06 | 0.22 | 0.62 | 0.89 |
| 3_16 | 0.1875 | 2.77 | 0.00 | 0.01 | 0.07 | 0.30 | 0.70 | 0.97 |
| 5_16 | 0.3125 | 3.38 | 0.00 | 0.01 | 0.07 | 0.53 | 1.00 | 1.00 |

**Interleaved Rayleigh (ideal, known CSI), 400 frames:**

| code | rate | +2 | 0 | −2 | −4 | −6 |
|---|---|---|---|---|---|---|
| 1_16 | 0.0625 | 0.00 | 0.00 | 0.05 | 0.35 | 0.94 |
| 3_16 | 0.1875 | 0.00 | 0.00 | 0.08 | 0.56 | 1.00 |
| 5_16 | 0.3125 | 0.00 | 0.00 | 0.07 | 1.00 | 1.00 |

The FER≈0.1 floors cluster within ~0.3 dB across a 5× rate span. Lower rate helps
only at the waterfall midpoint (≤1 dB AWGN, ~1–2 dB fading) and not at the
operating point, for a large goodput penalty.

## Findings — v2 floor (from repo docs, not re-measured)

- `docs/MODES.md`: DATAC15/16 (rate 1/3) are **acquisition-limited**; ~−7 dB
  SNR3k MPP, ≈−9.5 dB AWGN 90 %; *"decode failures near zero once acquired."*
  Going rate 1/2→1/3 already bought the ~2 dB payload-floor extension.
- `docs/SPEED-REGRESSION-FINDINGS.md`: for the fast modes the floor is
  **FEC-collapse under fading**, and *"acquisition is not the binding
  constraint"*; NVIS is **ISI-limited, not SNR-limited** (`docs/FADE-CLIFF-DECISION.md`).

None of these floors is set by the LDPC code rate.

## What would actually help the fringe (the constructive part)

The measured/​documented bottleneck is the **waveform**, not the FEC:

1. **Non-coherent weak-signal modulation** — this is what gave v1 its −13 dB
   reach (32-MFSK), *sharing the same LDPC*. v2 already ships codec2's
   **`FREEDV_MODE_FSK_LDPC` (mode 9)** — a non-coherent FSK+LDPC mode already in
   the mode list. Bringing an FSK/MFSK weak-signal mode into the ARQ ladder —
   either codec2's, or v1's proven 32-MFSK — is the real
   "revive a v1 mode" opportunity.
2. **More robust preamble/acquisition** for the long-frame robust modes, to
   unlock the SNR range below ~−7 dB where the current OFDM preamble fails.
3. **HARQ soft-combining** — already landed (`docs/HARQ-FINDINGS.md`); attacks the
   FEC-under-fading floor directly (3–5× at the fringe).
4. If lower LDPC rates are ever justified *after* acquisition is fixed, generate
   fresh codec2 codes (`ldpc_gen_c_h_file.m`) with a **longer block than N=1600**
   so the rate reduction actually yields coding gain — rather than reusing v1's
   short N=1600 matrices, whose weakness is what this study measured.

## Caveats

- Isolated LDPC min-sum (not sum-product) → absolute Es/N0 is ~0.3–0.5 dB
  pessimistic, applied equally to all codes; the relative comparison is robust.
- All-zero-codeword + BPSK isolates the *code*; real OFDM adds pilots/preamble
  overhead and a different (`SNR3k`) noise basis — hence no cross-basis numeric
  comparison to DATAC15's −9.5 dB is drawn.
- Fading test is *ideal interleaved* Rayleigh with known CSI (best case for
  coding gain); real block/ISI fading would favour lower rate no more than this.

## Reproduce

v1 matrices are read via `git show mercuryv1:source/physical_layer/…`;
the scratch harness (`sim.py`, `selftest.py`, `run_all.py`, `run_fade.py`) builds
H from `QCmatrixC`, validates the decoder on a regular (3,6) code, and sweeps
FER vs Es/N0 for the 1/16, 3/16, 5/16 codes on AWGN and interleaved Rayleigh.
