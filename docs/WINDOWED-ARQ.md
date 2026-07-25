# Windowed Multi-Frame ARQ — design record

Goal: beat VARA HF throughput in almost all conditions before 2.0, without
regressing low-SNR/marginal-signal support and without breaking broadcast.

## Why: the measured gap is turnaround, not modem speed

Mercury's raw modes already exceed v1.9.9 (docs/OTA-PHASE-A-SNR-CALIB.md), but
the ARQ is stop-and-wait: one frame per keydown, then ~700 ms IRS guard +
~0.64 s pattern ACK + ~900 ms ISS guard + 2 radio switches ≈ **2.5–3 s of dead
air per frame**, plus 600–660 ms preamble+postamble per keydown (measured, all
OFDM modes — see `utils/burst_rx_sweep.sh` methodology). At DATAC3 that is
~6.6 s spent per 3.2 s frame. Batching K frames under one keydown with one
consolidated ACK amortizes both taxes: at K=5, ~1.7× goodput before any
selective-repeat gain. VARA (many blocks per cycle + block ACK) and mercuryv1
(`send_batch` batch_size=5 + `ACK_MULTI`) both won this way.

## Raw-layer validation (gates in `utils/burst_rx_sweep.sh`)

1. **Multi-frame bursts decode.** All ladder modes (DATAC15/4/3/1/17, QAM16C2,
   DATAC16) decode K=5 bursts behind one preamble at 10/10, zero FER, when the
   RX's `frames_per_burst` matches the TX burst size.
2. **No noise penalty.** DATAC3 at ch SNR3k ≈ +2 dB: K=4 shared-preamble
   bursts decode 20/20, identical to K=1. Windowing itself costs nothing in
   robustness on AWGN (fading mid-burst is handled by the SACK + depth
   controller, and by HARQ later).
3. **The partial-burst pathology (why bursts must be self-describing).** The
   codec2 OFDM *burst* state machine (`ofdm_sync_state_machine_data_burst`)
   checks the UW **only in `trial`** (acquisition); once `synced` its ONLY exit
   is `packet_count >= packetsperburst`, counting **demodulated packet
   durations** (attempted, not CRC-good — so a mid-burst decode failure does
   not stall it). Consequences, all reproduced:
   - RX expects K, burst carries J<K → the machine consumes (K−J) packet-times
     of *following* audio as garbage and **eats the next keydown's preamble**
     ([2-burst][4-burst] with fpb=4 → 2/6).
   - `packetsperburst=0` ("never lose sync") never returns to search → decodes
     only the first burst of a gapped stream (4/12). Not usable for keyed ARQ.
   - A burst *shorter than expected at the end of RX* is harmless; an
     over-long burst (J>K_rx) drops only its own tail frames.

## Design consequences

- **Self-describing bursts.** Every DATA frame header carries
  *frames-remaining-in-this-keydown* (the FSM already computes
  `burst_remaining`; spare header flag bits exist). The RX acquires with a
  ceiling (`ARQ_BURST_MAX`) and, on **any** decoded frame, re-anchors the
  state machine to exit exactly at burst end via a small vendored-codec2
  addition (`freedv_set_packets_remaining()`: `packetsperburst = packet_count
  + remaining`). Robust to any single frame loss (any later frame re-anchors);
  no on-wire negotiation; retransmit bursts of arbitrary size just work.
  Fallback when *every* frame of a burst fails: the machine over-runs into the
  inter-burst gap — bounded by the ceiling; mercury's turnaround hook and the
  postamble acquisition path (already in codec2, mirrors the MFSK postamble
  fix) recover the following burst.
- **Selective repeat by stable seq + consolidated SACK.** ISS ring of in-flight
  frames (seq mod 256); IRS out-of-order reassembly above cumulative
  `rcv_base`; one ACK per burst carrying `rcv_base` + a small hole bitmap;
  retransmit only holes. Drains under fade because the degenerate case
  (depth→1, mode→MFSK floor) is exactly today's proven stop-and-wait.
- **Two decoupled controllers, evaluated per burst.** Mode = the existing
  delivery-driven ladder, stepped down only when a frame fails at depth 1;
  burst depth = AIMD on the SACK loss fraction. Never per-frame SNR (the
  go-back-N/OLLA oscillation removed by commit af38b1c must not return).
