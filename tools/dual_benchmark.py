#!/usr/bin/env python3
"""
Dual benchmark: test all Mercury speed modes on VB-Cable.
Tests each config in isolation (no gearshift) with text and binary data,
across both narrowband and wideband modes.

Usage:
  python tools/dual_benchmark.py
  python tools/dual_benchmark.py --bandwidth nb --configs 100,101,0 --data-type text
  python tools/dual_benchmark.py --bandwidth wb --configs 10 --duration 30
  python tools/dual_benchmark.py --skip-robust --data-type binary
"""

import argparse
import csv
import os
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime

MERCURY = r"C:\Program Files\Mercury\mercury.exe"
VB_IN = "CABLE Output"
VB_OUT = "CABLE Input"
RSP_PORT = 7015
CMD_PORT = 7025

# Config definitions matching common_defines.h
NB_CONFIGS = [100, 101, 102] + list(range(0, 17))  # ROBUST_0-2, CONFIG_0-16
WB_CONFIGS = [100, 101, 102] + list(range(0, 16))   # ROBUST_0-2, CONFIG_0-15

CONFIG_NAMES = {
    100: "ROBUST_0", 101: "ROBUST_1", 102: "ROBUST_2",
    **{i: f"CONFIG_{i}" for i in range(17)}
}


def get_duration(config, override=None):
    """Get test duration for a config."""
    if override:
        return override
    if config >= 100:       # ROBUST modes
        return 45
    elif config <= 6:       # CONFIG_0-6 (slow)
        return 45
    else:                   # CONFIG_7-16 (fast)
        return 30


def tcp_connect(port, timeout=30):
    """Connect to localhost:port with retries."""
    start = time.time()
    while time.time() - start < timeout:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(5)
            s.connect(("127.0.0.1", port))
            return s
        except (ConnectionRefusedError, socket.timeout, OSError):
            time.sleep(1)
    raise TimeoutError(f"Cannot connect to port {port}")


def collect_output(proc, lines, stop_event):
    """Collect stdout from a subprocess."""
    try:
        for line in iter(proc.stdout.readline, b''):
            if stop_event.is_set():
                break
            text = line.decode(errors='replace').rstrip()
            lines.append(text)
    except Exception:
        pass


