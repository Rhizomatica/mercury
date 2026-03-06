#!/usr/bin/env python3
"""
Quick compression loopback test for Mercury.
Launches commander + responder on VB-Cable at a fixed config,
sends either random binary or text data, measures throughput.

Usage:
  python tools/compression_test.py --config 10 --mode binary --duration 60
  python tools/compression_test.py --config 10 --mode text --file pg84.txt --duration 60
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
    except:
        pass

def main():
    parser = argparse.ArgumentParser(description="Mercury compression loopback test")
    parser.add_argument("--config", type=int, default=10, help="PHY config (default: 10)")
    parser.add_argument("--mode", choices=["binary", "text"], default="binary")
    parser.add_argument("--file", default=None, help="Text file to send (required for text mode)")
    parser.add_argument("--duration", type=int, default=60, help="Measurement duration in seconds")
    parser.add_argument("--mercury", default=MERCURY, help="Mercury binary path")
    parser.add_argument("--gearshift", action="store_true", help="Enable gearshift")
    parser.add_argument("--wideband", action="store_true", help="Use wideband mode (default: narrowband)")
    parser.add_argument("--encrypt", action="store_true", help="Enable encryption (-E strict)")
    args = parser.parse_args()

    if args.mode == "text" and not args.file:
        print("ERROR: --file required for text mode")
        sys.exit(1)

    # Prepare TX data
    if args.mode == "binary":
        tx_data = bytes(range(256)) * 4  # 1024 bytes, high entropy
        print(f"TX data: binary pattern (1024 bytes, repeating 0x00-0xFF)")
    else:
        with open(args.file, "rb") as f:
            tx_data = f.read()
        print(f"TX data: {args.file} ({len(tx_data)} bytes)")

    # Kill any existing Mercury instances
    os.system("taskkill /F /IM mercury.exe 2>nul >nul")
    time.sleep(1)

    stop_event = threading.Event()
    rsp_lines = []
    cmd_lines = []

    robust_flag = ["-R"] if args.config >= 100 else []
    gear_flag = ["-g"] if args.gearshift else []
    nb_flag = [] if args.wideband else ["-n"]
    encrypt_flag = ["-E", "strict"] if args.encrypt else []
    compress_flag = ["-F", "on"]  # Always force compression on for this test

    # Start responder
    rsp_cmd = [
        args.mercury, "-m", "ARQ", "-s", str(args.config),
        *robust_flag, *gear_flag, *encrypt_flag, *compress_flag,
        "-p", str(RSP_PORT), "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi", *nb_flag,
        "-Q", "0"
    ]
    print(f"RSP: {' '.join(rsp_cmd)}")
    rsp = subprocess.Popen(rsp_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    threading.Thread(target=collect_output, args=(rsp, rsp_lines, stop_event), daemon=True).start()
    time.sleep(4)

    # Start commander
    cmd_cmd = [
        args.mercury, "-m", "ARQ", "-s", str(args.config),
        *robust_flag, *gear_flag, *encrypt_flag, *compress_flag,
        "-p", str(CMD_PORT), "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi", *nb_flag,
        "-Q", "0"
    ]
    print(f"CMD: {' '.join(cmd_cmd)}")
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
        rsp.kill()
        cmd.kill()
        sys.exit(1)

    # Set up responder
    rsp_ctrl.sendall(b"MYCALL TESTB\r")
    time.sleep(0.5)
    rsp_ctrl.sendall(b"LISTEN ON\r")
    time.sleep(1)

    # RX thread: collect received data (both count and raw bytes for integrity check)
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

    # TX thread: pump data into commander
    tx_sent = [0]
    tx_lock = threading.Lock()

    def tx_loop():
        cmd_data.settimeout(30)
        pos = 0
        while not stop_event.is_set():
            # Send in 1KB chunks, wrapping around
            end = min(pos + 1024, len(tx_data))
            chunk = tx_data[pos:end]
            if not chunk:
                pos = 0
                continue
            try:
                cmd_data.sendall(chunk)
                with tx_lock:
                    tx_sent[0] += len(chunk)
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
        rsp.kill()
        cmd.kill()
        sys.exit(1)

    print(f"CONNECTED on config {args.config}")
    if args.encrypt:
        print("Waiting for key exchange to complete...")
        time.sleep(15)  # Key exchange: ~2 round trips at OFDM speed
    else:
        time.sleep(2)

    # Measure throughput
    with rx_lock:
        start_rx = rx_bytes[0]
    start_time = time.time()

    print(f"Measuring throughput for {args.duration}s...")
    for t in range(args.duration):
        time.sleep(1)
        if (t + 1) % 10 == 0:
            with rx_lock:
                cur_rx = rx_bytes[0] - start_rx
            elapsed = time.time() - start_time
            bps = (cur_rx * 8) / elapsed
            print(f"  [{t+1}s] RX={cur_rx} bytes, {bps:.0f} bps")

    end_time = time.time()
    with rx_lock:
        end_rx = rx_bytes[0]

    total_rx = end_rx - start_rx
    total_time = end_time - start_time
    bps = (total_rx * 8) / total_time if total_time > 0 else 0
    bpm = (total_rx * 60) / total_time if total_time > 0 else 0

    print(f"\n=== RESULTS (config {args.config}, {args.mode}) ===")
    print(f"Duration: {total_time:.1f}s")
    print(f"RX bytes: {total_rx}")
    print(f"Throughput: {bps:.0f} bps ({bpm:.0f} bytes/min)")

    # Data integrity check: compare RX against TX stream
    with rx_lock:
        rx_data = bytes(rx_buf)
    if len(rx_data) > 0:
        # Build expected TX stream (tx_data repeated/wrapped to match RX length)
        expected_len = len(rx_data)
        expected = bytearray()
        while len(expected) < expected_len:
            remaining = expected_len - len(expected)
            expected.extend(tx_data[:remaining])
        expected = bytes(expected)

        if rx_data == expected:
            print(f"\n*** DATA INTEGRITY: PASS ({len(rx_data)} bytes verified) ***")
        else:
            # Find first mismatch
            first_err = -1
            for i in range(min(len(rx_data), len(expected))):
                if rx_data[i] != expected[i]:
                    first_err = i
                    break
            if first_err >= 0:
                ctx_start = max(0, first_err - 8)
                ctx_end = min(len(rx_data), first_err + 8)
                rx_hex = rx_data[ctx_start:ctx_end].hex(' ')
                ex_hex = expected[ctx_start:ctx_end].hex(' ')
                print(f"\n*** DATA INTEGRITY: FAIL ***")
                print(f"  First mismatch at byte {first_err} (of {len(rx_data)})")
                print(f"  TX position in source: {first_err % len(tx_data)}")
                print(f"  Expected: {ex_hex}")
                print(f"  Received: {rx_hex}")

                # Count total mismatched bytes
                errors = sum(1 for i in range(min(len(rx_data), len(expected)))
                             if rx_data[i] != expected[i])
                print(f"  Total mismatched bytes: {errors}/{len(rx_data)} ({100*errors/len(rx_data):.1f}%)")
            else:
                print(f"\n*** DATA INTEGRITY: PASS ({len(rx_data)} bytes verified) ***")
    else:
        print(f"\n*** DATA INTEGRITY: NO DATA RECEIVED ***")

    # Show crypto stats
    crypto_cmd = [l for l in cmd_lines if "[CRYPTO" in l or "[CMD-RX]" in l or "[ACK-CTRL]" in l]
    crypto_rsp = [l for l in rsp_lines if "[CRYPTO" in l or "[CMD-RX]" in l or "[ACK-CTRL]" in l]
    if crypto_cmd or crypto_rsp:
        print(f"\n=== ENCRYPTION ===")
        if crypto_cmd:
            print(f"Commander crypto ({len(crypto_cmd)} entries):")
            for l in crypto_cmd[:30]:
                print(f"  {l}")
        if crypto_rsp:
            print(f"Responder crypto ({len(crypto_rsp)} entries):")
            for l in crypto_rsp[:30]:
                print(f"  {l}")

    # Show streaming stats
    stream_lines = [l for l in cmd_lines + rsp_lines if "[STREAMING]" in l]
    if stream_lines:
        print(f"\nStreaming log ({len(stream_lines)} entries):")
        for l in stream_lines[:20]:
            print(f"  {l}")
        if len(stream_lines) > 20:
            print(f"  ... ({len(stream_lines) - 20} more)")

    # Show compression stats from stdout
    compress_lines = [l for l in cmd_lines if "[COMPRESS]" in l or "[COMPRESS-TX]" in l]
    decompress_lines = [l for l in rsp_lines if "[COMPRESS]" in l or "[DECOMPRESS]" in l or "[ASSEMBLE]" in l or "[ACK-GATE" in l]
    # Show OK and FAIL decode entries separately
    ok_decodes = [l for l in rsp_lines if "[RX-DECODE" in l and "OK:" in l]
    fail_decodes = [l for l in rsp_lines if "[RX-DECODE" in l and ("FAIL:" in l or "NO-PREAMBLE" in l)]
    ofdm_anti = [l for l in rsp_lines if "[OFDM-ANTI" in l]
    ofdm_skip = [l for l in rsp_lines if "[OFDM-SKIP]" in l]
    if compress_lines:
        print(f"\nCommander compression log ({len(compress_lines)} entries):")
        for l in compress_lines[:10]:
            print(f"  {l}")
        if len(compress_lines) > 10:
            print(f"  ... ({len(compress_lines) - 10} more)")
    if decompress_lines:
        print(f"\nResponder decompression/assembly log ({len(decompress_lines)} entries):")
        for l in decompress_lines[:20]:
            print(f"  {l}")
        if len(decompress_lines) > 20:
            print(f"  ... ({len(decompress_lines) - 20} more)")
    print(f"\nOK decodes: {len(ok_decodes)}  FAIL decodes: {len(fail_decodes)}")
    if ok_decodes:
        print(f"First 20 OK decodes:")
        for l in ok_decodes[:20]:
            print(f"  {l}")
    if fail_decodes:
        print(f"First 30 FAIL decodes:")
        for l in fail_decodes[:30]:
            print(f"  {l}")
        if len(fail_decodes) > 30:
            print(f"  ... ({len(fail_decodes) - 30} more)")
    if ofdm_anti:
        print(f"\nOFDM anti-re-decode/anti-spin ({len(ofdm_anti)} entries):")
        for l in ofdm_anti[:30]:
            print(f"  {l}")
        if len(ofdm_anti) > 30:
            print(f"  ... ({len(ofdm_anti) - 30} more)")
    if ofdm_skip:
        print(f"\nOFDM-SKIP (search_raw after SUCCESS, {len(ofdm_skip)} entries):")
        for l in ofdm_skip[:30]:
            print(f"  {l}")
        if len(ofdm_skip) > 30:
            print(f"  ... ({len(ofdm_skip) - 30} more)")

    # Cleanup
    stop_event.set()
    time.sleep(1)
    for proc in [rsp, cmd]:
        try:
            proc.kill()
        except:
            pass

if __name__ == "__main__":
    main()