- **Fast compact SACK carrier**: prototype an extended Welch-Costas pattern
  (bitmap bits on top of today's ACK/BREAK tones) vs a short robust coded
  burst; window==1 keeps today's 0.64 s pattern ACK bit-for-bit (fringe path
  unchanged); bidirectional flows piggyback the SACK on reverse DATA.
- **MFSK stays window==1 permanently** (13.5 s burst dwarfs the turnaround;
  fringe robustness preserved). Broadcast is untouched by the ARQ window; the
  shared modem pool change is gated by `utils/burst_rx_sweep.sh` + a broadcast
  RX/TX check.

## Root-cause finding (task #91): frame size must be decoupled from mode

Activating K>1 (`burst_frames`=3 on the OFDM modes) exposed a fundamental flaw
that also latently exists at K=1: **one ARQ frame == one modem frame, sized to
the current mode's payload**, so a frame created at a fast mode is too large to
retransmit at a robust one.  Concretely (reproduced with `tests/sim/win_repro.c`
on a clean 12 dB channel, 20 kB transfer): the ladder climbs to QAM16C2, reads
a 1205-byte frame, QAM16C2 fails (its sim cliff is 13 dB), and `mode_that_fits`
can never place that 1205-byte frame in any smaller mode (DATAC17 tops out at
1172 user bytes).  The frame is **trapped at QAM16C2** — the delivery-driven
step-down is powerless, it retransmits until the retry budget + no-progress
timer expire, and the link disconnects mid-transfer (stalls at ~5.6 kB/20 kB).

This directly violates the low-SNR constraint: a large frame cannot degrade to
the MFSK floor.  A modem frame is a fixed-size LDPC codeword per mode (QAM16C2
is always 1213 bytes), so the only design that gives BOTH fast-mode throughput
AND re-sendability-at-the-floor is **block-packing**: a fixed small ARQ block
(re-sendable at any mode, down to the MFSK floor), with a fast-mode modem frame
carrying MANY blocks (a count + length-prefixed blocks).  This is exactly what
v1 (`send_batch`), VARA, and OpenARQ do — the ARQ block is decoupled from the
modem frame.  It is the core enabler of the leap and a wire-format change.

The Step-C windowed FSM (selective repeat, SACK, per-burst ACK, mode mirror) is
the right machinery and stays; block-packing changes the UNIT it operates on
(block, not mode-sized frame).

### Block-packing wire design (the fix, this branch)

**ARQ block = the retransmission unit**, `ARQ_BLOCK_DATA_MAX = 44` user bytes,
own mod-256 seq.  44 is the largest block that fits a DATAC4 modem frame
(54 payload − 8 frame hdr − 2 block hdr), so a block is re-sendable at EVERY
ladder rung down to the MFSK floor — the trap is gone and low-SNR degradation
is preserved.  A DATA modem frame is now a **container**:

```
[0] framer byte (PACKET_TYPE_ARQ_DATA)
[1] subtype = DATA
[2] flags: HAS_DATA(6) | burst_remaining[2:0]   (frame-level: modem frames left in keydown)
[3] session_id
[4] block_count (1..N)                            (was tx_seq — blocks carry their own seq)
[5] rx_ack_seq (piggyback cumulative ACK)
[6] snr_raw
[7] 0 (was payload_valid low byte — per-block len replaces it)
[8..] block_count × { seq(1) | len(1, 1..44) | data(len) }
```

A fast mode packs many blocks (QAM16C2 ≈ 26); MFSK carries 1.  The IRS unpacks
the container into per-block reassembly and re-anchors the OFDM burst state
machine per MODEM FRAME exactly as before (`burst_remaining` is unchanged,
frame-level).

