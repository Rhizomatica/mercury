#!/usr/bin/env python3
"""
Auto-compression loopback test for Mercury.

Tests three modes:
  1. Force compress (-F on): compression ON from start, no B2F needed
  2. B2F auto-detect: send SID-prefixed data, verify deferred arming
  3. No compression (default): no -F, no SID, verify stays OFF

Usage:
  python tools/test_autocompress.py --mode force     # Test -F on
  python tools/test_autocompress.py --mode b2f       # Test B2F auto-detect
  python tools/test_autocompress.py --mode none       # Verify no compression
  python tools/test_autocompress.py --mode all        # Run all three
"""

import argparse
import os
import socket
import subprocess
import sys
import threading
import time

MERCURY = r"C:\Program Files\Mercury\mercury.exe"
VB_IN = "CABLE Output"
VB_OUT = "CABLE Input"
RSP_PORT = 7015
CMD_PORT = 7025
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))

def tcp_connect(port, timeout=30):
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
    try:
        for line in iter(proc.stdout.readline, b''):
            if stop_event.is_set():
                break
            text = line.decode(errors='replace').rstrip()
            lines.append(text)
    except:
        pass

def run_test(mode, config=6, duration=45, gearshift=False):
    """Run a single auto-compression test.

    mode: 'force' | 'b2f' | 'none'
    """
    print(f"\n{'='*60}")
    print(f"  AUTO-COMPRESS TEST: mode={mode}, config={config}")
    print(f"{'='*60}\n")

    # Prepare TX data
    if mode == "b2f":
        b2f_file = os.path.join(TOOLS_DIR, "pg84_b2f.txt")
        if not os.path.exists(b2f_file):
            print(f"ERROR: {b2f_file} not found. Run this script from mercury/tools/")
            return False
        with open(b2f_file, "rb") as f:
            tx_data = f.read()
        print(f"TX data: pg84_b2f.txt ({len(tx_data)} bytes, B2F SID header)")
    else:
        pg84_file = os.path.join(TOOLS_DIR, "pg84.txt")
        if not os.path.exists(pg84_file):
            print(f"ERROR: {pg84_file} not found")
            return False
        with open(pg84_file, "rb") as f:
            tx_data = f.read()
        print(f"TX data: pg84_full.txt ({len(tx_data)} bytes, plain text)")

    # Kill any existing Mercury instances
    os.system("taskkill /F /IM mercury.exe 2>nul >nul")
    time.sleep(1)

    stop_event = threading.Event()
    rsp_lines = []
    cmd_lines = []

    # Build mercury command lines
    force_flag = ["-F", "on"] if mode == "force" else []
    gear_flag = ["-g"] if gearshift else []

    rsp_cmd = [
        MERCURY, "-m", "ARQ", "-s", str(config),
        *gear_flag, *force_flag,
        "-p", str(RSP_PORT), "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi",
        "-Q", "0"
    ]
    cmd_cmd = [
        MERCURY, "-m", "ARQ", "-s", str(config),
        *gear_flag, *force_flag,
        "-p", str(CMD_PORT), "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi",
        "-Q", "0"
    ]

    print(f"RSP: {' '.join(rsp_cmd)}")
    print(f"CMD: {' '.join(cmd_cmd)}")

    # Launch
    rsp = subprocess.Popen(rsp_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    threading.Thread(target=collect_output, args=(rsp, rsp_lines, stop_event), daemon=True).start()
    time.sleep(4)

    cmd = subprocess.Popen(cmd_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    threading.Thread(target=collect_output, args=(cmd, cmd_lines, stop_event), daemon=True).start()
    time.sleep(4)

    # Connect TCP
    print("Connecting TCP sockets...")
    try:
        rsp_ctrl = tcp_connect(RSP_PORT)
        rsp_data = tcp_connect(RSP_PORT + 1)
        cmd_ctrl = tcp_connect(CMD_PORT)
        cmd_data = tcp_connect(CMD_PORT + 1)
    except TimeoutError as e:
        print(f"ERROR: {e}")
        stop_event.set()
        rsp.kill(); cmd.kill()
        return False

    # Set up responder
    rsp_ctrl.sendall(b"MYCALL TESTB\r")
    time.sleep(0.5)
    rsp_ctrl.sendall(b"LISTEN ON\r")
    time.sleep(1)

    # RX thread
    rx_bytes = [0]
    rx_lock = threading.Lock()

    def rx_loop():
        rsp_data.settimeout(2)
        while not stop_event.is_set():
            try:
                data = rsp_data.recv(4096)
                if data:
                    with rx_lock:
                        rx_bytes[0] += len(data)
            except socket.timeout:
                continue
            except (ConnectionError, OSError):
                break

    rx_thread = threading.Thread(target=rx_loop, daemon=True)
    rx_thread.start()

    # TX thread
    tx_sent = [0]

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
                tx_sent[0] += len(chunk)
                pos = end
                if pos >= len(tx_data):
                    pos = 0
            except socket.timeout:
                continue
            except (ConnectionError, OSError):
                break

    # Connect FIRST, then start TX thread (B2F handler only initialized after connection)
    cmd_ctrl.sendall(b"CONNECT TESTA TESTB\r")
    print("Waiting for connection...")

    # Wait for CONNECTED
    cmd_ctrl.settimeout(2)
    connected = False
    start = time.time()
    buf = b''
    while time.time() - start < 120:
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
        print("ERROR: Connection failed")
        stop_event.set()
        rsp.kill(); cmd.kill()
        return False

    print(f"CONNECTED on config {config}")
    time.sleep(2)

    # Start TX AFTER connection (B2F filter_tx needs handler initialized)
    tx_thread = threading.Thread(target=tx_loop, daemon=True)
    tx_thread.start()

    # Measure for duration
    with rx_lock:
        start_rx = rx_bytes[0]
    start_time = time.time()

    print(f"Sending data for {duration}s...")
    for t in range(duration):
        time.sleep(1)
        if (t + 1) % 10 == 0:
            with rx_lock:
                cur_rx = rx_bytes[0] - start_rx
            elapsed = time.time() - start_time
            bps = (cur_rx * 8) / elapsed if elapsed > 0 else 0
            print(f"  [{t+1}s] RX={cur_rx} bytes, {bps:.0f} bps")

    end_time = time.time()
    with rx_lock:
        total_rx = rx_bytes[0] - start_rx
    total_time = end_time - start_time
    bps = (total_rx * 8) / total_time if total_time > 0 else 0

    # ============ Analyze Results ============
    print(f"\n{'='*60}")
    print(f"  RESULTS: mode={mode}")
    print(f"{'='*60}")
    print(f"Duration: {total_time:.1f}s")
    print(f"RX bytes: {total_rx}")
    print(f"Throughput: {bps:.0f} bps")

    # Extract key log lines
    def grep(lines, patterns):
        return [l for l in lines if any(p in l for p in patterns)]

    cmd_compress = grep(cmd_lines, ["[COMPRESS]"])
    rsp_compress = grep(rsp_lines, ["[COMPRESS]"])
    cmd_b2f = grep(cmd_lines, ["[B2F-TX]", "[B2F-RX]", "[B2F]"])
    rsp_b2f = grep(rsp_lines, ["[B2F-TX]", "[B2F-RX]", "[B2F]"])
    cmd_decompress = grep(cmd_lines, ["[COMPRESS-TX]", "[DECOMPRESS]"])
    rsp_decompress = grep(rsp_lines, ["[COMPRESS-TX]", "[DECOMPRESS]", "[ASSEMBLE]"])
    cmd_overload = grep(cmd_lines, ["[RX-OVERLOAD]"])
    rsp_overload = grep(rsp_lines, ["[RX-OVERLOAD]"])

    print(f"\n--- Commander Compression Log ({len(cmd_compress)} entries) ---")
    for l in cmd_compress[:15]:
        print(f"  {l}")
    if len(cmd_compress) > 15:
        print(f"  ... ({len(cmd_compress) - 15} more)")

    print(f"\n--- Responder Compression Log ({len(rsp_compress)} entries) ---")
    for l in rsp_compress[:15]:
        print(f"  {l}")
    if len(rsp_compress) > 15:
        print(f"  ... ({len(rsp_compress) - 15} more)")

    if cmd_b2f or rsp_b2f:
        print(f"\n--- B2F Detection Log ---")
        for l in (cmd_b2f + rsp_b2f)[:10]:
            print(f"  {l}")

    if cmd_decompress or rsp_decompress:
        print(f"\n--- Decompression Log ({len(rsp_decompress)} entries) ---")
        for l in rsp_decompress[:10]:
            print(f"  {l}")
        if len(rsp_decompress) > 10:
            print(f"  ... ({len(rsp_decompress) - 10} more)")

    if cmd_overload or rsp_overload:
        print(f"\n--- RX Overload ---")
        for l in (cmd_overload + rsp_overload)[:5]:
            print(f"  {l}")

    # NAck count
    cmd_nacks = grep(cmd_lines, ["NAck", "NACK", "nack"])
    rsp_nacks = grep(rsp_lines, ["NAck", "NACK", "nack"])
    nack_total = len(cmd_nacks) + len(rsp_nacks)
    print(f"\nNAcks: {nack_total}")

    # ============ Verdict ============
    passed = True
    verdicts = []

    if mode == "force":
        # Expect: "Force-enabled" on both sides, compression active
        cmd_force = any("Force-enabled" in l for l in cmd_compress)
        rsp_force = any("Force-enabled" in l for l in rsp_compress)
        if cmd_force and rsp_force:
            verdicts.append("PASS: Force-enabled on both sides")
        else:
            verdicts.append(f"FAIL: Force-enabled CMD={cmd_force} RSP={rsp_force}")
            passed = False

        has_compress_tx = any("[COMPRESS-TX]" in l or "algo=" in l or "zstd" in l.lower() or "ppmd" in l.lower()
                              for l in cmd_lines)
        if has_compress_tx or total_rx > 0:
            verdicts.append(f"PASS: Data flowing ({total_rx} bytes)")
        else:
            verdicts.append("FAIL: No data received")
            passed = False

    elif mode == "b2f":
        # Expect: "Deferred" on both sides, then B2F detection, then armed
        cmd_deferred = any("Deferred" in l for l in cmd_compress)
        rsp_deferred = any("Deferred" in l for l in rsp_compress)
        if cmd_deferred and rsp_deferred:
            verdicts.append("PASS: Deferred negotiation on both sides")
        else:
            verdicts.append(f"FAIL: Deferred CMD={cmd_deferred} RSP={rsp_deferred}")
            passed = False

        cmd_b2f_detect = any("B2F detected" in l for l in cmd_compress)
        if cmd_b2f_detect:
            verdicts.append("PASS: Commander detected B2F")
        else:
            verdicts.append("FAIL: Commander did not detect B2F")
            passed = False

        cmd_armed = any("Armed after B2F ACK" in l for l in cmd_compress)
        if cmd_armed:
            verdicts.append("PASS: Commander armed after ACK")
        else:
            verdicts.append("FAIL: Commander did not arm after ACK")
            passed = False

        rsp_armed = any("Armed by B2F detection" in l for l in rsp_compress)
        if rsp_armed:
            verdicts.append("PASS: Responder armed by B2F detection")
        else:
            verdicts.append("FAIL: Responder did not arm")
            passed = False

        if total_rx > 0:
            verdicts.append(f"PASS: Data flowing ({total_rx} bytes)")
        else:
            verdicts.append("FAIL: No data received")
            passed = False

    elif mode == "none":
        # Expect: "Deferred" on both sides, NO arming
        cmd_deferred = any("Deferred" in l for l in cmd_compress)
        if cmd_deferred:
            verdicts.append("PASS: Deferred negotiation (not force-enabled)")
        else:
            # Could also have no compress lines at all if capabilities don't match
            cmd_force = any("Force-enabled" in l for l in cmd_compress)
            if cmd_force:
                verdicts.append("FAIL: Unexpectedly force-enabled")
                passed = False
            else:
                verdicts.append("PASS: No force-enable")

        cmd_armed = any("Armed" in l for l in cmd_compress)
        rsp_armed = any("Armed" in l for l in rsp_compress)
        if not cmd_armed and not rsp_armed:
            verdicts.append("PASS: Compression stayed OFF (no arming)")
        else:
            verdicts.append(f"FAIL: Unexpected arming CMD={cmd_armed} RSP={rsp_armed}")
            passed = False

        if total_rx > 0:
            verdicts.append(f"PASS: Data flowing without compression ({total_rx} bytes)")
        else:
            verdicts.append("FAIL: No data received")
            passed = False

    print(f"\n--- Verdict ---")
    for v in verdicts:
        status = "OK" if v.startswith("PASS") else "!!"
        print(f"  [{status}] {v}")
    print(f"\n  {'*** PASS ***' if passed else '*** FAIL ***'}")

    # Cleanup
    stop_event.set()
    time.sleep(1)
    for proc in [rsp, cmd]:
        try:
            proc.kill()
        except:
            pass
    time.sleep(2)

    return passed

def main():
    parser = argparse.ArgumentParser(description="Mercury auto-compression test")
    parser.add_argument("--mode", choices=["force", "b2f", "none", "all"], default="all",
                        help="Test mode (default: all)")
    parser.add_argument("--config", type=int, default=6, help="PHY config (default: 6)")
    parser.add_argument("--duration", type=int, default=45, help="Duration per test in seconds")
    parser.add_argument("--gearshift", action="store_true", help="Enable gearshift")
    args = parser.parse_args()

    if args.mode == "all":
        modes = ["force", "none", "b2f"]
    else:
        modes = [args.mode]

    results = {}
    for m in modes:
        ok = run_test(m, config=args.config, duration=args.duration, gearshift=args.gearshift)
        results[m] = ok

    print(f"\n{'='*60}")
    print(f"  SUMMARY")
    print(f"{'='*60}")
    all_pass = True
    for m, ok in results.items():
        status = "PASS" if ok else "FAIL"
        print(f"  {m:10s}: {status}")
        if not ok:
            all_pass = False
    print(f"\n  Overall: {'ALL PASS' if all_pass else 'SOME FAILED'}")

if __name__ == "__main__":
    main()
