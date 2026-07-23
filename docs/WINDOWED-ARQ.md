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