**Ladder change (user-visible, documented for review):** with a 44-byte block,
DATAC15 (22 user bytes/frame, cliff ≈ −7 dB) is **strictly dominated by MFSK**
— MFSK is both more robust (−13 dB) AND higher-goodput (90 B/13.5 s = 6.7 B/s
vs DATAC15's 5 B/s) — so it is **dropped from the ARQ ladder** (kept in the
mode table for control/broadcast/compat).  DATAC4 (−4 dB, 7.9 B/s) is **kept**:
it is genuinely faster than MFSK in the −4..0 dB band, so the marginal-signal
path is not penalized.  New ladder (`ARQ_LADDER_LEVELS = 6`):
`MFSK → DATAC4 → DATAC3 → DATAC1 → DATAC17 → QAM16C2`.  tx mode =
`ladder[speed_level]` directly (every rung holds a 44 B block, so the old
`mode_that_fits` bump — and the mode/mirror mismatch it caused — is gone).

**Window + ACK:** `ARQ_WIN_SLOTS = 32` blocks in flight (selective repeat);
SACK bitmap = 4 bytes (covers base+1..+32), which fits the DATAC16 14-byte
control frame (8 hdr + 4).  **`ARQ_WIN_SLOTS` MUST divide 256** — the slot index
is `seq % ARQ_WIN_SLOTS` and seqs are mod-256, so a window straddling the
255→0 wrap aliases two live seqs onto one slot unless N | 256 (see the bug
below).  Keydown blocks = min(32, `MODEM_RX_BURST_CEILING`×blocks-per-frame).
Clean *new* multi-block burst → coded ACK(rcv_base); degenerate 1-block
MFSK-floor burst → fast Welch-Costas pattern; a burst with holes OR a
duplicate → coded SACK.  A **fast pattern-with-bits carrier** and a **larger
window** (both gated today by DATAC16's 14 bytes) are the Phase-2 lever —
correctness/floor first.  `MFSK stays 1 modem frame/keydown` (floor unchanged).

**HARQ:** Chase soft-combining requires bit-identical retransmits; block-packing
selective repeat repacks holes / changes mode, so HARQ is **default-off** for
the block-packing branch (env `MERCURY_HARQ=1` forces on).  Phase 3b restores it
by front-loading the lowest un-acked hole as a bit-identical burst frame-0.

**Validation.**  Full unit suite green (20/20 binaries, incl. block-codec
round-trip + all windowed FSM/ladder/sim tests); `tests/sim/win_repro.c` drains
20 kB through climb→deep-fade→recover at 100 % integrity across 11 seeds × 3
fade depths (the exact scenario that stalled pre-block-packing).  Two subtle
selective-repeat bugs the sim caught and that are now fixed:
 1. **Slot aliasing at the seq-wrap** (the deterministic stall at block 246):
    `ARQ_WIN_SLOTS` was 48, which does not divide 256, so a wrapping window
    aliased seq 246 and seq 6 onto slot 6 — creating block 262 silently
    clobbered block 246.  Fixed by `ARQ_WIN_SLOTS = 32` (divides 256).
 2. **Bare pattern ACK on a duplicate** over-retired: a seq-less pattern re-sent
    for an already-delivered 1-block burst (our earlier ACK was lost) was read
    by a pipelined ISS as "my current tx_base arrived", retiring a block the
    peer never got.  Fixed: a duplicate (or any hole/multi-block) burst is acked
    by a coded `rcv_base`, never a bare pattern.  A stale/aliased coded-ACK base
    outside the live [tx_base, tx_seq] window is also now rejected (mod-256
    generation aliasing from a delayed/echoed ACK).

## Real-modem finding (-x sock): K=1 fast-climbs; fixed K>1 is SLOWER → adaptive depth

Block-packing validated end-to-end on the real modem over the virtual-clock
`-x sock` transport (skywave `mercury_sock` adapter, clean profile, SN~16 dB):

| config | 2048 B | peak bitrate | notes |
|---|---|---|---|
| block-packing, **K=1** frame/keydown | 3/3 complete | **3135 bps** | fast-ramps to QAM16C2 in ~35 s |
| block-packing, **fixed K>1** (DATAC4=3…QAM16C2=5) | completes but SLOW | 980 bps | crawls; never reaches QAM16C2 |

**Fixed high K>1 is *slower* than K=1**, not broken.  An instrumented K>1 run
completed (2048/2048) but the timeline showed the killer: K>1 keydowns are
21–32 s, and the delivery-driven ladder climbs one rung per clean burst, so it
crawls MFSK→DATAC4→DATAC3→DATAC1 over ~130 s and never reaches QAM16C2 — while
K=1's short ~7 s keydowns fast-ramp to QAM16C2 in ~35 s.  Long keydowns cripple
the climb.  (The earlier "2024/2048 stall" was an *intermittent* tail retransmit
loss, not the core problem.)

**Fix = adaptive burst depth** (`arq_session_t.burst_depth`, driven in
`record_tx_outcome`): a session starts at depth 1 for the fast climb, holds at 1
while the mode is still moving (a clean burst that climbs, or any dirty burst
that steps down), and grows one frame per clean burst — up to the mode's
`burst_frames` cap — only once the ladder SETTLES.  So a good channel fast-climbs
at K=1 to QAM16C2 (already ~1.1 kB/keydown), and a marginal channel that settles
at DATAC3/DATAC4 grows K>1 there, where 5 frames amortize the ~2.5-3 s turnaround
~2-3× — the beat-VARA lever exactly where it helps most, and where the block
window is NOT binding.  Unit-gated by `test_burst_depth_adaptive` (starts 1,
holds 1 climbing, grows to cap when settled, resets to 1 on loss).

**Window binding (a separate later lever).**  At the FAST modes the 32-block
window (capped by the DATAC16 4-byte SACK) holds only ~1 frame
(QAM16C2 = 26 blocks/frame, DATAC17 = 25, DATAC1 = 10), so K>1 can't send
multiple full frames there.  This costs little today (good channels reach those
modes and are already fast at K=1), so it's deferred: growing the window needs a
**wider/faster SACK carrier than DATAC16's 14 bytes** — the "faster compact
windowed ACK" — a Phase-2b optimization for squeezing more at the very top.

## Phase 2b — the real good-channel speed leap (bigger window + wider SACK)

**Why it's needed.** On a good channel the ladder fast-climbs to QAM16C2, where
the throughput leap must come from K>1 amortizing the ~2.5–3 s turnaround across
frames.  But at QAM16C2 one frame already holds 26 blocks, and the current
window is 32 blocks — so K is capped at ~1.2 frames: **K>1 does not engage on
good channels.**  Bench A/B confirmed block-packing ≈ trunk on clean/poor (the
K>1 lever never fires); the win only appears where trunk's trap stalls (SN~11).
To beat VARA on good channels the window must hold ≥ K full fast-mode frames.

**Throughput target.** At QAM16C2, K=1 ≈ 163 B/s (1144 B / ~7 s incl.
turnaround); K=2 (window ≥ 52) ≈ 208 B/s (+28 %); K=5 (window ≥ 130) ≈ 249 B/s
(+53 %).  Window **64** (next divisor of 256 — the mod-256 slot-aliasing
constraint) unlocks K=2 at QAM16C2/DATAC17 and is the first meaningful step.

**MEASURED (bench, -x sock, skywave mercury_sock adapter, clean profile, 16 kB,
both reaching QAM16C2 / peak 3135 bps, SN ~15 dB, 2 runs each):**

| build               | goodput (avg)  | transfer time | vs HEAD |
|---------------------|----------------|---------------|---------|
| window 32 (HEAD)    | 78.3 B/s       | 209 s         | —       |
| window 64           | 98.4 B/s       | 166.5 s       | +25.5 % |
| **window 128**      | **115.6 B/s**  | 141.75 s      | **+47.6 %** |

**window 128 = +47.6 % over HEAD** (K≈5 at QAM16C2, matching the +53 %
prediction; +17.5 % over window 64), low variance (98.7/98.1 and 115.4/115.8).
This is the first DEMONSTRATED good-channel bench speedup; block-packing K=1
alone was ~equal to trunk (K>1 never engaged because the 32-slot window held one
QAM16C2 frame).  A 16 kB transfer is needed to see it: smaller transfers finish
during the MFSK-start climb before depth grows at the top rung.  Window 128 uses
half the mod-256 seq space; the 4-byte SACK still covers only base+1..+32 (a
stuck base trailing 32+ delivered blocks retransmits the rest redundantly — a
clean channel never stalls its base, and a poor channel keeps depth 1, so this
does not bite in practice; verified no poor-channel regression on the bench).

**The blocker = the SACK carrier.**  Window 64 needs a 64-bit (8-byte) hole
bitmap; the ACK frame (8-byte hdr + 8 = 16 B) no longer fits DATAC16 (14 B).
Design options (mapped; pick on the bench):
  1. **RLE / run-length SACK on DATAC16** (recommended first try).  A burst's
     holes are usually a whole failed frame's worth of CONTIGUOUS blocks, so
     encode `rcv_base` + runs of (delivered,missing) lengths.  A single failed
     QAM16C2 frame = one 26-long missing run = ~2 bytes.  Fits DATAC16's 6 spare
     bytes for the common case; a `>N runs` overflow marker falls back to
     go-back-N for that one burst.  No modem plumbing — stays on the robust
     control mode.  Only real cost: RLE encode/decode + `iss_apply_sack` rewrite.
  2. **SACK on DATAC15** (30-byte payload → 176-bit window bitmap).  DATAC15's
     cliff == DATAC16's (both ~−7 dB), so it is equally robust.  Cost: the ISS
     must also decode DATAC15 in WAIT_ACK (dual-mode ACK RX — modem plumbing).
  3. **Bigger blocks at fast modes** — REJECTED: a >44-byte block cannot fit
     DATAC4/DATAC3 on retransmit, re-introducing a (smaller) immutable-frame trap.

Adaptive depth (already landed) then grows into the bigger window automatically;
the mode caps and the depth controller need no change.

**MUST be bench/OTA-validated.**  Phase 2b is a wire-format change whose only
payoff is throughput, and the deterministic two-FSM sim is BLIND to the
real-modem behavior that decides it (it passed fixed-K>1 that the bench then
showed crawling).  So implement it against a WORKING `-x sock` bench (or OTA) and
A/B every step — do NOT ship on sim-green alone.  (The `-x sock` rig degrades
after ~20-25 transfers in one session — empty output / connect-fail — so this
needs a fresh session; deterministic tests gate correctness, the bench gates the
speed claim.)

## Phase 2c — fast windowed ACK (epoch-tagged pattern)

**Why (measured critical path).** On a clean K=5 QAM16C2 keydown the ACK is a
3.74 s coded DATAC16 frame (a multi-block burst can't use the seq-less pattern —
a stale pattern would over-retire the window), vs the 0.64 s Welch-Costas
pattern used at the floor.  That coded ACK is ~3 s of the ~21 s cycle — **~15%,
serial, every keydown.**  Letting clean multi-block bursts use a pattern ACK is
the last big good-channel lever (est. +15% on top of the +47.6%).

### Modem-codec layer — DONE + unit-validated

- **Epoch-tagged pattern = base ack/break pattern (16 symbols, UNCHANGED) + a
  short appended Welch-Costas MINI-PATTERN** (`MFSK_EPOCH_LEN`=6 symbols)
  encoding a 2-bit epoch.  The mini-pattern is one of 4 spread sequences
  (`MFSK_EPOCH_BASE_SEQ` shifted by 2·epoch, so any two epochs differ in every
  symbol by 2/4/6 tones — never M/2, which would alias under an FFT-window
  timing error).
- **Detected by the SAME robust matched filter as the base pattern**
  (`mfsk_detect_pattern`), NOT a raw fixed-offset FFT read.  The earlier
  single-4-ary-symbol design was abandoned: the base pattern only locates the
  burst to a Nofdm/8 grid (and can be >GI off after the RX LPF), so a single
  appended symbol demodulated at a fixed offset smears energy across carriers
  (M/2 image + adjacent-carrier ICI) and the epoch value is unrecoverable.  The
  matched filter slides its own fine timing search and does per-symbol peak-tone
  matching, so it tolerates that error — the reason the 16-symbol base pattern
  is robust to −13 dB in the first place.
- **Decision:** score all 4 candidate sequences over the region past the base
  pattern; accept the best only if it matches ≥ `MFSK_EPOCH_MIN_MATCH`(=4) of 6
  symbols AND beats the runner-up by ≥ `MFSK_EPOCH_MARGIN`(=2).  Measured
  (`tests/modem/test_pattern_ack_detection.c`, noise-injection): real epoch
  scores 5–6/6, wrong candidates ≤3, a bare pattern's trailing region ≤2 → a
  bare ACK is NEVER misread as tagged (fringe path byte-identical + safe).
- **`pattern_kind` encoding** (no callback-signature change): `0/1` = bare
  ACK/break (byte-identical to today — the FRINGE path is untouched);
  `MFSK_PATTERN_TAGGED(0x80) | epoch<<1 | break` = epoch-tagged (fast ACK).
- **Inert until the FSM uses it:** the FSM still emits bare patterns, and the
  detector only sets TAGGED when a real mini-pattern is on-air, so current
  behaviour is unchanged.

### Protocol/FSM layer — DONE + sim-validated (behind MERCURY_FAST_ACK)

- **Epoch carrier.** The plan's "DATA flags[5:4]" was NOT available — under the
  fast-modes 11-bit valid-length change those bits are LEN_HI/LEN_B9/LEN_B10.
  The 2-bit per-keydown `ack_epoch` rides **DATA byte 7** (the `ack_delay` slot,
  which is IRS→ISS and so unused on a DATA frame).  `arq_protocol_build_data_blocks`
  gained an `epoch` param; the parser exposes it as `ev.data_epoch`.
- **Flow (wired).** ISS increments `sess->tx_burst_epoch` (mod 4) per keydown and
  stamps it in every DATA frame; the IRS records it (`rx_burst_epoch`).  On a
  CLEAN, complete, all-new MULTI-block burst the IRS sends an epoch-tagged
  pattern echoing that epoch (`send_ack(sess, rx_burst_epoch)`) instead of the
  coded SACK.  The ISS in WAIT_ACK, on a tagged pattern (`ev.ack_epoch >= 0`):
  epoch == `tx_burst_epoch` → `iss_retire_all` (whole window); mismatch → stale,
  ignored (retry timer covers a genuinely lost ACK).  Bare pattern (epoch -1) →
  `iss_retire_one` (single-block, unchanged); holes → coded SACK.
- **Fail-safe + fringe-safe.** The fast ACK fires ONLY for clean multi-block
  bursts (fast modes, good SNR) — the MFSK floor stays K=1 with the plain
  2-pattern ACK, untouched.  A misread/stale epoch fails the match → ISS falls
  back to retry/coded-ACK: never an over-retirement, only a missed speedup.
  `ack_epoch`/`data_epoch` default to -1 at every event producer (arq.c, the
  sim, the FSM test harness), so a coded ACK is never mistaken for epoch 0.
- **Gate.** `MERCURY_FAST_ACK` (getenv) default-OFF: when off the IRS never
  emits a tagged pattern, so the feature is fully inert and behaviour is exactly
  today's.  Stays off until a healthy `-x sock` bench (or OTA) confirms the
  on-air epoch false-classification rate and the +15% gain.
- **Deterministic tests.** `tests/sim/test_arq_sim.c`:
  `test_sim_fast_windowed_ack` (flag on: clean 3 kB transfer completes
  byte-exact AND emits >0 tagged patterns — proves the whole chain) and
  `test_sim_fast_ack_off_no_tagged` (flag off: byte-exact, ZERO tagged — the
  regression guard).  Plus the DATA byte-7 epoch round-trip in
  `test_arq_protocol.c`.

## Phases

0. Deterministic instruments: burst-capable two-FSM sim (done — sim outbox
   FIFO + keydown grouping + independent per-frame erasure), this raw-layer
   gate (done), skywave A/B config.
1. Minimum leap: RX multi-frame (self-describing bursts) + compact SACK +
   ISS ring/IRS reassembly, fixed K=2..4.
2. Adaptive depth (SACK-loss AIMD) + burst-boundary ladder + ACK-driven
   retransmit (fallback timer derived from burst duration, never fixed).
3. HARQ: off on K>1 bursts first (stays on at the floor); within-burst
   frame-0 combining later.
4. MFSK floor unchanged; 5. skywave A/B vs VARA/Armstrong/v1.9.9 across an
   SNR×fade grid (success: > stop-and-wait everywhere, ≥ VARA on the large
   majority of cells, fringe ≥ today).

## Phase 5 — beat-VARA A/B: baseline, honest target, and how to run it

The reference is the **frozen v2 skywave bakeoff** (`skywave/SKYWAVE-BAKEOFF-
REPORT-2026-07-23-v2.pdf`): all five modems (armstrong, Mercury 1.9.9/1.9.10,
VARA HF, FreeDATA, ARDOP) measured head-to-head on one machine, identical
seeds, equal-PEP drive, ALSA-loopback real-time audio. Its Mercury column is
the **pre-windowing stop-and-wait baseline**, so Phase 5 = re-run only the
Mercury column with the `arq-windowed` binary (fast-ACK on) and drop it into
the frozen table. (We do NOT run VARA — its numbers are already in the report.)

**Frozen Tier-2 bulk (32 KB), B/s:**

| cell            | VARA | armstrong | freedata | ardop | Mercury 1.9.9 |
|-----------------|------|-----------|----------|-------|---------------|
| clean           | 464  | 292       | 161      | 148   | **111**       |
| mid-SNR AWGN    | 283  | 146       | 160      | 86    | 98            |
| moderate fade   | 155  | 118       | 131      | 62    | 82            |

**Honest target (do not overclaim).** The windowed leap removes the stop-and-
wait *turnaround tax* (deterministic + `-x sock` proof: +47.6% window-128,
+12% fast-ACK, block-packing trap and fade-drain fixed). It does NOT change the
codec2-OFDM *modem ceiling*. So on clean/light-fade bulk, VARA's high-order QAM
(464 B/s at equal PEP) is out of reach for ANY ARQ change — even armstrong, the
robustness leader, concedes that cell at 292. The contestable ground vs VARA is
exactly the report's finding: **fading + low SNR**, where VARA's QAM gears
collapse and armstrong beats it in 9 of 14 faded cells. The defensible Phase-5
claim is therefore: *the leap lifts Mercury from ~half-the-field to competitive
with armstrong/FreeDATA on bulk, strictly beats stop-and-wait everywhere, and
contests VARA in the fade/low-SNR cells* — not "beats VARA everywhere."

**Rig requirement (why it is not run in the dev sandbox).** The bakeoff's
`mercury` adapter uses the **4-card `snd-aloop` ALSA rig** + a per-modem
equal-PEP TXGAIN calibration (`results/mercury_txgain.txt`) + a CPU-headroom
validity gate on a controlled machine. A bare `modprobe snd-aloop` gives only
one Loopback card, so channel_sim's 4-device relay map has nowhere to bind and
every connect fails (`NOCONN`) — a transport-setup gap, not a Mercury result.
Apples-to-apples numbers must come from the report's rig (or an equivalently
configured, quiet host).

