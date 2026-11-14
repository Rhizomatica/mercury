# Efficiency and performance: throughput, airtime, and CPU on current trunk

Measured 2026-11-12/13 on `mercuryv2` at `237481e7`, bench and simulation
only, no radio. **Freshness note (2026-11-14): trunk has moved 62 commits
and one release since (v1.9.24), including #545, which changes the rung
estimate to prefer data frames' readings (#501's fix), #548, and #538 --
the very machinery these rows exercise. The tables below describe
`237481e7`; re-running the grid on current trunk is a rerun of the same
drivers, not a rewrite.** Three instruments: the integration harness
(`TestMercuryARQTransfer`, `MERCURY_CH_ENGINE=watterson`, fixed seeds), the
carousel simulator (`carousel_bench` with `SIM_STATS=1` and a new per-station
air/keydown line), and two CPU probes (`utils/modem_cpu_probe`, new, next to
`utils/idle_cpu_probe`). Raw rows: `docs/channel-figures/throughput_1.9.23.csv`
and `docs/channel-figures/efficiency_airtime.csv`.

## Answer

**The stack is CPU-cheap and channel-honest; the cost of a session is the
protocol around the data, not the modem.** Concretely:

- **Throughput is ahead of VARA's 2020 published figures at the SNRs that
  matter on HF, and behind them above ~13 dB.** On all three channels the
  32 KB transfer runs 1.2-2.8x VARA's bps at 5-10 dB SNR3k; from ~13 dB up
  the ladder's 16-QAM rung caps Mercury near 1950 bps on AWGN and 1400 on
  Poor while VARA climbs to 2330-4990. This is the same shape the 2020
  comparison found, on a current build.
- **A message-session's airtime is mostly not data.** An 8 KB bulk transfer
  spends 68-89 % of its on-air time on data frames; an exchange of short
  messages on the same channel spends 50-78 %, the rest going to control
  frames (19-50 %), the floor and patterns (4-7 %), and silence (19-31 %).
- **The modem is nowhere near the CPU wall.** Listening costs 1.5-3.3 % of
  a core per mode on x86; talking (encode + in-sync decode) 0.8-3.7 %,
  QAM16C2 the expensive one. Scaled conservatively to a Pi 4 core the worst
  mode is still under a fifth of one core.

## 1. Throughput (integration harness, watterson engine)

Method: `MERCURY_CH_ENGINE=watterson`, `MERCURY_CH_FADING` one of
`ccir-good`/`ccir-poor` (awgn: no fading preset), `MERCURY_CH_NO` on the
same even grid the earlier sweep used (-20 to -40, 4 dB steps), payloads
4096 and 32768 bytes, seeds 1 and 2 (`MERCURY_TEST_SEED`, the fixed-channel
mode), one run per cell except where noted. SNR3k = -No - 14.82. Wall time
is go test's own measure of the test; bps is payload over wall minus
connect (the connect phase measured 7.8-7.9 s in every run). Every cell
passed; no timeouts.

### Against the same cells on the earlier engine comparison

`throughput_old_vs_standard.csv` recorded wall seconds per cell (host
"mac"/"marvin", older build). Where cells match, the deltas are within
noise in both directions (-48 % to +30 %, median +4 %): **the throughput
this study's cells measure is the earlier study's**, and nothing in the
1.9.x scoring and teardown work moved it systematically. One cell moved in
kind, not degree: `ccir-poor` at No=-20 -- 4096 B, the cell the earlier
sweep logged as a 6882 s **failure** -- passes in 97 s now (seeds 1 and 2,
95 and 100 s).

### Against VARA's published 2020 figures

N5TW's IONOS comparison read from the charts (+-5 %), same channel
definitions and SNR3k convention as the standard presets. Our SNR points
sit 0.2-2.8 dB off VARA's 5 dB grid; the nearest point is used, so each
ratio carries that skew plus chart-reading error. n=2 seeds per cell, so
each row is a mean of two fixed realisations, not a distribution.

| arm | No | SNR3k | size | Mercury bps | VARA bps | Mercury/VARA |
|---|---|---|---|---|---|---|
| awgn | -20 | 5.2 | 32768 | 701 | 310 (5 dB) | 2.26 |
| awgn | -20 | 5.2 | 4096 | 451 | 310 (5 dB) | 1.45 |
| awgn | -24 | 9.2 | 32768 | 976 | 710 (10 dB) | 1.37 |
| awgn | -28 | 13.2 | 32768 | 1787 | 1510 (15 dB) | 1.18 |
| awgn | -32 | 17.2 | 32768 | 1933 | 1510 (15 dB) | 1.28 |
| awgn | -36 | 21.2 | 32768 | 1983 | 3150 (20 dB) | 0.63 |
| awgn | -40 | 25.2 | 32768 | 1954 | 4990 (25 dB) | 0.39 |
| ccir-good | -20 | 5.2 | 32768 | 295 | 170 (5 dB) | 1.74 |
| ccir-good | -24 | 9.2 | 32768 | 799 | 400 (10 dB) | 2.00 |
| ccir-good | -28 | 13.2 | 32768 | 755 | 870 (15 dB) | 0.87 |
| ccir-good | -32 | 17.2 | 32768 | 1284 | 870 (15 dB) | 1.48 |
| ccir-good | -36 | 21.2 | 32768 | 1639 | 1570 (20 dB) | 1.04 |
| ccir-good | -40 | 25.2 | 32768 | 1677 | 2590 (25 dB) | 0.65 |
| ccir-poor | -20 | 5.2 | 32768 | 552 | 200 (5 dB) | 2.76 |
| ccir-poor | -24 | 9.2 | 32768 | 866 | 490 (10 dB) | 1.77 |
| ccir-poor | -28 | 13.2 | 32768 | 853 | 950 (15 dB) | 0.90 |
| ccir-poor | -32 | 17.2 | 32768 | 1070 | 950 (15 dB) | 1.13 |
| ccir-poor | -36 | 21.2 | 32768 | 1406 | 1450 (20 dB) | 0.97 |
| ccir-poor | -40 | 25.2 | 32768 | 1386 | 2330 (25 dB) | 0.59 |

