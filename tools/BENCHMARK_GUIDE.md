# Mercury Benchmark Guide

## Prerequisites

- Mercury installed at `C:\Program Files\Mercury\mercury.exe` (or pass `--mercury PATH`)
- VB-Audio Virtual Cable installed and working
- Python packages: `numpy`, `sounddevice` (required), `matplotlib` (optional, for charts)

```
pip install numpy sounddevice matplotlib
```

## Quick Start

**Smoke test** (5 min) — verify everything works with a single config and short measurements:
```
python mercury_benchmark.py sweep --configs 0 --snr-start 20 --snr-stop 5 --snr-step -5 --measure-duration 30
```

**Quick benchmark** (~1.5-2 hours) — representative subset of configs with shorter measurements:
```
python mercury_benchmark.py sweep --configs 100,0,8,16 --measure-duration 60 --snr-step -5
python mercury_benchmark.py stress --num-bursts 5 --min-dur 60 --max-dur 90
python mercury_benchmark.py adaptive --measure-duration 60 --snr-step -5
```

**Full benchmark** (~5-6 hours) — all configs, fine SNR resolution, thorough stress test:
```
python mercury_benchmark.py sweep
python mercury_benchmark.py stress
python mercury_benchmark.py adaptive
```

## Sub-commands

### `sweep` — Fixed-Config SNR Sweep

Generates a VARA-style performance chart. For each config, starts a fresh modem pair at that config (no gearshift), connects, then steps through SNR levels from high to low, measuring throughput at each point.

```
python mercury_benchmark.py sweep [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--configs` | `100,101,102,0,2,4,6,8,10,12,14,16` | Comma-separated config numbers to test |
| `--snr-start` | `30` | Starting SNR in dB (high = less noise) |
| `--snr-stop` | `-5` | Ending SNR in dB (low = more noise) |
| `--snr-step` | `-3` | Step between SNR points (negative) |
| `--measure-duration` | `120` | Seconds to measure at each SNR point |
| `--settle-time` | `15` | Seconds of silence between SNR points |

**How it works:**
1. For each config, starts commander + responder at that config with gearshift disabled
2. Connects and waits for data to start flowing
3. At each SNR level, injects AWGN noise and measures how many bytes get through
4. If throughput drops to 0 for 2 consecutive SNR points (waterfall), skips the rest
5. Kills processes and moves to the next config

**Output:** CSV with per-config-per-SNR throughput data + PNG chart (if matplotlib installed)

**Examples:**
```
# Just ROBUST modes
python mercury_benchmark.py sweep --configs 100,101,102 --measure-duration 90

# High-speed configs only, fine SNR resolution
python mercury_benchmark.py sweep --configs 12,14,16 --snr-start 25 --snr-stop 5 --snr-step -2

# Fast scan across all configs
python mercury_benchmark.py sweep --measure-duration 60 --snr-step -5
```

### `stress` — Random Noise Stress Test

Tests modem robustness under adversarial conditions. Starts with turboshift to find the channel ceiling, then hits the link with random noise bursts of varying duration and amplitude.

```
python mercury_benchmark.py stress [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--num-bursts` | `10` | Number of noise bursts |
| `--min-dur` | `60` | Minimum burst duration (seconds) |
| `--max-dur` | `180` | Maximum burst duration (seconds) |
| `--snr-low` | `-3` | Lowest burst SNR (strongest noise) |
| `--snr-high` | `15` | Highest burst SNR (weakest noise) |
| `--start-config` | `100` | Starting config (100 = ROBUST_0) |
| `--seed` | random | Random seed for reproducible runs |

**How it works:**
1. Starts at ROBUST_0 with gearshift enabled, turboshifts to ceiling config
2. Generates a random schedule of noise bursts (each with random duration and SNR)
3. Executes each burst: noise on, monitor throughput + events, noise off, recovery period
4. Tracks gearshift, BREAK, NAck events throughout
5. Final 60s recovery period after all bursts

**Output:**
- Per-burst CSV (bytes during noise, bytes during recovery, events)
- Timeline CSV (per-second: cumulative bytes, noise state)
- Timeline chart showing throughput vs noise periods
- Full modem log

**Examples:**
```
# Quick stress: 5 moderate bursts
python mercury_benchmark.py stress --num-bursts 5 --min-dur 60 --max-dur 90

# Brutal: strong noise, long bursts
python mercury_benchmark.py stress --num-bursts 8 --min-dur 120 --max-dur 180 --snr-low -5 --snr-high 5

# Reproducible run
python mercury_benchmark.py stress --seed 42
```

### `adaptive` — Gearshift SNR Sweep

Like `sweep` but with gearshift enabled in a single session. Starts at ROBUST_0, turboshifts to ceiling, then sweeps SNR downward (and optionally back up). Shows how the modem naturally adapts its config as channel conditions change.

