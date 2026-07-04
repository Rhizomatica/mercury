# S1 fade-cliff fix (`fix-s1-fadecliff`) — merge-decision simulation report

**Date:** 2026-07-04/05 · **Branch under decision:** `fix-s1-fadecliff` @ `a014b3c`
(vs trunk `mercuryv2` @ `e7a2d84`) · **Question:** does the branch have real
advantages over trunk, and does it regress anything?

## What the branch changes

Under a fade below the active payload mode's cliff, trunk transfers stall
instead of downgrading to the DATAC15 floor (S1 — proven by an in-tree
executable test that was shipped `TEST_IGNORE`d).  The branch:

1. bounds the asymmetric-link reverse-loss hold (`ARQ_REVERSE_HOLD_MAX` = 3
   consecutive holds; a clean delivery re-arms it);
2. advances `consecutive_retries` per ACK timeout (the only failure evidence
   a fade produces — no ACK ever arrives to run the normal accounting);
3. allows a **downgrade-only** mode probe with an unACKed window: the
   MODE_ACK now carries the peer's `rx_expected` (`ARQ_FLAG_CTRL_ACKSEQ`),
   which resolves the window — confirmed-undelivered bytes are restaged and
   re-framed at the new mode;
4. `encode_snr` rounds negative SNRs correctly (`floorf`);
5. never fires a probe after the no-progress budget is spent, so a dead peer
   leads to a bounded disconnect (fix for Pedro's OTA finding, see below).

Wire note: MODE_ACK gains a flag + a previously-zero header field.  Old
receivers ignore both; a new sender ↔ old receiver falls back to the old
"keep mode, keep window" behaviour (no interop break, but both ends need the
branch to benefit).

## Instruments (and their own validation)

- **Two-FSM deterministic sim** (`tests/sim`): real `arq_fsm.c`/protocol under
  a virtual clock and seeded channels.  Extended on this branch with a
  mode-aware SNR-cliff model and an empirical per-mode-PER model.
- **Watterson FIFO harness** (`MERCURY_CH_ENGINE=watterson`): calibration
  independently cross-checked against AE4JY PathSim (`Rhizomatica/pathsim`)
  — measured SNR within ±0.2 dB of target, matching delivery cliffs.
  (That cross-check also found and fixed two real pathsim bugs upstream.)
- **loopsim**: two real Mercury instances over kernel `snd-aloop`, AWGN.
- **pathsim offline** (`--midlat-dist-nvis`): per-mode delivery measured with
  the freedv raw tools, then driven through the deterministic sim.
- **OTA**: Pedro's estacao6↔estacao10 session of 2026-07-04 ran the branch
  build (`21266a6`) on real sky.

## Results

### 1. Deterministic fade-cliff test (the S1 proof)

`test_sim_fade_cliff_downgrades` (previously `TEST_IGNORE`d as documentation
of the bug): connect clean → fade below every non-floor mode's cliff → the
branch descends to DATAC15 and delivers all 16 KB; trunk starves above the
cliff.  Fuzz PER ceiling raised 0.25 → 0.40, 50 seeds green.

### 2. Watterson HF fading A/B (1054 B, two rounds)

| Condition (measured SNR) | trunk r1 | trunk r2 | branch r2 (final) |
|---|---|---|---|
| Moderate (~1–3 dB) | 1054 ✅ 298 s | 458 ❌ | **1054 ✅ 298 s** |
| Poor, No −17 | 1054 ✅ 217 s | 1054 ✅ 216 s | **1054 ✅ 217 s** |
| Poor, No −13 (below cliff) | 344 ❌ | 596 ❌ | **684** (3× runs: 684/684/684 vs trunk 344/596/596) |

Notes: the harness Watterson is unseeded — trunk itself flips PASS/FAIL at
"Moderate", so only cross-round patterns count.  Round 1 caught a real branch
regression (the probe could *upgrade* into a fade; ~2 min go-back-N stall) —
fixed by making probes downgrade-only (`21266a6`) before round 2.

### 3. loopsim AWGN, 3-way with v1.9.9 (2048 B)

| Channel | v1.9.9 | trunk | branch |
|---|---|---|---|
| Clean | 57.3 s ✅ | 62.0 s ✅ | 54.8 s ✅ |
| Noise 1.0 (≈ −4 dB) | **0 bytes** ❌ | 592 s ✅ | 601 s ✅ |

Parity everywhere; both post-1.9.9 builds rescue the −4 dB channel where
v1.9.9 moves nothing.

### 4. NVIS-disturbed (the deployment channel) — empirical-PER sim bench

pathsim `--midlat-dist-nvis` (7 ms delay spread, 1 Hz Doppler) measured with
freedv raw tools at SNR 10 is **ISI-limited, not SNR-limited** (delivery is
flat from SNR 16 down to 4): DATAC15 ≈ 80 % delivery, DATAC3 ≈ 33 %,
DATAC1 ≈ 11 %.  This is the exact S1 trap: SNR reads healthy while fast modes
die.  Those measured per-mode PERs drive the deterministic sim (8 KB, 30
virtual minutes, frames stamped 10 dB):

| seed | trunk delivered | branch delivered |
|---|---|---|
| 1 | 22 B | 3188 B |
| 2 | 22 B | 4572 B |
| 3 | 22 B | 4214 B |
| 4 | 22 B | 524 B |
| 5 | 22 B | 4952 B |

Trunk delivers exactly one floor frame (sent before the ladder climbs), then
stalls at DATAC1 forever — the healthy SNR feeds the unbounded reverse-loss
hold and the window guard, and no downgrade path can fire.  The branch keeps
moving (integrity OK on every seed).  Branch behaviour is not perfect — the
SNR trap re-arms after each hold expiry, and two seeds ended in a no-progress
disconnect before finishing — but the difference is categorical: **~24–225×
more data delivered on the channel these stations actually operate on.**

### 5. OTA (Pedro, estacao6 ↔ estacao10, 2026-07-04, branch build)

- S1 mechanics observed working on real sky: bounded holds engaging
  (`hold 1/3`, re-armed by clean deliveries), OLLA climbing +0→+3 dB, ladder
  ascending DATAC15→DATAC4→DATAC3→DATAC1 at burst boundaries on a −5 dB
  forward path, restage firing correctly on a mid-window downgrade.
- **Real bug found** ("IRS desconectou e a ISS continuou transmitindo"): with
  a dead peer, the probe→failed-MODE_REQ→revert loop reset the retry budget
  every ~200 s, preempting the no-progress disconnect indefinitely.  Fixed
  (`a014b3c`): probes never fire once the no-progress budget is spent;
  guarded by `test_sim_peer_loss_disconnects` (verified to fail on the bug).
- Pre-existing gap noted (trunk too, NOT addressed here): an ISS idling with
  an **empty** backlog never keepalive-times-out — only the IRS monitors
  inactivity.  Belongs to the keepalive-persistence work.

## Verdict

- **No regression found anywhere workable**: clean AWGN, harsh AWGN, and
  workable fading are at parity with trunk (after the round-1 probe fix).
- **Real, repeatable advantage exactly where the fix aims**: below the fade
  cliff (3× consistent on Watterson) and on ISI-limited NVIS-disturbed
  (trunk effectively cannot transfer at all; the branch moves data on every
  seed).
- **OTA exposure already happened** and the one defect it surfaced is fixed
  and regression-guarded.

**Recommendation: merge `fix-s1-fadecliff` @ `a014b3c` into `mercuryv2`**,
with one more Pedro OTA session on the `a014b3c` build as confirmation.
Known follow-ups (not blockers): idle-ISS keepalive gap (pre-existing); the
NVIS SNR-trap re-arming suggests a future "ISI channel" heuristic (delivery
persistently contradicting SNR should cap the ladder); pathsim streaming
mode still needs work for live bursty bridges (offline/continuous use is
validated — the NVIS numbers above come from the validated offline path).

## Reproducing

```
# deterministic suite (includes the cliff + peer-loss guards)
make -C tests test_arq_sim && ./tests/test_arq_sim

# Watterson A/B (per build: set MERCURY_BIN)
MERCURY_CH_ENGINE=watterson MERCURY_CH_FADING=poor MERCURY_CH_NO=-13 \
MERCURY_TEST_PAYLOAD_KB=1 go test -C tests/integration -run TestMercuryARQTransfer$

# NVIS per-mode PER measurement (offline pathsim + freedv raw tools), then
# the empirical-PER bench: see tests/sim sim_channel_set_mode_per(); the
# bench driver used for §4 lives in the session scratchpad (nvis_bench.c).
```