What it says: ahead at and below ~10 dB everywhere, crossing at ~13 dB, and
capped above -- on AWGN at 25 dB Mercury holds 1954 bps against VARA's
4990. The 4 KB rows run 40-75 % of the 32 KB rows at the same SNR: the
fixed cost of a session (connect, ladder climb, disconnect, the polls
between turns) is paid whole by a small message. That cost is the next
section's subject.

## 2. Where the airtime goes (carousel simulator)

Method: `carousel_bench` with `SIM_STATS=1`, 10 seeds per cell, on six
channels; traffic is an 8 KB one-way transfer ("bulk"), an 8-message
exchange with turns every 2-4 s ("dense"), and five 30-byte writes spaced
~9 s ("sparse" -- a request/response or typed-terminal session). SIM_STATS
separates the control mode's (DATAC16) air from the data modes'; the floor
and patterns are the residual against each station's total air. Raw rows in
`efficiency_airtime.csv`.

Mean share of *on-air* time (both stations), and the silent share of the
session:

| channel | traffic | data % | control % | floor % | silent % of session | keys |
|---|---|---|---|---|---|---|
| awgn:0.02 | bulk | 68.3 | 24.4 | 7.3 | 9.2 | 7 |
| awgn:0.02 | dense | 47.1 | 45.6 | 7.2 | 25.0 | 19 |
| awgn:0.02 | sparse | 42.5 | 50.1 | 7.4 | 31.2 | 14 |
| cliff:8 | bulk | 80.0 | 16.6 | 3.4 | 6.4 | 7 |
| cliff:8 | dense | 57.5 | 37.5 | 5.0 | 21.1 | 20 |
| cliff:8 | sparse | 50.0 | 44.3 | 5.7 | 28.6 | 14 |
| cliff:3 | bulk | 89.4 | 6.7 | 3.9 | 10.4 | 28 |
| cliff:3 | dense | 79.3 | 16.3 | 4.4 | 14.3 | 44 |
| cliff:3 | sparse | 76.1 | 19.2 | 4.6 | 18.8 | 24 |
| fade:8:0.5 | bulk | 77.6 | 17.9 | 4.4 | 16.0 | 23 |
| fade:8:0.5 | dense | 65.0 | 29.8 | 5.2 | 22.6 | 39 |
| fade:8:0.5 | sparse | 61.5 | 33.0 | 5.5 | 26.0 | 23 |
| fade:3:0.5 | bulk | 86.9 | 9.2 | 3.9 | 11.3 | 44 |
| fade:3:0.5 | dense | 77.0 | 18.4 | 4.6 | 16.3 | 71 |
| fade:3:0.5 | sparse | 76.5 | 19.0 | 4.5 | 18.7 | 36 |
| nvis | bulk | 78.3 | 17.4 | 4.3 | 16.5 | 128 |
| nvis | dense | 70.8 | 24.4 | 4.8 | 21.6 | 180 |
| nvis | sparse | 71.5 | 23.5 | 5.1 | 22.1 | 88 |

What it says:

- **Message traffic pays a control tax bulk never sees.** Bulk keeps data
  at 68-89 % of air; the same channel carrying short messages spends 19-50 %
  of its air on DATAC16 control frames. The sparse session is the worst
  case by design -- each small write lands in a session whose turn
  machinery (poll, ack, handover) costs more than the payload.
- **The silent share is 6-31 %.** A session's wall time is not its air
  time: between 6 % (clean fast channel, bulk) and 31 % (quiet channel,
  sparse writes) of the session is nobody keyed. The sparse cases are
  #495's regime -- the caller holding its next keydown for a response slot
  nothing came in -- and their silent share is what the early slot release
  buys back.
- **Bytes per keydown collapse with message size**: ~1138 B/keydown for
  bulk on a clean channel against 18-113 for sparse writes on the same
  channels. The per-keydown fixed cost (lead, tail, poll, the answer
  round trip) is what that number is dividing by.
