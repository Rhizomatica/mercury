# Mercury FreeDV Data Modes

Mercury uses the stock codec2 OFDM data modes plus two Mercury-specific custom
modes, `datac15` and `datac16`, defined in `modem/freedv/ofdm_mode.c`.  Both
are built on the rate-1/3 `H_256_768_22` LDPC code with 3-carrier narrowband
geometry, targeting operation below the SNR floor of `datac4`/`datac13`.

## Mode summary

In the spirit of the upstream
[codec2 README_data.md](https://github.com/drowe67/codec2/blob/main/README_data.md)
table.  Conventions: payload rate = (payload + 2 CRC bytes) × 8 / duration;
duration excludes the ~0.66 s preamble; FEC is (coded bits, data bits) after
shortening; MPP = multipath-poor channel (1 Hz Doppler, 2 ms delay spread),
single-frame bursts, 100 trials, "delivered" = acquired AND decoded.

| FreeDV Mode | RF bandwidth (Hz) | Payload data rate bits/s | Payload bytes/frame | FEC | Duration (sec) | MPP test | Use case |
|---|---|---|---|---|---|---|---|
| DATAC0  | 500  | 291 | 14  | (256,128)   | 0.44 | 70/100 at 0 dB ¹ | Reverse link ACK packets |
| DATAC1  | 1700 | 980 | 510 | (8192,4096) | 4.18 | 92/100 at 5 dB ¹ | Forward link data (medium SNR) |
| DATAC3  | 500  | 321 | 126 | (2048,1024) | 3.19 | 74/100 at 0 dB ¹ | Forward link data (low SNR) |
| DATAC4  | 250  | 87  | 54  | (1472,448)  | 5.17 | 90/100 at −4 dB ¹ ² | Forward link data (low SNR) |
| DATAC13 | 200  | 64  | 14  | (384,128)   | 2.0  | 90/100 at −4 dB ¹ ² | Reverse link ACK packets (low SNR) |
| DATAC14 | 250  | 58  | 3   | (112,56)    | 0.69 | 90/100 at −2 dB ¹ | Reverse link ACK packets (low SNR) |
| **DATAC15** | 200 | 68 | 30 | (768,256)  | 3.74 | 81/100 at −4 dB, 70/100 at −7 dB ² | Forward link data (very low SNR / fringe) |
| **DATAC16** | 200 | 42 | 14 | (640,128)  | 3.08 | 78/100 at −4 dB, 67/100 at −7 dB ² | Control/ACK packets (very low SNR / fringe) |
| **DATAC17** | 2100 | 1410 | 1180 | (15936,9456) | 6.71 | bench TBD (target ≈ +9 dB) | Forward link data (intermediate SNR, ~2× DATAC1 goodput) |
| QAM16C2 | 2100 | 3100 | 1213 | (16200,9720) | 3.2 | 90/100 at 15 dB ¹ | Forward link data (high SNR, ~2.9× DATAC1 goodput) |

¹ Upstream codec2 figures.
² Mercury bench (ch simulator, `--mpp`, Octave-generated fading file).  The
bench reproduces upstream DATAC13 exactly (90/100 at −4 dB) and DATAC4 within
0.5 dB (89/100 at −3.6 dB), so the DATAC15/16 rows are directly comparable.

## DATAC17 and QAM16C2: the fast end of the ladder

Under stop-and-wait ARQ every frame pays a ~5.3 s ACK cycle, so goodput — not
raw bps — picks the ladder order: DATAC1 ≈ 50 B/s, DATAC17 ≈ 98 B/s, QAM16C2
≈ 142 B/s.  DATAC17 is a Mercury custom mode: QPSK on the same rate-0.6
`H_16200_9720` codeword QAM16C2 uses, shortened to np=61 so its 1180-byte
frame stays distinct from QAM16C2's 1213 bytes (the ARQ infers the peer TX
mode from frame size).  QAM16C2 is ported from the Rhizomatica codec2 fork
(dr-qam16-cport); it runs unclipped (PAPR ≈ 10 dB), so on peak-limited
transmitters its effective +15 dB operating point is harder to reach than the
clipped QPSK modes' figures suggest.

