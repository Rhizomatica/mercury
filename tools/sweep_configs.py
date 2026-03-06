#!/usr/bin/env python3
"""
Sweep all Mercury configs (NB + WB) with compression, fastest first.
Runs compression_test.py for each config and collects results.

Usage:
  python tools/sweep_configs.py --file tools/pg84_15k.txt
"""

import argparse
import os
import re
import subprocess
import sys
import time

# Duration per config — faster configs need less time.
# CONFIG_16 ≈ 5735 bps, CONFIG_0 ≈ 84 bps
# Just enough for a few batches — throughput measurement, not endurance.
CONFIG_DURATIONS = {
    16: 45, 15: 45, 14: 45, 13: 45, 12: 45,
    11: 60, 10: 60, 9: 60, 8: 60, 7: 90,
    6: 90, 5: 90, 4: 120, 3: 120, 2: 180,
    1: 180, 0: 300,
}

def run_config(config, mode, file_path, wideband, duration):
    """Run a single compression test and return results dict."""
    bw_label = "WB" if wideband else "NB"
    print(f"\n{'='*60}")
    print(f"  CONFIG_{config} {bw_label} — duration {duration}s")
    print(f"{'='*60}")

    cmd = [
        sys.executable, "tools/compression_test.py",
        "--config", str(config),
        "--mode", mode,
        "--duration", str(duration),
    ]
    if file_path:
        cmd += ["--file", file_path]
    if wideband:
        cmd += ["--wideband"]

    env = os.environ.copy()
    env["PYTHONUNBUFFERED"] = "1"

    result = {"config": config, "bw": bw_label, "bps": 0, "bytes": 0,
              "integrity": "UNKNOWN", "ack_gate": "UNKNOWN", "suppressions": 0}

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=duration + 180, env=env)
        output = proc.stdout + proc.stderr
        print(output)

        # Parse results
        m = re.search(r'Throughput:\s+(\d+)\s+bps', output)
        if m:
            result["bps"] = int(m.group(1))
        m = re.search(r'RX bytes:\s+(\d+)', output)
        if m:
            result["bytes"] = int(m.group(1))
        if "DATA INTEGRITY: PASS" in output:
            result["integrity"] = "PASS"
        elif "DATA INTEGRITY: FAIL" in output:
            result["integrity"] = "FAIL"
        elif "NO DATA RECEIVED" in output:
            result["integrity"] = "NO DATA"

        # Count ACK-GATE
        passes = output.count("[ACK-GATE] PASS")
        suppressions = output.count("[ACK-GATE] Suppressing")
        result["ack_gate"] = f"{passes}P/{suppressions}S"
        result["suppressions"] = suppressions

        if "Connection failed" in output or "Cannot connect" in output:
            result["integrity"] = "CONN FAIL"

    except subprocess.TimeoutExpired:
        result["integrity"] = "TIMEOUT"
        print(f"  TIMEOUT after {duration + 180}s")
        os.system("taskkill /F /IM mercury.exe 2>nul >nul")

    return result


def main():
    parser = argparse.ArgumentParser(description="Sweep Mercury configs")
    parser.add_argument("--file", default="tools/pg84_15k.txt",
                        help="Text file to send")
    parser.add_argument("--min-config", type=int, default=0,
                        help="Minimum config to test (default: 0)")
    parser.add_argument("--max-config", type=int, default=16,
                        help="Maximum config to test (default: 16)")
    parser.add_argument("--nb-only", action="store_true",
                        help="Only test narrowband")
    parser.add_argument("--wb-only", action="store_true",
                        help="Only test wideband")
    args = parser.parse_args()

    results = []

    # Build config list: fastest first
    configs = list(range(args.max_config, args.min_config - 1, -1))

    # NB goes up to CONFIG_16, WB up to CONFIG_15
    for config in configs:
        if not args.wb_only:
            # NB test
            dur = CONFIG_DURATIONS.get(config, 120)
            r = run_config(config, "text", args.file, wideband=False, duration=dur)
            results.append(r)
            time.sleep(3)

        if not args.nb_only:
            # WB test (max CONFIG_15)
            if config <= 15:
                dur = CONFIG_DURATIONS.get(config, 120)
                r = run_config(config, "text", args.file, wideband=True, duration=dur)
                results.append(r)
                time.sleep(3)

    # Print summary table
    print(f"\n{'='*70}")
    print(f"  COMPRESSION THROUGHPUT SWEEP — SUMMARY")
    print(f"{'='*70}")
    print(f"{'Config':<10} {'BW':<4} {'bps':>8} {'bytes':>10} {'Integrity':<10} {'ACK-Gate':<12}")
    print(f"{'-'*10} {'-'*4} {'-'*8} {'-'*10} {'-'*10} {'-'*12}")
    for r in results:
        print(f"CONFIG_{r['config']:<3} {r['bw']:<4} {r['bps']:>8} {r['bytes']:>10} "
              f"{r['integrity']:<10} {r['ack_gate']:<12}")

    # Totals
    total_pass = sum(1 for r in results if r["integrity"] == "PASS")
    total_fail = sum(1 for r in results if r["integrity"] == "FAIL")
    total_supp = sum(r["suppressions"] for r in results)
    print(f"\nPASS: {total_pass}  FAIL: {total_fail}  Total suppressions: {total_supp}")


if __name__ == "__main__":
    main()
