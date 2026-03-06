#!/usr/bin/env python3
"""
Encryption loopback test: verify encrypted ARQ session works end-to-end.
Runs commander + responder on VB-Cable with PSK + encryption enabled,
sends text data, verifies plaintext received matches.

Usage:
  python tools/encryption_test.py
  python tools/encryption_test.py --config 10 --bandwidth wb --duration 30
  python tools/encryption_test.py --config 4 --bandwidth nb --duration 60
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

# A fixed PSK for testing (32 bytes = 64 hex chars)
TEST_PSK = "deadbeefcafebabe0123456789abcdef0011223344556677" + "8899aabbccddeeff"


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
    except Exception:
        pass


def main():
    parser = argparse.ArgumentParser(description="Mercury encryption loopback test")
    parser.add_argument("--mercury", default=MERCURY, help="Mercury binary path")
    parser.add_argument("--config", type=int, default=10, help="Config number (default: 10)")
    parser.add_argument("--bandwidth", choices=["nb", "wb"], default="wb",
                        help="Bandwidth mode (default: wb)")
    parser.add_argument("--duration", type=int, default=30,
                        help="Measurement duration in seconds (default: 30)")
    parser.add_argument("--psk", default=TEST_PSK, help="PSK hex string")
    parser.add_argument("--encryption", choices=["strict", "fast"], default="strict",
                        help="Encryption mode (default: strict)")
    args = parser.parse_args()

    # Find text file for TX data
    text_file = None
    for candidate in ["tools/pg84.txt", "pg84.txt",
                      os.path.join(os.path.dirname(__file__), "pg84.txt")]:
        if os.path.exists(candidate):
            text_file = candidate
            break
    if not text_file:
        print("ERROR: Cannot find pg84.txt text file")
        sys.exit(1)

    with open(text_file, "rb") as f:
        tx_data = f.read()
    print(f"TX data: {text_file} ({len(tx_data)} bytes)")

    is_nb = args.bandwidth == "nb"
    config = args.config

    print(f"\n{'='*70}")
    print(f"  ENCRYPTION LOOPBACK TEST")
    print(f"  Config: CONFIG_{config} | BW: {args.bandwidth.upper()} | "
          f"Encryption: {args.encryption} | PSK: {args.psk[:16]}...")
    print(f"  Duration: {args.duration}s")
    print(f"{'='*70}")

    # Kill any existing instances
    os.system("taskkill /F /IM mercury.exe 2>nul >nul")
    time.sleep(1)

    stop_event = threading.Event()
    rsp_lines = []
    cmd_lines = []

    # Build flags
    if is_nb:
        bw_flags = ["-M", "nb", "-G", "8.3"]
    else:
        bw_flags = ["-M", "auto"]

    common_flags = [
        "-m", "ARQ", "-s", str(config),
        *bw_flags,
        "-F", "on",
        "-E", args.encryption,
        "-K", args.psk,
        "-i", VB_IN, "-o", VB_OUT, "-x", "wasapi",
    ]

    # Start responder
    rsp_cmd = [args.mercury, *common_flags, "-p", str(RSP_PORT)]
    print(f"  RSP: {' '.join(rsp_cmd)}")
    rsp = subprocess.Popen(rsp_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    threading.Thread(target=collect_output, args=(rsp, rsp_lines, stop_event),
                     daemon=True).start()
    time.sleep(4)

    # Start commander
    cmd_cmd = [args.mercury, *common_flags, "-p", str(CMD_PORT)]
    print(f"  CMD: {' '.join(cmd_cmd)}")
    cmd = subprocess.Popen(cmd_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    threading.Thread(target=collect_output, args=(cmd, cmd_lines, stop_event),
                     daemon=True).start()
    time.sleep(4)

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

        # RX collection
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

        # TX feeder
        def tx_loop():
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
                except (socket.timeout, ConnectionError, OSError):
                    break

        # Connect and wait for CONNECTED
        cmd_ctrl.sendall(b"CONNECT TESTA TESTB\r")
        print("  Waiting for connection + encryption key exchange...")

        cmd_ctrl.settimeout(2)
        connected = False
        buf = b''
        start = time.time()
        encryption_seen = False
        while time.time() - start < 180:
            try:
                data = cmd_ctrl.recv(4096)
                if data:
                    buf += data
                    while b'\r' in buf:
                        idx = buf.find(b'\r')
                        line = buf[:idx]
                        buf = buf[idx+1:]
                        line_str = line.decode(errors='replace')
                        if 'CONNECTED' in line_str and 'DISCONNECTED' not in line_str:
                            connected = True
                            break
                    if connected:
                        break
            except socket.timeout:
                continue

        if not connected:
            print("  ERROR: Connection failed")
            # Check logs for encryption-related messages
            for l in cmd_lines + rsp_lines:
                if any(k in l.lower() for k in ['encrypt', 'key', 'psk', 'kx', 'chacha']):
                    print(f"    {l[:200]}")
            raise Exception("CONNECTION_FAILED")

        print(f"  CONNECTED (elapsed {time.time()-start:.1f}s)")

        # Check for encryption activation in logs
        for l in cmd_lines + rsp_lines:
            if 'ENCRYPTION ACTIVE' in l or 'KX complete' in l or 'encryption' in l.lower():
                print(f"  >> {l[:200]}")
                encryption_seen = True

        # Start TX
        tx_thread = threading.Thread(target=tx_loop, daemon=True)
        tx_thread.start()
        time.sleep(0.5)

        # Measure throughput
        with rx_lock:
            start_rx = rx_bytes[0]
        start_time = time.time()

        print(f"  Measuring throughput for {args.duration}s...")
        for t in range(args.duration):
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

        # Data integrity check
        with rx_lock:
            rx_data_buf = bytes(rx_buf)
        if len(rx_data_buf) > 0:
            expected = bytearray()
            while len(expected) < len(rx_data_buf):
                remaining = len(rx_data_buf) - len(expected)
                expected.extend(tx_data[:remaining])
            expected = bytes(expected)

            if rx_data_buf == expected:
                integrity = "PASS"
            else:
                errors = sum(1 for i in range(min(len(rx_data_buf), len(expected)))
                             if rx_data_buf[i] != expected[i])
                integrity = f"FAIL ({errors}/{len(rx_data_buf)} mismatched)"
        else:
            integrity = "NO_DATA"

        # Check encryption evidence in logs
        for l in cmd_lines + rsp_lines:
            if any(k in l for k in ['ENCRYPTION ACTIVE', 'KX complete', 'ChaCha20',
                                     'X25519', 'PSK auth', 'key_exchange']):
                if not encryption_seen:
                    print(f"  >> {l[:200]}")
                encryption_seen = True

        # Print results
        print(f"\n{'='*70}")
        print(f"  RESULTS")
        print(f"{'='*70}")
        print(f"  RX bytes:    {total_rx}")
        print(f"  Throughput:  {bps:.0f} bps")
        print(f"  Integrity:   {integrity}")
        print(f"  Encryption:  {'CONFIRMED' if encryption_seen else 'NOT CONFIRMED (check logs)'}")
        print(f"{'='*70}")

        if integrity == "PASS" and encryption_seen:
            print(f"\n  *** ENCRYPTION TEST PASSED ***\n")
        elif integrity == "PASS":
            print(f"\n  *** DATA PASSED but encryption status unclear ***\n")
        else:
            print(f"\n  *** TEST FAILED: {integrity} ***\n")

        # Dump encryption-related log lines
        print("  Encryption log lines:")
        for label, lines in [("CMD", cmd_lines), ("RSP", rsp_lines)]:
            for l in lines:
                if any(k in l.lower() for k in ['encrypt', 'key', 'psk', 'kx',
                                                  'chacha', 'x25519', 'cap_enc',
                                                  'mlkem', 'monocypher']):
                    print(f"    [{label}] {l[:200]}")

        # Close sockets
        for s in [rsp_ctrl, rsp_data, cmd_ctrl, cmd_data]:
            try:
                s.close()
            except Exception:
                pass

    except TimeoutError as e:
        print(f"  ERROR: {e}")
    except Exception as e:
        print(f"  ERROR: {e}")
        # Dump all log lines for debugging
        print(f"\n  --- CMD log ({len(cmd_lines)} lines) ---")
        for l in cmd_lines[-30:]:
            print(f"    {l[:200]}")
        print(f"\n  --- RSP log ({len(rsp_lines)} lines) ---")
        for l in rsp_lines[-30:]:
            print(f"    {l[:200]}")
    finally:
        stop_event.set()
        time.sleep(1)
        for proc in [rsp, cmd]:
            try:
                proc.kill()
            except Exception:
                pass


if __name__ == "__main__":
    main()