**Ready-to-run recipe (on the configured rig):**
```
# 1. equal-PEP calibration (writes results/mercury_txgain.txt)
MERCURY_BIN=/path/to/mercury-arq-windowed MERCURY_FAST_ACK=1 \
  python3 -m skywave.sweep_runner --calibrate-pep mercury

# 2. Tier-2 bulk cells (matches the frozen report)
cat > cells-bulk.json <<'EOF'
[{"sigma":0,"watterson":"off","payload":32768,"timeout":600,"reps":2},
 {"sigma":4000,"watterson":"off","payload":32768,"timeout":600,"reps":2},
 {"sigma":4000,"watterson":"moderate","payload":32768,"timeout":600,"reps":2}]
EOF
MERCURY_BIN=/path/to/mercury-arq-windowed MERCURY_FAST_ACK=1 \
  python3 -m skywave.sweep_runner mercury cells-bulk.json out-windowed.csv windowed
# then paste the goodput column next to the frozen VARA/armstrong columns above.
```
The fade/low-SNR tiers (the anti-VARA cells) use the same runner with the
report's Tier-1/Tier-3 cell lists.

## Phase 2c — fast-ACK fade & fringe validation (Watterson), pre-OTA

Robustness + speed validation of `MERCURY_FAST_ACK` before the OTA flip, using
our Watterson tooling.  Tools committed under `utils/` (re-runnable on any rig):
`pattern_fade_probe.c` + `fastack_fade_phy.sh` (deterministic PHY), and
`fastack_fade_ab.sh` / `fastack_lowsnr_ab.sh` (end-to-end sock A/B).

