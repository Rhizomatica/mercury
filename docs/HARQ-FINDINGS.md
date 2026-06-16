# HARQ soft-combining — feasibility proof (freedv layer)

**Status:** modem-layer hook implemented and measured; ARQ integration is the
follow-on. This is the decisive low-SNR/under-fading lever identified in
[SPEED-REGRESSION-FINDINGS.md](SPEED-REGRESSION-FINDINGS.md) — the piece OLLA
(see the ARQ work) cannot supply because Mercury has no HARQ today.

## What was added

A Chase (LLR-accumulation) soft-combining hook in the vendored OFDM data path,
**off by default** so the stock decode is bit-for-bit unchanged when disabled:

- `freedv_700.c` — after `symbols_to_llrs()`, if HARQ is enabled and soft info
  from a previous CRC-failed copy is held, the retained LLRs are **added** to the
  current frame's LLRs before `ldpc_decode_frame()`. On CRC success the held
  buffer is dropped; on failure the (already-combined) LLRs are retained so the
  next retransmission combines with *all* prior copies of the same frame.
- `freedv_api.c` / `freedv_api.h` — `freedv_set_harq(f, enable)` and
  `freedv_harq_reset(f)` (the caller must reset whenever the next frame is a NEW
  payload rather than a retransmission, so stale soft info is never combined
  across different codewords). Buffer allocated in the OFDM data open, freed in
  `freedv_close`.
- `freedv_data_raw_rx.c` — `--harq` flag to exercise it from the raw tools.

LLRs from independent noise/fade realisations of the *same* codeword add
coherently, so each retransmission lifts the effective Es/No (~3 dB per doubling
in AWGN; more under fading, where copies fade independently).

## Measurement

`freedv_data_raw_tx <mode> --testframes N --bursts N` emits N **bit-identical**
CRC-protected bursts (each its own preamble — exactly a Mercury retransmission).
Piped through `ch` (AWGN, and `--mpp` poor fading: 1 Hz Doppler / 2 ms), decoded
once normally and once with `--harq`. "deliv" = CRC-valid frames delivered.
AWGN SNR3k = `-No - 14.82` dB (measured from `ch`).

### AWGN, DATAC1 (510-byte frames), 200 identical bursts

| SNR3k   | single-shot | HARQ        |
|---------|-------------|-------------|
| +0.2 dB | **1/200** (0.5%) | **100/200** (50%) |
| −0.8 dB | 0/195 (0%)  | 94/195 (48%) |

### AWGN, DATAC3 (126-byte robust), 150 identical bursts

| SNR3k   | single-shot | HARQ        |
|---------|-------------|-------------|
| −3.8 dB | 140/149     | 140/149     |  ← above the cliff, both fine
| −4.8 dB | **6/143** (4%)   | **72/143** (50%) |
| −5.8 dB | 0/130 (0%)  | 61/130 (47%) |

### Poor fading (`ch --mpp`), DATAC1, ~150–170 bursts processed*

| mean SNR | single-shot | HARQ        |
|----------|-------------|-------------|
| ~7 dB    | 167/170     | 167/170     |
| ~5 dB    | 153/164     | 154/164     |
| ~3 dB    | 93/160 (58%) | 115/160 (72%) |
| ~1 dB    | **16/154** (10%) | **69/154** (45%) |

\* the stock `ch` poor-fading file truncates the run; the comparison is
apples-to-apples on the same processed input.

## Takeaway

In the cliff region the link is **dead single-shot** (FER ≈ 99–100%) but HARQ
delivers ~50% — i.e. ~one frame per **two** transmissions where plain ARQ
retransmission (independent copies) would need ~hundreds. That is a ~3 dB AWGN
coding gain and a **4–5× delivery gain at the fading cliff** (10%→45%). This is
the lever that makes a low target FER both optimal *and* robust, and the path to
beating v1.9.9/VARA at the fringe — not achievable at the ARQ layer alone.

## ARQ integration — LANDED (modem.c)

HARQ is now wired into Mercury's RX decoder (`rx_decoder_bind_mode`, `modem.c`):
on each mode bind it calls `freedv_harq_reset()` + `freedv_set_harq()`, enabling
combining for **data modes only** (not the DATAC16 control plane), gated by a
`MERCURY_HARQ=0` runtime kill-switch (`harq_enabled()`).

**Why this is safe with no per-frame reset logic:** every ARQ mode is
`burst_frames == 1` (one DATA frame per preamble — multi-frame bursts are still
dormant, `arq_protocol.c` mode table + `modem.c` `frames_per_burst=1`). So the
IRS only ever sees retransmissions of the single in-flight `rx_expected` frame
until it is delivered; freedv auto-clears the residual on CRC success, and the
mode-bind reset stops a pooled instance combining a stale failed frame with a
different payload after a mode excursion. The within-burst pollution hazard
(below) cannot occur until multi-frame bursts are switched on — at which point
the combiner must additionally gate to the first frame after each acquisition.

**Harness validation** (`tests/integration`, FIFO + ch): clean SNR PASS (HARQ a
no-op at 18 dB), AWGN cliff SN −5.1 dB PASS (transfer completes), no regression,
no crash under combining stress. A statistically meaningful goodput A/B is NOT
achievable in the harness — transfers are tiny, `ch` noise is unseeded, and the
SNR that makes the *data* mode fail also fails the no-HARQ *connect* handshake
(DATAC16). **The real A/B is the dummy load**: v1.9.9 vs OLLA vs OLLA+HARQ.

### Within-burst hazard (only relevant once multi-frame bursts are enabled)

Wire `freedv_set_harq` on the IRS data-RX path and reset at the right moments.
**Frame-identity subtlety (important):** the data path is NOT pure stop-and-wait
— it is go-back-N with multi-frame bursts (`arq_fsm.c` `tx_window`, `burst_max =
tm->burst_frames`, forced to 1 only during the startup window). Within one burst
the frames are seqs N, N+1, N+2 — *different* codewords. The naive running-sum
would combine across them and pollute the retained LLRs, which can turn an
otherwise-decodable frame into a CRC failure (a throughput regression, though
never data corruption — a bad combine just fails CRC and is retransmitted).

Correct rule, exploiting go-back-N: the retransmission of the in-flight
`rx_expected` is always the **first data frame after a fresh preamble
acquisition** (the sender resends from `rx_expected`). So:

1. Combine **only** the first post-acquisition data frame; for the 2nd..Nth
   frames in the same burst, decode without combining and without overwriting
   the retained buffer (they are new seqs, dropped anyway if `rx_expected`
   didn't decode). Needs a "frames-since-sync == 0" signal from the modem layer
   (`modem.c` `process_received_frame`, freedv `rx_status` sync bits).
2. `freedv_harq_reset()` whenever the FSM delivers a frame and advances
   `rx_expected` (`arq_fsm.c:517`), and on any mode change (a different mode is a
   different codeword — the pooled per-mode freedv instances already keep their
   buffers separate, but reset defends against returning to a stale instance).

Simplest safe first cut: gate HARQ on `burst_frames == 1` (true stop-and-wait —
the robust low-SNR modes where HARQ matters most and within-burst pollution
cannot occur), then generalise with the post-acquisition gate. Retransmits are
bit-identical (`tx_window[].buf`, same seq) so no wire-format change is needed.
Validate over the FIFO+ch harness at low SNR (No ≈ −16…−18, the cliff the
modem-layer proof used), then the dummy load and field stations.