```
python mercury_benchmark.py adaptive [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--snr-start` | `30` | Starting SNR (dB) |
| `--snr-stop` | `-5` | Ending SNR (dB) |
| `--snr-step` | `-3` | SNR step (negative) |
| `--measure-duration` | `120` | Seconds per SNR point |
| `--settle-time` | `15` | Recovery between points |
| `--no-round-trip` | off | Skip the sweep back up |

**How it works:**
1. Starts at ROBUST_0 with gearshift, turboshifts to ceiling
2. Sweeps SNR from high to low, letting gearshift adapt the config at each level
3. Optionally sweeps back up to test recovery and gear-up behavior
4. Records throughput + which config the modem chose at each SNR level

**Output:** CSV with SNR, throughput, direction (DOWN/UP), config at each point + chart

**Examples:**
```
# Quick adaptive sweep
python mercury_benchmark.py adaptive --measure-duration 60 --snr-step -5

# One-way only (no return sweep)
python mercury_benchmark.py adaptive --no-round-trip

# Fine resolution around the interesting range
python mercury_benchmark.py adaptive --snr-start 20 --snr-stop 0 --snr-step -2 --measure-duration 90
```

## Common Options

These apply to all sub-commands:

| Option | Default | Description |
|--------|---------|-------------|
| `--mercury` | `C:\Program Files\Mercury\mercury.exe` | Path to mercury executable |
| `--output-dir` | `./benchmark_results` | Where CSV, charts, and logs are saved |
| `--timeout` | `600` | Per-scenario timeout (seconds) |

## Output Files

All output goes to `--output-dir` (default: `./benchmark_results/`), timestamped:

```
benchmark_results/
  benchmark_sweep_20260212_143000.csv       # Sweep data
  benchmark_sweep_20260212_143000.png       # Sweep chart
  logs_20260212_143000/                     # Per-config modem logs
    ROBUST_0.log
    CONFIG_0.log
    ...
  benchmark_stress_20260212_180000.csv      # Stress per-burst data
  benchmark_stress_timeline_20260212_180000.csv  # Stress per-second timeline
  benchmark_stress_20260212_180000.png      # Stress timeline chart
  benchmark_stress_20260212_180000.log      # Stress modem log
  benchmark_adaptive_20260212_190000.csv    # Adaptive sweep data
  benchmark_adaptive_20260212_190000.png    # Adaptive chart
  benchmark_adaptive_20260212_190000.log    # Adaptive modem log
```

## Time Estimates

| Run | Configs | SNR Points | Measure Time | Total Estimate |
|-----|---------|-----------|-------------|----------------|
| Sweep (full) | 12 | 12 per config | 120s | ~5-6 hours |
| Sweep (quick) | 4 | 8 per config | 60s | ~50 min |
| Stress (full) | n/a | 10 bursts | avg 120s | ~35 min |
| Stress (quick) | n/a | 5 bursts | avg 75s | ~15 min |
| Adaptive (full) | n/a | 23 (round trip) | 120s | ~55 min |
| Adaptive (quick) | n/a | 15 (round trip) | 60s | ~20 min |

The sweep time is a worst case — waterfall detection skips remaining SNR points once throughput drops to 0 for 2 consecutive measurements, which typically saves 30-50% of the time.

## Config Reference

| Config | Name | Modulation | Max Throughput |
|--------|------|-----------|---------------|
| 100 | ROBUST_0 | 32-MFSK | ~14 bps |
| 101 | ROBUST_1 | 16-MFSK | ~22 bps |
| 102 | ROBUST_2 | 16-MFSK | ~87 bps |
| 0 | CONFIG_0 | BPSK 1/16 | 84 bps |
| 1-4 | CONFIG_1-4 | BPSK-QPSK | 168-421 bps |
| 5-9 | CONFIG_5-9 | QPSK-8PSK | 631-1682 bps |
| 10-13 | CONFIG_10-13 | 8PSK-16QAM | 2102-3363 bps |
| 14-16 | CONFIG_14-16 | 16QAM-32QAM | 3924-5735 bps |

## Tips

- **Start with a smoke test** to make sure VB-Cable and mercury are working before committing to a multi-hour run.
- **Use `--seed`** with the stress test if you want to reproduce a specific failure.
- **If matplotlib isn't installed**, the script still works — it saves CSV data and prints a message about the missing chart. You can plot the CSV later with any tool.
- **Lower `--measure-duration`** trades accuracy for speed. 60s is reasonable for fast configs (CONFIG_8+), but slow configs (ROBUST_0, CONFIG_0) may only complete 1-2 ARQ round trips in 60s, giving noisy throughput numbers. 120s is safer.
- **The sweep chart** uses a log scale for throughput (Y axis) since configs span 14 bps to 5735 bps.
