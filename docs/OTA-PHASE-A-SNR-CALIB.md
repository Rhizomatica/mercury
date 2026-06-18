# Phase A OTA Test — Per-Mode SNR Calibration (get off the DATAC15 floor)

## Why
On uucp transfers mercuryv2 was pinned to the DATAC15 floor (~30 bps) because
OLLA's per-mode SNR estimate was inconsistent: the OFDM Es/No estimator reads
symbol-magnitude variance, which is mode-dependent, so the faster modes
under-reported SNR by 3–10 dB (QAM16C2 ~9 dB low). OLLA measures peer SNR on
whatever payload mode is currently flying, so the instant it climbed off
DATAC15 the faster mode under-reported and OLLA false-downgraded straight back
to the floor (the "gear-shift oscillation").

## The fix (branch `exp-snr-calib`)
1. **Per-mode SNR calibration** — `freedv_snr_calib()` in
   `modem/freedv/freedv_700.c` adds per-mode offsets (DATAC4 +1, DATAC1 +3,
   DATAC17 +3.5, DATAC3 +5, QAM16C2 +9 dB) so every mode reports the true SNR3k
   scale the `ARQ_SNR_MIN_*` thresholds are defined against. Deterministically
   validated vs the codec2 ch.c AWGN reference: post-fix all modes read within
   ~1–2 dB of true across 6–15 dB (was a 6–10 dB spread).
2. **peer_snr_valid** — `datalink_arq/arq_fsm.c` no longer treats a genuine 0 dB
   report (`snr_raw=128` → 0.0 dB) as "no reading", so climbing isn't stalled at
   the fade cliff.
3. A throttled `OLLA-state:` log (every 4 s, INF) so you can watch the climb.

**Bench result (estacao2↔estacao3, dummy load):** on a 12–15 dB link OLLA
climbed `DATAC15 → DATAC3 → DATAC17` (mode 22→12→24) and held DATAC17 — was
stuck on DATAC15. This OTA test confirms it on a real São Roque ↔ Belo Horizonte
link.

## Build & deploy — each station, as root, from /root/mercury
    cd /root/mercury
    sudo git fetch origin exp-snr-calib
    sudo git checkout -B exp-snr-calib FETCH_HEAD
    sudo make -j4
    # gentle restart — do NOT restart sbitx (it drifts the gain):
    sudo pkill -TERM mercury; sleep 5; sudo pkill -9 mercury; sleep 1
    sudo install -m755 /root/mercury/mercury /usr/bin/mercury
    cd /root/mercury && sudo setsid -f /usr/bin/mercury -v -S >/root/mercury/manual.log 2>&1 </dev/null
    sleep 14
Verify: `pgrep -c mercury` = 1 on both, `pgrep -c sbitx` = 1 (untouched).

## CHECK LINK SYMMETRY FIRST (important)
The dummy-load bench had a strong asymmetry — one direction ~0 dB, the other
~12 dB — which caps throughput (ARQ needs BOTH the DATA path and the ACK path).
Before the real run, do a short transfer each way and read the receiver's SNR:
    sudo grep -aoE "snr=[-0-9.]+ sync=[0-9]" /root/mercury/manual.log | tail
Both directions should show healthy SNR. If one is much weaker, fix TX drive /
RX gain on that radio before trusting the throughput numbers.

## Run the test (standard uucp HFP path)
On the CALLER (replace PU2UIT-X with the OTHER station's uucp name):
    head -c 20480 /dev/urandom | sudo tee /root/test20k >/dev/null
    uucp -r /root/test20k 'PU2UIT-X!~/t20'
    sudo setsid -f uucico -f -S PU2UIT-X >/dev/null 2>&1 </dev/null
Stale lock? `sudo rm -f /var/lock/LCK..*` on BOTH stations, then retry.

## What to read
- **The climb** (caller, /root/mercury/manual.log):
    sudo grep -aE "OLLA-state|Mode negotiation|Ladder" /root/mercury/manual.log
  Expect `Mode negotiation: 22 -> ... -> 24` (DATAC15→…→DATAC17), and 25
  (QAM16C2) if SNR ≥ ~18 dB. `peer_snr=` should be non-zero and track the link.
- **Throughput** (caller, uucp Log):
    sudo grep -a "Call complete" /var/log/uucp/Log | tail -1
  → `Call complete (<sec> seconds <bytes> bytes <bps> bps)`.
- **Mode at receiver** (other station):
    sudo grep -aE "Decoded frame mode=" /root/mercury/manual.log | tail | grep -oE "mode=[0-9]+ \([A-Z0-9]+\)"

## Targets
| mode (where OLLA lands) | ARQ goodput | needs ~SNR |
|---|---|---|
| DATAC15 (old floor)     | ~30 bps     | any        |
| DATAC1                  | ~510 bps    | +3 dB      |
| DATAC17                 | ~890 bps    | +7 dB      |
| QAM16C2                 | ~1405 bps   | +13 dB     |
| VARA-HF (reference)     | ~600–2800   | adaptive   |

Decisively beating VARA across the SNR range also needs Phase B (multi-frame
bursts, K>1) to remove the stop-and-wait dead-air tax — that is the next work
item after this OTA confirms the mode-climb.

## Rollback
    sudo install -m755 /root/mercury.mv2fix /usr/bin/mercury   # last known-good
    # then the gentle restart above.
