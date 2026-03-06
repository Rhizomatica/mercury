# Mercury Modem — Benchmark Procedure

This document describes the full benchmark suite for the Mercury modem fork.
It covers environment setup, individual test phases, and the single command
to run everything unattended.

---

## 1. Prerequisites

### Hardware / Software
- **OS**: Windows 10/11 (MSYS2/MinGW64 toolchain)
- **Audio loopback**: VB-Audio Virtual Cable (WASAPI)
  - TX device: `CABLE Input (VB-Audio Virtual Cable)`
  - RX device: `CABLE Output (VB-Audio Virtual Cable)`
  - For parallel sweep: VB-Cable A and VB-Cable B (up to 8 channels)
- **Python 3.8+** with packages: `numpy`, `sounddevice`, `matplotlib` (optional, for charts)

### Build
```bash
cd mercury
./build.sh o3
./build.sh install    # installs to C:\Program Files\Mercury\mercury.exe
```

Verify:
```bash
"C:\Program Files\Mercury\mercury.exe" --version
```

### Audio Verification
Ensure VB-Cable is working and no other application is using the audio devices:
```bash
python -c "import sounddevice; print(sounddevice.query_devices())"
```
Look for `CABLE Input` and `CABLE Output` in the device list.

### mercury.ini Check
Ensure `TxGainDb` and `RxGainDb` are both `0` in
`%APPDATA%\Mercury\mercury.ini`, or run with `-n` (nogui) mode where INI
gains are not applied. The benchmark uses `-T`/`-G` flags to control levels.

---

## 2. Test Phases

The benchmark suite consists of 10 test phases plus a report generation step.
Each phase can be run independently or as part of the `full` meta-command.

All commands below assume the working directory is `mercury/tools/`.

### Phase 1: BER Waterfall (PLOT_PASSBAND loopback)

Internal passband loopback — no VB-Cable needed. Runs `mercury.exe -m PLOT_PASSBAND`
for each config, collects BER vs Es/N0 data points, generates waterfall charts.

- **Configs**: 20 WB + 20 NB = 40 total (3 ROBUST + 17 OFDM per bandwidth)
- **Parallelism**: Up to 8 mercury.exe instances (CPU-only, no audio contention)
- **Time**: ~10 min with 8 workers

```bash
python mercury_benchmark.py ber --configs all-wb,all-nb --max-workers 8
```

**Output**: `ber_results.csv`, `ber_waterfall_wb.png`, `ber_waterfall_nb.png`

### Phase 2: WB SNR Sweep (throughput vs SNR)

ARQ loopback over VB-Cable with AWGN noise injection. Measures bytes/min at
each SNR level for every WB config with gearshift disabled (fixed config).

- **Configs**: 20 WB (ROBUST_0–2, CONFIG_0–16)
- **SNR range**: 30 dB → -20 dB in -3 dB steps (17 points per config)
- **Parallelism**: Up to 8 parallel sessions via multi-channel VB-Cable
- **Time**: ~45 min with 8 channels

```bash
python mercury_benchmark.py sweep --configs all-wb --parallel --max-workers 8 \
    --snr-start 30 --snr-stop -20 --snr-step -3 \
    --measure-duration 120 --settle-time 15
```

**Output**: per-config CSVs, `benchmark_sweep_*.png` charts

### Phase 3: NB SNR Sweep (throughput vs SNR)

Same as Phase 2 but for narrowband (500 Hz) configs.

- **Configs**: 20 NB (ROBUST_0–2_NB, CONFIG_0–16_NB)
- **Time**: ~45 min with 8 channels

```bash
python mercury_benchmark.py sweep --configs all-nb --parallel --max-workers 8 \
    --snr-start 30 --snr-stop -20 --snr-step -3 \
    --measure-duration 120 --settle-time 15
```

### Phase 4: WB Stress Test

Single WB session with gearshift enabled, subjected to 10 random noise bursts
at varying SNR levels. Tests gearshift response, BREAK recovery, and NAck handling.

- **Bursts**: 10, duration 60–180s each, SNR -3 to +15 dB
- **Start config**: ROBUST_0 (gearshift climbs from there)
- **Time**: ~35 min