def run_single_test(mercury_bin, config, bandwidth, data_type, text_data,
                    duration, logs_dir=None, verbose=False):
    """Run a single benchmark test. Returns a result dict."""
    config_name = CONFIG_NAMES.get(config, f"UNKNOWN_{config}")
    is_robust = config >= 100
    is_nb = (bandwidth == "nb")

    print(f"\n{'='*70}")
    print(f"  {bandwidth.upper()} | {config_name} | {data_type} | {duration}s")
    print(f"{'='*70}")

    # Prepare TX data
    if data_type == "text":
        tx_data = text_data
    else:
        tx_data = bytes(range(256)) * 4  # 1024 bytes repeating

    # Kill any existing Mercury instances
    os.system("taskkill /F /IM mercury.exe 2>nul >nul")
    time.sleep(1)

    stop_event = threading.Event()
    rsp_lines = []
    cmd_lines = []

    # Build command flags
    robust_flag = ["-R"] if is_robust else []
    nb_flags = ["-M", "nb", "-G", "8.3", "-Q", "0"] if is_nb else ["-M", "auto", "-Q", "0"]

    # Start responder
    rsp_cmd = [
        mercury_bin, "-m", "ARQ", "-s", str(config),
        *robust_flag, *nb_flags,
        "-p", str(RSP_PORT), "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi", "-n",
    ]

    print(f"  RSP: {' '.join(rsp_cmd)}")
    rsp = subprocess.Popen(rsp_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    threading.Thread(target=collect_output, args=(rsp, rsp_lines, stop_event),
                     daemon=True).start()
    time.sleep(4)

    # Start commander
    cmd_cmd = [
        mercury_bin, "-m", "ARQ", "-s", str(config),
        *robust_flag, *nb_flags,
        "-p", str(CMD_PORT), "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi", "-n",
    ]

    print(f"  CMD: {' '.join(cmd_cmd)}")
    cmd = subprocess.Popen(cmd_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    threading.Thread(target=collect_output, args=(cmd, cmd_lines, stop_event),
                     daemon=True).start()
    time.sleep(4)

    result = {
        "bandwidth": bandwidth,
        "config": config,
        "config_name": config_name,
        "data_type": data_type,
        "duration_s": 0,
        "rx_bytes": 0,
        "throughput_bps": 0,
        "integrity": "N/A",
        "ok_decodes": 0,
        "fail_decodes": 0,
        "notes": "",
    }

    try:
        # Connect TCP
        print("  Connecting TCP sockets...")
        rsp_ctrl = tcp_connect(RSP_PORT)
        rsp_data = tcp_connect(RSP_PORT + 1)
        cmd_ctrl = tcp_connect(CMD_PORT)
        cmd_data = tcp_connect(CMD_PORT + 1)

        # Set up responder
        rsp_ctrl.sendall(b"MYCALL TESTB\r")
        time.sleep(0.5)
        rsp_ctrl.sendall(b"LISTEN ON\r")
        time.sleep(1)

        # RX thread
        rx_bytes = [0]
        rx_buf = bytearray()
        rx_lock = threading.Lock()

        def rx_loop():
            rsp_data.settimeout(2)
            while not stop_event.is_set():
                try:
                    data = rsp_data.recv(4096)
                    if data:
                        with rx_lock:
                            rx_bytes[0] += len(data)
                            rx_buf.extend(data)
                except socket.timeout:
                    continue
                except (ConnectionError, OSError):
                    break

        rx_thread = threading.Thread(target=rx_loop, daemon=True)
        rx_thread.start()

        # TX thread
        def tx_loop():
            cmd_data.settimeout(30)
            pos = 0
            while not stop_event.is_set():
                end = min(pos + 1024, len(tx_data))
                chunk = tx_data[pos:end]
                if not chunk:
                    pos = 0
                    continue
                try:
                    cmd_data.sendall(chunk)
                    pos = end
                    if pos >= len(tx_data):
                        pos = 0
                except socket.timeout:
                    continue
                except (ConnectionError, OSError):
                    break

        tx_thread = threading.Thread(target=tx_loop, daemon=True)
        tx_thread.start()

        # Connect
        cmd_ctrl.sendall(b"CONNECT TESTA TESTB\r")
        print("  Waiting for connection...")

        # Wait for CONNECTED
        cmd_ctrl.settimeout(2)
        connected = False
        connect_timeout = 180 if is_robust else 120
        start = time.time()
        buf = b''
        while time.time() - start < connect_timeout:
            try:
                data = cmd_ctrl.recv(4096)
                if data:
                    buf += data
                    while b'\r' in buf:
                        idx = buf.find(b'\r')
                        line = buf[:idx]
                        buf = buf[idx+1:]
                        if b'CONNECTED' in line and b'DISCONNECTED' not in line:
                            connected = True
                            break
                    if connected:
                        break
            except socket.timeout:
                continue

        if not connected:
            print("  ERROR: Connection failed")
            result["notes"] = "CONNECTION_FAILED"
            return result

        print(f"  CONNECTED")
        time.sleep(2)

        # Measure throughput
        with rx_lock:
            start_rx = rx_bytes[0]
        start_time = time.time()

        print(f"  Measuring throughput for {duration}s...")
        for t in range(duration):
            time.sleep(1)
            if (t + 1) % 10 == 0:
                with rx_lock:
                    cur_rx = rx_bytes[0] - start_rx
                elapsed = time.time() - start_time
                bps = (cur_rx * 8) / elapsed if elapsed > 0 else 0
                print(f"    [{t+1}s] RX={cur_rx} bytes, {bps:.0f} bps")

        end_time = time.time()
        with rx_lock:
            end_rx = rx_bytes[0]

        total_rx = end_rx - start_rx
        total_time = end_time - start_time
        bps = (total_rx * 8) / total_time if total_time > 0 else 0

        result["duration_s"] = round(total_time, 1)
        result["rx_bytes"] = total_rx
        result["throughput_bps"] = round(bps)

        # Data integrity check
        with rx_lock:
            rx_data = bytes(rx_buf)
        if len(rx_data) > 0:
            expected_len = len(rx_data)
            expected = bytearray()
            while len(expected) < expected_len:
                remaining = expected_len - len(expected)
                expected.extend(tx_data[:remaining])
            expected = bytes(expected)

            if rx_data == expected:
                result["integrity"] = "PASS"
            else:
                first_err = -1
                for i in range(min(len(rx_data), len(expected))):
                    if rx_data[i] != expected[i]:
                        first_err = i
                        break
                if first_err >= 0:
                    errors = sum(1 for i in range(min(len(rx_data), len(expected)))
                                 if rx_data[i] != expected[i])
                    result["integrity"] = f"FAIL@{first_err}"
                    result["notes"] = f"{errors}/{len(rx_data)} mismatched"
                else:
                    result["integrity"] = "PASS"
        else:
            result["integrity"] = "NO_DATA"

        # Count decode stats from stdout
        ok_decodes = [l for l in rsp_lines if "[RX-DECODE" in l and "OK:" in l]
        fail_decodes = [l for l in rsp_lines
                        if "[RX-DECODE" in l and ("FAIL:" in l or "NO-PREAMBLE" in l)]
        result["ok_decodes"] = len(ok_decodes)
        result["fail_decodes"] = len(fail_decodes)

        # Always show ACK-GATE diagnostics (batch completeness)
        for l in rsp_lines:
            if "[ACK-GATE" in l:
                print(f"    {l[:200]}")

        # Dump diagnostic lines when retransmit or NO_DATA
        has_retransmit = len(fail_decodes) > 0
        if result["integrity"] == "NO_DATA" or total_rx == 0 or has_retransmit:
            diag_tags = ["[FTR-OK]", "[FTR-FAIL]", "[RX-DECODE",
                         "[CFG]", "[OFDM-SKIP]", "[RX-TIMING]", "[NB-NEG]"]
            label = "NO_DATA" if (result["integrity"] == "NO_DATA" or total_rx == 0) else "RETRANSMIT"
            print(f"  --- RSP diagnostic [{label}] ({len(rsp_lines)} lines) ---")
            for l in rsp_lines:
                if any(t in l for t in diag_tags):
                    print(f"    {l[:200]}")
            print(f"  --- CMD diagnostic [{label}] ({len(cmd_lines)} lines) ---")
            for l in cmd_lines:
                if any(t in l for t in diag_tags):
                    print(f"    {l[:200]}")

        # Close sockets
        for s in [rsp_ctrl, rsp_data, cmd_ctrl, cmd_data]:
            try:
                s.close()
            except Exception:
                pass

    except TimeoutError as e:
        print(f"  ERROR: {e}")
        result["notes"] = "TCP_TIMEOUT"
    except Exception as e:
        print(f"  ERROR: {e}")
        result["notes"] = str(e)[:80]
    finally:
        stop_event.set()
        time.sleep(1)
        for proc in [rsp, cmd]:
            try:
                proc.kill()
            except Exception:
                pass

    # Save per-test logs for post-mortem debugging
    if logs_dir:
        tag = f"{bandwidth}_{config_name}_{data_type}"
        for label, lines in [("rsp", rsp_lines), ("cmd", cmd_lines)]:
            log_path = os.path.join(logs_dir, f"{tag}_{label}.log")
            try:
                with open(log_path, "w", encoding="utf-8", errors="replace") as f:
                    f.write(f"# {label.upper()} log for {tag}\n")
                    f.write(f"# Result: {result['integrity']} "
                            f"OK={result['ok_decodes']} FAIL={result['fail_decodes']} "
                            f"rx_bytes={result['rx_bytes']}\n\n")
                    f.write("\n".join(lines))
            except Exception as e:
                print(f"  WARNING: Could not save {label} log: {e}")

    # Print result summary
    print(f"  Result: {result['rx_bytes']} bytes, {result['throughput_bps']} bps, "
          f"integrity={result['integrity']}, "
          f"OK={result['ok_decodes']} FAIL={result['fail_decodes']}")

    return result


def _dedup_flag(cmd, flag):
    """Remove duplicate occurrences of a flag (keep last)."""
    positions = [i for i, x in enumerate(cmd) if x == flag]
    if len(positions) <= 1:
        return cmd
    # Remove all but last occurrence (flag + its value)
    to_remove = set()
    for pos in positions[:-1]:
        to_remove.add(pos)
        if pos + 1 < len(cmd) and not cmd[pos + 1].startswith("-"):
            to_remove.add(pos + 1)
    return [x for i, x in enumerate(cmd) if i not in to_remove]


def main():
    parser = argparse.ArgumentParser(description="Mercury dual benchmark")
    parser.add_argument("--mercury", default=MERCURY, help="Mercury binary path")
    parser.add_argument("--text-file", default=None,
                        help="Text file for text tests (default: tools/pg84.txt)")
    parser.add_argument("--bandwidth", choices=["nb", "wb", "both"], default="both",
                        help="Which bandwidths to test")
    parser.add_argument("--configs", default=None,
                        help="Comma-separated config list (e.g. 100,0,10)")
    parser.add_argument("--data-type", choices=["text", "binary", "both"],
                        default="both", help="Data types to test")
    parser.add_argument("--duration", type=int, default=None,
                        help="Override per-test duration in seconds")
    parser.add_argument("--skip-robust", action="store_true",
                        help="Skip ROBUST modes")
    args = parser.parse_args()

    # Find text file
    text_file = args.text_file
    if not text_file:
        # Try relative paths
        for candidate in ["tools/pg84.txt", "pg84.txt",
                          os.path.join(os.path.dirname(__file__), "pg84.txt")]:
            if os.path.exists(candidate):
                text_file = candidate
                break
    if not text_file or not os.path.exists(text_file):
        print("ERROR: Cannot find text file. Use --text-file to specify.")
        sys.exit(1)

    with open(text_file, "rb") as f:
        text_data = f.read()
    print(f"Text source: {text_file} ({len(text_data)} bytes)")

    # Build test matrix
    bandwidths = []
    if args.bandwidth in ("nb", "both"):
        bandwidths.append("nb")
    if args.bandwidth in ("wb", "both"):
        bandwidths.append("wb")

    data_types = []
    if args.data_type in ("text", "both"):
        data_types.append("text")
    if args.data_type in ("binary", "both"):
        data_types.append("binary")

    # Parse config list or use defaults
    if args.configs:
        user_configs = [int(c.strip()) for c in args.configs.split(",")]
    else:
        user_configs = None

    tests = []
    for bw in bandwidths:
        if user_configs:
            configs = user_configs
        else:
            configs = NB_CONFIGS if bw == "nb" else WB_CONFIGS
            if args.skip_robust:
                configs = [c for c in configs if c < 100]
        for cfg in configs:
            for dt in data_types:
                tests.append((bw, cfg, dt))

    print(f"\nTest matrix: {len(tests)} tests")
    for bw, cfg, dt in tests:
        print(f"  {bw.upper()} {CONFIG_NAMES.get(cfg, cfg):>10} {dt}")

    # Create output directory
    results_dir = os.path.join(os.path.dirname(__file__), "benchmark_results")
    os.makedirs(results_dir, exist_ok=True)

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    csv_path = os.path.join(results_dir, f"dual_{timestamp}.csv")
    logs_dir = os.path.join(results_dir, f"dual_{timestamp}_logs")
    os.makedirs(logs_dir, exist_ok=True)

    # Run tests
    results = []
    start_total = time.time()

    for idx, (bw, cfg, dt) in enumerate(tests):
        print(f"\n[{idx+1}/{len(tests)}]", end="")
        dur = get_duration(cfg, args.duration)
        r = run_single_test(args.mercury, cfg, bw, dt, text_data, dur,
                            logs_dir=logs_dir)
        results.append(r)

        # Write CSV incrementally
        with open(csv_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=[
                "bandwidth", "config", "config_name", "data_type",
                "duration_s", "rx_bytes", "throughput_bps",
                "integrity", "ok_decodes", "fail_decodes", "notes"
            ])
            writer.writeheader()
            writer.writerows(results)

    total_time = time.time() - start_total

    # Print summary table
    print(f"\n\n{'='*80}")
    print(f"  DUAL BENCHMARK RESULTS — {len(results)} tests in {total_time:.0f}s")
    print(f"{'='*80}")
    print(f"{'BW':>4} {'Config':>10} {'Data':>6} {'RX bytes':>10} {'bps':>8} "
          f"{'Integrity':>12} {'OK':>4} {'FAIL':>4} {'Notes'}")
    print(f"{'-'*80}")

    pass_count = 0
    fail_count = 0
    for r in results:
        ok = r["integrity"] == "PASS"
        if ok:
            pass_count += 1
        else:
            fail_count += 1
        marker = "" if ok else " ***"
        print(f"{r['bandwidth']:>4} {r['config_name']:>10} {r['data_type']:>6} "
              f"{r['rx_bytes']:>10} {r['throughput_bps']:>8} "
              f"{r['integrity']:>12} {r['ok_decodes']:>4} {r['fail_decodes']:>4} "
              f"{r['notes']}{marker}")

    print(f"{'-'*80}")
    print(f"PASS: {pass_count}  FAIL: {fail_count}  TOTAL: {len(results)}")
    print(f"CSV: {csv_path}")


if __name__ == "__main__":
    main()