## Why DATAC15/16: behavior below the DATAC4/13 floor

The new modes trade peak delivery rate for graceful degradation.  Their
longer frames span more fade cycles on MPP, which costs acquisitions at high
SNR (the ~85 % plateau below is acquisition-limited — decode failures are
near zero once acquired).  In exchange, the rate-1/3 code keeps delivering
where the rate-1/2 modes collapse.  Measured delivered/100 frames:

**MPP channel (1 Hz Doppler):**

| SNR (3 kHz) | DATAC15 | DATAC4 | DATAC16 | DATAC13 |
|---|---|---|---|---|
| −1 dB  | 87 | —  | 91 | —  |
| −3 dB  | 86 | 89 | 82 | 94 |
| −4 dB  | 81 | 84 | 78 | 90 |
| −5 dB  | 77 | 68 | 78 | 86 |
| −7 dB  | 70 | 41 | 67 | 73 |
| −9 dB  | 40 | 10 | 50 | 46 |
| −11 dB | 11 | 1  | 29 | 25 |

**AWGN channel:** ³

| SNR (3 kHz) | DATAC15 | DATAC4 | DATAC16 | DATAC13 |
|---|---|---|---|---|
| −7 dB  | 100 | 95 | 95 | 100 |
| −8 dB  | 95  | 65 | 95 | 100 |
| −9 dB  | 95  | 5  | 96 | 98 |
| −10 dB | 80  | 0  | 82 | 81 |
| −11 dB | 20  | 0  | 52 | 28 |

³ Per-mode SNRs vary ±0.6 dB around the row label (ch sets noise density, not
SNR).  DATAC15/DATAC4 cells and the −7/−8 dB DATAC16 cells are 20-trial runs
(±5 %); the rest are 100-trial.  AWGN 90 %-delivery points: DATAC15 ≈ −9.5 dB
vs DATAC4 ≈ −7.5 dB (the ~2 dB payload-floor extension); DATAC16 ≈ −9.3 dB vs
DATAC13 ≈ −9.6 dB, with DATAC16 degrading much more gracefully below that.

Operational consequences, reflected in the ARQ design (see `docs/ARQ.md`):

- **DATAC15** is the payload-ladder floor and initial mode.  Including the
  full ARQ cycle overhead (frame + guards + DATAC16 ACK ≈ frame + 5.3 s),
  goodput crosses over vs DATAC4 at ≈ −7.5 dB MPP / −8.5 dB AWGN; the
  ladder's `ARQ_SNR_MIN_DATAC4_DB = −6 dB` upgrade threshold (−5 dB effective
  with hysteresis) leaves ~1.5 dB margin to the crossover and ~3 dB to
  DATAC4's AWGN cliff.
- **DATAC16** replaced DATAC13 as the ARQ control mode.  At moderate SNR the
  two are comparable (ARQ retries absorb the difference); below −9 dB DATAC16
  delivers roughly twice as many control frames, which is what keeps a fringe
  session alive.
- Acquisition (preamble/UW detection) is the dominant loss for both new modes
  above −5 dB MPP — a future tuning candidate (`timing_mx_thresh`, UW length)
  independent of the FEC design.

## Reproducing the measurements

```sh
cd modem/freedv
# fading file (once; needs octave + signal pkg):
octave-cli -qf --eval 'pkg load signal; ch_fading("unittest/fast_fading_samples.float", 8000, 1.0, 8000*620)'
# (run from original_docs/octave, or add it to the octave path)

./freedv_data_raw_tx --testframes 100 --bursts 100 DATAC15 /dev/zero - | \
  ch - - --No -10 --mpp --fading_dir unittest | \
  freedv_data_raw_rx --testframes DATAC15 - /dev/null
```

`ch` reports the resulting SNR3k on stderr; delivered = `Tfrms − Tfers` from
the RX summary.  Drop `--mpp` for AWGN.  End-to-end ARQ behavior under noise
can be exercised with `tests/integration` (`MERCURY_CH_NO=-6 go test -run
TestMercuryARQTransfer`).