```bash
python mercury_benchmark.py stress --num-bursts 10 --start-config 100 \
    --snr-low -3 --snr-high 15
```

**Output**: `stress_results.csv`, `benchmark_stress_timeline_*.png`

### Phase 5: NB Stress Test

Same as Phase 4 but for narrowband mode.

```bash
python mercury_benchmark.py stress --num-bursts 10 --start-config 100 \
    --snr-low -3 --snr-high 15 -N
```

### Phase 6: WB Adaptive Gearshift

Single WB session with gearshift enabled, SNR swept down from 30 dB to -15 dB
then back up. Measures which config the gearshift selects at each SNR level
and the resulting throughput.

- **SNR range**: 30 → -15 → 30 dB in 3 dB steps (round-trip)
- **Time**: ~55 min

```bash
python mercury_benchmark.py adaptive --snr-start 30 --snr-stop -15 --snr-step -3 \
    --measure-duration 120 --settle-time 15
```

**Output**: `adaptive_results.csv`, `benchmark_adaptive_*.png`

### Phase 7: NB Adaptive Gearshift

Same as Phase 6 but for narrowband mode.

```bash
python mercury_benchmark.py adaptive --snr-start 30 --snr-stop -15 --snr-step -3 \
    --measure-duration 120 --settle-time 15 -N
```

### Phase 8: Data Integrity

End-to-end data correctness verification. Sends known byte patterns through
the ARQ link and verifies received byte count. Tests a representative subset
of configs across the speed range.

- **Configs**: ROBUST_0, ROBUST_2, CONFIG_0, CONFIG_8, CONFIG_15 (WB)
- **Time**: ~20 min

```bash
python mercury_benchmark.py integrity --configs wb:100,wb:102,wb:0,wb:8,wb:15
```

**Output**: `integrity_results.csv`

### Phase 9: NB/WB Auto-Negotiation

Tests all 4 bandwidth negotiation combinations to verify the auto-negotiation
protocol selects the correct bandwidth. This is the one test where `-Q 0` is
NOT used — we want negotiation to happen.

- **Combos**: WB+WB, NB+NB, WB+NB, NB+WB
- **Iterations**: 3 per combo (12 total)
- **Time**: ~15 min

```bash
python mercury_benchmark.py negotiate --iterations 3
```

**Output**: `negotiate_results.csv`

### Phase 10: Connection Stability

Repeated connect/disconnect cycles to test for crashes, hangs, and resource
leaks. Each cycle starts fresh mercury.exe instances.

- **Configs**: ROBUST_0 (WB), CONFIG_0 (WB)
- **Cycles**: 10 per config (20 total)
- **Time**: ~20 min

```bash
python mercury_benchmark.py stability --count 10 --configs wb:100,wb:0
```

**Output**: `stability_results.csv`

### Phase 11: Report Generation

Automatically runs after all phases in `full` mode. Collates all CSVs and
chart paths into a single Markdown report with tables, chart references, and
replication commands for each phase.

**Output**: `RESULTS_SUMMARY.md`

---

## 3. Run Everything

A single command runs all 10 test phases in sequence, then generates the
collated report. Each phase runs in a try/except — if one fails, it logs
the error and continues to the next.

```bash
cd mercury/tools
python mercury_benchmark.py full --max-workers 8
```

Results are written to `./benchmark_results/full_YYYYMMDD_HHMMSS/` with
subdirectories per phase and a `progress.log` tracking timestamps.

### Estimated Time

| Phase | Test | Time |
|-------|------|------|
| 1 | BER waterfall (8 parallel) | ~10 min |
| 2 | WB sweep (8 channels) | ~45 min |
| 3 | NB sweep (8 channels) | ~45 min |
| 4 | WB stress | ~35 min |
| 5 | NB stress | ~35 min |
| 6 | WB adaptive | ~55 min |
| 7 | NB adaptive | ~55 min |
| 8 | Data integrity | ~20 min |
| 9 | NB/WB negotiation | ~15 min |
| 10 | Connection stability | ~20 min |
| 11 | Report generation | ~1 min |
| **Total** | | **~5.5 hours** |

### Signal Level