**Leg A — deterministic PHY (epoch signal under Watterson 'poor' 2 ms/1 Hz fade,
N=200/point, `utils/watterson_test` channel).** The two decisive rates:

| SNR3k* | tagged_ok | misdecode | miss | **false_tag** |
|--------|-----------|-----------|------|---------------|
| +21 dB | 100%      | 0%        | 0%   | **0%**        |
| +13 dB | 100%      | 0%        | 0%   | **0%**        |
| +7 dB  | 100%      | 0%        | 0%   | **0%**        |
| +1 dB  | 100%      | 0%        | 0%   | **0%**        |
| −1 dB  | 99.9%     | 0%        | 0%   | **0%**        |
| −5 dB  | 99.1%     | 0%        | 0%   | **0%**        |
| −9 dB  | 96.1%     | 0%        | 0%   | **0%**        |
| −11 dB | 89.5%     | 0%        | 0.5% | **0%**        |
| −13 dB | 81.8%     | 0%        | 1.0% | **0%**        |
| **−15 dB** | 60.0% | 0%        | 9.2% | **0%**        |

*File-averaged, and the tone pattern reads ~6 dB hotter than a spread modem
frame, so the absolute axis is soft; the shape is the point.  Three ideal
results across the WHOLE range down to −15 dB:
(1) **false_tag = 0% at every SNR** — a bare pattern is never mis-read as
tagged, so a false `retire_all` (over-retirement / data loss) cannot happen at
any SNR.
(2) **misdecode = 0% at every SNR** — when the epoch is detected it is never the
*wrong* epoch; the only failure mode is "not detected" (safe fallback to coded
ACK / retry), never a wrong answer.
(3) **The tag adds zero robustness penalty** — `miss` tracks `bare_miss`
one-to-one (0.5/0.5, 1.0/1.0, 9.2/9.5), i.e. the epoch fails ONLY when the base
ACK pattern itself fails to detect.  It survives right down to the pattern-ACK's
own cliff (~−13/−15 dB, where even the plain ACK is at 60%), inheriting the
Welch-Costas matched-filter robustness that works to ~−13 dB.