- These shares are simulator numbers; the integration runs corroborate the
  totals (their bps above is payload over the same wall) but not the split
  -- no integration log records air per mode today. A `-L` timing log
  parsed the way `onair_logs.py` does would close that; not done here.

## 3. CPU (utils/modem_cpu_probe, utils/idle_cpu_probe)

Method: x86 (this bench box), one core, `CLOCK_PROCESS_CPUTIME_ID`.
Talking: 200 bursts per mode, raw-data encode and `freedv_rawdatarx` in
sync at +10 dB SNR3k awgn, sync held across frames (steady state; the
first burst's acquisition amortised away). Listening: `idle_cpu_probe`,
120 s of noise per mode, stock threshold. The MFSK floor is a different
modem and is not in this table (its detector's CPU is
`pattern_probe`'s cpu mode; its LDPC decode is in `mfsk_rx_probe`'s
territory).

| mode | bytes/frame | air s/frame | encode CPUx | decode CPUx | idle CPU% |
|---|---|---|---|---|---|
| DATAC3 | 128 | 3.41 | 0.003 | 0.004 | 2.5-3.1 |
| DATAC1 | 512 | 4.40 | 0.004 | 0.006 | 3.0-3.2 |
| DATAC17 | 1182 | 6.93 | 0.004 | 0.007 | 2.6-3.0 |
| QAM16C2 | 1215 | 3.30 | 0.004 | 0.024 | 1.5-1.6 |
| DATAC16 (control) | 16 | 3.30 | 0.003 | 0.007 | 2.7-3.3 |
| DATAC15 | 32 | 3.96 | 0.003 | 0.007 | 2.5-3.3 |
| DATAC4 | 56 | 5.39 | 0.003 | 0.006 | 2.3-3.0 |

CPUx = CPU seconds per second of audio (multiply by 100 for percent of one
core). All 100 of 100 frames delivered per mode; every mode decodes on this
channel, so no row is the CPU of a decoder that never commits a frame.

What it says:

- **The modem is not the bottleneck on any hardware this project targets.**
  Full-duplex talking is 0.7 % (DATAC3) to 2.9 % (QAM16C2) of one x86 core;
  listening 1.5-3.3 %. A Pi 4 core is conservatively 3-5x slower per clock
  on this kind of DSP, which puts the worst case -- QAM16C2 encode plus
  decode -- under a sixth of one core, with the always-on receive another
  sixth at most. A Pi runs the whole ladder with room for the UI, the TNC
  socket work and the audio stack.
- **QAM16C2's decode is the outlier** (2.4 % against 0.4-0.7 % for every
  other mode): the 1215-byte frame at 3.3 s air is the top rung's rate,
  paid for in decoder work. It is also the rung the ladder reaches least
  often on fading channels, so its cost is rare by construction.
- **Idle cost is flat across modes** (~1.5-3.3 %): the acquisition search,
  not the payload demodulator, is what listening spends. That is why
  #455/#470's acquisition knobs traded whole idle percents -- and why the
  wider +-150 Hz search (2.5x idle CPU, ACQ-TUNING-FINDINGS) remains the
  largest CPU number in the project, still under a tenth of a core.

## What this does not say

- Two fixed realisations per throughput cell (seeds 1 and 2): no
  distribution, no tails. The 2020 comparison's own four-seed spread on
  Poor at 25 dB was 857-1379 bps; our cells carry the same kind of
  variance the fixed seeds hide.
- The VARA figures are 2020 published numbers read off charts (+-5 %),
  of VARA HF 2300 -- not current VARA, and not VARA FM or VARA P2P.
- The airtime shares are the simulator's, at 12 dB nominal; a real band's
  noise, QSB and other stations will move the silent share up and the
  ladder's reach down.
- The CPU numbers are one x86 build, -O2; the Pi 4 statement is scaled
  arithmetic, not a Pi measurement.
- The MFSK floor's CPU is not measured here (pattern_probe covers its
  detector; the LDPC decode path has no probe yet).

## Reproduce

```
make -C tests test_carousel carousel_bench          # the sim + bench
make -C utils modem_cpu_probe idle_cpu_probe        # the CPU probes
tests/carousel_bench 4 fade:3:0.5                   # bulk
SIM_STATS=1 CAR_SCRIPT='A30+0,B30+9000,A30+18000,B30+27000,A30+36000' \
  tests/carousel_bench 1 cliff:8                    # sparse writes
utils/modem_cpu_probe 200                           # the CPU table
utils/idle_cpu_probe DATAC16 120                    # one listening row
cd tests/integration && MERCURY_CH_ENGINE=watterson \
  MERCURY_CH_FADING=ccir-poor MERCURY_CH_NO=-24 \
  MERCURY_TEST_PAYLOAD_B=32768 MERCURY_TEST_SEED=1 \
  go test -run 'TestMercuryARQTransfer$' -v -count=1
```

The sweep drivers are checked in next to the rows they produced:
`throughput_sweep.sh` (one channel's grid column, calling
`throughput_sweep_cell.sh` per cell) and `airtime_sweep.sh` with
`airtime_parse.py` (the accounting rows).