The default signal level is `-30.0 dBFS`. Mercury's native TX level is
`-4.4 dBFS` (WB) / `-11.1 dBFS` (NB). The benchmark automatically computes
TX attenuation (`-T`) and RX gain boost (`-G`) to match the target level.
Override with:

```bash
python mercury_benchmark.py full --max-workers 8 --signal-dbfs -25.0
```

---

## 4. Output Structure

```
benchmark_results/full_YYYYMMDD_HHMMSS/
├── progress.log                  # Phase timestamps and status
├── RESULTS_SUMMARY.md            # Collated report with tables and charts
├── ber/
│   ├── ber_results.csv
│   ├── ber_waterfall_wb.png
│   └── ber_waterfall_nb.png
├── sweep_wb/
│   ├── benchmark_sweep_*.csv     # Per-config throughput data
│   └── benchmark_sweep_*.png     # Throughput vs SNR charts
├── sweep_nb/
│   └── ...
├── stress_wb/
│   ├── stress_results.csv
│   └── benchmark_stress_timeline_*.png
├── stress_nb/
│   └── ...
├── adaptive_wb/
│   ├── adaptive_results.csv
│   └── benchmark_adaptive_*.png
├── adaptive_nb/
│   └── ...
├── integrity/
│   └── integrity_results.csv
├── negotiate/
│   └── negotiate_results.csv
└── stability/
    └── stability_results.csv
```

---

## 5. Re-running Individual Phases

If a phase fails or you want to re-run with different parameters, use the
individual subcommand. The `full` command's report generator reads CSVs from
the directory tree, so you can drop updated results into the same directory
and regenerate.

To list all available options for any subcommand:
```bash
python mercury_benchmark.py <subcommand> --help
```

---

## 6. VARA HF Comparison Sweep

Run the same SNR sweep against VARA HF for side-by-side comparison with
Mercury. VARA is a GUI application — you start it manually, the script
connects via TCP.

### Setup

1. **Start two VARA HF instances** with different TCP ports:
   - Instance A (responder): Settings → TCP port `8300`, audio = CABLE Output / CABLE Input
   - Instance B (commander): Settings → TCP port `8310`, audio = CABLE Output / CABLE Input
2. **Set VARA output level** to `-30 dBFS` via Windows mixer (right-click
   speaker → Volume Mixer → VARA). This matches the benchmark's default
   `--signal-dbfs -30` so noise injection is calibrated correctly.

### Run the sweep

```bash
python mercury_benchmark.py vara-sweep \
    --rsp-port 8300 --cmd-port 8310 \
    --bandwidth 2300 \
    --snr-start 30 --snr-stop -20 --snr-step -3 \
    --measure-duration 120 --settle-time 15
```

For NB comparison:
```bash
python mercury_benchmark.py vara-sweep \
    --rsp-port 8300 --cmd-port 8310 \
    --bandwidth 500 \
    --snr-start 30 --snr-stop -20 --snr-step -3 \
    --measure-duration 120 --settle-time 15
```

### Generate comparison chart

After both Mercury and VARA sweeps have run:

```bash
python mercury_benchmark.py compare \
    --mercury-csv benchmark_results/adaptive_wb/*.csv \
    --vara-csv benchmark_results/vara_sweep_*/*.csv \
    --output comparison_wb.png
```

**Output**: `comparison_wb.png` — Mercury configs as individual lines (blue),
VARA as a single adaptive line (red), both on the same throughput-vs-SNR axes.

---

## 7. Known Limitations

- **VB-Cable parallel**: Requires VB-Cable A and VB-Cable B for multi-channel
  mode. Single VB-Cable limits to 1 concurrent session.
- **NB CONFIG_0 START_CONNECTION**: OFDM CONFIG_0 `START_CONNECTION` is
  undecodable on VB-Cable (pre-existing upstream issue). NB sessions use
  ROBUST_0 for initial connection.
- **Bug #31**: `switch_narrowband_mode` can crash during NB probe Phase 2.
  All benchmark tests use `-Q 0` to disable probing (except `negotiate`,
  which tests the negotiation protocol itself).
- **matplotlib**: Required for chart generation. If not installed, CSVs are
  still produced but PNG charts are skipped.