**Leg B — end-to-end sock A/B (fast-ACK OFF→ON, 16 kB, virtual clock).**

| channel | OFF | ON | Δ |
|---------|-----|----|----|
| clean   | 115.4 | 129.0 | **+11.8%** |
| mid11 (mid-SNR, K>1 sweet spot) | 114.6 | 128.5 | **+12.1%** |

Byte-exact both arms; the speed win holds on the audio-ish transport, not just
`-x sock`.  ('poor' Watterson at 16 kB / 500 s timed out on BOTH arms — a
channel-vs-payload limit, not a fast-ACK regression: under heavy fade the ladder
sits at robust modes where the fast ACK doesn't fire at all.)

**Fringe no-regression — by construction (not by measurement).** The tagged
pattern is gated `clean_new && multi && fast_ack_enabled()` with
`multi = rx_burst_blocks > 1` (`arq_fsm.c` ~636).  At the low-SNR fringe the
ladder is at the 1-block MFSK/DATAC4 floor → `multi == false` → the code takes
the **bare** path, byte-identical to OFF.  So **ON ≡ OFF at the fringe** — the
sacred low-SNR path is untouched, provably, without a measurement.

**What still needs the dummy-load / OTA.** Reliable sub-zero-dB *end-to-end*
5 kB numbers could not be produced on the dev sock rig: its σ→SNR is uncalibrated
here (connects fail below an uncertain cliff; NP stats not written), and the
host is load-flaky.  That measurement belongs on the calibrated dummy-load /
São Roque↔BH OTA — `utils/fastack_lowsnr_ab.sh` (5 kB, σ sweep, reads true
snr3k) is ready to run there.

**Verdict for the flag.** Safe (false_tag 0% under fade), faster where it engages
(+12%), and provably neutral at the fringe.  The one remaining gate before
default-on is the OTA confirmation that the epoch survives a *real* HF reverse
path at the rate needed to realise the +12% (vs falling back to coded ACK).
