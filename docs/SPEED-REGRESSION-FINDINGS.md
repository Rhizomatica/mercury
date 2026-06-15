# Why `pre-2.0` is slower than v1.9.9 on a dummy-load uucp transfer — findings

**Status:** investigation complete; no code change merged. Two reusable test
instruments were added (see *Instruments*). The architecture direction
(Hybrid-ARQ / outer-loop link adaptation) is deferred to a separate decision.

## Question

On the sBitx dummy-load bench (estacao2 ↔ estacao3, uucp file transfer), the
`pre-2.0-improvements` build transfers a 5 KB file **~2× slower than the
`v1.9.9` tag**, despite `pre-2.0` having the *faster* custom modes
(datac15/16/17, qam16c2) that v1.9.9 lacks. Why?

## Method / instruments built

1. **FIFO + `ch` channel harness** (`tests/integration/`): two real `mercury`
   processes bridged through codec2's `ch` simulator at a controlled SNR, with
   real-time pacing that models the half-duplex turnaround. Extended here with
   `MERCURY_CH_FADING` (mpg/mpp/mpd) and `MERCURY_CH_GAIN` env knobs. Runs a
   full ARQ CONNECT + transfer in minutes, deterministically.
2. **Raw-tool acquisition sweep** (`modem/freedv/freedv_data_raw_{tx,rx}` +
   `ch`): per-burst `acquired` vs `decoded` counts vs SNR, AWGN and `--mpp`
   fading. Runs faster than real-time.

These replace the **variable-SNR dummy load**, which is unusable for isolating
a timing/protocol bug: its SNR "varies a lot", so every missed frame (DATA,
ACK, TURN_REQ) looks identical (loss → retry) whether the cause is the channel
or the code. Multiple on-air runs swung 287–415 s on the *same* build.

## What it is NOT

- **Not the speed ladder failing to reach the fast modes.** TMG logs show it
  reaches DATAC17 (`tx_start … mode=DATAC17`) — the data mode is faster than
  v1.9.9's DATAC1 (159 vs 47 B/s raw).
- **Not a turnaround/guard bug.** At a clean fixed SNR the FIFO harness
  transfers cleanly and fast (113 s, 0 timeouts, 0 turn-retries, 3/3 runs).
  Guard retuning (a mode-aware post-ACK guard) and mode-negotiation
  suppression were tried and **reverted** — unverifiable on the variable load
  and not the cause.
- **Not preamble/acquisition.** See the gate below.

## What it IS (measured)

### 1. FEC/decode collapse under fading — the dominant OTA floor

Raw-tool sweep, 100 bursts/point, codec2 `--mpp` fading:

| mode | SNR3k | acquired | decoded |
|---|---|---|---|
| DATAC16 / DATAC15 (robust) | 2–16 dB | 96–99 / 100 | 94–99 / 100 |
| DATAC17 | 9 dB | 100 | 94 |
| **DATAC17** | **5 dB** | **100** | **23** |
| DATAC17 | 1 dB | 93 | 0 |
| QAM16C2 | 11 dB | 83 | 53 |
| QAM16C2 | 5 dB | 47 | 0 |

(AWGN sweep: same shape — every mode *acquires* well below its FEC floor;
DATAC17 acquires to ~−3 dB but its LDPC needs ~+5; QAM16C2 acquires to ~0 dB,
FEC needs ~+13.)

**Acquisition is not the binding constraint.** DATAC17 acquires 100/100 at
5 dB under fading but decodes only 23 — the preamble does its job; the LDPC
can't carry the payload through the fades. Per the (now-retired) preamble
plan's own decision gate — *"if the floor is UW/FEC, stop; it's not a sync
problem"* — **preamble lengthening is not justified.** (Caveat: this measures
*steady-state* acquisition with a warmed rxbuf; the narrow *post-turnaround
warmup* case is not covered and is the one place a longer preamble could still
help.)

### 2. Gear-shift oscillation on the variable channel

`pre-2.0`'s gear-shifting is **inner-loop only**: fixed SNR→mode thresholds
(`select_best_mode`) + hysteresis + clean-ACK step-up + retry-forced-downgrade
+ a 6 s hold. On a variable channel the ladder climbs to DATAC17 on the *mean*
SNR (≥ 8 dB), the wideband frames fail on the *dips*, two retries force a
downgrade to the floor, the mean recovers, it re-climbs — **flapping**, each
flap a MODE_REQ/MODE_ACK round-trip (~12 s). On-air at 30 % power (~10 dB) this
ground for >8 min: `Ladder step-down to 0`, `Mode negotiation: 24→10` for an
8-byte backlog, repeated `tx_end seq=9` retransmits.

### Net

On the variable/low-SNR dummy load, `pre-2.0` reaches DATAC17 (which v1.9.9
doesn't have), DATAC17's FEC fails on the fades, and the inner-loop ladder
oscillates — while **v1.9.9 sits stably on robust DATAC1/3** (no wideband mode
to fail, no flap). That is the ~2× gap.

## Architecture assessment vs state of the art

| | gear-shift / link adaptation | failed-frame handling |
|---|---|---|
| PACTOR-III/IV | fast adaptive levels | **Memory-ARQ: soft-combine failed copies** |
| STANAG 4538 / MIL-STD-188-110C | fast link adaptation | **Hybrid-ARQ / incremental redundancy** |
| LTE/5G | inner-loop CQI→MCS **+ outer-loop (OLLA)** to a target BLER | **HARQ soft-combining** |
| **Mercury (today)** | **inner-loop only** + ad-hoc anti-oscillation patches | **plain go-back-N (failed frames discarded)** |

Mercury's gear-shifting is a reasonable heuristic but not SOTA. Two gaps map
directly onto the two measured root causes:

- **No Hybrid/Memory-ARQ.** Soft-combining failed DATA-frame copies would let
  the LDPC close under fading (several dB) — directly fixes finding #1, and
  reduces the failures that drive the oscillation. Highest-value, larger
  decoder change.
- **No outer-loop link adaptation.** A delivery-driven loop holding a target
  first-try FER would self-damp the gear-shifting — fixes finding #2 properly,
  replacing the hysteresis/hold/retry-downgrade heuristics. Developable as a
  deterministic FSM unit test (`tests/test_arq_fsm`).

## Recommendation (for separate architecture decision)

1. **Hybrid-ARQ soft-combining** — attacks the dominant measured floor
   (FEC-under-fading). Verify with the raw-tool sweep + FIFO harness.
2. **Gear-shift outer-loop damping** — quick path to v1.9.9 parity on variable
   channels; FSM unit test.
3. **Preamble/postamble** — *not* justified by the data, except the narrow
   post-turnaround warmup case.

## Dead-ends (do not repeat)

- Tuning ARQ guard constants (CHANNEL_GUARD / ISS_POST_ACK_GUARD): the
  clean-SNR harness is clean, so guards are not the cause.
- Isolating timing/protocol bugs on the variable-SNR dummy load: confounded;
  use the fixed-SNR harness.
- Preamble lengthening for AWGN/steady fading: acquisition isn't the floor.
